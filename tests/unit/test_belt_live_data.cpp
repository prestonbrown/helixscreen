// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/belt_live_data.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

TEST_CASE("a perfect match is 100 percent", "[belt][match]") {
    CHECK(belt_match_percent(86.0f, 86.0f) == Catch::Approx(100.0f));
}

TEST_CASE("match falls off with the difference", "[belt][match]") {
    // The reference machine measured A=86, B=82 - close but not matched.
    const float m = belt_match_percent(86.0f, 82.0f);
    CHECK(m > 80.0f);
    CHECK(m < 100.0f);
}

TEST_CASE("match is symmetric in sign", "[belt][match]") {
    CHECK(belt_match_percent(86.0f, 90.0f) ==
          Catch::Approx(belt_match_percent(86.0f, 82.0f)).margin(0.5f));
}

TEST_CASE("match clamps rather than going negative", "[belt][match]") {
    // An octave error must read as 0, not as -100. A negative percentage in a
    // progress bar renders as a full bar on some themes, which would show the
    // worst possible result as the best.
    CHECK(belt_match_percent(86.0f, 172.0f) >= 0.0f);
    CHECK(belt_match_percent(86.0f, 1.0f) >= 0.0f);
    CHECK(belt_match_percent(86.0f, 172.0f) <= 100.0f);
}

TEST_CASE("match is zero when there is no reference", "[belt][match]") {
    CHECK(belt_match_percent(0.0f, 86.0f) == 0.0f);
    CHECK(belt_match_percent(-1.0f, 86.0f) == 0.0f);
}

TEST_CASE("the idle hint waits for a real pause", "[belt][match]") {
    CHECK_FALSE(belt_should_show_idle_hint(0));
    CHECK_FALSE(belt_should_show_idle_hint(IDLE_HINT_MS - 1));
    CHECK(belt_should_show_idle_hint(IDLE_HINT_MS));
    CHECK(belt_should_show_idle_hint(IDLE_HINT_MS * 10));
}

TEST_CASE("a delta inside the instrument's resolution reads as matched", "[belt][match]") {
    // 1.51 Hz is one PSD bin at the 2048-sample window. Two readings one bin
    // apart are indistinguishable, so the verdict must not call them different.
    CHECK(belt_frequencies_match(86.0f, 87.51f));
    CHECK(belt_frequencies_match(86.0f, 84.49f));
    CHECK_FALSE(belt_frequencies_match(86.0f, 82.0f));
    CHECK_FALSE(belt_frequencies_match(86.0f, 90.0f));
    // Exactly at the resolution is not resolvable either.
    CHECK(belt_frequencies_match(86.0f, 86.0f + BELT_RESOLUTION_HZ - 0.01f));
}

TEST_CASE("waveform is downsampled to a fixed width", "[belt][livedata]") {
    std::vector<AccelSample> window(2048);
    for (size_t i = 0; i < window.size(); ++i) {
        window[i].x = static_cast<float>(i);
    }
    auto& d = BeltLiveData::instance();
    d.clear();
    d.set_waveform(window);
    CHECK(d.waveform().size() == BeltLiveData::TRACE_POINTS);
}

TEST_CASE("a short window still produces a trace", "[belt][livedata]") {
    // Early in a session the window is not full yet. Producing nothing would
    // leave the widget blank for the first second of every measurement.
    std::vector<AccelSample> window(17);
    auto& d = BeltLiveData::instance();
    d.clear();
    d.set_waveform(window);
    CHECK_FALSE(d.waveform().empty());
    CHECK(d.waveform().size() <= BeltLiveData::TRACE_POINTS);
}

TEST_CASE("an empty window clears rather than keeping stale data", "[belt][livedata]") {
    auto& d = BeltLiveData::instance();
    std::vector<AccelSample> window(512);
    for (size_t i = 0; i < window.size(); ++i) {
        window[i].x = static_cast<float>(i % 7);
    }
    d.set_waveform(window);
    REQUIRE_FALSE(d.waveform().empty());
    d.set_waveform({});
    CHECK(d.waveform().empty());
}

TEST_CASE("the waveform keeps the ring-down envelope, not its mean", "[belt][livedata]") {
    // A 2048-sample window reduced to 128 points puts 16 samples in a bucket.
    // Give the wave a 16-sample period so each bucket holds exactly one cycle:
    // the peak of that bucket is the amplitude, its mean is 2/pi of it (0.64).
    // A mean would therefore under-report the ring-down by a third and would
    // shrink further as the trace widens, which is why the reduction takes
    // maxima.
    constexpr float kAmplitude = 100.0f;
    std::vector<AccelSample> window(2048);
    for (size_t i = 0; i < window.size(); ++i) {
        const float decay = 1.0f - static_cast<float>(i) / static_cast<float>(window.size());
        window[i].x =
            decay * kAmplitude * std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 16.0f);
    }
    auto& d = BeltLiveData::instance();
    d.clear();
    d.set_waveform(window);
    REQUIRE(d.waveform().size() == BeltLiveData::TRACE_POINTS);

    // The loudest bucket must read close to the real amplitude, not to its
    // average. 0.64 * 100 = 64, so the 90 threshold separates the two.
    const float loudest = *std::max_element(d.waveform().begin(), d.waveform().end());
    CHECK(loudest > 0.9f * kAmplitude);

    // And the decay has to survive the reduction.
    CHECK(d.waveform().front() > 5.0f * d.waveform().back());
}

TEST_CASE("spectrum is downsampled and preserves peaks", "[belt][livedata]") {
    // Averaging bins would flatten an 86 Hz spike into the noise. The trace
    // exists to show where the energy is, so reduction must keep maxima.
    std::vector<std::pair<float, float>> psd;
    for (int i = 1; i <= 1000; ++i) {
        psd.emplace_back(static_cast<float>(i), i == 500 ? 1000.0f : 1.0f);
    }
    auto& d = BeltLiveData::instance();
    d.clear();
    d.set_spectrum(psd);
    REQUIRE(d.spectrum().size() == BeltLiveData::TRACE_POINTS);
    const float peak = *std::max_element(d.spectrum().begin(), d.spectrum().end());
    CHECK(peak == Catch::Approx(1000.0f));
}

TEST_CASE("an empty spectrum clears rather than keeping stale data", "[belt][livedata]") {
    auto& d = BeltLiveData::instance();
    d.set_spectrum({{10.0f, 1.0f}, {20.0f, 2.0f}});
    REQUIRE_FALSE(d.spectrum().empty());
    d.set_spectrum({});
    CHECK(d.spectrum().empty());
}

TEST_CASE("clear drops both traces", "[belt][livedata]") {
    auto& d = BeltLiveData::instance();
    d.set_waveform(std::vector<AccelSample>(64));
    d.set_spectrum({{10.0f, 1.0f}, {20.0f, 2.0f}});
    d.clear();
    CHECK(d.waveform().empty());
    CHECK(d.spectrum().empty());
}
