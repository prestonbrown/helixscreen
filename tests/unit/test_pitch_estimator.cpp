// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pitch_estimator.cpp
 * @brief Harmonic-aware fundamental estimation for belt plucks
 *
 * Ground truth comes from real Voron 2.4 captures in tests/fixtures/belt_plucks/.
 * The critical case is a signal whose 2nd harmonic is stronger than its
 * fundamental: peak-picking returns 2*f0, which is the bug this replaces.
 */

#include "../../include/belt_tension_types.h"
#include "../../include/pitch_estimator.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

/// Fixtures are Klipper-format CSV; tests run from the repository root.
std::vector<AccelSample> load_fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/belt_plucks/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_accel_csv(ss.str());
}

float fixture_sample_rate(const std::vector<AccelSample>& s) {
    REQUIRE(s.size() > 1);
    const float span = s.back().time - s.front().time;
    REQUIRE(span > 0.0f);
    return static_cast<float>(s.size() - 1) / span;
}

/// Synthetic pluck with the harmonic profile measured on a real A belt:
/// h1 -3 dB, h2 0 dB, h3 -14 dB, h4 -13 dB. The 2nd harmonic dominates the
/// fundamental, which is what defeats peak-picking. h3 and h4 must be present
/// or the harmonic product is degenerate and the test proves nothing.
std::vector<AccelSample> make_harmonic_heavy(float f0, float sample_rate, int count) {
    static const float amps[4] = {0.708f, 1.0f, 0.200f, 0.224f};
    std::vector<AccelSample> out(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / sample_rate;
        float v = 0.0f;
        for (int h = 0; h < 4; ++h) {
            v += amps[h] *
                 std::sin(2.0f * static_cast<float>(M_PI) * f0 * static_cast<float>(h + 1) * t);
        }
        // Deterministic low-level broadband content so the upper harmonics of a
        // wrong candidate are not exactly zero.
        v += 0.01f * std::sin(static_cast<float>(i) * 12.9898f);
        out[static_cast<size_t>(i)].time = t;
        out[static_cast<size_t>(i)].x = v;
        out[static_cast<size_t>(i)].y = 0.0f;
        out[static_cast<size_t>(i)].z = 9810.0f; // gravity, must be removed as DC
    }
    return out;
}

} // namespace

TEST_CASE("expected_frequency_for_span follows the Voron 110Hz@150mm reference",
          "[belt_tension][pitch]") {
    CHECK(expected_frequency_for_span(150.0f) == Catch::Approx(110.0f).margin(0.01f));
    CHECK(expected_frequency_for_span(300.0f) == Catch::Approx(55.0f).margin(0.01f));
    CHECK(expected_frequency_for_span(75.0f) == Catch::Approx(220.0f).margin(0.01f));
}

TEST_CASE("expected_frequency_for_span rejects nonsense spans", "[belt_tension][pitch]") {
    CHECK(expected_frequency_for_span(0.0f) == 0.0f);
    CHECK(expected_frequency_for_span(-10.0f) == 0.0f);
}

TEST_CASE("search_window_for_span brackets the expected fundamental", "[belt_tension][pitch]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(150.0f, &lo, &hi));
    CHECK(lo == Catch::Approx(77.0f).margin(0.5f));
    CHECK(hi == Catch::Approx(165.0f).margin(0.5f));

    // The floor MUST sit above half the expected fundamental, or the harmonic
    // product locks onto f0/2 instead of f0.
    CHECK(lo > expected_frequency_for_span(150.0f) * 0.5f);
}

TEST_CASE("search_window_for_span rejects nonsense spans", "[belt_tension][pitch]") {
    float lo = 0.0f, hi = 0.0f;
    CHECK_FALSE(search_window_for_span(0.0f, &lo, &hi));
    CHECK_FALSE(search_window_for_span(150.0f, nullptr, &hi));
}

TEST_CASE("estimate_pitch returns the fundamental when the 2nd harmonic dominates",
          "[belt_tension][pitch]") {
    const float sr = 3200.0f;
    const float search_lo = 63.0f, search_hi = 135.0f;
    auto samples = make_harmonic_heavy(90.0f, sr, 1600);
    // Bandwidth must cover 4*f0 or no candidate has a complete harmonic series.
    auto psd = compute_psd(samples, sr, required_bandwidth_hz(search_hi));
    REQUIRE(!psd.empty());

    // Peak-picking is wrong here by construction - it finds 180 Hz.
    auto naive = find_peak_frequency(psd, 20.0f, 300.0f);
    REQUIRE(naive.found);
    CHECK(naive.frequency == Catch::Approx(180.0f).margin(4.0f));

    auto est = estimate_pitch(psd, search_lo, search_hi);
    REQUIRE(est.valid);
    CHECK(est.frequency_hz == Catch::Approx(90.0f).margin(4.0f));
}

TEST_CASE("estimate_pitch recovers 86 Hz from real A-belt captures",
          "[belt_tension][pitch][golden]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    for (const auto* name : {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "a_belt_86hz_3.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto psd = compute_psd(samples, fixture_sample_rate(samples), required_bandwidth_hz(hi));
        auto est = estimate_pitch(psd, lo, hi);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(86.0f).margin(2.0f));
    }
}

TEST_CASE("estimate_pitch recovers 82 Hz from real B-belt captures",
          "[belt_tension][pitch][golden]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    for (const auto* name : {"b_belt_82hz_1.csv", "b_belt_82hz_2.csv", "b_belt_82hz_3.csv",
                             "b_belt_82hz_hard_case.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto psd = compute_psd(samples, fixture_sample_rate(samples), required_bandwidth_hz(hi));
        auto est = estimate_pitch(psd, lo, hi);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(82.0f).margin(2.0f));
    }
}

TEST_CASE("required_bandwidth_hz covers the whole harmonic series", "[belt_tension][pitch]") {
    // A 151mm span searches up to ~164 Hz; four harmonics of that is ~656 Hz.
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));
    const float bw = required_bandwidth_hz(hi);
    CHECK(bw > hi * 4.0f);
    CHECK(bw > 600.0f);
    // The shipping default of 250 Hz is nowhere near enough - that is the bug.
    CHECK(bw > 250.0f);

    CHECK(required_bandwidth_hz(0.0f) == 0.0f);
    CHECK(required_bandwidth_hz(160.0f, 0) == 0.0f);
}

TEST_CASE("estimate_pitch rejects degenerate input", "[belt_tension][pitch][edge_case]") {
    std::vector<std::pair<float, float>> empty;
    CHECK_FALSE(estimate_pitch(empty, 70.0f, 170.0f).valid);

    auto samples = load_fixture("a_belt_86hz_1.csv");
    auto psd = compute_psd(samples, fixture_sample_rate(samples), 700.0f);
    CHECK_FALSE(estimate_pitch(psd, 170.0f, 70.0f).valid);    // inverted window
    CHECK_FALSE(estimate_pitch(psd, 70.0f, 170.0f, 0).valid); // no harmonics
}
