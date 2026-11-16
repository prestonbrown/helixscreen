// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_analysis.h"
#include "motor_ripple_plan.h"

#include <cmath>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using namespace helix::calibration::ripple;
using Catch::Approx;

namespace {

constexpr float kTwoPi = 6.2831853f;

struct Tone {
    float freq_hz;
    float amp_x;
    float amp_y;
};

// A capture of `seconds` at `rate`, the tones summed with 1% white noise, and
// optional near-silence of `quiet_s` at each end.
std::vector<AccelSample> capture(float rate, float seconds, const std::vector<Tone>& tones,
                                 float quiet_s = 0.0f) {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 6.0f);
    const int quiet = static_cast<int>(quiet_s * rate);
    const int moving = static_cast<int>(seconds * rate);
    std::vector<AccelSample> out;
    for (int i = 0; i < moving + 2 * quiet; ++i) {
        const float t = static_cast<float>(i) / rate;
        AccelSample s{t, noise(rng) * 0.1f, noise(rng) * 0.1f, 9810.0f + noise(rng) * 0.1f};
        if (i >= quiet && i < quiet + moving) {
            for (const auto& tone : tones) {
                const float ph = kTwoPi * tone.freq_hz * t;
                s.x += tone.amp_x * std::sin(ph) + noise(rng);
                s.y += tone.amp_y * std::sin(ph);
            }
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

TEST_CASE("a ripple tone reads back at its amplitude and leaves the other harmonics quiet",
          "[ripple][analysis]") {
    const float f2 = ripple_freq_hz(86.3f, 2, 0.8f);
    const auto r =
        analyze_capture(capture(3200.0f, 3.0f, {{f2, 600.0f, 0.0f}}), 3200.0f, 86.3f, 0.8f);
    REQUIRE(r.ripple[1].has_value());
    CHECK(*r.ripple[1] == Approx(600.0f).epsilon(0.05));
    REQUIRE(r.ripple[0].has_value());
    REQUIRE(r.ripple[2].has_value());
    CHECK(*r.ripple[0] < 60.0f);
    CHECK(*r.ripple[2] < 60.0f);
    CHECK(r.broadband > 0.0f);
}

TEST_CASE("ripple on two axes adds in quadrature", "[ripple][analysis]") {
    const float f2 = ripple_freq_hz(86.3f, 2, 0.8f);
    const auto r =
        analyze_capture(capture(3200.0f, 3.0f, {{f2, 360.0f, 480.0f}}), 3200.0f, 86.3f, 0.8f);
    REQUIRE(r.ripple[1].has_value());
    CHECK(*r.ripple[1] == Approx(600.0f).epsilon(0.05));
}

TEST_CASE("idle stretches at either end of a capture do not dilute the ripple",
          "[ripple][analysis]") {
    const float f1 = ripple_freq_hz(172.5f, 1, 0.8f);
    const auto plain =
        analyze_capture(capture(3200.0f, 3.0f, {{f1, 800.0f, 0.0f}}), 3200.0f, 172.5f, 0.8f);
    const auto padded =
        analyze_capture(capture(3200.0f, 3.0f, {{f1, 800.0f, 0.0f}}, 0.5f), 3200.0f, 172.5f, 0.8f);
    REQUIRE(padded.ripple[0].has_value());
    CHECK(*padded.ripple[0] == Approx(800.0f).epsilon(0.05));
    // A second of silence is about 50 frames; only the few that straddle an
    // edge (up to 4 per edge at quarter-frame hops) carry motion and count.
    CHECK(padded.frames_used <= plain.frames_used + 8);
}

TEST_CASE("a slow accelerometer reports nothing above its ceiling", "[ripple][analysis]") {
    const float elec = 0.7764f;
    const float f1 = ripple_freq_hz(130.0f, 1, elec); // 167 Hz, under 0.45 x 387
    const auto r =
        analyze_capture(capture(387.0f, 3.0f, {{f1, 500.0f, 0.0f}}), 387.0f, 130.0f, elec);
    CHECK(ceiling_hz(387.0f) == Approx(174.15f));
    REQUIRE(r.ripple[0].has_value());
    CHECK(*r.ripple[0] == Approx(500.0f).epsilon(0.1));
    CHECK_FALSE(r.ripple[1].has_value());
    CHECK_FALSE(r.ripple[2].has_value());
}

TEST_CASE("an empty or too-short capture measures nothing", "[ripple][analysis]") {
    for (const auto& samples :
         {std::vector<AccelSample>{}, capture(3200.0f, 0.03f, {{200.0f, 500.0f, 0.0f}})}) {
        const auto r = analyze_capture(samples, 3200.0f, 86.3f, 0.8f);
        CHECK(r.frames_used == 0);
        for (const auto& v : r.ripple) {
            CHECK_FALSE(v.has_value());
        }
    }
}
