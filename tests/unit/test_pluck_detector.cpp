// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pluck_detector.cpp
 * @brief Noise floor, strength gating, and ring-down extraction
 */

#include "../../include/belt_tension_types.h"
#include "../../include/pluck_detector.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

std::vector<AccelSample> load_fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/belt_plucks/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_accel_csv(ss.str());
}

/// Flat noise at a chosen amplitude, plus a constant gravity offset on Z that
/// must be removed as DC or every RMS reading is wrong.
std::vector<AccelSample> make_noise(float amplitude, int count) {
    std::vector<AccelSample> out(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float phase = static_cast<float>(i);
        out[static_cast<size_t>(i)].time = static_cast<float>(i) / 3200.0f;
        out[static_cast<size_t>(i)].x = amplitude * std::sin(phase);
        out[static_cast<size_t>(i)].y = amplitude * std::cos(phase * 1.7f);
        out[static_cast<size_t>(i)].z = 9810.0f + amplitude * std::sin(phase * 0.3f);
    }
    return out;
}

/// Quiet run, then a decaying burst starting at `onset`.
std::vector<AccelSample> make_transient(int count, int onset, float quiet, float burst) {
    auto out = make_noise(quiet, count);
    for (int i = onset; i < count; ++i) {
        const float decay = std::exp(-static_cast<float>(i - onset) / 400.0f);
        const float t = static_cast<float>(i) / 3200.0f;
        out[static_cast<size_t>(i)].x +=
            burst * decay * std::sin(2.0f * static_cast<float>(M_PI) * 86.0f * t);
    }
    return out;
}

} // namespace

TEST_CASE("window_rms removes DC before measuring", "[belt_tension][pluck_detect]") {
    auto quiet = make_noise(10.0f, 512);
    const float rms = PluckDetector::window_rms(quiet.data(), quiet.size());
    // Gravity is 9810 on Z; if DC were not removed this would be ~9810.
    CHECK(rms < 100.0f);
    CHECK(rms > 0.0f);
}

TEST_CASE("window_rms handles degenerate input", "[belt_tension][pluck_detect][edge_case]") {
    CHECK(PluckDetector::window_rms(nullptr, 0) == 0.0f);
    std::vector<AccelSample> one(1);
    CHECK(PluckDetector::window_rms(one.data(), 0) == 0.0f);
}

TEST_CASE("learn_noise_floor establishes a positive baseline", "[belt_tension][pluck_detect]") {
    PluckDetector det;
    auto quiet = make_noise(10.0f, 2048);
    REQUIRE(det.learn_noise_floor(quiet));
    CHECK(det.noise_floor() > 0.0f);
    CHECK(det.noise_floor() ==
          Catch::Approx(PluckDetector::window_rms(quiet.data(), quiet.size())).epsilon(0.01));
}

TEST_CASE("learn_noise_floor rejects an empty buffer", "[belt_tension][pluck_detect][edge_case]") {
    PluckDetector det;
    std::vector<AccelSample> empty;
    CHECK_FALSE(det.learn_noise_floor(empty));
}

TEST_CASE("gate rejects a weak transient and accepts a firm one", "[belt_tension][pluck_detect]") {
    PluckDetector det;
    det.set_noise_floor(10.0f);

    auto weak = make_noise(30.0f, 2048);  // ~3x floor
    auto firm = make_noise(200.0f, 2048); // well past 9x

    CHECK_FALSE(det.passes_gate(weak.data(), weak.size()));
    CHECK(det.passes_gate(firm.data(), firm.size()));
}

TEST_CASE("gate threshold is exactly MIN_RMS_RATIO", "[belt_tension][pluck_detect]") {
    PluckDetector det;
    auto probe = make_noise(100.0f, 2048);
    const float rms = PluckDetector::window_rms(probe.data(), probe.size());

    det.set_noise_floor(rms / (PluckDetector::MIN_RMS_RATIO * 1.05f));
    CHECK(det.passes_gate(probe.data(), probe.size()));

    det.set_noise_floor(rms / (PluckDetector::MIN_RMS_RATIO * 0.95f));
    CHECK_FALSE(det.passes_gate(probe.data(), probe.size()));
}

TEST_CASE("gate rejects the real weak-pluck capture", "[belt_tension][pluck_detect][golden]") {
    // Captured at 1.4x the noise floor. Analysing it yields 112 Hz, which is
    // meaningless - the gate is what stops that number reaching a user. This
    // capture never rang, so it fails the gate whether measured on a
    // detection window or (as here, against the fixture itself) a ring-down -
    // unlike the firm captures below, there is no decay to confuse the two.
    auto weak = load_fixture("weak_pluck_reject.csv");
    REQUIRE(weak.size() > 1000);

    PluckDetector det;
    det.set_noise_floor(265.0f); // the floor measured during that session
    CHECK_FALSE(det.passes_gate(weak.data(), weak.size()));
}

TEST_CASE("firm plucks ring down well clear of a weak one",
          "[belt_tension][pluck_detect][golden]") {
    // The fixtures store extracted ring-downs, not detection windows, so they
    // can't exercise passes_gate()/MIN_RMS_RATIO directly - see the contract
    // note on PluckDetector::rms_ratio(). What they do support is checking
    // that a firm strike's ring-down is unambiguously stronger than a weak
    // one's, even after both have decayed the same SKIP_MS. Measured
    // separation is roughly 2.0-2.5x; 1.5x leaves margin without being
    // vacuous.
    auto weak = load_fixture("weak_pluck_reject.csv");
    const float weak_rms = PluckDetector::window_rms(weak.data(), weak.size());

    for (const auto* name :
         {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "b_belt_82hz_1.csv", "b_belt_82hz_2.csv"}) {
        auto s = load_fixture(name);
        const float rms = PluckDetector::window_rms(s.data(), s.size());
        INFO("fixture " << name << " rms=" << rms << " weak_rms=" << weak_rms);
        CHECK(rms >= weak_rms * 1.5f);
    }
}

TEST_CASE("extract_ringdown skips the impact spike", "[belt_tension][pluck_detect]") {
    const float sr = 3200.0f;
    const int onset = 1000;
    auto buffer = make_transient(6000, onset, 5.0f, 500.0f);

    PluckWindow win;
    REQUIRE(PluckDetector::extract_ringdown(buffer, sr, &win));

    const size_t expected_len = static_cast<size_t>(sr * PluckDetector::ANALYZE_MS / 1000.0f);
    CHECK(win.samples.size() == expected_len);

    // The window must start after the onset, not before it.
    const float skip_s = PluckDetector::SKIP_MS / 1000.0f;
    CHECK(win.samples.front().time >= buffer[static_cast<size_t>(onset)].time + skip_s * 0.9f);
}

TEST_CASE("extract_ringdown fails on a buffer that is too short",
          "[belt_tension][pluck_detect][edge_case]") {
    auto tiny = make_noise(10.0f, 64);
    PluckWindow win;
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 3200.0f, &win));
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 3200.0f, nullptr));
    CHECK_FALSE(PluckDetector::extract_ringdown(tiny, 0.0f, &win));
}
