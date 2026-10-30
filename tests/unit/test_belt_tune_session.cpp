// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_tension_types.h"
#include "belt_test_curves.h"
#include "belt_tune_session.h"
#include "shaper_csv_parser.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using helix::calibration::test::lorentzian_curve;
using helix::calibration::test::response_from;
using helix::calibration::test::shifted;
using helix::calibration::test::windowed;

namespace {

constexpr float kLo = 5.0f;
constexpr float kHi = 135.0f;
constexpr float kBinHz = 3200.0f / 4096.0f;

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

BeltCurve load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + "belt_sweeps/" + name;
    auto data = parse_resonance_csv(path);
    INFO("fixture missing or unparsable: " << path);
    REQUIRE(data.error == ResonanceCsvError::NONE);
    REQUIRE_FALSE(data.curve.empty());
    return data.curve;
}

/// One path of a hardware capture pair, through the parser: path 'A' reads
/// `<name>_axis_1_-1.csv`, 'B' reads `<name>_axis_1_1.csv`.
BeltResponse load_fixture_response(const std::string& name, char path) {
    const std::string file =
        fixture_dir() + "belt_sweeps/" + name + (path == 'A' ? "_axis_1_-1.csv" : "_axis_1_1.csv");
    auto data = parse_resonance_csv(file);
    INFO("fixture missing or unparsable: " << file);
    REQUIRE(data.error == ResonanceCsvError::NONE);
    REQUIRE_FALSE(data.in_plane.empty());
    return BeltResponse{data.curve, data.in_plane};
}

/// A sharp synthetic peak: its zone centroid sits close to it.
BeltCurve one_peak(float hz, float lo = kLo, float hi = kHi) {
    return lorentzian_curve({{hz, 1e4f}}, lo, hi, kBinHz, 0.01f, 1.0f);
}

BeltResponse peak_response(float hz) {
    return response_from(one_peak(hz));
}

/// A session whose round 0 is the two responses, compared on psd_xyz as RESULTS does.
BeltTuneSession session_from(const BeltResponse& a, const BeltResponse& b, BeltTensionerKind kind,
                             float lo = kLo, float hi = kHi) {
    return BeltTuneSession(a, b, compare_belt_paths(a.xyz, b.xyz, lo, hi), lo, hi, kind);
}

/// Round 0 from one peak per path over the full band.
BeltTuneSession session_for(float a_hz, float b_hz, BeltTensionerKind kind) {
    return session_from(peak_response(a_hz), peak_response(b_hz), kind);
}

/// The unexcited floor of a synthetic narrow sweep.
constexpr float kFloor = 0.01f;

/// Both paths swept over the session's sweep window: driven bins inside it,
/// the unexcited floor across the rest of the band.
const BeltTuneRound& sweep_curves(BeltTuneSession& s, const BeltCurve& a, const BeltCurve& b) {
    REQUIRE(s.sweep_window().has_value());
    const auto w = *s.sweep_window();
    return s.add_round(windowed(response_from(a), w, kFloor),
                       windowed(response_from(b), w, kFloor));
}

const BeltTuneRound& sweep(BeltTuneSession& s, float a_hz, float b_hz) {
    return sweep_curves(s, one_peak(a_hz), one_peak(b_hz));
}

/// The zone's baseline pair, both rounds alike: the stage moves on to TUNE.
void baseline(BeltTuneSession& s, float a_hz, float b_hz) {
    REQUIRE(s.stage() == BeltTuneStage::BASELINE);
    sweep(s, a_hz, b_hz);
    sweep(s, a_hz, b_hz);
    REQUIRE(s.stage() == BeltTuneStage::TUNE);
}

/// A narrow sweep of one symmetric three-bin peak on a 0.25 Hz grid, zero
/// across the rest of the sweep window and FLOOR outside it: its zone
/// centroid is exactly centre_hz and its zone power 4 x scale.
BeltResponse triangle(float centre_hz, float scale, BeltSweepWindow swept) {
    BeltCurve c;
    for (int i = 0; i <= 520; ++i) {
        const float f = kLo + 0.25f * static_cast<float>(i);
        float v = (f < swept.freq_start_hz || f > swept.freq_end_hz) ? kFloor : 0.0f;
        if (f == centre_hz) {
            v = 2.0f * scale;
        } else if (f == centre_hz - 0.25f || f == centre_hz + 0.25f) {
            v = scale;
        }
        c.emplace_back(f, v);
    }
    return response_from(c);
}

/// Both paths swept as exact three-bin peaks: their centroids are a_hz and b_hz.
const BeltTuneRound& sweep_exact(BeltTuneSession& s, float a_hz, float b_hz) {
    REQUIRE(s.sweep_window().has_value());
    const auto w = *s.sweep_window();
    return s.add_round(triangle(a_hz, 1.0f, w), triangle(b_hz, 1.0f, w));
}

/// tuning(), with the baseline pair read as exact peaks: the gap is exactly
/// b1_hz - a_hz. The rate is the calibration's, measured on Lorentzian peaks.
BeltTuneSession tuning_exact(float a_hz, float b0_hz, float b1_hz, BeltTensionerKind kind) {
    auto s = session_for(a_hz, b0_hz, kind);
    REQUIRE(s.add_calibration_sweep(peak_response(a_hz), peak_response(b1_hz)).registered);
    sweep_exact(s, a_hz, b1_hz);
    sweep_exact(s, a_hz, b1_hz);
    REQUIRE(s.stage() == BeltTuneStage::TUNE);
    return s;
}

/// A session past calibration and its baseline pair: round 0 at (a_hz,
/// b0_hz), B adjusted, and the calibration turn moved B to b1_hz.
BeltTuneSession tuning(float a_hz, float b0_hz, float b1_hz, BeltTensionerKind kind) {
    auto s = session_for(a_hz, b0_hz, kind);
    REQUIRE(s.stage() == BeltTuneStage::CALIBRATE);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    REQUIRE(s.add_calibration_sweep(peak_response(a_hz), peak_response(b1_hz)).registered);
    baseline(s, a_hz, b1_hz);
    return s;
}

/// A flat zone: no peak anywhere in the sweep.
BeltCurve flat_curve(float level = 5.0f) {
    BeltCurve flat;
    for (float f = kLo; f < kHi; f += kBinHz) {
        flat.emplace_back(f, level);
    }
    return flat;
}

BeltPeak pk(float hz, float amp) {
    return BeltPeak{hz, amp};
}

BeltComparison cmp_with(std::vector<BeltPeakPair> pairs, std::vector<BeltPeak> unpaired_a = {},
                        std::vector<BeltPeak> unpaired_b = {}) {
    BeltComparison c;
    c.valid = true;
    c.peaks.pairs = std::move(pairs);
    c.peaks.unpaired_a = std::move(unpaired_a);
    c.peaks.unpaired_b = std::move(unpaired_b);
    c.peaks.threshold_hz = belt_verdict::kPairMaxHz;
    return c;
}

} // namespace

// ============================================================================
// Shape warning, real captures, tolerance
// ============================================================================

TEST_CASE("shape warning: an unpaired tallest peak above the amplitude floor", "[belt][tune]") {
    SECTION("both tallest peaks unpaired") {
        CHECK(tallest_peak_unpaired(cmp_with({{pk(60.0f, 30.0f), pk(61.0f, 30.0f)}},
                                             {pk(98.0f, 100.0f)}, {pk(110.0f, 90.0f)})));
    }
    SECTION("either tallest peak unpaired") {
        CHECK(tallest_peak_unpaired(cmp_with(
            {{pk(36.0f, 100.0f), pk(35.0f, 95.0f)}, {pk(131.0f, 50.0f), pk(133.0f, 45.0f)}},
            {pk(118.0f, 10.0f)}, {pk(119.5f, 98.0f)})));
    }
    SECTION("paired tallest peaks raise none") {
        CHECK_FALSE(tallest_peak_unpaired(cmp_with(
            {{pk(50.0f, 100.0f), pk(52.0f, 100.0f)}, {pk(80.0f, 60.0f), pk(86.0f, 60.0f)}})));
    }
    SECTION("an unpaired tallest peak under 20% of the other path's is a weak path") {
        CHECK_FALSE(tallest_peak_unpaired(
            cmp_with({{pk(98.0f, 100.0f), pk(99.0f, 10.0f)}}, {}, {pk(50.0f, 15.0f)})));
    }
    SECTION("an invalid comparison raises none") {
        CHECK_FALSE(tallest_peak_unpaired(BeltComparison{}));
    }
}

TEST_CASE("tune zone on real captures", "[belt][tune]") {
    SECTION("Voron 2.4: B's unpaired belt hump steers against A's nearest peak") {
        const BeltResponse a{load_fixture("voron24_kalico_axis_1_-1.csv"), {}};
        const BeltResponse b{load_fixture("voron24_kalico_axis_1_1.csv"), {}};
        const auto c = tune_zone_candidates(a, b, 5.0f, 135.0f);
        REQUIRE_FALSE(c.empty());
        const auto it = std::find_if(c.begin(), c.end(), [](const BeltZoneCandidate& z) {
            return z.zone.source == BeltZoneSource::TALLEST_UNPAIRED;
        });
        REQUIRE(it != c.end());
        INFO("zone A " << it->zone.adj_hz << " B " << it->zone.ref_hz << " window "
                       << it->zone.window.freq_start_hz << "-" << it->zone.window.freq_end_hz);
        CHECK(it->zone.adj_hz == Catch::Approx(131.5f).margin(0.1f));
        CHECK(it->zone.ref_hz == Catch::Approx(119.5f).margin(0.1f));
        // Midpoint 125.5 +- (12/2 + 5), clamped at the band top.
        CHECK(it->zone.window.freq_start_hz == Catch::Approx(114.5f).margin(0.5f));
        CHECK(it->zone.window.freq_end_hz == Catch::Approx(135.0f).margin(0.5f));

        const auto s = session_from(a, b, BeltTensionerKind::SCREW, 5.0f, 135.0f);
        CHECK(s.show_mechanical_hint());
    }

    SECTION("K1C (98.9%): candidates now, and the stage follows the gap") {
        const BeltResponse a{load_fixture("k1c_axis_1_-1.csv"), {}};
        const BeltResponse b{load_fixture("k1c_axis_1_1.csv"), {}};
        const auto s = session_from(a, b, BeltTensionerKind::SCREW, 5.0f, 133.333f);
        REQUIRE_FALSE(s.candidates().empty());
        CHECK_FALSE(s.zone().has_value());
        const float threshold = std::max(s.tolerance_hz(), belt_tune::kCalibrationMinMoveHz);
        INFO("gap " << s.gap_hz() << " threshold " << threshold);
        CHECK(s.stage() == (std::abs(s.gap_hz()) <= threshold ? BeltTuneStage::ALREADY_MATCHED
                                                              : BeltTuneStage::CALIBRATE));
    }
}

TEST_CASE("matched tolerance is one bin, at least 1 Hz", "[belt][tune]") {
    CHECK(matched_tolerance_hz(lorentzian_curve({{100.0f, 1.0f}}, 80.0f, 120.0f)) ==
          Catch::Approx(1.0f));
    CHECK(matched_tolerance_hz(lorentzian_curve({{100.0f, 1.0f}}, 80.0f, 120.0f, 1.5f)) ==
          Catch::Approx(1.5f).margin(0.01f));
    CHECK(matched_tolerance_hz({}) == Catch::Approx(belt_tune::kMatchedMinHz));
    CHECK(matched_tolerance_hz({{100.0f, 1.0f}}) == Catch::Approx(belt_tune::kMatchedMinHz));
}

TEST_CASE("zone candidates: the amplitude floor follows the in-band maximum", "[belt][tune]") {
    // The 60 Hz pair is 15% of the 100 Hz peak: under the floor. At 25% it counts.
    const auto under = response_from(
        lorentzian_curve({{100.0f, 1e4f}, {60.0f, 1.5e3f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    CHECK(tune_zone_candidates(under, under, kLo, kHi).size() == 1);
    const auto over = response_from(
        lorentzian_curve({{100.0f, 1e4f}, {60.0f, 2.5e3f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    CHECK(tune_zone_candidates(over, over, kLo, kHi).size() == 2);

    // compare_belt_paths records the stronger curve's in-band maximum.
    const auto a = lorentzian_curve({{60.0f, 5e3f}}, kLo, kHi);
    const auto b = lorentzian_curve({{60.0f, 7e3f}}, kLo, kHi);
    float top = 0.0f;
    for (const auto& [f, v] : b) {
        top = std::max(top, v);
    }
    CHECK(compare_belt_paths(a, b, kLo, kHi).band_max_psd == Catch::Approx(top));
}

TEST_CASE("tune peak: prominence over the unexcited floor", "[belt][tune]") {
    SECTION("the real Voron B hump in a centred 20 Hz window holds a peak") {
        const auto full = load_fixture("voron24_kalico_axis_1_1.csv");
        const BeltSweepWindow w{109.5f, 129.5f};
        const auto in_window = curve_in_band(full, w.freq_start_hz, w.freq_end_hz);
        float lo = in_window.front().second, hi = lo;
        for (const auto& [f, v] : in_window) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        // The hump is broad: inside the window it never reaches 1.5x its own minimum.
        REQUIRE(hi < belt_tune::kPeakMinProminence * lo);
        float floor = hi;
        for (const auto& [f, v] : curve_in_band(full, 5.0f, 135.0f)) {
            floor = std::min(floor, v);
        }
        const auto swept = windowed(full, w, floor);
        CHECK(window_holds_peak(swept, w, w, 5.0f, 135.0f));
    }

    SECTION("a window at the floor holds no peak") {
        const BeltSweepWindow w{95.0f, 117.0f};
        const auto flat = windowed(lorentzian_curve({}, kLo, kHi, kBinHz, 5.0f), w, 5.0f);
        REQUIRE(unexcited_floor_psd(flat, w, kLo, kHi).has_value());
        CHECK_FALSE(window_holds_peak(flat, w, w, kLo, kHi));
    }

    SECTION("with too few unexcited bins, the window's own minimum is the floor") {
        const BeltSweepWindow w{95.0f, 117.0f};
        const auto only_window = lorentzian_curve({{106.0f, 1e4f}}, 95.0f, 117.5f);
        CHECK_FALSE(unexcited_floor_psd(only_window, w, kLo, kHi).has_value());
        CHECK(window_holds_peak(only_window, w, w, kLo, kHi));
    }
}

// ============================================================================
// Stages, already matched, calibration
// ============================================================================

TEST_CASE("tune stages: screw and unknown calibrate, spring baselines, nothing is NO_ZONE",
          "[belt][tune]") {
    for (auto kind : {BeltTensionerKind::SCREW, BeltTensionerKind::UNKNOWN}) {
        auto s = session_for(104.0f, 100.0f, kind);
        CHECK(s.stage() == BeltTuneStage::CALIBRATE);
        CHECK_FALSE(s.zone().has_value());
        CHECK_FALSE(s.sweep_window().has_value());
        CHECK_FALSE(s.needs_baseline());
    }

    SECTION("spring: the strongest candidate, its looser path adjusted") {
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        CHECK(s.stage() == BeltTuneStage::BASELINE);
        CHECK(s.needs_baseline());
        REQUIRE(s.zone().has_value());
        CHECK(s.adjusted_path() == BeltPath::PATH_B);
        CHECK(s.zone()->adj_hz == Catch::Approx(100.0f).margin(0.5f));
        CHECK(s.zone()->ref_hz == Catch::Approx(104.0f).margin(0.5f));
        CHECK(s.gap_hz() < 0.0f);
        REQUIRE(s.sweep_window().has_value());
    }

    SECTION("no candidates: NO_ZONE, and a round books no turns") {
        const auto flat = response_from(flat_curve());
        auto s = session_from(flat, flat, BeltTensionerKind::SCREW);
        CHECK(s.candidates().empty());
        CHECK(s.stage() == BeltTuneStage::NO_ZONE);
        CHECK_FALSE(s.zone().has_value());
        s.record_turns(0.5f);
        CHECK(s.add_round(flat, flat).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
    }
}

TEST_CASE("already matched: the K1C's settled belts, for every tensioner", "[belt][tune]") {
    const auto a = load_fixture_response("k1c_settled", 'A');
    const auto b = load_fixture_response("k1c_settled", 'B');
    for (auto kind :
         {BeltTensionerKind::SCREW, BeltTensionerKind::UNKNOWN, BeltTensionerKind::SPRING}) {
        INFO("kind " << static_cast<int>(kind));
        auto s = session_from(a, b, kind, 5.0f, 133.0f);
        CHECK(s.stage() == BeltTuneStage::ALREADY_MATCHED);
        CHECK_FALSE(s.zone().has_value());
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
        CHECK(s.gap_hz() == Catch::Approx(-0.14f).margin(0.05f));
        REQUIRE(s.candidates().size() == 1);
        CHECK(s.candidates()[0].zone.window.freq_start_hz == Catch::Approx(48.4f).margin(0.1f));
        CHECK(s.candidates()[0].zone.window.freq_end_hz == Catch::Approx(68.4f).margin(0.1f));

        s.calibrate_anyway();
        if (kind == BeltTensionerKind::SPRING) {
            CHECK(s.stage() == BeltTuneStage::BASELINE);
            REQUIRE(s.sweep_window().has_value());
            CHECK(s.sweep_window()->freq_start_hz == 39.0f);
            CHECK(s.sweep_window()->freq_end_hz == 88.0f);
            continue;
        }
        CHECK(s.stage() == BeltTuneStage::CALIBRATE);
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
        CHECK(s.advice().path == BeltPath::PATH_A);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
        CHECK(s.advice().turns == (kind == BeltTensionerKind::SCREW
                                       ? std::optional<float>(belt_tune::kCalibrationTurns)
                                       : std::nullopt));
        // calibrate_anyway() at CALIBRATE changes nothing.
        s.calibrate_anyway();
        CHECK(s.stage() == BeltTuneStage::CALIBRATE);
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
    }

    SECTION("the repeat capture is matched too") {
        const auto s = session_from(load_fixture_response("k1c_settled_repeat", 'A'),
                                    load_fixture_response("k1c_settled_repeat", 'B'),
                                    BeltTensionerKind::SCREW, 5.0f, 133.0f);
        CHECK(s.stage() == BeltTuneStage::ALREADY_MATCHED);
        CHECK(std::abs(s.gap_hz()) == Catch::Approx(0.23f).margin(0.05f));
    }
}

TEST_CASE("already matched: a 2 Hz gap is not already matched", "[belt][tune]") {
    const auto s = session_from(load_fixture_response("voron24_settled", 'A'),
                                load_fixture_response("voron24_settled", 'B'),
                                BeltTensionerKind::SCREW, 5.0f, 135.0f);
    CHECK(s.stage() == BeltTuneStage::CALIBRATE);
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.gap_hz() == Catch::Approx(-1.95f).margin(0.05f));
}

TEST_CASE("already matched: a pair within one bin", "[belt][tune]") {
    // 1.5 Hz bins: the pair is 1.5 Hz apart, over the 1 Hz floor but within a bin.
    const auto a = response_from(lorentzian_curve({{99.5f, 1e4f}}, kLo, kHi, 1.5f));
    const auto b = response_from(lorentzian_curve({{101.0f, 1e4f}}, kLo, kHi, 1.5f));
    auto s = session_from(a, b, BeltTensionerKind::SCREW);
    CHECK(s.tolerance_hz() == Catch::Approx(1.5f).margin(0.01f));
    CHECK(s.stage() == BeltTuneStage::ALREADY_MATCHED);
    CHECK_FALSE(s.zone().has_value());
    CHECK_FALSE(s.has_next_zone());
}

TEST_CASE("already matched: under the calibration's 1.5 Hz wander, over one bin", "[belt][tune]") {
    // 0.78 Hz bins: tolerance 1 Hz, so the 1.5 Hz wander floor decides.
    const auto within = session_for(101.3f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(within.tolerance_hz() == Catch::Approx(1.0f));
    INFO("gap " << within.gap_hz());
    REQUIRE(std::abs(within.gap_hz()) > within.tolerance_hz());
    CHECK(within.stage() == BeltTuneStage::ALREADY_MATCHED);
    const auto over = session_for(102.2f, 100.0f, BeltTensionerKind::SCREW);
    INFO("gap " << over.gap_hz());
    REQUIRE(std::abs(over.gap_hz()) > belt_tune::kCalibrationMinMoveHz);
    CHECK(over.stage() == BeltTuneStage::CALIBRATE);
}

TEST_CASE(
    "calibration advice: one full turn of the looser belt, and a swap loosens the tighter one",
    "[belt][tune]") {
    auto screw = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(screw.stage() == BeltTuneStage::CALIBRATE);
    CHECK(screw.adjusted_path() == BeltPath::PATH_B);
    CHECK(screw.advice().path == BeltPath::PATH_B);
    CHECK(screw.advice().direction == BeltTurnDirection::TIGHTEN);
    CHECK(screw.advice().turns == std::optional<float>(1.0f));

    auto unknown = session_for(104.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    CHECK(unknown.advice().direction == BeltTurnDirection::TIGHTEN);
    CHECK_FALSE(unknown.advice().turns.has_value());

    CHECK(screw.can_swap_adjusted_path());
    CHECK(screw.swap_adjusted_path());
    CHECK(screw.stage() == BeltTuneStage::CALIBRATE);
    CHECK(screw.adjusted_path() == BeltPath::PATH_A);
    CHECK(screw.advice().path == BeltPath::PATH_A);
    // A is the tighter belt: closing the gap from its side means loosening it.
    CHECK(screw.advice().direction == BeltTurnDirection::LOOSEN);
    CHECK(screw.advice().turns == std::optional<float>(1.0f));
}

TEST_CASE("calibration swap: refused once a calibration step is in", "[belt][tune]") {
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE_FALSE(s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f)).registered);
    REQUIRE(s.stage() == BeltTuneStage::CALIBRATE);
    CHECK_FALSE(s.can_swap_adjusted_path());
    CHECK_FALSE(s.swap_adjusted_path());
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    CHECK(s.gap_hz() < 0.0f);
    // The give-up after the second step still names B's two turns.
    const auto r = s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f));
    CHECK(r.gave_up);
    CHECK(r.turns == 2.0f);
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(2.0f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
}

TEST_CASE("calibration swap: loosening the tighter belt registers the gap closing",
          "[belt][tune]") {
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.swap_adjusted_path());
    REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);

    SECTION("A falls 3 Hz: the advised way") {
        const auto r = s.add_calibration_sweep(peak_response(101.0f), peak_response(100.0f));
        CHECK(r.registered);
        CHECK(r.move_hz == Catch::Approx(3.0f).margin(0.3f));
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(3.0f).margin(0.3f));
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
        CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(-1.0f));
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
    }
    SECTION("A rises 3 Hz: the wrong way, not registered") {
        const auto r = s.add_calibration_sweep(peak_response(107.0f), peak_response(100.0f));
        CHECK_FALSE(r.registered);
        CHECK(r.move_hz < 0.0f);
    }
    SECTION("two steps that move nothing give up on A") {
        s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f));
        const auto r = s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f));
        CHECK(r.gave_up);
        CHECK(r.turns == 2.0f);
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
        CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(-2.0f));
    }
}

TEST_CASE("calibration on a real shape: the K1C with B shifted down", "[belt][tune]") {
    const auto a = load_fixture_response("k1c_settled", 'A');
    const auto b = shifted(load_fixture_response("k1c_settled", 'B'), -4.0f);
    const auto a2 = load_fixture_response("k1c_settled_repeat", 'A');
    const auto b2 = load_fixture_response("k1c_settled_repeat", 'B');

    auto s = session_from(a, b, BeltTensionerKind::SCREW, 5.0f, 133.0f);
    REQUIRE(s.stage() == BeltTuneStage::CALIBRATE);
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.gap_hz() == Catch::Approx(-2.96f).margin(0.05f));
    REQUIRE(s.candidates().size() == 1);
    CHECK(s.candidates()[0].zone.window.freq_start_hz == Catch::Approx(46.4f).margin(0.1f));
    CHECK(s.candidates()[0].zone.window.freq_end_hz == Catch::Approx(66.4f).margin(0.1f));

    SECTION("a 2.2 Hz move registers on the first step") {
        const auto r = s.add_calibration_sweep(a2, shifted(b2, -1.5f));
        CHECK(r.registered);
        CHECK(r.move_hz == Catch::Approx(2.20f).margin(0.05f));
        CHECK(r.turns == 1.0f);
        CHECK_FALSE(r.gave_up);
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(2.20f).margin(0.05f));
        CHECK(s.stage() == BeltTuneStage::BASELINE);
        REQUIRE(s.sweep_window().has_value());
        CHECK(s.sweep_window()->freq_start_hz == 37.0f);
        CHECK(s.sweep_window()->freq_end_hz == 86.0f);
    }

    SECTION("an unmoved belt does not register, and two steps give up") {
        const auto first = s.add_calibration_sweep(a2, shifted(b2, -4.0f));
        CHECK_FALSE(first.registered);
        CHECK(first.move_hz == Catch::Approx(0.16f).margin(0.05f));
        CHECK_FALSE(first.gave_up);
        CHECK(s.stage() == BeltTuneStage::CALIBRATE);
        const auto second = s.add_calibration_sweep(a2, shifted(b2, -4.0f));
        CHECK_FALSE(second.registered);
        CHECK(second.gave_up);
        CHECK(second.turns == 2.0f);
        CHECK(s.stage() == BeltTuneStage::NO_ZONE);
        CHECK_FALSE(s.zone().has_value());
    }
}

TEST_CASE("calibration on Voron data: one turn does not register, two do", "[belt][tune]") {
    auto s = session_from(load_fixture_response("voron24_settled", 'A'),
                          load_fixture_response("voron24_settled", 'B'), BeltTensionerKind::SCREW,
                          5.0f, 135.0f);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);

    // The 27.8-47.8 candidate moved +0.06; the 118.6-135 zone 1.17 the wrong way.
    const auto one = s.add_calibration_sweep(load_fixture_response("voron24_turn1", 'A'),
                                             load_fixture_response("voron24_turn1", 'B'));
    CHECK_FALSE(one.registered);
    CHECK(one.turns == 1.0f);
    CHECK(one.move_hz == Catch::Approx(0.06f).margin(0.05f));
    CHECK(s.stage() == BeltTuneStage::CALIBRATE);

    const auto two = s.add_calibration_sweep(load_fixture_response("voron24_turn2", 'A'),
                                             load_fixture_response("voron24_turn2", 'B'));
    CHECK(two.registered);
    CHECK(two.move_hz == Catch::Approx(3.43f).margin(0.05f));
    CHECK(s.stage() == BeltTuneStage::BASELINE);
    REQUIRE(s.zone().has_value());
    CHECK(s.zone()->window.freq_start_hz == Catch::Approx(118.6f).margin(0.1f));
    CHECK(s.zone()->window.freq_end_hz == Catch::Approx(135.0f).margin(0.1f));
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(1.715f).margin(0.03f));
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(2.0f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
    REQUIRE(s.sweep_window().has_value());
    CHECK(s.sweep_window()->freq_start_hz == 109.0f);
    CHECK(s.sweep_window()->freq_end_hz == 135.0f);
    CHECK_FALSE(s.has_next_zone());
    const auto hint = s.power_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->stronger == BeltPath::PATH_B);
    CHECK(hint->ratio == Catch::Approx(15.3f).margin(0.3f));
}

TEST_CASE("calibration on Voron data: a no-turn repeat never registers", "[belt][tune]") {
    auto s = session_from(load_fixture_response("voron24_settled", 'A'),
                          load_fixture_response("voron24_settled", 'B'), BeltTensionerKind::SCREW,
                          5.0f, 135.0f);
    const auto a = load_fixture_response("voron24_settled_repeat", 'A');
    const auto b = load_fixture_response("voron24_settled_repeat", 'B');
    const auto first = s.add_calibration_sweep(a, b);
    CHECK_FALSE(first.registered);
    CHECK_FALSE(first.gave_up);
    const auto second = s.add_calibration_sweep(a, b);
    CHECK_FALSE(second.registered);
    CHECK(second.gave_up);
    CHECK(s.stage() == BeltTuneStage::NO_ZONE);
}

TEST_CASE("calibration: a move the wrong way never registers", "[belt][tune]") {
    for (const float b_hz : {97.0f, 94.0f}) {
        INFO("B loosened to " << b_hz);
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
        REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
        const auto r = s.add_calibration_sweep(peak_response(104.0f), peak_response(b_hz));
        CHECK_FALSE(r.registered);
        CHECK(r.move_hz < -2.0f);
        CHECK(s.stage() == BeltTuneStage::CALIBRATE);
    }
}

TEST_CASE("calibration: registered zones walk best move first; unmoved ones are skipped",
          "[belt][tune]") {
    // B is looser in all three zones. The turn moves 97 by 3 Hz, 47 by 2 Hz
    // and 72 not at all.
    const auto a = response_from(lorentzian_curve({{50.0f, 1e4f}, {75.0f, 1e4f}, {100.0f, 1e4f}},
                                                  kLo, kHi, kBinHz, 0.01f, 1.0f));
    const auto b = [](float low, float mid, float high) {
        return response_from(lorentzian_curve({{low, 1e4f}, {mid, 1e4f}, {high, 1e4f}}, kLo, kHi,
                                              kBinHz, 0.01f, 1.0f));
    };
    auto s = session_from(a, b(47.0f, 72.0f, 97.0f), BeltTensionerKind::SCREW);
    REQUIRE(s.candidates().size() == 3);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    const auto r = s.add_calibration_sweep(a, b(49.0f, 72.0f, 100.0f));
    REQUIRE(r.registered);
    CHECK(r.move_hz == Catch::Approx(3.0f).margin(0.3f));
    REQUIRE(s.zone().has_value());
    CHECK(s.zone()->ref_hz == Catch::Approx(100.0f).margin(0.5f));
    REQUIRE(s.has_next_zone());
    REQUIRE(s.peek_next_zone().has_value());
    CHECK(s.peek_next_zone()->adj_hz == Catch::Approx(50.0f).margin(0.5f));

    baseline(s, 100.0f, 100.0f);
    REQUIRE(s.next_zone());
    CHECK(s.needs_baseline());
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.zone()->ref_hz == Catch::Approx(50.0f).margin(0.5f));
    CHECK(s.rounds().empty());
    CHECK_FALSE(s.has_next_zone());
    CHECK_FALSE(s.next_zone());
    CHECK(s.zone()->ref_hz == Catch::Approx(50.0f).margin(0.5f));
}

TEST_CASE("calibration: a sweep outside CALIBRATE changes nothing", "[belt][tune]") {
    SECTION("spring, at BASELINE") {
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        const auto window = s.zone()->window;
        const auto r = s.add_calibration_sweep(peak_response(104.0f), peak_response(103.0f));
        CHECK_FALSE(r.registered);
        CHECK(r.turns == 0.0f);
        CHECK(s.stage() == BeltTuneStage::BASELINE);
        CHECK(s.zone()->window.freq_start_hz == window.freq_start_hz);
        CHECK(s.zone()->window.freq_end_hz == window.freq_end_hz);
    }
    SECTION("screw, already matched") {
        auto s = session_for(100.5f, 100.0f, BeltTensionerKind::SCREW);
        REQUIRE(s.stage() == BeltTuneStage::ALREADY_MATCHED);
        const auto r = s.add_calibration_sweep(peak_response(100.5f), peak_response(104.0f));
        CHECK_FALSE(r.registered);
        CHECK(s.stage() == BeltTuneStage::ALREADY_MATCHED);
        CHECK_FALSE(s.zone().has_value());
        CHECK(s.total_turns(BeltPath::PATH_A) == 0.0f);
        CHECK(s.total_turns(BeltPath::PATH_B) == 0.0f);
    }
    SECTION("screw, tuning") {
        auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
        const float turns = s.total_turns(BeltPath::PATH_B);
        CHECK_FALSE(
            s.add_calibration_sweep(peak_response(104.0f), peak_response(103.0f)).registered);
        CHECK(s.stage() == BeltTuneStage::TUNE);
        CHECK(s.total_turns(BeltPath::PATH_B) == turns);
        CHECK(s.hz_per_turn() == Catch::Approx(2.0f).margin(0.3f));
    }
}

// ============================================================================
// Baseline pair and repeatability
// ============================================================================

TEST_CASE("baseline: a zone is measured twice before any verdict", "[belt][tune]") {
    auto s = session_for(104.0f, 98.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f)).registered);
    const auto rate = s.hz_per_turn();
    REQUIRE(rate.has_value());

    const auto& first = sweep(s, 104.0f, 100.0f);
    CHECK(first.outcome == BeltTuneOutcome::BASELINE);
    CHECK_FALSE(first.no_turn);
    CHECK(s.stage() == BeltTuneStage::BASELINE);
    const auto& second = sweep(s, 104.0f, 100.0f);
    CHECK(second.outcome == BeltTuneOutcome::BASELINE);
    CHECK(s.stage() == BeltTuneStage::TUNE);
    CHECK_FALSE(s.needs_baseline());
    CHECK_FALSE(s.previous_gap_hz().has_value());
    CHECK(s.hz_per_turn() == rate);
    CHECK(s.gap_hz() == Catch::Approx(-4.0f).margin(0.5f));

    SECTION("a pair within tolerance is MATCHED") {
        auto m = session_for(104.0f, 98.0f, BeltTensionerKind::SCREW);
        REQUIRE(m.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f)).registered);
        sweep(m, 104.0f, 103.6f);
        CHECK(sweep(m, 104.0f, 103.6f).outcome == BeltTuneOutcome::MATCHED);
        CHECK(m.stage() == BeltTuneStage::TUNE);
        CHECK(m.zone_matched());
    }
}

TEST_CASE("repeatability: the baseline pair's gap and power must agree", "[belt][tune]") {
    // Spring: the zone is entered at BASELINE with B adjusted.
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    const auto w = *s.sweep_window();
    const auto pair = [&](float b1, float b2, float a_scale2, float b_scale2) {
        REQUIRE(s.add_round(triangle(104.0f, 1.0f, w), triangle(b1, 1.0f, w)).outcome ==
                BeltTuneOutcome::BASELINE);
        REQUIRE(s.stage() == BeltTuneStage::BASELINE);
        return s.add_round(triangle(104.0f, a_scale2, w), triangle(b2, b_scale2, w)).outcome;
    };

    SECTION("a gap that moved exactly 1.5 Hz repeats") {
        CHECK(pair(100.0f, 101.5f, 1.0f, 1.0f) == BeltTuneOutcome::BASELINE);
        CHECK(s.stage() == BeltTuneStage::TUNE);
        CHECK(s.gap_hz() == Catch::Approx(-2.5f));
    }
    SECTION("a gap that moved 2 Hz does not") {
        CHECK(pair(100.0f, 102.0f, 1.0f, 1.0f) == BeltTuneOutcome::UNREPEATABLE);
        CHECK(s.stage() == BeltTuneStage::UNREPEATABLE);
    }
    SECTION("power exactly 2x on either path repeats") {
        CHECK(pair(100.0f, 100.0f, 2.0f, 0.5f) == BeltTuneOutcome::BASELINE);
        CHECK(s.stage() == BeltTuneStage::TUNE);
    }
    SECTION("power 2.5x on the reference path does not") {
        CHECK(pair(100.0f, 100.0f, 2.5f, 1.0f) == BeltTuneOutcome::UNREPEATABLE);
    }
    SECTION("power 2.5x down on the adjusted path does not") {
        CHECK(pair(100.0f, 100.0f, 1.0f, 0.4f) == BeltTuneOutcome::UNREPEATABLE);
    }
}

TEST_CASE("repeatability: a pair round with no peak discards the pair", "[belt][tune]") {
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
    const auto w = *s.sweep_window();
    REQUIRE(s.add_round(triangle(104.0f, 1.0f, w), triangle(100.0f, 1.0f, w)).outcome ==
            BeltTuneOutcome::BASELINE);
    const auto flat = response_from(flat_curve());
    REQUIRE(s.add_round(flat, flat).outcome == BeltTuneOutcome::NO_PEAK);
    CHECK(s.stage() == BeltTuneStage::BASELINE);
    // A fresh pair: this round is its first, never judged against the reading
    // before the no-peak round (a 2.5 Hz gap move).
    CHECK(s.add_round(triangle(104.0f, 1.0f, w), triangle(102.5f, 1.0f, w)).outcome ==
          BeltTuneOutcome::BASELINE);
    CHECK(s.stage() == BeltTuneStage::BASELINE);
    CHECK(s.add_round(triangle(104.0f, 1.0f, w), triangle(102.5f, 1.0f, w)).outcome ==
          BeltTuneOutcome::BASELINE);
    CHECK(s.stage() == BeltTuneStage::TUNE);
}

TEST_CASE("rounds: only tuning rounds are counted", "[belt][tune]") {
    auto s = tuning(112.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    CHECK(s.total_rounds() == 0);
    s.record_turns(1.0f);
    sweep(s, 112.0f, 103.0f);
    CHECK(s.total_rounds() == 1);
}

TEST_CASE("repeatability: no advice until a fresh pair agrees", "[belt][tune]") {
    auto s = session_for(104.0f, 98.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f)).registered);
    REQUIRE(sweep(s, 104.0f, 100.0f).outcome == BeltTuneOutcome::BASELINE);
    REQUIRE(sweep(s, 104.0f, 103.0f).outcome == BeltTuneOutcome::UNREPEATABLE);
    REQUIRE(s.stage() == BeltTuneStage::UNREPEATABLE);
    CHECK_FALSE(s.advice().turns.has_value());
    CHECK_FALSE(s.advice().spring_reseat);
    CHECK_FALSE(s.needs_baseline());

    // Re-measure: the next round opens a fresh pair.
    CHECK(sweep(s, 104.0f, 101.0f).outcome == BeltTuneOutcome::BASELINE);
    CHECK(s.stage() == BeltTuneStage::BASELINE);
    CHECK(sweep(s, 104.0f, 101.0f).outcome == BeltTuneOutcome::BASELINE);
    CHECK(s.stage() == BeltTuneStage::TUNE);
    CHECK(s.advice().turns.has_value());

    SECTION("spring: no re-seat while unrepeatable") {
        auto sp = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        sweep(sp, 104.0f, 100.0f);
        REQUIRE(sweep(sp, 104.0f, 103.0f).outcome == BeltTuneOutcome::UNREPEATABLE);
        CHECK_FALSE(sp.advice().spring_reseat);
    }
}

namespace {

/// The Voron's 118.6-135 Hz zone, registered from the 10-15 captures by two
/// calibration turns of B.
BeltTuneSession voron_zone() {
    auto s = session_from(load_fixture_response("voron24_settled", 'A'),
                          load_fixture_response("voron24_settled", 'B'), BeltTensionerKind::SCREW,
                          5.0f, 135.0f);
    s.add_calibration_sweep(load_fixture_response("voron24_turn1", 'A'),
                            load_fixture_response("voron24_turn1", 'B'));
    REQUIRE(s.add_calibration_sweep(load_fixture_response("voron24_turn2", 'A'),
                                    load_fixture_response("voron24_turn2", 'B'))
                .registered);
    return s;
}

const BeltTuneRound& add_capture(BeltTuneSession& s, const std::string& name) {
    return s.add_round(load_fixture_response(name, 'A'), load_fixture_response(name, 'B'));
}

/// The capture with every bin outside `w` removed: a sweep that read nothing else.
BeltResponse trimmed(const BeltResponse& r, BeltSweepWindow w) {
    return BeltResponse{curve_in_band(r.xyz, w.freq_start_hz, w.freq_end_hz),
                        curve_in_band(r.in_plane, w.freq_start_hz, w.freq_end_hz)};
}

} // namespace

TEST_CASE("repeatability: the Voron's 10-15 narrow no-turn repeats are unrepeatable",
          "[belt][tune]") {
    for (const char* set : {"voron24_narrow_tuned", "voron24_narrow_nolaps"}) {
        INFO(set);
        auto s = voron_zone();
        const auto& first = add_capture(s, std::string(set) + "_r1");
        INFO("first gap " << first.gap_hz);
        REQUIRE(first.outcome == BeltTuneOutcome::BASELINE);
        const auto& second = add_capture(s, std::string(set) + "_r2");
        INFO("second gap " << second.gap_hz);
        CHECK(second.outcome == BeltTuneOutcome::UNREPEATABLE);
        CHECK(s.stage() == BeltTuneStage::UNREPEATABLE);
    }
}

TEST_CASE("repeatability: the K1C's settled pair repeats", "[belt][tune]") {
    auto s = session_from(load_fixture_response("k1c_settled", 'A'),
                          load_fixture_response("k1c_settled", 'B'), BeltTensionerKind::SPRING,
                          5.0f, 133.0f);
    s.calibrate_anyway();
    REQUIRE(s.stage() == BeltTuneStage::BASELINE);
    const auto w = *s.sweep_window();
    CHECK(s.add_round(trimmed(load_fixture_response("k1c_settled", 'A'), w),
                      trimmed(load_fixture_response("k1c_settled", 'B'), w))
              .outcome == BeltTuneOutcome::BASELINE);
    const auto& second = s.add_round(trimmed(load_fixture_response("k1c_settled_repeat", 'A'), w),
                                     trimmed(load_fixture_response("k1c_settled_repeat", 'B'), w));
    CHECK(second.outcome == BeltTuneOutcome::MATCHED);
    CHECK(s.stage() == BeltTuneStage::TUNE);
}

TEST_CASE("no-turn rounds on real data: a repeat narrow sweep is never a turn verdict",
          "[belt][tune]") {
    // The 10-14 narrow captures over the 118.6-135 zone: gap (B - A) +0.05,
    // then -1.22 with nothing turned.
    auto s = voron_zone();
    CHECK(add_capture(s, "voron24_narrow_r1").outcome == BeltTuneOutcome::BASELINE);
    add_capture(s, "voron24_narrow_r1");
    REQUIRE(s.stage() == BeltTuneStage::TUNE);
    CHECK(s.gap_hz() == Catch::Approx(0.05f).margin(0.05f));
    const auto& r = add_capture(s, "voron24_narrow_r2");
    CHECK(r.no_turn);
    CHECK(r.gap_hz == Catch::Approx(-1.22f).margin(0.05f));
    // The Voron's 1.5 Hz bins put -1.22 within tolerance: never WRONG_WAY or OVERSHOT.
    CHECK(r.outcome == BeltTuneOutcome::MATCHED);
    CHECK(s.hz_per_turn() == Catch::Approx(1.715f).margin(0.03f));
}

// ============================================================================
// Range, zone bins, OUT_OF_WINDOW
// ============================================================================

TEST_CASE("range: a widened zone at the top of a K1C band still sweeps inside it", "[belt][tune]") {
    constexpr float kK1cMax = 133.33f;
    auto s = session_from(peak_response(126.0f), peak_response(114.0f), BeltTensionerKind::SPRING,
                          kLo, kK1cMax);
    REQUIRE(s.zone().has_value());
    REQUIRE(s.zone()->source == BeltZoneSource::TALLEST_UNPAIRED);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.sweep_window()->freq_end_hz == 133.0f);
    const float end_before = s.zone()->window.freq_end_hz;

    // B ran past the zone's top edge.
    const auto& r = sweep(s, 126.0f, 133.0f);
    REQUIRE(r.outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    CHECK(s.zone()->window.freq_end_hz > end_before);
    CHECK(s.zone()->window.freq_end_hz <= kK1cMax);
    CHECK(s.sweep_window()->freq_end_hz == 133.0f);
}

TEST_CASE("zone bins only: margin spikes never move the gap; an edge spike moves the zone",
          "[belt][tune]") {
    const auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    const auto zone = s.zone()->window;
    const auto sw = *s.sweep_window();
    REQUIRE(sw.freq_start_hz < zone.freq_start_hz);
    REQUIRE(sw.freq_end_hz > zone.freq_end_hz);
    const auto spike = [](BeltCurve c, float hz, float value) {
        auto it = std::min_element(c.begin(), c.end(), [hz](const auto& l, const auto& r) {
            return std::abs(l.first - hz) < std::abs(r.first - hz);
        });
        it->second = value;
        return c;
    };
    const float big = 10.0f * 1e4f;
    const float below = (sw.freq_start_hz + zone.freq_start_hz) / 2.0f;
    const float above = (sw.freq_end_hz + zone.freq_end_hz) / 2.0f;

    auto clean = s;
    sweep_curves(clean, one_peak(104.0f), one_peak(101.0f));
    auto spiked = s;
    const auto& r = sweep_curves(spiked, spike(spike(one_peak(104.0f), below, big), above, big),
                                 spike(spike(one_peak(101.0f), below, big), above, big));
    CHECK(r.outcome == BeltTuneOutcome::CLOSER);
    CHECK(spiked.gap_hz() == clean.gap_hz());

    auto edge = s;
    CHECK(sweep_curves(edge, one_peak(104.0f),
                       spike(one_peak(101.0f), zone.freq_end_hz - kBinHz / 2.0f, big))
              .outcome == BeltTuneOutcome::OUT_OF_WINDOW);
}

// ============================================================================
// Outcomes
// ============================================================================

TEST_CASE("tune round outcomes", "[belt][tune]") {
    // A at 104 is the reference; B, adjusted, at 100 after its calibration turn.
    auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    REQUIRE(s.gap_hz() == Catch::Approx(-4.0f).margin(0.5f));
    const float gap0 = s.gap_hz();

    SECTION("CLOSER: the gap shrank, same sign") {
        const auto& r = sweep(s, 104.0f, 102.0f);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        CHECK(r.gap_hz == Catch::Approx(-2.0f).margin(0.5f));
        REQUIRE(s.previous_gap_hz().has_value());
        CHECK(*s.previous_gap_hz() == gap0);
        CHECK(s.adjusted_hz() == Catch::Approx(102.0f).margin(0.5f));
        CHECK(s.reference_hz() == Catch::Approx(104.0f).margin(0.5f));
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
        CHECK_FALSE(s.zone_matched());
    }
    SECTION("OVERSHOT: the gap changed sign and the advice flips") {
        CHECK(sweep(s, 104.0f, 105.5f).outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(s.gap_hz() > 0.0f);
        CHECK(s.advice().direction == BeltTurnDirection::LOOSEN);
    }
    SECTION("WRONG_WAY: the gap moved against the advice") {
        CHECK(sweep(s, 104.0f, 98.0f).outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    }
    SECTION("NO_MOVEMENT: under 0.8 Hz either way") {
        CHECK(sweep(s, 104.0f, 100.7f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 104.0f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }
    SECTION("0.9 Hz is a move") {
        CHECK(sweep(s, 104.0f, 100.9f).outcome == BeltTuneOutcome::CLOSER);
    }
    SECTION("both paths drifting 3 Hz together is not a move") {
        CHECK(sweep(s, 107.0f, 103.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 104.0f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }
    SECTION("the reference moving away is the gap moving the wrong way") {
        CHECK(sweep(s, 106.0f, 100.0f).outcome == BeltTuneOutcome::WRONG_WAY);
    }
    SECTION("MATCHED: the gap is within tolerance") {
        CHECK(sweep(s, 104.0f, 103.5f).outcome == BeltTuneOutcome::MATCHED);
        CHECK(s.zone_matched());
    }
    SECTION("NO_PEAK: a flat adjusted path leaves the gap alone") {
        const auto w = *s.sweep_window();
        const auto& r = s.add_round(windowed(peak_response(104.0f), w, kFloor),
                                    windowed(response_from(flat_curve()), w, 5.0f));
        CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
        CHECK_FALSE(r.reference_lost);
        CHECK(s.gap_hz() == gap0);
        CHECK_FALSE(s.previous_gap_hz().has_value());
    }
    SECTION("NO_PEAK: the reference's peak left the zone") {
        const auto w = *s.sweep_window();
        SECTION("its zone is flat") {
            const auto& r = s.add_round(windowed(response_from(flat_curve()), w, 5.0f),
                                        windowed(peak_response(101.0f), w, kFloor));
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK(r.reference_lost);
        }
        SECTION("it ran off the zone's low edge") {
            const auto& r = sweep(s, s.zone()->window.freq_start_hz - 3.0f, 101.0f);
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK(r.reference_lost);
            CHECK(s.gap_hz() == gap0);
        }
        SECTION("both flat: no path is singled out") {
            const auto flat = windowed(response_from(flat_curve()), w, 5.0f);
            const auto& r = s.add_round(flat, flat);
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK_FALSE(r.reference_lost);
        }
    }
}

TEST_CASE("tune outcomes under LOOSEN advice", "[belt][tune]") {
    // After the swap A, at 104, is adjusted against B at 100: loosen A.
    auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    s.swap_adjusted_path();
    REQUIRE(s.adjusted_path() == BeltPath::PATH_A);
    REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);

    SECTION("CLOSER") {
        CHECK(sweep(s, 102.0f, 100.0f).outcome == BeltTuneOutcome::CLOSER);
    }
    SECTION("OVERSHOT") {
        CHECK(sweep(s, 98.5f, 100.0f).outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    }
    SECTION("WRONG_WAY") {
        CHECK(sweep(s, 106.0f, 100.0f).outcome == BeltTuneOutcome::WRONG_WAY);
    }
    SECTION("NO_MOVEMENT") {
        CHECK(sweep(s, 103.3f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }
}

TEST_CASE("tune OUT_OF_WINDOW: re-centred on the reference, widened, once per zone",
          "[belt][tune]") {
    // Calibration: one turn moved B 4 Hz, so a turn's 11 Hz overshoot is plausible.
    auto s = tuning(104.0f, 96.0f, 100.0f, BeltTensionerKind::SCREW);
    const auto zone = s.zone()->window;
    REQUIRE(zone.freq_end_hz == Catch::Approx(110.0f).margin(0.5f));
    s.record_turns(1.0f);

    SECTION("the next sweep covers the peak and measures the overshoot") {
        const auto& r = sweep(s, 104.0f, 111.0f);
        CHECK(r.outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        CHECK_FALSE(s.previous_gap_hz().has_value());
        const auto w = s.zone()->window;
        CHECK((w.freq_start_hz + w.freq_end_hz) / 2.0f == Catch::Approx(104.0f).margin(0.5f));
        CHECK(w.freq_end_hz > zone.freq_end_hz);
        const auto& next = sweep(s, 104.0f, 111.0f); // stepper left untouched
        CHECK(next.outcome == BeltTuneOutcome::OVERSHOT);
        CHECK_FALSE(next.no_turn);
        CHECK(next.turns_measured == 1.0f);
        CHECK(s.gap_hz() == Catch::Approx(7.0f).margin(1.0f));
    }
    SECTION("a second edge in a row is NO_PEAK and the zone stays") {
        CHECK(sweep(s, 104.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto w = s.zone()->window;
        REQUIRE(w.freq_end_hz < 125.0f);
        CHECK(sweep(s, 104.0f, 125.0f).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.zone()->window.freq_start_hz == w.freq_start_hz);
        CHECK(s.zone()->window.freq_end_hz == w.freq_end_hz);
        // A measured round re-arms it.
        REQUIRE(sweep(s, 104.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
        CHECK(sweep(s, 104.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    }
    SECTION("a gentle slope to the edge is NO_PEAK, a steep one OUT_OF_WINDOW") {
        const auto ramp = [&](float top) {
            BeltCurve c;
            for (float f = zone.freq_start_hz; f <= zone.freq_end_hz; f += kBinHz) {
                const float t = (f - zone.freq_start_hz) / (zone.freq_end_hz - zone.freq_start_hz);
                c.emplace_back(f, 10.0f + (top - 10.0f) * t);
            }
            return response_from(c);
        };
        const auto ref = peak_response(104.0f);
        const BeltResponse ref_zone{
            curve_in_band(ref.xyz, zone.freq_start_hz, zone.freq_end_hz),
            curve_in_band(ref.in_plane, zone.freq_start_hz, zone.freq_end_hz)};
        // Rises to 1.4x its minimum at the edge: under kPeakMinProminence.
        CHECK(s.add_round(ref_zone, ramp(14.0f)).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.zone()->window.freq_end_hz == zone.freq_end_hz);
        CHECK(s.add_round(ref_zone, ramp(20.0f)).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    }
}

// ============================================================================
// IMPLAUSIBLE
// ============================================================================

TEST_CASE("implausible: a gap move past the bound is dropped and its turns carry", "[belt][tune]") {
    // Rate 2 Hz/turn; one turn bounds the move at max(6, 3 x 1 x 2) = 6 Hz.
    auto s = tuning_exact(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.hz_per_turn() == Catch::Approx(2.0f).margin(0.2f));
    REQUIRE(s.gap_hz() == -4.0f);
    const auto prev = s.previous_gap_hz();
    const float ref = s.reference_hz();
    const float adj = s.adjusted_hz();
    const auto rate = s.hz_per_turn();

    s.record_turns(1.0f);
    const auto& r = sweep_exact(s, 104.0f, 107.0f); // the gap moved 7 Hz
    CHECK(r.outcome == BeltTuneOutcome::IMPLAUSIBLE);
    CHECK(r.turns_measured == 0.0f);
    CHECK(s.gap_hz() == -4.0f);
    CHECK(s.previous_gap_hz() == prev);
    CHECK(s.reference_hz() == ref);
    CHECK(s.adjusted_hz() == adj);
    CHECK(s.hz_per_turn() == rate);
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(2.0f));

    // One more turn, and the gap closed 2 Hz: both turns are behind it.
    s.record_turns(1.0f);
    const auto& next = sweep_exact(s, 104.0f, 102.0f);
    CHECK(next.outcome == BeltTuneOutcome::CLOSER);
    CHECK(next.turns_measured == 2.0f);
    REQUIRE(s.previous_gap_hz().has_value());
    CHECK(*s.previous_gap_hz() == -4.0f);
    // The new sample is 2 Hz / 2 turns = 1; the median with the calibration's 2.
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx((1.0f + *rate) / 2.0f));
}

TEST_CASE("implausible: without a rate the bound is 6 Hz", "[belt][tune]") {
    auto s = tuning_exact(104.0f, 98.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    REQUIRE_FALSE(s.hz_per_turn().has_value());
    SECTION("6.5 Hz is a fault") {
        CHECK(sweep_exact(s, 104.0f, 106.5f).outcome == BeltTuneOutcome::IMPLAUSIBLE);
    }
    SECTION("exactly 6 Hz is a reading") {
        CHECK(sweep_exact(s, 104.0f, 106.0f).outcome == BeltTuneOutcome::OVERSHOT);
    }
    SECTION("5.5 Hz is a reading") {
        CHECK(sweep_exact(s, 104.0f, 105.5f).outcome == BeltTuneOutcome::OVERSHOT);
    }
    SECTION("two recorded turns without a rate allow 12 Hz") {
        auto t = tuning_exact(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
        t.swap_adjusted_path();
        REQUIRE_FALSE(t.hz_per_turn().has_value());
        REQUIRE(t.gap_hz() == 4.0f);
        // A loosened past B: the gap moved -7 Hz for two turns.
        t.record_turns(2.0f);
        CHECK(sweep_exact(t, 97.0f, 100.0f).outcome == BeltTuneOutcome::OVERSHOT);
        auto u = tuning_exact(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
        u.swap_adjusted_path();
        u.record_turns(1.0f);
        CHECK(sweep_exact(u, 97.0f, 100.0f).outcome == BeltTuneOutcome::IMPLAUSIBLE);
    }
}

// ============================================================================
// Turn sizes and the rate
// ============================================================================

TEST_CASE("tune advice: screw turns from the calibration's rate, half a turn up to two",
          "[belt][tune]") {
    SECTION("rate 2, gap 2: one turn") {
        auto s = tuning_exact(104.0f, 100.0f, 102.0f, BeltTensionerKind::SCREW);
        CHECK(s.advice().turns == std::optional<float>(1.0f));
    }
    SECTION("rate 4, gap 1.25, under 1.5x tolerance: a quarter") {
        auto s = tuning_exact(104.0f, 98.75f, 102.75f, BeltTensionerKind::SCREW);
        REQUIRE(s.tolerance_hz() == Catch::Approx(1.0f));
        CHECK(s.advice().turns == std::optional<float>(0.25f));
    }
    SECTION("rate 8, gap 1.75: a quarter is clamped up to a half") {
        auto s = tuning_exact(104.0f, 94.25f, 102.25f, BeltTensionerKind::SCREW);
        CHECK(s.advice().turns == std::optional<float>(0.5f));
    }
    SECTION("rate 8, gap exactly 1.5x tolerance: half a turn") {
        auto s = tuning_exact(104.0f, 94.5f, 102.5f, BeltTensionerKind::SCREW);
        CHECK(s.advice().turns == std::optional<float>(0.5f));
    }
    SECTION("rate 2, gap 10: clamped to two") {
        auto s = tuning_exact(112.0f, 100.0f, 102.0f, BeltTensionerKind::SCREW);
        CHECK(s.advice().turns == std::optional<float>(2.0f));
    }
    SECTION("a swap clears the rate: one turn first") {
        auto s = tuning(104.0f, 100.0f, 102.0f, BeltTensionerKind::SCREW);
        s.swap_adjusted_path();
        CHECK_FALSE(s.hz_per_turn().has_value());
        CHECK(s.advice().turns == std::optional<float>(belt_tune::kFirstTurn));
        SECTION("a turn that moved nothing doubles") {
            s.record_turns(1.0f);
            REQUIRE(sweep(s, 104.0f, 102.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
            CHECK(s.advice().turns == std::optional<float>(2.0f));
        }
    }
    SECTION("unknown and spring tensioners never carry turns") {
        for (auto kind : {BeltTensionerKind::UNKNOWN, BeltTensionerKind::SPRING}) {
            auto s = session_for(104.0f, 98.0f, kind);
            if (kind == BeltTensionerKind::UNKNOWN) {
                REQUIRE(s.add_calibration_sweep(peak_response(104.0f), peak_response(100.0f))
                            .registered);
            }
            baseline(s, 104.0f, 100.0f);
            CHECK_FALSE(s.advice().turns.has_value());
            s.record_turns(0.5f);
            const auto& r = sweep(s, 104.0f, 102.0f);
            CHECK_FALSE(r.turns_made.has_value());
            CHECK_FALSE(s.hz_per_turn().has_value());
            CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
            CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
            CHECK(s.advice().spring_reseat == (kind == BeltTensionerKind::SPRING));
        }
    }
}

TEST_CASE("tune rate: the calibration sample is the first, then the median of real moves",
          "[belt][tune]") {
    // Calibration: one turn, 2 Hz.
    auto s = tuning(112.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    // One turn for 3 Hz.
    s.record_turns(1.0f);
    CHECK(sweep(s, 112.0f, 103.0f).turns_made == std::optional<float>(1.0f));
    // One turn barely moved it: excluded from the rate.
    s.record_turns(1.0f);
    const auto& still = sweep(s, 112.0f, 103.5f);
    CHECK(still.outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK(still.turns_made == std::optional<float>(1.0f));
    // Half a turn for 2 Hz: 4 Hz/turn.
    s.record_turns(0.5f);
    sweep(s, 112.0f, 105.5f);
    // Median of 2, 3 and 4; counting the 0.5 Hz/turn round would give 2.5.
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(3.0f).margin(0.3f));
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(3.5f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
    // Tuning rounds only: the baseline pair is not counted.
    CHECK(s.total_rounds() == 3);
    CHECK(s.rounds().size() == 5);
}

TEST_CASE("tune rate: common drift and a wrong-way round teach nothing", "[belt][tune]") {
    auto s = tuning(112.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    s.record_turns(1.0f);
    SECTION("both paths rose 2 Hz: the gap held") {
        const auto& r = sweep(s, 114.0f, 102.0f);
        REQUIRE(r.turns_made == std::optional<float>(1.0f));
        CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(s.hz_per_turn() == Catch::Approx(2.0f).margin(0.2f));
        // A turn that closed the gap 4 Hz while both drifted 1 Hz: 4 Hz/turn.
        s.record_turns(1.0f);
        REQUIRE(sweep(s, 115.0f, 107.0f).outcome == BeltTuneOutcome::CLOSER);
    }
    SECTION("the gap widened") {
        REQUIRE(sweep(s, 112.0f, 98.0f).outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK(s.hz_per_turn() == Catch::Approx(2.0f).margin(0.2f));
        // The next real move: one turn for 4 Hz.
        s.record_turns(1.0f);
        REQUIRE(sweep(s, 112.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
    }
    CHECK(s.hz_per_turn() == Catch::Approx(3.0f).margin(0.3f));
}

TEST_CASE("tune session: turns are totalled per belt, calibration included, across a swap",
          "[belt][tune]") {
    auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.0f));
    s.record_turns(0.5f);
    sweep(s, 104.0f, 101.0f);
    s.swap_adjusted_path();
    REQUIRE(s.adjusted_path() == BeltPath::PATH_A);
    REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);
    const auto window = s.zone()->window;
    s.record_turns(0.25f);
    sweep(s, 103.0f, 101.0f);
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.5f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(-0.25f));
    CHECK(s.zone()->window.freq_start_hz == window.freq_start_hz);
    CHECK(s.zone()->window.freq_end_hz == window.freq_end_hz);
}

TEST_CASE("turn counts read as quarter-fraction glyphs", "[belt][tune]") {
    using helix::calibration::format_belt_turns;
    CHECK(format_belt_turns(0.25f) == "¼");
    CHECK(format_belt_turns(0.5f) == "½");
    CHECK(format_belt_turns(0.75f) == "¾");
    CHECK(format_belt_turns(1.0f) == "1");
    CHECK(format_belt_turns(1.25f) == "1¼");
    CHECK(format_belt_turns(1.75f) == "1¾");
    CHECK(format_belt_turns(2.0f) == "2");
    // Loosening totals are negative; the words carry the direction.
    CHECK(format_belt_turns(-0.75f) == "¾");
    // Rounds to the nearest quarter.
    CHECK(format_belt_turns(0.125f) == "¼");
    CHECK(format_belt_turns(0.375f) == "½");
    CHECK(format_belt_turns(0.36f) == "¼");
    CHECK(format_belt_turns(0.1f) == "0");
}

// ============================================================================
// No-turn rounds
// ============================================================================

TEST_CASE("tune session: an unrecorded stepper is a round with no turn", "[belt][tune]") {
    auto s = tuning(112.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    const auto rate = s.hz_per_turn();
    REQUIRE(rate.has_value());

    SECTION("a small move the wrong way is no movement, not a wrong way") {
        const float before = s.gap_hz();
        const auto& r = sweep(s, 112.0f, 99.1f);
        CHECK(r.no_turn);
        CHECK_FALSE(r.turns_made.has_value());
        CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        // The reading is adopted.
        CHECK(s.gap_hz() < before - 0.5f);
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.0f));
    }
    SECTION("a large move either way is still no movement and teaches no rate") {
        CHECK(sweep(s, 112.0f, 98.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 112.0f, 102.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(s.hz_per_turn() == rate);
    }
    SECTION("a match is still a match") {
        auto near = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
        const auto& r = sweep(near, 104.0f, 103.5f);
        CHECK(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::MATCHED);
    }
    SECTION("a recorded half turn is booked") {
        s.record_turns(0.5f);
        const auto& r = sweep(s, 112.0f, 102.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.turns_made == std::optional<float>(0.5f));
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.5f));
    }
    SECTION("a recorded zero is no turn") {
        s.record_turns(0.0f);
        const auto& r = sweep(s, 112.0f, 100.1f);
        CHECK(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK_FALSE(r.turns_made.has_value());
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.0f));
    }
}

TEST_CASE("tune session: a turn carried past a peakless round is still a turn", "[belt][tune]") {
    // Rate 4 Hz/turn, gap -4, zone 90-110.
    auto s = tuning(104.0f, 96.0f, 100.0f, BeltTensionerKind::SCREW);
    s.record_turns(1.0f);

    SECTION("OUT_OF_WINDOW, then an untouched round that closed the gap") {
        REQUIRE(sweep(s, 104.0f, 111.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto& r = sweep(s, 104.0f, 102.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        // The turn moved the gap 2 Hz: the median of 4 and 2.
        CHECK(s.hz_per_turn() == Catch::Approx(3.0f).margin(0.3f));
    }
    SECTION("OUT_OF_WINDOW, then an untouched round that widened the gap") {
        REQUIRE(sweep(s, 104.0f, 111.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto& r = sweep(s, 104.0f, 98.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK(s.hz_per_turn() == Catch::Approx(4.0f).margin(0.3f));
    }
    SECTION("NO_PEAK, then an untouched round that closed the gap") {
        const auto w = *s.sweep_window();
        REQUIRE(s.add_round(windowed(peak_response(104.0f), w, kFloor),
                            windowed(response_from(flat_curve()), w, 5.0f))
                    .outcome == BeltTuneOutcome::NO_PEAK);
        const auto& r = sweep(s, 104.0f, 102.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        CHECK(r.turns_measured == 1.0f);
    }
    SECTION("the round after a measured one is a repeat check again") {
        REQUIRE(sweep(s, 104.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
        CHECK(sweep(s, 104.0f, 102.1f).no_turn);
    }
}

TEST_CASE("tune advice: the doubled turn survives a repeat check and a peakless round",
          "[belt][tune]") {
    auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
    s.swap_adjusted_path();
    s.record_turns(1.0f);
    SECTION("a repeat check in between changes nothing") {
        REQUIRE(sweep(s, 104.0f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        REQUIRE(s.advice().turns == std::optional<float>(2.0f));
        REQUIRE(sweep(s, 104.0f, 100.0f).no_turn);
    }
    SECTION("a turn carried past a peakless round still doubles") {
        const auto w = *s.sweep_window();
        REQUIRE(s.add_round(windowed(response_from(flat_curve()), w, 5.0f),
                            windowed(peak_response(100.0f), w, kFloor))
                    .outcome == BeltTuneOutcome::NO_PEAK);
        // The stepper is left untouched; the turn moved nothing.
        const auto& r = sweep(s, 104.0f, 100.0f);
        REQUIRE_FALSE(r.no_turn);
        REQUIRE(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(r.turns_measured == Catch::Approx(1.0f));
    }
    CHECK(s.advice().turns == std::optional<float>(2.0f));
}

// ============================================================================
// Hints and zones
// ============================================================================

TEST_CASE("tune hints: unpaired zones and stalled rounds", "[belt][tune]") {
    SECTION("an unpaired tallest peak shows the mechanical causes at once") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        REQUIRE(s.candidates().front().zone.source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(s.show_mechanical_hint());
    }
    SECTION("screw and unknown stall after eight turn rounds") {
        for (auto kind : {BeltTensionerKind::SCREW, BeltTensionerKind::UNKNOWN}) {
            auto s = tuning(104.0f, 98.0f, 100.0f, kind);
            for (int i = 0; i < 7; ++i) {
                s.record_turns(1.0f);
                REQUIRE(sweep(s, 104.0f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
            }
            CHECK_FALSE(s.show_mechanical_hint());
            s.record_turns(1.0f);
            sweep(s, 104.0f, 100.0f);
            CHECK(s.show_mechanical_hint());
        }
    }
    SECTION("no-turn rounds never stall") {
        auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::SCREW);
        for (int i = 0; i < belt_tune::kStallRounds; ++i) {
            REQUIRE(sweep(s, 104.0f, 100.0f).no_turn);
        }
        CHECK_FALSE(s.show_mechanical_hint());
        s.record_turns(1.0f);
        CHECK_FALSE(sweep(s, 104.0f, 100.0f).no_turn);
    }
    SECTION("implausible rounds never stall") {
        auto s = tuning(104.0f, 98.0f, 100.0f, BeltTensionerKind::UNKNOWN);
        for (int i = 0; i < belt_tune::kStallRounds; ++i) {
            REQUIRE(sweep(s, 104.0f, 107.0f).outcome == BeltTuneOutcome::IMPLAUSIBLE);
        }
        CHECK_FALSE(s.show_mechanical_hint());
    }
    SECTION("spring stalls after two rounds without improvement") {
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        baseline(s, 104.0f, 100.0f);
        CHECK_FALSE(s.show_mechanical_hint());
        sweep(s, 104.0f, 100.0f);
        CHECK_FALSE(s.show_mechanical_hint());
        sweep(s, 104.0f, 100.0f);
        CHECK(s.show_mechanical_hint());
    }
    SECTION("spring rounds that improve do not stall") {
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        baseline(s, 104.0f, 100.0f);
        REQUIRE(sweep(s, 104.0f, 101.0f).outcome == BeltTuneOutcome::CLOSER);
        REQUIRE(sweep(s, 104.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
        CHECK_FALSE(s.show_mechanical_hint());
        REQUIRE(sweep(s, 104.0f, 102.1f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK_FALSE(s.show_mechanical_hint());
        REQUIRE(sweep(s, 104.0f, 102.2f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(s.show_mechanical_hint());
    }
}

namespace {

/// Two zones where B is looser, 47/50 and 100/104; the calibration turn
/// moved B 2 Hz in each, the 100 Hz zone first.
BeltTuneSession two_zone_session(BeltTensionerKind kind) {
    const auto a = response_from(
        lorentzian_curve({{50.0f, 1e4f}, {104.0f, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    const auto b = [](float low, float high) {
        return response_from(
            lorentzian_curve({{low, 1e4f}, {high, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    };
    auto s = session_from(a, b(46.0f, 98.0f), kind);
    REQUIRE(s.add_calibration_sweep(a, b(48.0f, 101.0f)).registered);
    REQUIRE(s.zone()->ref_hz == Catch::Approx(104.0f).margin(0.5f));
    return s;
}

} // namespace

TEST_CASE("tune zones: a later zone starts from its own baseline", "[belt][tune]") {
    const auto a = lorentzian_curve({{50.0f, 1e4f}, {104.0f, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f);
    const auto b = [](float low, float high) {
        return lorentzian_curve({{low, 1e4f}, {high, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f);
    };
    auto s = two_zone_session(BeltTensionerKind::SCREW);
    sweep_curves(s, a, b(48.0f, 101.0f));
    sweep_curves(s, a, b(48.0f, 101.0f));
    s.record_turns(1.0f);
    REQUIRE(sweep_curves(s, a, b(49.0f, 103.5f)).outcome == BeltTuneOutcome::MATCHED);
    const auto rate = s.hz_per_turn();
    REQUIRE(rate.has_value());

    REQUIRE(s.next_zone());
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.needs_baseline());
    CHECK(s.rounds().empty());
    CHECK_FALSE(s.zone_matched());
    CHECK(s.total_rounds() == 1);
    // The first zone's turn already moved B's 48 Hz mode to 49: a baseline,
    // not a move for the suggested turn.
    const auto& base = sweep_curves(s, a, b(49.0f, 103.5f));
    CHECK(base.outcome == BeltTuneOutcome::BASELINE);
    CHECK_FALSE(base.turns_made.has_value());
    sweep_curves(s, a, b(49.0f, 103.5f));
    CHECK_FALSE(s.needs_baseline());
    CHECK_FALSE(s.previous_gap_hz().has_value());
    CHECK(s.adjusted_hz() == Catch::Approx(49.0f).margin(0.5f));
    CHECK(s.hz_per_turn() == rate);
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(2.0f));
    CHECK_FALSE(s.has_next_zone());
}

TEST_CASE("tune zones: a swap sticks across zones", "[belt][tune]") {
    auto s = two_zone_session(BeltTensionerKind::UNKNOWN);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    s.swap_adjusted_path();
    sweep(s, 104.0f, 104.0f);
    sweep(s, 104.0f, 104.0f);
    REQUIRE(s.zone_matched());
    REQUIRE(s.next_zone());
    CHECK(s.adjusted_path() == BeltPath::PATH_A);
    CHECK(s.zone()->adj_hz == Catch::Approx(50.0f).margin(0.5f));

    auto t = two_zone_session(BeltTensionerKind::UNKNOWN);
    sweep(t, 104.0f, 104.0f);
    sweep(t, 104.0f, 104.0f);
    REQUIRE(t.next_zone());
    CHECK(t.adjusted_path() == BeltPath::PATH_B);
}

TEST_CASE("tune zones: spring walks every candidate", "[belt][tune]") {
    const auto a = response_from(
        lorentzian_curve({{50.0f, 1e4f}, {104.0f, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    const auto b = response_from(
        lorentzian_curve({{46.0f, 1e4f}, {100.0f, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f));
    auto s = session_from(a, b, BeltTensionerKind::SPRING);
    REQUIRE(s.candidates().size() == 2);
    REQUIRE(s.has_next_zone());
    REQUIRE(s.next_zone());
    CHECK(s.needs_baseline());
    CHECK_FALSE(s.next_zone());
}

// ============================================================================
// In-plane zone candidates, centroids, power and the sweep window
// ============================================================================

namespace {

/// The frequency of the tallest bin of `curve` inside the zone (inclusive).
float tallest_bin_hz(const BeltCurve& curve, BeltSweepWindow zone) {
    const auto x = curve_in_band(curve, zone.freq_start_hz, zone.freq_end_hz);
    REQUIRE_FALSE(x.empty());
    return std::max_element(x.begin(), x.end(),
                            [](const auto& l, const auto& r) { return l.second < r.second; })
        ->first;
}

/// The curve with the bin nearest `hz` raised to `value`.
BeltCurve with_spike(BeltCurve curve, float hz, float value) {
    auto it = std::min_element(curve.begin(), curve.end(), [hz](const auto& l, const auto& r) {
        return std::abs(l.first - hz) < std::abs(r.first - hz);
    });
    it->second = value;
    return curve;
}

/// Each path's peaks, the same on both, as a response with 0.9 in-plane share.
BeltResponse peaks_response(std::vector<std::pair<float, float>> peaks) {
    return response_from(lorentzian_curve(std::move(peaks), kLo, kHi, kBinHz, 0.01f, 1.0f));
}

} // namespace

TEST_CASE("zone candidates: the Voron's settled capture has two, strongest first", "[belt][tune]") {
    const auto a = load_fixture_response("voron24_settled", 'A');
    const auto b = load_fixture_response("voron24_settled", 'B');
    const auto c = tune_zone_candidates(a, b, 5.0f, 135.0f);
    REQUIRE(c.size() == 2);
    CHECK(c[0].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[0].zone.adj_hz == Catch::Approx(128.6f).margin(0.1f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(128.6f).margin(0.1f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(118.6f).margin(0.1f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(135.0f).margin(0.1f));
    CHECK(c[1].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[1].zone.adj_hz == Catch::Approx(37.8f).margin(0.1f));
    CHECK(c[1].zone.ref_hz == Catch::Approx(37.8f).margin(0.1f));
    CHECK(c[1].zone.window.freq_start_hz == Catch::Approx(27.8f).margin(0.1f));
    CHECK(c[1].zone.window.freq_end_hz == Catch::Approx(47.8f).margin(0.1f));
    CHECK(c[0].strength > c[1].strength);
}

TEST_CASE("zone candidates: the K1C's settled capture has one", "[belt][tune]") {
    const auto a = load_fixture_response("k1c_settled", 'A');
    const auto b = load_fixture_response("k1c_settled", 'B');
    const auto c = tune_zone_candidates(a, b, 5.0f, 133.0f);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.adj_hz == Catch::Approx(58.4f).margin(0.1f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(58.4f).margin(0.1f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(48.4f).margin(0.1f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(68.4f).margin(0.1f));
}

TEST_CASE("zone centroid: the in-plane centroids of the hardware captures", "[belt][tune]") {
    struct Row {
        const char* name;
        BeltSweepWindow zone;
        float a_hz;
        float b_hz;
    };
    const Row rows[] = {
        {"voron24_settled", {118.6f, 135.0f}, 129.12f, 127.17f},
        {"voron24_turn1", {118.6f, 135.0f}, 130.04f, 126.92f},
        {"voron24_turn2", {118.6f, 135.0f}, 128.31f, 129.79f},
        {"voron24_settled_repeat", {118.6f, 135.0f}, 130.28f, 126.37f},
        {"k1c_settled", {48.4f, 68.4f}, 59.19f, 59.34f},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto a =
            zone_centroid_hz(tuning_curve(load_fixture_response(row.name, 'A')), row.zone);
        const auto b =
            zone_centroid_hz(tuning_curve(load_fixture_response(row.name, 'B')), row.zone);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(*a == Catch::Approx(row.a_hz).margin(0.05f));
        CHECK(*b == Catch::Approx(row.b_hz).margin(0.05f));
    }
}

TEST_CASE("zone centroid: the zone centroid wanders less than the tallest zone bin",
          "[belt][tune]") {
    const BeltSweepWindow zone{118.6f, 135.0f};
    for (const char path : {'A', 'B'}) {
        INFO("path " << path);
        const auto settled = tuning_curve(load_fixture_response("voron24_settled", path));
        const auto repeat = tuning_curve(load_fixture_response("voron24_settled_repeat", path));
        const float centroid_move =
            std::abs(*zone_centroid_hz(repeat, zone) - *zone_centroid_hz(settled, zone));
        const float tallest_move =
            std::abs(tallest_bin_hz(repeat, zone) - tallest_bin_hz(settled, zone));
        CHECK(centroid_move == Catch::Approx(path == 'A' ? 1.16f : 0.80f).margin(0.05f));
        CHECK(tallest_move == Catch::Approx(1.5f).margin(0.1f));
        CHECK(centroid_move < tallest_move);
    }
}

TEST_CASE("zone centroid: too few zone bins or no power is no centroid", "[belt][tune]") {
    const BeltCurve two_bins = {{100.0f, 1.0f}, {101.0f, 2.0f}, {120.0f, 3.0f}};
    CHECK_FALSE(zone_centroid_hz(two_bins, {99.0f, 110.0f}).has_value());
    const BeltCurve zeros = {{100.0f, 0.0f}, {101.0f, 0.0f}, {102.0f, 0.0f}};
    CHECK_FALSE(zone_centroid_hz(zeros, {99.0f, 110.0f}).has_value());
    const BeltCurve three = {{100.0f, 1.0f}, {101.0f, 2.0f}, {102.0f, 1.0f}};
    REQUIRE(zone_centroid_hz(three, {100.0f, 102.0f}).has_value());
    CHECK(*zone_centroid_hz(three, {100.0f, 102.0f}) == Catch::Approx(101.0f));
}

TEST_CASE("zone power hint: the Voron's B path resonates far harder", "[belt][tune]") {
    const BeltSweepWindow zone{118.6f, 135.0f};
    SECTION("settled") {
        const auto h =
            zone_power_hint(tuning_curve(load_fixture_response("voron24_settled", 'A')),
                            tuning_curve(load_fixture_response("voron24_settled", 'B')), zone);
        REQUIRE(h.has_value());
        CHECK(h->stronger == BeltPath::PATH_B);
        CHECK(h->ratio == Catch::Approx(3.65f).margin(0.05f));
        CHECK(h->freq_hz == Catch::Approx(127.17f).margin(0.05f));
    }
    SECTION("two turns") {
        const auto h =
            zone_power_hint(tuning_curve(load_fixture_response("voron24_turn2", 'A')),
                            tuning_curve(load_fixture_response("voron24_turn2", 'B')), zone);
        REQUIRE(h.has_value());
        CHECK(h->stronger == BeltPath::PATH_B);
        CHECK(h->ratio == Catch::Approx(15.3f).margin(0.3f));
    }
    SECTION("the K1C's paths are within 3x") {
        CHECK_FALSE(zone_power_hint(tuning_curve(load_fixture_response("k1c_settled", 'A')),
                                    tuning_curve(load_fixture_response("k1c_settled", 'B')),
                                    {48.4f, 68.4f})
                        .has_value());
    }
}

TEST_CASE("zone power hint: equal, exactly 3x and zero power give none", "[belt][tune]") {
    const BeltSweepWindow zone{100.0f, 104.0f};
    const auto flat = [](float v) {
        BeltCurve c;
        for (float f = 100.0f; f <= 104.0f; f += 1.0f) {
            c.emplace_back(f, v);
        }
        return c;
    };
    CHECK(zone_power(flat(2.0f), zone) == Catch::Approx(10.0f));
    CHECK_FALSE(zone_power_hint(flat(1.0f), flat(1.0f), zone).has_value());
    CHECK_FALSE(zone_power_hint(flat(1.0f), flat(3.0f), zone).has_value());
    CHECK_FALSE(zone_power_hint(flat(0.0f), flat(3.0f), zone).has_value());
    const auto h = zone_power_hint(flat(4.0f), flat(1.0f), zone);
    REQUIRE(h.has_value());
    CHECK(h->stronger == BeltPath::PATH_A);
    CHECK(h->ratio == Catch::Approx(4.0f));
    CHECK(h->freq_hz == Catch::Approx(102.0f));
}

TEST_CASE("zone candidates: a peak needs half its motion in the XY plane", "[belt][tune]") {
    const auto peak = one_peak(100.0f);
    const auto b = response_from(peak, 0.9f);
    CHECK(tune_zone_candidates(response_from(peak, 0.4f), b, kLo, kHi).empty());
    CHECK(tune_zone_candidates(response_from(peak, 0.5f), b, kLo, kHi).size() == 1);
}

TEST_CASE("in-plane fraction: no per-axis columns reads 1, no xyz power reads 0", "[belt][tune]") {
    CHECK(in_plane_fraction(BeltResponse{one_peak(100.0f), {}}, 100.0f) == 1.0f);
    const BeltCurve zeros = {{99.0f, 0.0f}, {100.0f, 0.0f}, {101.0f, 0.0f}};
    CHECK(in_plane_fraction(BeltResponse{zeros, zeros}, 100.0f) == 0.0f);
    const BeltResponse r{{{99.0f, 4.0f}, {100.0f, 8.0f}}, {{99.0f, 1.0f}, {100.0f, 6.0f}}};
    CHECK(in_plane_fraction(r, 99.9f) == Catch::Approx(0.75f));
    CHECK(&tuning_curve(r) == &r.in_plane);
    const BeltResponse xyz_only{{{99.0f, 4.0f}}, {}};
    CHECK(&tuning_curve(xyz_only) == &xyz_only.xyz);
}

TEST_CASE("zone candidates: without per-axis columns, psd_xyz is tuned", "[belt][tune]") {
    const auto a = load_fixture_response("voron24_settled", 'A');
    const auto b = load_fixture_response("voron24_settled", 'B');
    const auto bare =
        tune_zone_candidates(BeltResponse{a.xyz, {}}, BeltResponse{b.xyz, {}}, kLo, kHi);
    const auto same =
        tune_zone_candidates(BeltResponse{a.xyz, a.xyz}, BeltResponse{b.xyz, b.xyz}, kLo, kHi);
    REQUIRE_FALSE(bare.empty());
    REQUIRE(bare.size() == same.size());
    for (size_t i = 0; i < bare.size(); ++i) {
        CHECK(bare[i].zone.adj_hz == same[i].zone.adj_hz);
        CHECK(bare[i].zone.ref_hz == same[i].zone.ref_hz);
        CHECK(bare[i].zone.window.freq_start_hz == same[i].zone.window.freq_start_hz);
        CHECK(bare[i].zone.window.freq_end_hz == same[i].zone.window.freq_end_hz);
        CHECK(bare[i].strength == same[i].strength);
    }
}

TEST_CASE("zone candidates: a weaker candidate within 10 Hz of a stronger one is dropped",
          "[belt][tune]") {
    SECTION("30 and 37 Hz keep only 37") {
        const auto r = peaks_response({{30.0f, 1e4f}, {37.0f, 1.5e4f}});
        const auto c = tune_zone_candidates(r, r, kLo, kHi);
        REQUIRE(c.size() == 1);
        CHECK(c[0].zone.adj_hz == Catch::Approx(37.0f).margin(0.5f));
    }
    SECTION("30 and 45 Hz keep both") {
        const auto r = peaks_response({{30.0f, 1e4f}, {45.0f, 1.5e4f}});
        const auto c = tune_zone_candidates(r, r, kLo, kHi);
        REQUIRE(c.size() == 2);
        CHECK(c[0].zone.adj_hz == Catch::Approx(45.0f).margin(0.5f));
        CHECK(c[1].zone.adj_hz == Catch::Approx(30.0f).margin(0.5f));
    }
    SECTION("six well-separated pairs leave the strongest four") {
        const auto r = peaks_response({{20.0f, 3e3f},
                                       {40.0f, 4e3f},
                                       {60.0f, 5e3f},
                                       {80.0f, 6e3f},
                                       {100.0f, 7e3f},
                                       {120.0f, 8e3f}});
        const auto c = tune_zone_candidates(r, r, kLo, kHi);
        REQUIRE(c.size() == static_cast<size_t>(belt_tune::kMaxZoneCandidates));
        const float want[] = {120.0f, 100.0f, 80.0f, 60.0f};
        for (size_t i = 0; i < c.size(); ++i) {
            CHECK(c[i].zone.adj_hz == Catch::Approx(want[i]).margin(0.5f));
        }
    }
}

TEST_CASE("zone candidates: a matched pair is a candidate", "[belt][tune]") {
    const auto r = response_from(one_peak(100.0f));
    const auto c = tune_zone_candidates(r, r, kLo, kHi);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[0].zone.adj_hz == c[0].zone.ref_hz);
}

TEST_CASE("zone candidates: an unpaired tallest peak steers against the nearest", "[belt][tune]") {
    const auto c = tune_zone_candidates(response_from(one_peak(110.0f)),
                                        response_from(one_peak(98.0f)), kLo, kHi);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.source == BeltZoneSource::TALLEST_UNPAIRED);
    CHECK(c[0].zone.adj_hz == Catch::Approx(110.0f).margin(0.5f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(98.0f).margin(0.5f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(93.0f).margin(0.5f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(115.0f).margin(0.5f));
}

TEST_CASE("zone sweep window: whole Hz, inside the printer's range", "[belt][tune]") {
    struct Row {
        BeltSweepWindow zone;
        float band_min;
        float band_max;
        float start;
        float end;
    };
    const Row rows[] = {
        {{118.6f, 135.0f}, 5.0f, 135.0f, 109.0f, 135.0f},
        {{20.0f, 40.0f}, 5.0f, 135.0f, 10.0f, 60.0f},
        {{8.0f, 30.0f}, 5.0f, 135.0f, 5.0f, 50.0f},
        {{48.4f, 68.4f}, 5.0f, 133.33f, 39.0f, 88.0f},
        {{100.0f, 120.0f}, 5.0f, 133.33f, 90.0f, 133.0f},
        {{8.0f, 30.0f}, 5.5f, 135.0f, 6.0f, 50.0f},
    };
    for (const auto& row : rows) {
        INFO(row.zone.freq_start_hz << "-" << row.zone.freq_end_hz << " in " << row.band_min << "-"
                                    << row.band_max);
        const auto w = zone_sweep_window(row.zone, row.band_min, row.band_max);
        CHECK(w.freq_start_hz == row.start);
        CHECK(w.freq_end_hz == row.end);
        CHECK(w.freq_start_hz == std::floor(w.freq_start_hz));
        CHECK(w.freq_end_hz == std::floor(w.freq_end_hz));
        CHECK(w.freq_start_hz >= row.band_min);
        CHECK(w.freq_end_hz <= row.band_max);
    }
}

TEST_CASE("zone analysis: the sweep margins never reach a centroid, power or floor",
          "[belt][tune]") {
    const BeltSweepWindow zone{90.0f, 110.0f};
    const auto clean = lorentzian_curve({{100.0f, 1e4f}}, kLo, kHi, kBinHz, 0.01f, 1.0f);
    const float spike = 10.0f * 1e4f;
    for (const float at : {85.0f, 125.0f}) {
        INFO("spike at " << at);
        const auto spiked = with_spike(clean, at, spike);
        REQUIRE(zone_centroid_hz(clean, zone).has_value());
        CHECK(zone_centroid_hz(spiked, zone) == zone_centroid_hz(clean, zone));
        CHECK(zone_power(spiked, zone) == zone_power(clean, zone));
    }

    SECTION("the unexcited floor reads bins outside the swept range") {
        // Margin bins at the floor, everything outside the sweep well above
        // the zone's peak: only the bins outside `swept` say "no peak".
        const BeltSweepWindow swept{80.0f, 130.0f};
        BeltCurve c = lorentzian_curve({{100.0f, 10.0f}}, 70.0f, 140.0f, kBinHz, 0.01f, 1.0f);
        for (auto& [f, v] : c) {
            if (f < swept.freq_start_hz || f > swept.freq_end_hz) {
                v = 10.0f;
            }
        }
        CHECK_FALSE(window_holds_peak(c, zone, swept, 70.0f, 140.0f));
        CHECK(window_holds_peak(c, zone, zone, 70.0f, 140.0f));
    }
}
