// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pluck_detector.cpp
 * @brief Noise floor, strength gating, and ring-down extraction
 */

#include "../../include/belt_tension_types.h"
#include "../../include/pluck_detector.h"
#include "belt_test_signals.h"

#include <cmath>
#include <cstdint>
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

/// The fixtures record their own rate; every one is near this and none is
/// exactly 3200. Derive it rather than hardcode it.
float fixture_rate(const std::vector<AccelSample>& s) {
    REQUIRE(s.size() > 1);
    const float span = s.back().time - s.front().time;
    REQUIRE(span > 0.0f);
    return static_cast<float>(s.size() - 1) / span;
}

/// The fixtures' measured rate, near enough for synthesised buffers.
constexpr float kRate = 3091.0f;

/// The live detection window the gate and the shape checks run on.
constexpr size_t kWindow = 2048;

using helix::calibration::test::Hiss;

/// Broadband bed at the fixtures' rate - see belt_test_signals.h.
std::vector<AccelSample> hiss_bed(size_t count, float amp, Hiss& rng) {
    return helix::calibration::test::hiss_bed(count, amp, kRate, rng);
}

/// h1 -3 dB, h2 0 dB, h3 -14 dB, h4 -13 dB - the harmonic profile measured on
/// a real A belt.
float harmonic_series(float f0, float t) {
    static const float amps[4] = {0.708f, 1.0f, 0.200f, 0.224f};
    float v = 0.0f;
    for (int h = 0; h < 4; ++h) {
        v += amps[h] *
             std::sin(2.0f * static_cast<float>(M_PI) * f0 * static_cast<float>(h + 1) * t);
    }
    return v;
}

/// A steady tone running the whole window: the fan case, and the closest
/// analogue to the three false plucks the reference machine reported.
std::vector<AccelSample> steady_tone(size_t count, float freq, float amp, float hiss_amp) {
    Hiss rng;
    auto out = hiss_bed(count, hiss_amp, rng);
    for (size_t i = 0; i < count; ++i) {
        out[i].x += amp * std::sin(2.0f * static_cast<float>(M_PI) * freq * out[i].time);
    }
    return out;
}

/// A plucked string: silence, a strike, then an exponentially decaying
/// harmonic series.
std::vector<AccelSample> plucked(size_t count, size_t onset, float f0, float amp, float hiss_amp) {
    Hiss rng;
    auto out = hiss_bed(count, hiss_amp, rng);
    for (size_t i = onset; i < count; ++i) {
        const float dt = static_cast<float>(i - onset) / kRate;
        out[i].x += amp * std::exp(-dt / 0.20f) * harmonic_series(f0, out[i].time);
    }
    return out;
}

/// A thump: an impact with the same sharp onset and decay a pluck has, but
/// with its energy spread across the band instead of sitting on a harmonic
/// series. This is the hard case for the tool - a door closing, or the
/// toolhead settling after a park - and only the spectrum can tell it apart.
///
/// The leading edge is a single full-scale sample. At 3091 Hz one sample is
/// 0.32 ms, which is longer than the rise time of a real impact - a door
/// latch, or a toolhead settling - so this is the correctly band-limited
/// representation of one, not a delta chosen for convenience.
///
/// A raised-cosine attack over 1 ms was tried instead, to make find_onset()
/// locate the edge rather than be handed it. It does not work: for broadband
/// energy the loudest SAMPLE is a random draw, and with a finite rise it lands
/// tens of samples inside the burst (measured: 62), so the reference ends up
/// inside the event and the thump fails the temporal checks. That would lose
/// what this fixture exists for - being the case only the SPECTRUM can reject,
/// which is what makes the harmonic-concentration mutation meaningful.
/// find_onset() is exercised against a plucked string, which is what it is
/// for, by "find_onset locates the strike, not the loudest ring-down sample".
std::vector<AccelSample> noise_burst(size_t count, size_t onset, float amp, float hiss_amp) {
    Hiss rng;
    auto out = hiss_bed(count, hiss_amp, rng);
    Hiss burst{777u};
    for (size_t i = onset; i < count; ++i) {
        const float dt = static_cast<float>(i - onset) / kRate;
        const float env = amp * std::exp(-dt / 0.20f);
        const bool impact = (i == onset);
        out[i].x += env * (impact ? 1.0f : burst.next());
        out[i].y += env * (impact ? 1.0f : burst.next());
        out[i].z += env * (impact ? 1.0f : burst.next());
    }
    return out;
}

/// An envelope that grows across the window instead of decaying - a machine
/// winding up, not a string ringing down.
std::vector<AccelSample> swelling(size_t count, float freq, float amp, float hiss_amp) {
    Hiss rng;
    auto out = hiss_bed(count, hiss_amp, rng);
    const float total = static_cast<float>(count) / kRate;
    for (size_t i = 0; i < count; ++i) {
        const float env = std::exp((out[i].time - total) / 0.20f);
        out[i].x += amp * env * std::sin(2.0f * static_cast<float>(M_PI) * freq * out[i].time);
    }
    return out;
}

/// Set the floor so this window measures exactly `ratio` times it.
PluckDetector detector_at(const std::vector<AccelSample>& win, float ratio) {
    PluckDetector det;
    det.set_noise_floor(PluckDetector::window_rms(win.data(), win.size()) / ratio);
    return det;
}

/// What BeltListenSession asks of a window before it will analyse it.
bool pluck_shaped(const std::vector<AccelSample>& w, float rate) {
    return PluckDetector::has_sharp_onset(w.data(), w.size(), rate) &&
           PluckDetector::has_pluck_decay(w.data(), w.size(), rate);
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

// ============================================================================
// Shape: energy says something happened, shape says whether it was a pluck
// ============================================================================

TEST_CASE("a steady tone is rejected however loud it is", "[belt_tension][pluck_detect]") {
    // Three transients at 44-53x the floor were reported as plucks on the
    // reference machine by a maintainer who plucked nothing. Energy alone
    // cannot tell them apart from a strike, and raising MIN_RMS_RATIO would
    // not have helped - this one is 50x.
    auto win = steady_tone(kWindow, 115.0f, 500.0f, 5.0f);
    auto det = detector_at(win, 50.0f);

    REQUIRE(det.passes_gate(win.data(), win.size())); // the old gate accepts it
    CHECK_FALSE(PluckDetector::has_sharp_onset(win.data(), win.size(), kRate));
    CHECK_FALSE(PluckDetector::has_pluck_decay(win.data(), win.size(), kRate));
    CHECK_FALSE(pluck_shaped(win, kRate));
}

TEST_CASE("an envelope that grows across the window is rejected", "[belt_tension][pluck_detect]") {
    auto win = swelling(kWindow, 115.0f, 500.0f, 5.0f);
    auto det = detector_at(win, 50.0f);

    REQUIRE(det.passes_gate(win.data(), win.size()));
    CHECK_FALSE(PluckDetector::has_sharp_onset(win.data(), win.size(), kRate));
    CHECK_FALSE(pluck_shaped(win, kRate));
}

TEST_CASE("a genuine pluck at 12x passes the shape checks", "[belt_tension][pluck_detect]") {
    // The guard against fixing false accepts by raising MIN_RMS_RATIO: this
    // strike is barely over the existing gate and must still be accepted.
    auto win = plucked(kWindow, 700, 99.0f, 400.0f, 5.0f);
    auto det = detector_at(win, 12.0f);

    REQUIRE(det.passes_gate(win.data(), win.size()));
    CHECK(PluckDetector::has_sharp_onset(win.data(), win.size(), kRate));
    CHECK(PluckDetector::has_pluck_decay(win.data(), win.size(), kRate));
}

TEST_CASE("a sharp broadband thump clears the temporal checks", "[belt_tension][pluck_detect]") {
    // Deliberately NOT rejected here: a thump has a real onset and a real
    // decay, so the temporal checks cannot tell it from a pluck. Only its
    // spectrum can - see harmonic_concentration() in test_pitch_estimator.cpp.
    // This pins which check owns which failure.
    auto win = noise_burst(kWindow, 700, 400.0f, 5.0f);
    CHECK(pluck_shaped(win, kRate));
}

TEST_CASE("has_sharp_onset rejects a window whose onset is at its start",
          "[belt_tension][pluck_detect][edge_case]") {
    // The pre-strike quiet IS the evidence. An extracted ring-down begins at
    // the strike, so it has none - which is why the check runs on the live
    // detection window and never on extract_ringdown() output.
    auto ringdown = load_fixture("b_belt_82hz_1.csv");
    CHECK_FALSE(
        PluckDetector::has_sharp_onset(ringdown.data(), ringdown.size(), fixture_rate(ringdown)));
}

TEST_CASE("shape checks reject degenerate input", "[belt_tension][pluck_detect][edge_case]") {
    auto win = plucked(kWindow, 700, 99.0f, 400.0f, 5.0f);
    CHECK_FALSE(PluckDetector::has_sharp_onset(nullptr, 0, kRate));
    CHECK_FALSE(PluckDetector::has_pluck_decay(nullptr, 0, kRate));
    CHECK_FALSE(PluckDetector::has_sharp_onset(win.data(), win.size(), 0.0f));
    CHECK_FALSE(PluckDetector::has_pluck_decay(win.data(), win.size(), 0.0f));
    CHECK_FALSE(PluckDetector::has_sharp_onset(win.data(), 8, kRate));
    CHECK_FALSE(PluckDetector::has_pluck_decay(win.data(), 8, kRate));
}

TEST_CASE("find_onset locates the strike, not the loudest ring-down sample",
          "[belt_tension][pluck_detect]") {
    auto win = plucked(kWindow, 700, 99.0f, 400.0f, 5.0f);
    const size_t onset = PluckDetector::find_onset(win.data(), win.size());
    INFO("onset=" << onset);
    CHECK(onset >= 700);
    CHECK(onset < 700 + static_cast<size_t>(kRate * 0.02f));
    CHECK(PluckDetector::find_onset(nullptr, 0) == 0);
}

TEST_CASE("every real capture decays the way a plucked string does",
          "[belt_tension][pluck_detect][golden]") {
    // The regression guard on the shape checks: these are real plucks on real
    // hardware. A decay rule tuned until the synthetic cases pass but that
    // rejects a genuine capture has made the tool worse, not safer.
    for (const auto* name :
         {"a_belt_86hz_1.csv", "a_belt_86hz_2.csv", "a_belt_86hz_3.csv", "b_belt_82hz_1.csv",
          "b_belt_82hz_2.csv", "b_belt_82hz_3.csv", "b_belt_82hz_hard_case.csv"}) {
        auto s = load_fixture(name);
        INFO("fixture " << name);
        CHECK(PluckDetector::has_pluck_decay(s.data(), s.size(), fixture_rate(s)));
    }

    // The capture that never rang: its envelope is flat and ragged, so it
    // fails the decay rule as well as the energy gate.
    auto weak = load_fixture("weak_pluck_reject.csv");
    CHECK_FALSE(PluckDetector::has_pluck_decay(weak.data(), weak.size(), fixture_rate(weak)));
}

TEST_CASE("ringdown_ready waits for a strike that has only just landed",
          "[belt_tension][pluck_detect]") {
    // A firm strike trips the energy gate the moment its leading edge enters
    // the window. Judging the envelope then would reject exactly the firmest
    // plucks, since those are the ones that trip the gate earliest.
    const size_t just_landed = kWindow - static_cast<size_t>(kRate * 0.05f);
    auto early = plucked(kWindow, just_landed, 99.0f, 400.0f, 5.0f);
    CHECK_FALSE(PluckDetector::ringdown_ready(early.data(), early.size(), kRate));
    CHECK_FALSE(PluckDetector::has_pluck_decay(early.data(), early.size(), kRate));

    auto settled = plucked(kWindow, 700, 99.0f, 400.0f, 5.0f);
    CHECK(PluckDetector::ringdown_ready(settled.data(), settled.size(), kRate));

    CHECK_FALSE(PluckDetector::ringdown_ready(nullptr, 0, kRate));
    CHECK_FALSE(PluckDetector::ringdown_ready(settled.data(), settled.size(), 0.0f));
}
