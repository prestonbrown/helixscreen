// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_main_loop_heartbeat.cpp
 * @brief Tests for main-loop liveness detection.
 *
 * The detector exists because a deadlocked UI thread is invisible to everything
 * else: during the wake-path deadlock, every background thread kept running and
 * the watchdog saw a perfectly healthy process for 36 minutes. Only the LVGL
 * loop itself can prove it is alive.
 *
 * The threshold logic is a pure state machine so these tests never have to
 * sleep for a minute — time is a parameter, not a wait.
 */

#include "../../include/main_loop_heartbeat.h"

#include <cstdint>

#include "../catch_amalgamated.hpp"

using helix::MainLoopHangDetector;
using helix::MainLoopHeartbeat;

namespace {
constexpr uint32_t THRESHOLD_MS = 60000;
}

TEST_CASE("MainLoopHeartbeat is unarmed until the loop actually ticks", "[hang][heartbeat]") {
    MainLoopHeartbeat::reset();
    REQUIRE_FALSE(MainLoopHeartbeat::armed());
    REQUIRE(MainLoopHeartbeat::count() == 0);

    MainLoopHeartbeat::beat();

    REQUIRE(MainLoopHeartbeat::armed());
    REQUIRE(MainLoopHeartbeat::count() == 1);
    MainLoopHeartbeat::reset();
}

TEST_CASE("MainLoopHangDetector stays quiet while the loop advances", "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);
    uint64_t count = 0;

    // Ten minutes of healthy operation, sampled every 5s like the real loop.
    for (uint64_t t = 0; t <= 600000; t += 5000) {
        CHECK(d.sample(++count, t) == 0);
    }
    CHECK_FALSE(d.stalled());
}

TEST_CASE("MainLoopHangDetector reports once the counter stops past the threshold",
          "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);

    REQUIRE(d.sample(1, 0) == 0);     // seed
    REQUIRE(d.sample(1, 30000) == 0); // stuck, but only 30s
    REQUIRE(d.sample(1, 59999) == 0); // still under
    const uint32_t stalled = d.sample(1, 60000);

    REQUIRE(stalled == 60000);
    REQUIRE(d.stalled());
}

TEST_CASE("MainLoopHangDetector reports a stall only once", "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);

    REQUIRE(d.sample(7, 0) == 0);
    REQUIRE(d.sample(7, 60000) == 60000);

    // A wedged loop is sampled every 5s forever. Re-reporting would flood the
    // log and the telemetry queue with one event per sample.
    for (uint64_t t = 65000; t <= 600000; t += 5000) {
        CHECK(d.sample(7, t) == 0);
    }
    CHECK(d.stalled());
}

TEST_CASE("MainLoopHangDetector re-arms after the loop recovers", "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);

    REQUIRE(d.sample(1, 0) == 0);
    REQUIRE(d.sample(1, 60000) == 60000);
    REQUIRE(d.stalled());

    // Loop moves again: a long-but-survivable block is not a permanent verdict.
    REQUIRE(d.sample(2, 61000) == 0);
    CHECK_FALSE(d.stalled());

    // And a genuinely new stall is reported again.
    REQUIRE(d.sample(2, 100000) == 0);
    REQUIRE(d.sample(2, 121000) == 60000);
}

TEST_CASE("MainLoopHangDetector honours a changed threshold", "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);
    d.set_threshold_ms(10000);
    REQUIRE(d.threshold_ms() == 10000);

    REQUIRE(d.sample(1, 0) == 0);
    REQUIRE(d.sample(1, 9000) == 0);
    REQUIRE(d.sample(1, 10000) == 10000);
}

TEST_CASE("MainLoopHangDetector treats threshold 0 as disabled", "[hang][heartbeat]") {
    MainLoopHangDetector d(0);

    REQUIRE(d.sample(1, 0) == 0);
    // An hour wedged, and it still says nothing because it was turned off.
    REQUIRE(d.sample(1, 3600000) == 0);
    CHECK_FALSE(d.stalled());
}

TEST_CASE("MainLoopHangDetector does not fire on a first sample", "[hang][heartbeat]") {
    MainLoopHangDetector d(THRESHOLD_MS);

    // Seeding at a large timestamp must not read as "already stalled that long"
    // — the monitor thread starts well after process start.
    REQUIRE(d.sample(42, 5000000) == 0);
    CHECK_FALSE(d.stalled());
}
