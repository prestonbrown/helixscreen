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
#include "belt_test_signals.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

using helix::calibration::test::Hiss;

constexpr float kRate = 3091.0f;
/// A 150 mm span searches 77-165 Hz for its fundamental - both figures the
/// reference machine's background peaks land inside.
constexpr float kSpan = 150.0f;

/// A steady tone with its own harmonic series, which is what a fan produces:
/// a blade-pass fundamental plus multiples of it. A pure sinusoid would be an
/// easier target than reality, since the harmonic product spectrum only locks
/// onto a contaminant that has a series to lock onto.
void add_series(std::vector<AccelSample>& buf, float f0, float amp, const float (&profile)[4],
                float decay_s) {
    for (size_t i = 0; i < buf.size(); ++i) {
        const float t = buf[i].time;
        float v = 0.0f;
        for (int h = 0; h < 4; ++h) {
            v += profile[h] *
                 std::sin(2.0f * static_cast<float>(M_PI) * f0 * static_cast<float>(h + 1) * t);
        }
        if (decay_s > 0.0f) {
            v *= std::exp(-t / decay_s);
        }
        buf[i].x += amp * v;
    }
}

/// Broadband bed with gravity on X - see belt_test_signals.h.
std::vector<AccelSample> hiss_bed(size_t count, float amp, uint32_t seed) {
    Hiss rng{seed};
    return helix::calibration::test::hiss_bed(count, amp, kRate, rng);
}

const float kFanProfile[4] = {1.0f, 0.5f, 0.3f, 0.2f};
const float kBeltProfile[4] = {0.8f, 1.0f, 0.3f, 0.25f};

/// The quiet window the session captures before the user plucks anything,
/// with a fan already running in it.
QuietSpectrum quiet_with_fan(float fan_hz, float fan_amp) {
    auto quiet = hiss_bed(3000, 1.0f, 11u);
    add_series(quiet, fan_hz, fan_amp, kFanProfile, 0.0f);
    return quiet_spectrum_for_span(quiet, kRate, kSpan);
}

/// The ring-down: the same fan, still running, plus a genuine belt tone.
std::vector<AccelSample> ringdown_with_fan(float fan_hz, float fan_amp, float belt_hz,
                                           float belt_amp) {
    auto rd = hiss_bed(1545, 1.0f, 23u);
    add_series(rd, fan_hz, fan_amp, kFanProfile, 0.0f);
    add_series(rd, belt_hz, belt_amp, kBeltProfile, 0.25f);
    return rd;
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

TEST_CASE("estimate_pitch rejects an all-zero PSD instead of confidently returning bin 0",
          "[belt_tension][pitch][edge_case]") {
    // best_product starts at -1.0; every candidate here scores exactly 0.0,
    // which used to beat that sentinel and return the first in-window bin as
    // "valid". A silent all-zero PSD (dead sensor, gated-out capture) must
    // come back invalid, not confidently wrong.
    std::vector<std::pair<float, float>> psd;
    const float resolution = 4.0f;
    for (size_t i = 0; i < 200; ++i) {
        psd.emplace_back(static_cast<float>(i + 1) * resolution, 0.0f);
    }
    auto est = estimate_pitch(psd, 63.0f, 135.0f);
    CHECK_FALSE(est.valid);
}

TEST_CASE("estimate_pitch_for_span reproduces the golden fixtures at 151mm",
          "[belt_tension][pitch][golden]") {
    for (const auto* name : {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "a_belt_86hz_3.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto est = estimate_pitch_for_span(samples, fixture_sample_rate(samples), 151.0f);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(86.0f).margin(2.0f));
    }

    for (const auto* name : {"b_belt_82hz_1.csv", "b_belt_82hz_2.csv", "b_belt_82hz_3.csv",
                             "b_belt_82hz_hard_case.csv"}) {
        auto samples = load_fixture(name);
        REQUIRE(samples.size() > 1000);
        auto est = estimate_pitch_for_span(samples, fixture_sample_rate(samples), 151.0f);
        INFO("fixture " << name);
        REQUIRE(est.valid);
        CHECK(est.frequency_hz == Catch::Approx(82.0f).margin(2.0f));
    }
}

TEST_CASE("estimate_pitch_for_span rejects degenerate span and sample rate",
          "[belt_tension][pitch][edge_case]") {
    auto samples = load_fixture("a_belt_86hz_1.csv");
    const float sr = fixture_sample_rate(samples);
    CHECK_FALSE(estimate_pitch_for_span(samples, sr, 0.0f).valid);
    CHECK_FALSE(estimate_pitch_for_span(samples, sr, -151.0f).valid);
    CHECK_FALSE(estimate_pitch_for_span(samples, 0.0f, 151.0f).valid);
    CHECK_FALSE(estimate_pitch_for_span(samples, -sr, 151.0f).valid);
}

TEST_CASE("a PSD computed at compute_psd's default bandwidth yields no pitch estimate",
          "[belt_tension][pitch][edge_case]") {
    // This is the trap the composer exists to close: compute_psd()'s default
    // 250 Hz cap does not cover 4 harmonics of a typical belt fundamental, so
    // every candidate's harmonic series runs off the end of the array and
    // estimate_pitch() has nothing complete to score. A caller who computes
    // the PSD by hand and forgets required_bandwidth_hz() gets this - no
    // crash, no error return, just a silent invalid estimate.
    auto samples = load_fixture("a_belt_86hz_1.csv");
    const float sr = fixture_sample_rate(samples);
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    auto psd = compute_psd(samples, sr); // default bandwidth, no required_bandwidth_hz()
    auto est = estimate_pitch(psd, lo, hi);
    CHECK_FALSE(est.valid);
}

// ============================================================================
// The quiet spectrum: a belt tone is absent before the pluck, a fan is not
// ============================================================================

TEST_CASE("QuietSpectrum reports a planted tone as prominent and the rest as ordinary",
          "[belt_tension][pitch]") {
    const auto bg = quiet_with_fan(115.0f, 60.0f);
    REQUIRE(bg.valid());

    CHECK(bg.prominence_at(115.0f) > BACKGROUND_PROMINENCE_TOLERANCE);
    CHECK(bg.weight_at(115.0f) < 1.0f);
    CHECK(bg.weight_at(115.0f) > 0.0f); // a discount, never a veto

    // A frequency the fan does not occupy is ordinary, so it is not discounted.
    CHECK(bg.prominence_at(99.0f) < BACKGROUND_PROMINENCE_TOLERANCE);
    CHECK(bg.weight_at(99.0f) == 1.0f);
}

TEST_CASE("QuietSpectrum discounts nothing when it has nothing to say",
          "[belt_tension][pitch][edge_case]") {
    QuietSpectrum empty;
    CHECK_FALSE(empty.valid());
    CHECK(empty.weight_at(115.0f) == 1.0f); // silent, not blocking
    CHECK(empty.prominence_at(115.0f) == 0.0f);

    // An all-zero spectrum has no median to normalise against - a dead sensor
    // must not make every candidate infinitely contaminated.
    std::vector<std::pair<float, float>> zeros;
    for (size_t i = 0; i < 200; ++i) {
        zeros.emplace_back(static_cast<float>(i + 1) * 2.0f, 0.0f);
    }
    QuietSpectrum dead;
    CHECK_FALSE(dead.learn(zeros));
    CHECK_FALSE(dead.valid());
    CHECK(dead.weight_at(115.0f) == 1.0f);

    QuietSpectrum too_short;
    CHECK_FALSE(too_short.learn({{2.0f, 1.0f}, {4.0f, 1.0f}}));
}

TEST_CASE("quiet_spectrum_for_span rejects degenerate input", "[belt_tension][pitch][edge_case]") {
    auto quiet = hiss_bed(3000, 1.0f, 5u);
    CHECK_FALSE(quiet_spectrum_for_span(quiet, kRate, 0.0f).valid());
    CHECK_FALSE(quiet_spectrum_for_span(quiet, 0.0f, kSpan).valid());
    CHECK_FALSE(quiet_spectrum_for_span({}, kRate, kSpan).valid());
}

TEST_CASE("a fan inside the search window does not win the pitch estimate",
          "[belt_tension][pitch]") {
    // The reference machine's failure: a steady tone lands inside 77-165 Hz
    // and is louder than the belt, so the harmonic product locks onto it. One
    // belt read 88 / 92 / 94 / 100 / 107 Hz on successive plucks of a belt
    // that was not changing.
    const auto bg = quiet_with_fan(115.0f, 60.0f);
    REQUIRE(bg.valid());
    auto rd = ringdown_with_fan(115.0f, 60.0f, 99.0f, 8.0f);

    auto psd = compute_psd(rd, kRate, required_bandwidth_hz(165.0f));
    REQUIRE(!psd.empty());

    // Without the quiet spectrum the estimator confidently returns the fan.
    auto blind = estimate_pitch(psd, 77.0f, 165.0f);
    REQUIRE(blind.valid);
    CHECK(blind.frequency_hz == Catch::Approx(115.0f).margin(3.0f));

    auto informed = estimate_pitch(psd, 77.0f, 165.0f, DEFAULT_HARMONICS, &bg);
    REQUIRE(informed.valid);
    CHECK(informed.frequency_hz == Catch::Approx(99.0f).margin(3.0f));
}

TEST_CASE("a fan sitting on a harmonic of the true fundamental does not win either",
          "[belt_tension][pitch]") {
    // Harder case: the contaminant is at 164 Hz, which is both inside the
    // search window in its own right and exactly 2*f0 of the real 82 Hz belt.
    // The true candidate is scored on a bin the fan owns, so a hard reject on
    // any contaminated harmonic would throw the right answer away.
    const auto bg = quiet_with_fan(164.0f, 60.0f);
    REQUIRE(bg.valid());
    auto rd = ringdown_with_fan(164.0f, 60.0f, 82.0f, 8.0f);

    auto psd = compute_psd(rd, kRate, required_bandwidth_hz(165.0f));
    auto blind = estimate_pitch(psd, 77.0f, 165.0f);
    REQUIRE(blind.valid);
    CHECK(blind.frequency_hz == Catch::Approx(164.0f).margin(3.0f));

    auto informed = estimate_pitch(psd, 77.0f, 165.0f, DEFAULT_HARMONICS, &bg);
    REQUIRE(informed.valid);
    CHECK(informed.frequency_hz == Catch::Approx(82.0f).margin(3.0f));
}

TEST_CASE("estimate_pitch_for_span forwards the quiet spectrum", "[belt_tension][pitch]") {
    const auto bg = quiet_with_fan(115.0f, 60.0f);
    auto rd = ringdown_with_fan(115.0f, 60.0f, 99.0f, 8.0f);

    auto blind = estimate_pitch_for_span(rd, kRate, kSpan);
    REQUIRE(blind.valid);
    CHECK(blind.frequency_hz == Catch::Approx(115.0f).margin(3.0f));

    auto informed = estimate_pitch_for_span(rd, kRate, kSpan, DEFAULT_HARMONICS, nullptr, &bg);
    REQUIRE(informed.valid);
    CHECK(informed.frequency_hz == Catch::Approx(99.0f).margin(3.0f));
}

// ============================================================================
// Harmonic concentration: was this event a pluck at all?
// ============================================================================

TEST_CASE("real captures concentrate their energy on a harmonic series",
          "[belt_tension][pitch][golden]") {
    float lo = 0.0f, hi = 0.0f;
    REQUIRE(search_window_for_span(151.0f, &lo, &hi));

    for (const auto* name : {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "a_belt_86hz_3.csv"}) {
        auto s = load_fixture(name);
        auto psd = compute_psd(s, fixture_sample_rate(s), required_bandwidth_hz(hi));
        const float c = harmonic_concentration(psd, 86.0f, DEFAULT_HARMONICS, lo);
        INFO("fixture " << name << " concentration " << c);
        CHECK(c > MIN_HARMONIC_CONCENTRATION);
    }
    for (const auto* name : {"b_belt_82hz_1.csv", "b_belt_82hz_2.csv", "b_belt_82hz_3.csv"}) {
        auto s = load_fixture(name);
        auto psd = compute_psd(s, fixture_sample_rate(s), required_bandwidth_hz(hi));
        const float c = harmonic_concentration(psd, 82.0f, DEFAULT_HARMONICS, lo);
        INFO("fixture " << name << " concentration " << c);
        CHECK(c > MIN_HARMONIC_CONCENTRATION);
    }
}

TEST_CASE("a broadband thump does not concentrate on any harmonic series",
          "[belt_tension][pitch]") {
    // The check that rejects a door closing or a toolhead settling: those clear
    // the energy gate easily and have a real onset and decay, so the spectrum
    // is the only thing that can tell them from a pluck.
    //
    // The load-bearing assertion is on the 90th percentile, not on the largest
    // of the 200 draws. Measured across six disjoint 200-seed blocks, the
    // maximum swings about 9% (0.191 to 0.227) while p90 swings about 1.3%
    // (0.157 to 0.161), so a max-based assertion has a failure threshold that
    // depends on which block it happens to hold - the same largest-of-N trap
    // MIN_HARMONIC_CONCENTRATION itself went through. p90 gives a reproducible
    // one.
    std::vector<float> concentrations;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        Hiss rng{seed};
        std::vector<AccelSample> burst(1545);
        for (size_t i = 0; i < burst.size(); ++i) {
            const float t = static_cast<float>(i) / kRate;
            const float env = 200.0f * std::exp(-t / 0.25f);
            burst[i].time = t;
            burst[i].x = 9810.0f + env * rng.next();
            burst[i].y = env * rng.next();
            burst[i].z = env * rng.next();
        }
        auto psd = compute_psd(burst, kRate, required_bandwidth_hz(165.0f));
        auto est = estimate_pitch(psd, 77.0f, 165.0f);
        if (!est.valid) {
            continue; // the estimator found nothing; the gate never sees it
        }
        concentrations.push_back(
            harmonic_concentration(psd, est.frequency_hz, DEFAULT_HARMONICS, 77.0f));
    }
    REQUIRE(concentrations.size() > 150); // the estimator returns SOMETHING for noise

    std::sort(concentrations.begin(), concentrations.end());
    const auto quantile = [&](double p) {
        return concentrations[static_cast<size_t>(p *
                                                  static_cast<double>(concentrations.size() - 1))];
    };
    const float p50 = quantile(0.50);
    const float p90 = quantile(0.90);
    const size_t at_or_above =
        static_cast<size_t>(std::count_if(concentrations.begin(), concentrations.end(),
                                          [](float c) { return c >= MIN_HARMONIC_CONCENTRATION; }));

    INFO("n=" << concentrations.size() << " p50 " << p50 << " p90 " << p90 << " max "
              << concentrations.back());
    CHECK(p90 < MIN_HARMONIC_CONCENTRATION);
    // Deterministic for this seed range, and not a margin: no thump in it is
    // accepted. Crossings do exist further out - see the constant's note.
    CHECK(at_or_above == 0);
    // ...and none of the above passes vacuously. A change making
    // harmonic_concentration return 0 for every seed would satisfy every
    // upper bound here.
    CHECK(p50 > 0.05f);
}

TEST_CASE("concentration discounts a fan that owns the band", "[belt_tension][pitch]") {
    // Without the quiet spectrum, a genuine pluck under a loud fan scores as
    // unconcentrated as a thump - the fan holds most of the band's energy and
    // none of it sits on the belt's harmonics. Rejecting on that would refuse
    // to measure exactly the machine this work exists for.
    const auto bg = quiet_with_fan(115.0f, 60.0f);
    auto rd = ringdown_with_fan(115.0f, 60.0f, 99.0f, 8.0f);
    auto psd = compute_psd(rd, kRate, required_bandwidth_hz(165.0f));

    const float blind = harmonic_concentration(psd, 99.0f, DEFAULT_HARMONICS, 77.0f);
    const float informed = harmonic_concentration(psd, 99.0f, DEFAULT_HARMONICS, 77.0f, &bg);
    INFO("blind " << blind << " informed " << informed);
    CHECK(blind < MIN_HARMONIC_CONCENTRATION);
    CHECK(informed > MIN_HARMONIC_CONCENTRATION);
}

TEST_CASE("harmonic_concentration rejects degenerate input", "[belt_tension][pitch][edge_case]") {
    auto s = load_fixture("a_belt_86hz_1.csv");
    auto psd = compute_psd(s, fixture_sample_rate(s), 700.0f);
    CHECK(harmonic_concentration({}, 86.0f, DEFAULT_HARMONICS, 77.0f) == 0.0f);
    CHECK(harmonic_concentration(psd, 0.0f, DEFAULT_HARMONICS, 77.0f) == 0.0f);
    CHECK(harmonic_concentration(psd, 86.0f, 0, 77.0f) == 0.0f);
    // A band that starts above its own top is empty, not 100% concentrated.
    CHECK(harmonic_concentration(psd, 86.0f, DEFAULT_HARMONICS, 5000.0f) == 0.0f);
}
