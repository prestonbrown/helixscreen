// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_invalid_sample_tracker.cpp
 * @brief Log-volume policy for permanently-rejected samples (#1348)
 *
 * TemperatureHistoryManager::add_sample_internal() used to log every rejected
 * sample. The reject predicate is permanent for an open or unmounted
 * thermistor, so on a six-tool machine with no tools mounted that came to
 * ~19 lines/sec forever: one field bundle was 95.5% this single message, which
 * evicted everything else from the 20,000-line crash ring and cut the bundle's
 * coverage to 16 minutes out of a 7.7 hour uptime.
 *
 * InvalidSampleTracker is the pure policy that fixes it: report the two ends of
 * a run of rejections plus a heartbeat on a doubling interval in between, so
 * log volume is O(log uptime) instead of O(uptime) while a maintainer can still
 * see which key was bad, what it was reading, for how long, and how many samples
 * were dropped.
 *
 * No LVGL, no clock, no logging — the tracker is a pure function of the verdicts
 * and timestamps fed to it, so these tests pin the policy directly.
 */

#include "../../include/invalid_sample_tracker.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::InvalidSampleLog;
using helix::InvalidSampleReport;
using helix::InvalidSampleTracker;

namespace {

constexpr int64_t ONE_SECOND_MS = 1000;
/// A thermistor reading -70°C, the shape reported in #1348 (min_temp: -100).
constexpr int OPEN_THERMISTOR_DECI = -700;
constexpr int SANE_DECI = 2050;

/// Feed @p count invalid samples at 1 Hz starting at @p start_ms, collecting
/// every loggable report along with the offset at which it fired.
struct Emission {
    int64_t at_ms;
    InvalidSampleReport report;
};

std::vector<Emission> feed_invalid(InvalidSampleTracker& tracker, const std::string& key,
                                   int64_t count, int64_t start_ms = 0) {
    std::vector<Emission> out;
    for (int64_t i = 0; i < count; ++i) {
        const int64_t t = start_ms + i * ONE_SECOND_MS;
        const InvalidSampleReport r = tracker.record(key, false, OPEN_THERMISTOR_DECI, t);
        if (r.loggable()) {
            out.push_back({t, r});
        }
    }
    return out;
}

} // namespace

// ============================================================================
// The bug itself: volume must not scale with sample count
// ============================================================================

TEST_CASE("InvalidSampleTracker bounds log volume over a permanent bad run", "[1348][logging]") {
    InvalidSampleTracker tracker;

    // One hour of 1 Hz samples from a thermistor that never recovers.
    constexpr int64_t SAMPLES = 3600;
    const auto emitted = feed_invalid(tracker, "extruder", SAMPLES);

    // The whole point: not one line per sample.
    CHECK(tracker.log_events() < SAMPLES / 100);
    CHECK(static_cast<int64_t>(emitted.size()) == tracker.log_events());

    // Backoff schedule from the public constants: the run opens at t=0, then
    // heartbeats fire the first time a sample lands at or past the window,
    // doubling from FIRST_REPEAT_MS each time —
    //   open  t=0
    //   +60s  t=60      next window 120s
    //   +120s t=180     next window 240s
    //   +240s t=420     next window 480s
    //   +480s t=900     next window 960s
    //   +960s t=1860    next window capped-bound 1800s
    //   (next would be t=3660, past the hour)
    REQUIRE(emitted.size() == 6);
    CHECK(emitted[0].at_ms == 0);
    CHECK(emitted[1].at_ms == 60 * ONE_SECOND_MS);
    CHECK(emitted[2].at_ms == 180 * ONE_SECOND_MS);
    CHECK(emitted[3].at_ms == 420 * ONE_SECOND_MS);
    CHECK(emitted[4].at_ms == 900 * ONE_SECOND_MS);
    CHECK(emitted[5].at_ms == 1860 * ONE_SECOND_MS);

    // Every sample is still counted even though almost none are logged.
    CHECK(tracker.dropped_in_run("extruder") == SAMPLES);
}

TEST_CASE("InvalidSampleTracker keeps the heartbeat under the cap over a long run",
          "[1348][logging]") {
    InvalidSampleTracker tracker;

    // The 7.7 hour uptime from the bundle in #1348.
    constexpr int64_t SAMPLES = 27720;
    const auto emitted = feed_invalid(tracker, "extruder5", SAMPLES);

    // ~20 lines instead of ~27,720. Bound it generously so the assertion is
    // about the growth rate, not the exact schedule (pinned above).
    CHECK(tracker.log_events() > 5);
    CHECK(tracker.log_events() < 30);

    // Once the interval saturates, spacing must be exactly MAX_REPEAT_MS.
    REQUIRE(emitted.size() >= 3);
    const int64_t last_gap = emitted.back().at_ms - emitted[emitted.size() - 2].at_ms;
    CHECK(last_gap == InvalidSampleTracker::MAX_REPEAT_MS);
    for (size_t i = 1; i < emitted.size(); ++i) {
        CHECK((emitted[i].at_ms - emitted[i - 1].at_ms) <= InvalidSampleTracker::MAX_REPEAT_MS);
    }
}

// ============================================================================
// Diagnostic content: the bounded lines must still say what went wrong
// ============================================================================

TEST_CASE("InvalidSampleTracker reports the opening transition with the offending value",
          "[1348][logging]") {
    InvalidSampleTracker tracker;

    const InvalidSampleReport first = tracker.record("heater_bed", false, -640, 5000);
    CHECK(first.what == InvalidSampleLog::Entered);
    CHECK(first.loggable());
    CHECK(first.dropped == 1);
    CHECK(first.duration_ms == 0);
    CHECK(first.first_temp_deci == -640);

    // The very next sample is silent - that is the fix.
    const InvalidSampleReport second = tracker.record("heater_bed", false, -641, 6000);
    CHECK(second.what == InvalidSampleLog::Nothing);
    CHECK_FALSE(second.loggable());
    CHECK(tracker.log_events() == 1);
}

TEST_CASE("InvalidSampleTracker heartbeat carries the running drop count and age",
          "[1348][logging]") {
    InvalidSampleTracker tracker;

    feed_invalid(tracker, "extruder", 60); // t = 0..59s, opens the run
    CHECK(tracker.log_events() == 1);

    const InvalidSampleReport beat = tracker.record("extruder", false, -700, 60 * ONE_SECOND_MS);
    REQUIRE(beat.what == InvalidSampleLog::StillInvalid);
    CHECK(beat.dropped == 61);
    CHECK(beat.duration_ms == 60 * ONE_SECOND_MS);
    CHECK(beat.first_temp_deci == OPEN_THERMISTOR_DECI);
}

TEST_CASE("InvalidSampleTracker reports the transition back to valid", "[1348][logging]") {
    InvalidSampleTracker tracker;

    feed_invalid(tracker, "extruder2", 300); // t = 0..299s
    const int64_t events_during_run = tracker.log_events();
    REQUIRE(tracker.in_run("extruder2"));

    const InvalidSampleReport recovered =
        tracker.record("extruder2", true, SANE_DECI, 300 * ONE_SECOND_MS);

    REQUIRE(recovered.what == InvalidSampleLog::Recovered);
    CHECK(recovered.loggable());
    CHECK(recovered.dropped == 300);
    CHECK(recovered.duration_ms == 300 * ONE_SECOND_MS);
    CHECK(recovered.first_temp_deci == OPEN_THERMISTOR_DECI);
    CHECK(recovered.last_temp_deci == OPEN_THERMISTOR_DECI);
    CHECK(tracker.log_events() == events_during_run + 1);

    // Run is closed: the key is forgotten, and healthy samples stay silent.
    CHECK_FALSE(tracker.in_run("extruder2"));
    CHECK(tracker.dropped_in_run("extruder2") == 0);
    CHECK_FALSE(tracker.record("extruder2", true, SANE_DECI, 301 * ONE_SECOND_MS).loggable());
    CHECK(tracker.log_events() == events_during_run + 1);
}

TEST_CASE("InvalidSampleTracker treats a second bad run as a fresh transition", "[1348][logging]") {
    InvalidSampleTracker tracker;

    feed_invalid(tracker, "extruder", 5);
    tracker.record("extruder", true, SANE_DECI, 5 * ONE_SECOND_MS);

    // A new run must announce itself immediately rather than inheriting the
    // previous run's backoff window (otherwise a flapping sensor goes silent).
    const InvalidSampleReport again = tracker.record("extruder", false, -900, 6 * ONE_SECOND_MS);
    CHECK(again.what == InvalidSampleLog::Entered);
    CHECK(again.dropped == 1);
    CHECK(again.first_temp_deci == -900);
    CHECK(again.duration_ms == 0);
}

// ============================================================================
// Isolation and edges
// ============================================================================

TEST_CASE("InvalidSampleTracker tracks each key independently", "[1348][logging]") {
    InvalidSampleTracker tracker;

    // Six tools, all open - the machine from the bundle.
    const std::vector<std::string> tools = {"extruder",  "extruder1", "extruder2",
                                            "extruder3", "extruder4", "extruder5"};
    // 50 samples at 1 Hz stays inside the first backoff window, so each key
    // should account for exactly one line: its own opening transition.
    for (const auto& tool : tools) {
        feed_invalid(tracker, tool, 50);
    }

    CHECK(tracker.log_events() == static_cast<int64_t>(tools.size()));
    for (const auto& tool : tools) {
        CHECK(tracker.dropped_in_run(tool) == 50);
    }

    // Recovering one tool must not disturb the others' runs.
    tracker.record("extruder3", true, SANE_DECI, 50 * ONE_SECOND_MS);
    CHECK_FALSE(tracker.in_run("extruder3"));
    CHECK(tracker.in_run("extruder"));
    CHECK(tracker.dropped_in_run("extruder5") == 50);
}

TEST_CASE("InvalidSampleTracker stays silent for a key that is never invalid", "[1348][logging]") {
    InvalidSampleTracker tracker;

    for (int64_t i = 0; i < 1000; ++i) {
        CHECK_FALSE(tracker.record("extruder", true, SANE_DECI, i * ONE_SECOND_MS).loggable());
    }
    CHECK(tracker.log_events() == 0);
    CHECK_FALSE(tracker.in_run("extruder"));
}

TEST_CASE("InvalidSampleTracker clamps a backwards clock step to zero age", "[1348][logging]") {
    InvalidSampleTracker tracker;

    tracker.record("extruder", false, OPEN_THERMISTOR_DECI, 10'000'000);
    // NTP steps the wall clock backwards mid-run; age must not go negative.
    const InvalidSampleReport recovered = tracker.record("extruder", true, SANE_DECI, 9'000'000);
    REQUIRE(recovered.what == InvalidSampleLog::Recovered);
    CHECK(recovered.duration_ms == 0);
}
