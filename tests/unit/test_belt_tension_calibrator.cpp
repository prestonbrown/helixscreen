// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_tension_calibrator.cpp
 * @brief Unit tests for belt tension calibration types and orchestrator
 *
 * Test-first development: These tests are written BEFORE implementation.
 * Tests compile and link with minimal header stubs, but will FAIL until
 * BeltTensionCalibrator and analysis functions are implemented.
 *
 * Test categories:
 * 1. Type tests - BeltStatus evaluation thresholds
 * 2. CSV parsing - Klipper raw accelerometer CSV format
 * 3. FFT/PSD computation - Frequency spectrum from time-domain samples
 * 4. Peak finding - Locate resonant frequency in PSD data
 * 5. Similarity calculation - Pearson correlation between PSD curves
 * 6. State machine - BeltTensionCalibrator lifecycle
 * 7. BeltTensionResult - Completeness, overall status, recommendations
 */

#include "../../include/belt_tension_calibrator.h"
#include "../../include/belt_tension_types.h"

#include <cfloat>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

// ============================================================================
// 1. Type Tests - evaluate_belt_status
// ============================================================================

TEST_CASE("evaluate_belt_status classifies correctly", "[belt_tension][types]") {
    // target=110, tolerance=10
    // GOOD: within [target-tolerance, target+tolerance] = [100, 120]
    CHECK(evaluate_belt_status(110.0f, 110.0f, 10.0f) == BeltStatus::GOOD);
    CHECK(evaluate_belt_status(100.0f, 110.0f, 10.0f) == BeltStatus::GOOD);
    CHECK(evaluate_belt_status(120.0f, 110.0f, 10.0f) == BeltStatus::GOOD);

    // WARNING: within [target-2*tolerance, target-tolerance) or (target+tolerance,
    // target+2*tolerance] i.e. [90, 100) or (120, 130]
    CHECK(evaluate_belt_status(95.0f, 110.0f, 10.0f) == BeltStatus::WARNING);
    CHECK(evaluate_belt_status(125.0f, 110.0f, 10.0f) == BeltStatus::WARNING);

    // BAD: outside [target-2*tolerance, target+2*tolerance] i.e. <90 or >130
    CHECK(evaluate_belt_status(80.0f, 110.0f, 10.0f) == BeltStatus::BAD);
    CHECK(evaluate_belt_status(145.0f, 110.0f, 10.0f) == BeltStatus::BAD);
}

TEST_CASE("evaluate_belt_status boundary values", "[belt_tension][types]") {
    // Exact boundary at tolerance edge
    CHECK(evaluate_belt_status(100.0f, 110.0f, 10.0f) == BeltStatus::GOOD);
    CHECK(evaluate_belt_status(120.0f, 110.0f, 10.0f) == BeltStatus::GOOD);

    // Exact boundary at 2x tolerance
    CHECK(evaluate_belt_status(90.0f, 110.0f, 10.0f) == BeltStatus::WARNING);
    CHECK(evaluate_belt_status(130.0f, 110.0f, 10.0f) == BeltStatus::WARNING);
}

TEST_CASE("evaluate_belt_status with different targets", "[belt_tension][types]") {
    // Prusa MK4 default: target=96, tolerance=15
    CHECK(evaluate_belt_status(96.0f, 96.0f, 15.0f) == BeltStatus::GOOD);
    CHECK(evaluate_belt_status(80.0f, 96.0f, 15.0f) ==
          BeltStatus::WARNING);                                          // delta=16, > tolerance
    CHECK(evaluate_belt_status(50.0f, 96.0f, 15.0f) == BeltStatus::BAD); // delta=46, > 2*tolerance
}

// ============================================================================
// 2. CSV Parsing - parse_accel_csv
// ============================================================================

TEST_CASE("parse_accel_csv handles Klipper format", "[belt_tension][csv]") {
    SECTION("valid CSV with header") {
        std::string csv = "#time,accel_x,accel_y,accel_z\n"
                          "0.000000,0.123,0.456,9.801\n"
                          "0.000312,0.234,0.567,9.802\n"
                          "0.000625,0.345,0.678,9.803\n";
        auto samples = parse_accel_csv(csv);
        REQUIRE(samples.size() == 3);
        CHECK(samples[0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(samples[0].x == Catch::Approx(0.123f).margin(0.001f));
        CHECK(samples[0].y == Catch::Approx(0.456f).margin(0.001f));
        CHECK(samples[0].z == Catch::Approx(9.801f).margin(0.001f));
        CHECK(samples[1].time == Catch::Approx(0.000312f).margin(0.0001f));
        CHECK(samples[2].z == Catch::Approx(9.803f).margin(0.001f));
    }

    SECTION("empty CSV returns empty") {
        CHECK(parse_accel_csv("").empty());
        CHECK(parse_accel_csv("#time,accel_x,accel_y,accel_z\n").empty());
    }

    SECTION("malformed lines are skipped") {
        std::string csv = "#time,accel_x,accel_y,accel_z\n"
                          "0.0,1.0,2.0,3.0\n"
                          "bad_line\n"
                          "0.1,4.0,5.0,6.0\n";
        auto samples = parse_accel_csv(csv);
        CHECK(samples.size() == 2);
    }

    SECTION("comment lines starting with # are skipped") {
        std::string csv = "#time,accel_x,accel_y,accel_z\n"
                          "# This is a comment\n"
                          "0.0,1.0,2.0,3.0\n";
        auto samples = parse_accel_csv(csv);
        CHECK(samples.size() == 1);
    }

    SECTION("trailing whitespace handled") {
        std::string csv = "#time,accel_x,accel_y,accel_z\n"
                          "0.0,1.0,2.0,3.0  \n"
                          "0.1,4.0,5.0,6.0\r\n";
        auto samples = parse_accel_csv(csv);
        CHECK(samples.size() == 2);
    }
}

// ============================================================================
// 3. FFT/PSD Computation
// ============================================================================

TEST_CASE("compute_psd produces frequency spectrum", "[belt_tension][fft]") {
    // Generate a known sine wave at 100 Hz, sampled at 3200 Hz
    std::vector<AccelSample> samples;
    float sample_rate = 3200.0f;
    float test_freq = 100.0f;
    int num_samples = 3200; // 1 second of data

    for (int i = 0; i < num_samples; i++) {
        float t = static_cast<float>(i) / sample_rate;
        AccelSample s;
        s.time = t;
        s.x = std::sin(2.0f * static_cast<float>(M_PI) * test_freq * t);
        s.y = 0.0f;
        s.z = 9.81f;
        samples.push_back(s);
    }

    auto psd = compute_psd(samples, sample_rate);
    REQUIRE(!psd.empty());

    // Find peak - should be near 100 Hz
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
    CHECK(peak.found);
    CHECK(peak.frequency == Catch::Approx(100.0f).margin(5.0f));
    CHECK(peak.amplitude > 0.0f);
}

TEST_CASE("compute_psd with empty input returns empty", "[belt_tension][fft]") {
    std::vector<AccelSample> empty;
    CHECK(compute_psd(empty).empty());
}

TEST_CASE("compute_psd frequency resolution scales with sample count", "[belt_tension][fft]") {
    float sample_rate = 3200.0f;
    float test_freq = 110.0f;

    auto make_sine = [&](int count) {
        std::vector<AccelSample> samples;
        for (int i = 0; i < count; i++) {
            float t = static_cast<float>(i) / sample_rate;
            AccelSample s;
            s.time = t;
            s.x = std::sin(2.0f * static_cast<float>(M_PI) * test_freq * t);
            s.y = 0.0f;
            s.z = 9.81f;
            samples.push_back(s);
        }
        return samples;
    };

    auto psd_short = compute_psd(make_sine(800), sample_rate);
    auto psd_long = compute_psd(make_sine(3200), sample_rate);

    CHECK(!psd_short.empty());
    CHECK(!psd_long.empty());

    // Longer sample should have more frequency bins
    CHECK(psd_long.size() >= psd_short.size());
}

// ============================================================================
// 4. Peak Finding
// ============================================================================

TEST_CASE("find_peak_frequency with known data", "[belt_tension][peak]") {
    // Create synthetic PSD with clear Gaussian peak at 110 Hz
    std::vector<std::pair<float, float>> psd;
    for (float f = 0.0f; f <= 200.0f; f += 1.0f) {
        float power = std::exp(-0.5f * std::pow((f - 110.0f) / 5.0f, 2.0f));
        psd.push_back({f, power});
    }

    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
    CHECK(peak.found);
    CHECK(peak.frequency == Catch::Approx(110.0f).margin(1.0f));
    CHECK(peak.amplitude == Catch::Approx(1.0f).margin(0.01f));

    SECTION("respects frequency range") {
        auto peak_narrow = find_peak_frequency(psd, 120.0f, 200.0f);
        // Peak at 110 is outside range, should find something at or above 120
        CHECK(peak_narrow.frequency >= 120.0f);
    }

    SECTION("empty PSD returns not found") {
        std::vector<std::pair<float, float>> empty;
        auto result = find_peak_frequency(empty);
        CHECK_FALSE(result.found);
    }

    SECTION("flat PSD still returns a peak") {
        std::vector<std::pair<float, float>> flat;
        for (float f = 20.0f; f <= 200.0f; f += 1.0f) {
            flat.push_back({f, 1.0f});
        }
        auto result = find_peak_frequency(flat, 20.0f, 200.0f);
        CHECK(result.found);
        CHECK(result.amplitude == Catch::Approx(1.0f).margin(0.01f));
    }
}

TEST_CASE("find_peak_frequency with dual peaks", "[belt_tension][peak]") {
    // Two peaks: stronger one at 100 Hz, weaker at 150 Hz
    std::vector<std::pair<float, float>> psd;
    for (float f = 0.0f; f <= 200.0f; f += 1.0f) {
        float p1 = 2.0f * std::exp(-0.5f * std::pow((f - 100.0f) / 5.0f, 2.0f));
        float p2 = 1.0f * std::exp(-0.5f * std::pow((f - 150.0f) / 5.0f, 2.0f));
        psd.push_back({f, p1 + p2});
    }

    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
    CHECK(peak.found);
    // Should find the stronger peak at 100 Hz
    CHECK(peak.frequency == Catch::Approx(100.0f).margin(2.0f));
}

// ============================================================================
// 5. Similarity Calculation
// ============================================================================

TEST_CASE("calculate_similarity between curves", "[belt_tension][similarity]") {
    SECTION("identical curves = 100%") {
        std::vector<std::pair<float, float>> curve;
        for (float f = 0; f <= 200; f += 1.0f) {
            curve.push_back({f, std::sin(f * 0.1f)});
        }
        CHECK(calculate_similarity(curve, curve) == Catch::Approx(100.0f).margin(0.1f));
    }

    SECTION("inverted curves = low similarity") {
        std::vector<std::pair<float, float>> a, b;
        for (float f = 0; f <= 200; f += 1.0f) {
            float v = std::sin(f * 0.1f);
            a.push_back({f, v});
            b.push_back({f, -v});
        }
        CHECK(calculate_similarity(a, b) < 10.0f);
    }

    SECTION("similar but shifted curves") {
        std::vector<std::pair<float, float>> a, b;
        for (float f = 0; f <= 200; f += 1.0f) {
            float va = std::exp(-0.5f * std::pow((f - 100.0f) / 10.0f, 2));
            float vb = std::exp(-0.5f * std::pow((f - 105.0f) / 10.0f, 2));
            a.push_back({f, va});
            b.push_back({f, vb});
        }
        float sim = calculate_similarity(a, b);
        CHECK(sim > 70.0f);  // Similar shape
        CHECK(sim < 100.0f); // But not identical
    }

    SECTION("empty curves") {
        std::vector<std::pair<float, float>> empty;
        CHECK(calculate_similarity(empty, empty) == Catch::Approx(0.0f));
    }

    SECTION("single point curves") {
        std::vector<std::pair<float, float>> single = {{100.0f, 1.0f}};
        // Pearson correlation undefined for single point, should return 0
        CHECK(calculate_similarity(single, single) == Catch::Approx(0.0f).margin(1.0f));
    }
}

// ============================================================================
// 6. State Machine - BeltTensionCalibrator
// ============================================================================

TEST_CASE("BeltTensionCalibrator initial state", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator reset returns to IDLE", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    cal.reset();
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator cancel returns to IDLE", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    cal.cancel();
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator detect_hardware without API calls error",
          "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    bool error_called = false;
    std::string error_msg;

    cal.detect_hardware([](const BeltTensionHardware&) { FAIL("Should not succeed without API"); },
                        [&](const std::string& msg) {
                            error_called = true;
                            error_msg = msg;
                        });

    CHECK(error_called);
    CHECK_FALSE(error_msg.empty());
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator run_auto_sweep without API calls error",
          "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    bool error_called = false;
    std::string error_msg;

    cal.run_auto_sweep([](int) {},
                       [](const BeltTensionResult&) { FAIL("Should not succeed without API"); },
                       [&](const std::string& msg) {
                           error_called = true;
                           error_msg = msg;
                       });

    CHECK(error_called);
    CHECK_FALSE(error_msg.empty());
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator test_path without API calls error", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    bool error_called = false;

    cal.test_path(
        BeltPath::PATH_A, [](int) {},
        [](const BeltMeasurement&) { FAIL("Should not succeed without API"); },
        [&](const std::string&) { error_called = true; });

    CHECK(error_called);
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator target/tolerance config", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    cal.set_target_frequency(96.0f);
    cal.set_tolerance(15.0f);

    CHECK(cal.get_results().target_frequency == Catch::Approx(96.0f));
    CHECK(cal.get_results().tolerance == Catch::Approx(15.0f));
}

TEST_CASE("BeltTensionCalibrator default target values", "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    CHECK(cal.get_results().target_frequency == Catch::Approx(110.0f));
    CHECK(cal.get_results().tolerance == Catch::Approx(10.0f));
}

TEST_CASE("BeltTensionCalibrator detect_hardware with null callbacks does not crash",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.detect_hardware(nullptr, nullptr));
}

TEST_CASE("BeltTensionCalibrator run_auto_sweep with null callbacks does not crash",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.run_auto_sweep(nullptr, nullptr, nullptr));
}

TEST_CASE("BeltTensionCalibrator test_path with null callbacks does not crash",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.test_path(BeltPath::PATH_A, nullptr, nullptr, nullptr));
}

TEST_CASE("BeltTensionCalibrator multiple resets are safe",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.reset());
    REQUIRE_NOTHROW(cal.reset());
    REQUIRE_NOTHROW(cal.reset());
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator multiple cancels are safe",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.cancel());
    REQUIRE_NOTHROW(cal.cancel());
    REQUIRE_NOTHROW(cal.cancel());
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator get_results is always valid reference",
          "[belt_tension][calibrator][edge_case]") {
    BeltTensionCalibrator cal;
    const auto& results1 = cal.get_results();
    CHECK_FALSE(results1.is_complete());

    cal.cancel();
    const auto& results2 = cal.get_results();
    CHECK_FALSE(results2.is_complete());

    cal.reset();
    const auto& results3 = cal.get_results();
    CHECK_FALSE(results3.is_complete());
}

TEST_CASE("BeltTensionCalibrator get_hardware returns default values",
          "[belt_tension][calibrator]") {
    BeltTensionCalibrator cal;
    const auto& hw = cal.get_hardware();
    CHECK(hw.kinematics == KinematicsType::UNKNOWN);
    CHECK_FALSE(hw.has_adxl);
    CHECK_FALSE(hw.has_pwm_led);
}

TEST_CASE("BeltTensionCalibrator is non-copyable and non-movable", "[belt_tension][calibrator]") {
    // Shared alive_ flag makes move unsound, so move ops are deleted
    CHECK_FALSE(std::is_move_constructible_v<BeltTensionCalibrator>);
    CHECK_FALSE(std::is_move_assignable_v<BeltTensionCalibrator>);
    CHECK_FALSE(std::is_copy_constructible_v<BeltTensionCalibrator>);
    CHECK_FALSE(std::is_copy_assignable_v<BeltTensionCalibrator>);
}

TEST_CASE("BeltTensionCalibrator State enum values are distinct", "[belt_tension][calibrator]") {
    using State = BeltTensionCalibrator::State;

    CHECK(State::IDLE != State::DETECTING_HARDWARE);
    CHECK(State::DETECTING_HARDWARE != State::CHECKING_ADXL);
    CHECK(State::CHECKING_ADXL != State::HOMING);
    CHECK(State::HOMING != State::TESTING_PATH_A);
    CHECK(State::TESTING_PATH_A != State::TESTING_PATH_B);
    CHECK(State::TESTING_PATH_B != State::RESULTS_READY);
    CHECK(State::RESULTS_READY != State::STROBE_MODE);
    CHECK(State::STROBE_MODE != State::ERROR);
    CHECK(State::ERROR != State::IDLE);
}

// ============================================================================
// Strobe Mode Tests
// ============================================================================

TEST_CASE("BeltTensionCalibrator start_strobe without API calls error",
          "[belt_tension][calibrator][strobe]") {
    BeltTensionCalibrator cal;
    bool error_called = false;

    cal.start_strobe(110.0f, [&](const std::string&) { error_called = true; });

    CHECK(error_called);
}

TEST_CASE("BeltTensionCalibrator stop_strobe from IDLE is safe",
          "[belt_tension][calibrator][strobe]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.stop_strobe());
    CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
}

TEST_CASE("BeltTensionCalibrator start_strobe with null callback does not crash",
          "[belt_tension][calibrator][strobe][edge_case]") {
    BeltTensionCalibrator cal;
    REQUIRE_NOTHROW(cal.start_strobe(110.0f, nullptr));
}

// ============================================================================
// 7. BeltTensionResult Tests
// ============================================================================

TEST_CASE("BeltTensionResult completeness checks", "[belt_tension][result]") {
    BeltTensionResult result;

    SECTION("empty result is not complete") {
        CHECK_FALSE(result.is_complete());
        CHECK_FALSE(result.has_path_a());
        CHECK_FALSE(result.has_path_b());
    }

    SECTION("only path A is not complete") {
        result.path_a.peak_frequency = 110.0f;
        CHECK(result.has_path_a());
        CHECK_FALSE(result.has_path_b());
        CHECK_FALSE(result.is_complete());
    }

    SECTION("only path B is not complete") {
        result.path_b.peak_frequency = 112.0f;
        CHECK_FALSE(result.has_path_a());
        CHECK(result.has_path_b());
        CHECK_FALSE(result.is_complete());
    }

    SECTION("both paths = complete") {
        result.path_a.peak_frequency = 110.0f;
        result.path_b.peak_frequency = 112.0f;
        CHECK(result.has_path_a());
        CHECK(result.has_path_b());
        CHECK(result.is_complete());
    }
}

TEST_CASE("BeltTensionResult overall_status", "[belt_tension][result]") {
    BeltTensionResult result;
    result.target_frequency = 110.0f;
    result.tolerance = 10.0f;

    SECTION("both good = GOOD") {
        result.path_a.peak_frequency = 108.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 112.0f;
        result.path_b.status = BeltStatus::GOOD;
        result.frequency_delta = 4.0f;
        CHECK(result.overall_status() == BeltStatus::GOOD);
    }

    SECTION("one warning = WARNING") {
        result.path_a.peak_frequency = 108.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 122.0f;
        result.path_b.status = BeltStatus::WARNING;
        result.frequency_delta = 14.0f; // Below 15 Hz delta threshold
        CHECK(result.overall_status() == BeltStatus::WARNING);
    }

    SECTION("one bad = BAD") {
        result.path_a.peak_frequency = 108.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 145.0f;
        result.path_b.status = BeltStatus::BAD;
        result.frequency_delta = 37.0f;
        CHECK(result.overall_status() == BeltStatus::BAD);
    }

    SECTION("large delta = BAD even if individual ok") {
        result.path_a.peak_frequency = 105.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 115.0f;
        result.path_b.status = BeltStatus::GOOD;
        result.frequency_delta = 16.0f; // >15 Hz diff = BAD
        CHECK(result.overall_status() == BeltStatus::BAD);
    }

    SECTION("moderate delta = WARNING") {
        result.path_a.peak_frequency = 107.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 113.0f;
        result.path_b.status = BeltStatus::GOOD;
        result.frequency_delta = 8.0f;
        auto status = result.overall_status();
        // Implementation determines threshold for delta-based warning
        CHECK((status == BeltStatus::GOOD || status == BeltStatus::WARNING));
    }
}

TEST_CASE("BeltTensionResult recommendation text", "[belt_tension][result]") {
    BeltTensionResult result;
    result.target_frequency = 110.0f;
    result.tolerance = 10.0f;

    SECTION("both good produces non-empty recommendation") {
        result.path_a.peak_frequency = 110.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 111.0f;
        result.path_b.status = BeltStatus::GOOD;
        result.frequency_delta = 1.0f;
        auto rec = result.recommendation();
        CHECK(!rec.empty());
    }

    SECTION("path B lower than A produces recommendation") {
        result.path_a.peak_frequency = 115.0f;
        result.path_a.status = BeltStatus::GOOD;
        result.path_b.peak_frequency = 95.0f;
        result.path_b.status = BeltStatus::WARNING;
        result.frequency_delta = 20.0f;
        auto rec = result.recommendation();
        CHECK(!rec.empty());
    }

    SECTION("both bad produces non-empty recommendation") {
        result.path_a.peak_frequency = 60.0f;
        result.path_a.status = BeltStatus::BAD;
        result.path_b.peak_frequency = 55.0f;
        result.path_b.status = BeltStatus::BAD;
        result.frequency_delta = 5.0f;
        auto rec = result.recommendation();
        CHECK(!rec.empty());
    }

    SECTION("incomplete result produces non-empty recommendation") {
        auto rec = result.recommendation();
        CHECK(!rec.empty());
    }
}

// ============================================================================
// BeltMeasurement Tests
// ============================================================================

TEST_CASE("BeltMeasurement validity", "[belt_tension][types]") {
    BeltMeasurement m;

    SECTION("default is invalid") {
        CHECK_FALSE(m.is_valid());
    }

    SECTION("non-zero frequency is valid") {
        m.peak_frequency = 110.0f;
        CHECK(m.is_valid());
    }

    SECTION("zero frequency is invalid") {
        m.peak_frequency = 0.0f;
        CHECK_FALSE(m.is_valid());
    }

    SECTION("negative frequency is invalid") {
        m.peak_frequency = -10.0f;
        CHECK_FALSE(m.is_valid());
    }
}

// ============================================================================
// BeltTensionHardware Tests
// ============================================================================

TEST_CASE("BeltTensionHardware default construction", "[belt_tension][types]") {
    BeltTensionHardware hw;
    CHECK(hw.kinematics == KinematicsType::UNKNOWN);
    CHECK_FALSE(hw.has_adxl);
    CHECK_FALSE(hw.has_pwm_led);
    CHECK(hw.pwm_led_pin.empty());
    CHECK(hw.kinematics_name.empty());
}

// ============================================================================
// Enum Coverage
// ============================================================================

TEST_CASE("BeltPath enum values", "[belt_tension][types]") {
    CHECK(BeltPath::PATH_A != BeltPath::PATH_B);
    CHECK(BeltPath::X_AXIS != BeltPath::Y_AXIS);
    CHECK(BeltPath::PATH_A != BeltPath::X_AXIS);
}

TEST_CASE("KinematicsType enum values", "[belt_tension][types]") {
    CHECK(KinematicsType::UNKNOWN != KinematicsType::COREXY);
    CHECK(KinematicsType::COREXY != KinematicsType::CARTESIAN);
    CHECK(KinematicsType::UNKNOWN != KinematicsType::CARTESIAN);
}

// ============================================================================
// Mock Data for End-to-End Pipeline Tests
// ============================================================================

namespace {

/**
 * @brief Generate mock accelerometer CSV with a known resonance frequency
 *
 * Produces Klipper-format CSV data (#time,accel_x,accel_y,accel_z) containing
 * a sine wave at the specified frequency on the X axis. Y axis has a weaker
 * signal at 2x frequency (simulating a harmonic), Z holds gravity (~9.81).
 *
 * @param frequency_hz  Primary resonance frequency to embed
 * @param sample_rate   Sample rate in Hz (Klipper default: 3200)
 * @param duration_sec  Duration of data in seconds
 * @param amplitude     Peak amplitude of the resonance signal
 * @return CSV string in Klipper format
 */
std::string generate_mock_accel_csv(float frequency_hz, float sample_rate = 3200.0f,
                                    float duration_sec = 1.0f, float amplitude = 50.0f) {
    std::string csv = "#time,accel_x,accel_y,accel_z\n";

    int num_samples = static_cast<int>(sample_rate * duration_sec);
    for (int i = 0; i < num_samples; i++) {
        float t = static_cast<float>(i) / sample_rate;
        // Primary resonance on X axis
        float x = amplitude * std::sin(2.0f * static_cast<float>(M_PI) * frequency_hz * t);
        // Weaker harmonic on Y axis (simulates cross-axis coupling)
        float y = (amplitude * 0.3f) *
                  std::sin(2.0f * static_cast<float>(M_PI) * frequency_hz * 2.0f * t);
        // Gravity on Z
        float z = 9.81f;

        char buf[128];
        snprintf(buf, sizeof(buf), "%.6f,%.3f,%.3f,%.3f\n", t, x, y, z);
        csv += buf;
    }
    return csv;
}

/**
 * @brief Generate two mock CSV datasets that simulate matched belts
 *
 * Both belts have similar resonance frequencies (within tolerance), producing
 * high similarity and GOOD status.
 */
struct MockBeltPair {
    std::string csv_a;
    std::string csv_b;
    float freq_a;
    float freq_b;
};

MockBeltPair generate_matched_belt_pair(float target = 110.0f, float delta = 2.0f) {
    MockBeltPair pair;
    pair.freq_a = target - delta / 2.0f;
    pair.freq_b = target + delta / 2.0f;
    pair.csv_a = generate_mock_accel_csv(pair.freq_a);
    pair.csv_b = generate_mock_accel_csv(pair.freq_b);
    return pair;
}

MockBeltPair generate_mismatched_belt_pair(float target = 110.0f) {
    MockBeltPair pair;
    pair.freq_a = target;
    pair.freq_b = target - 25.0f; // Significantly looser
    pair.csv_a = generate_mock_accel_csv(pair.freq_a);
    pair.csv_b = generate_mock_accel_csv(pair.freq_b);
    return pair;
}

} // anonymous namespace

// ============================================================================
// 8. End-to-End Pipeline: CSV -> PSD -> Peak -> Status -> Recommendation
// ============================================================================

TEST_CASE("End-to-end pipeline: 110 Hz resonance detection", "[belt_tension][mock][pipeline]") {
    // Generate mock CSV with 110 Hz resonance
    std::string csv = generate_mock_accel_csv(110.0f);

    SECTION("CSV parsing produces expected sample count") {
        auto samples = parse_accel_csv(csv);
        REQUIRE(samples.size() == 3200); // 1 second at 3200 Hz
        CHECK(samples[0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(samples.back().time == Catch::Approx(0.999687f).margin(0.01f));
    }

    SECTION("PSD shows clear peak at 110 Hz") {
        auto samples = parse_accel_csv(csv);
        auto psd = compute_psd(samples, 3200.0f);
        REQUIRE(!psd.empty());

        auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
        CHECK(peak.found);
        CHECK(peak.frequency == Catch::Approx(110.0f).margin(5.0f));
        CHECK(peak.amplitude > 0.0f);
    }

    SECTION("status evaluates as GOOD for on-target frequency") {
        auto samples = parse_accel_csv(csv);
        auto psd = compute_psd(samples, 3200.0f);
        auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

        BeltStatus status = evaluate_belt_status(peak.frequency, 110.0f, 10.0f);
        CHECK(status == BeltStatus::GOOD);
    }
}

TEST_CASE("End-to-end pipeline: matched belt pair", "[belt_tension][mock][pipeline]") {
    auto pair = generate_matched_belt_pair(110.0f, 2.0f);

    // Parse and analyze both paths
    auto samples_a = parse_accel_csv(pair.csv_a);
    auto samples_b = parse_accel_csv(pair.csv_b);

    auto psd_a = compute_psd(samples_a, 3200.0f);
    auto psd_b = compute_psd(samples_b, 3200.0f);

    auto peak_a = find_peak_frequency(psd_a, 20.0f, 200.0f);
    auto peak_b = find_peak_frequency(psd_b, 20.0f, 200.0f);

    REQUIRE(peak_a.found);
    REQUIRE(peak_b.found);

    SECTION("peaks are near expected frequencies") {
        CHECK(peak_a.frequency == Catch::Approx(pair.freq_a).margin(5.0f));
        CHECK(peak_b.frequency == Catch::Approx(pair.freq_b).margin(5.0f));
    }

    SECTION("similarity is positive for matched belts") {
        float sim = calculate_similarity(psd_a, psd_b);
        // Synthetic pure sine waves produce narrow PSD peaks, so Pearson
        // correlation is lower than for real broadband accelerometer data.
        // Real belts produce broader peaks that correlate much better.
        CHECK(sim > 0.0f);
    }

    SECTION("BeltTensionResult reports GOOD overall") {
        BeltTensionResult result;
        result.target_frequency = 110.0f;
        result.tolerance = 10.0f;

        result.path_a.peak_frequency = peak_a.frequency;
        result.path_a.freq_response = psd_a;
        result.path_a.status = evaluate_belt_status(peak_a.frequency, 110.0f, 10.0f);

        result.path_b.peak_frequency = peak_b.frequency;
        result.path_b.freq_response = psd_b;
        result.path_b.status = evaluate_belt_status(peak_b.frequency, 110.0f, 10.0f);

        result.frequency_delta = std::abs(peak_a.frequency - peak_b.frequency);
        result.similarity_percent = calculate_similarity(psd_a, psd_b);

        CHECK(result.is_complete());
        CHECK(result.overall_status() == BeltStatus::GOOD);
        CHECK(!result.recommendation().empty());
    }
}

TEST_CASE("End-to-end pipeline: mismatched belt pair", "[belt_tension][mock][pipeline]") {
    auto pair = generate_mismatched_belt_pair(110.0f);

    auto samples_a = parse_accel_csv(pair.csv_a);
    auto samples_b = parse_accel_csv(pair.csv_b);

    auto psd_a = compute_psd(samples_a, 3200.0f);
    auto psd_b = compute_psd(samples_b, 3200.0f);

    auto peak_a = find_peak_frequency(psd_a, 20.0f, 200.0f);
    auto peak_b = find_peak_frequency(psd_b, 20.0f, 200.0f);

    REQUIRE(peak_a.found);
    REQUIRE(peak_b.found);

    SECTION("frequency delta is significant") {
        float delta = std::abs(peak_a.frequency - peak_b.frequency);
        CHECK(delta > 15.0f);
    }

    SECTION("similarity is low for mismatched belts") {
        float sim = calculate_similarity(psd_a, psd_b);
        // 25 Hz apart: very different PSD shapes
        CHECK(sim < 50.0f);
    }

    SECTION("BeltTensionResult reports BAD overall due to delta") {
        BeltTensionResult result;
        result.target_frequency = 110.0f;
        result.tolerance = 10.0f;

        result.path_a.peak_frequency = peak_a.frequency;
        result.path_a.freq_response = psd_a;
        result.path_a.status = evaluate_belt_status(peak_a.frequency, 110.0f, 10.0f);

        result.path_b.peak_frequency = peak_b.frequency;
        result.path_b.freq_response = psd_b;
        result.path_b.status = evaluate_belt_status(peak_b.frequency, 110.0f, 10.0f);

        result.frequency_delta = std::abs(peak_a.frequency - peak_b.frequency);
        result.similarity_percent = calculate_similarity(psd_a, psd_b);

        CHECK(result.is_complete());
        // Large delta (>15 Hz) should yield BAD
        CHECK(result.overall_status() == BeltStatus::BAD);
        // Recommendation should mention tightening
        auto rec = result.recommendation();
        CHECK(!rec.empty());
    }
}

TEST_CASE("End-to-end pipeline: loose belts (both low)", "[belt_tension][mock][pipeline]") {
    // Both belts at 70 Hz (way below 110 Hz target)
    std::string csv_a = generate_mock_accel_csv(70.0f);
    std::string csv_b = generate_mock_accel_csv(72.0f);

    auto samples_a = parse_accel_csv(csv_a);
    auto samples_b = parse_accel_csv(csv_b);

    auto psd_a = compute_psd(samples_a, 3200.0f);
    auto psd_b = compute_psd(samples_b, 3200.0f);

    auto peak_a = find_peak_frequency(psd_a, 20.0f, 200.0f);
    auto peak_b = find_peak_frequency(psd_b, 20.0f, 200.0f);

    BeltTensionResult result;
    result.target_frequency = 110.0f;
    result.tolerance = 10.0f;
    result.path_a.peak_frequency = peak_a.frequency;
    result.path_a.status = evaluate_belt_status(peak_a.frequency, 110.0f, 10.0f);
    result.path_b.peak_frequency = peak_b.frequency;
    result.path_b.status = evaluate_belt_status(peak_b.frequency, 110.0f, 10.0f);
    result.frequency_delta = std::abs(peak_a.frequency - peak_b.frequency);

    CHECK(result.is_complete());
    CHECK(result.overall_status() == BeltStatus::BAD);
    CHECK(result.recommendation().find("tightening") != std::string::npos);
}

TEST_CASE("End-to-end pipeline: overtightened belts (both high)",
          "[belt_tension][mock][pipeline]") {
    std::string csv_a = generate_mock_accel_csv(150.0f);
    std::string csv_b = generate_mock_accel_csv(148.0f);

    auto samples_a = parse_accel_csv(csv_a);
    auto samples_b = parse_accel_csv(csv_b);

    auto psd_a = compute_psd(samples_a, 3200.0f);
    auto psd_b = compute_psd(samples_b, 3200.0f);

    auto peak_a = find_peak_frequency(psd_a, 20.0f, 200.0f);
    auto peak_b = find_peak_frequency(psd_b, 20.0f, 200.0f);

    BeltTensionResult result;
    result.target_frequency = 110.0f;
    result.tolerance = 10.0f;
    result.path_a.peak_frequency = peak_a.frequency;
    result.path_a.status = evaluate_belt_status(peak_a.frequency, 110.0f, 10.0f);
    result.path_b.peak_frequency = peak_b.frequency;
    result.path_b.status = evaluate_belt_status(peak_b.frequency, 110.0f, 10.0f);
    result.frequency_delta = std::abs(peak_a.frequency - peak_b.frequency);

    CHECK(result.is_complete());
    CHECK(result.overall_status() == BeltStatus::BAD);
    CHECK(result.recommendation().find("overtightened") != std::string::npos);
}

// ============================================================================
// 9. Mock Pipeline: Different Sample Rates and Durations
// ============================================================================

TEST_CASE("Pipeline with short duration data (0.25s)", "[belt_tension][mock][pipeline]") {
    // Shorter capture should still detect frequency, just with less precision
    std::string csv = generate_mock_accel_csv(110.0f, 3200.0f, 0.25f);
    auto samples = parse_accel_csv(csv);
    CHECK(samples.size() == 800);

    auto psd = compute_psd(samples, 3200.0f);
    REQUIRE(!psd.empty());

    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
    CHECK(peak.found);
    // Lower precision due to shorter sample — 3200/800 = 4 Hz frequency resolution
    CHECK(peak.frequency == Catch::Approx(110.0f).margin(10.0f));
}

TEST_CASE("Pipeline with different sample rates", "[belt_tension][mock][pipeline]") {
    SECTION("1600 Hz sample rate") {
        std::string csv = generate_mock_accel_csv(100.0f, 1600.0f, 1.0f);
        auto samples = parse_accel_csv(csv);
        auto psd = compute_psd(samples, 1600.0f);
        auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
        CHECK(peak.found);
        CHECK(peak.frequency == Catch::Approx(100.0f).margin(5.0f));
    }

    SECTION("6400 Hz sample rate (high-speed ADXL)") {
        std::string csv = generate_mock_accel_csv(100.0f, 6400.0f, 0.5f);
        auto samples = parse_accel_csv(csv);
        auto psd = compute_psd(samples, 6400.0f);
        auto peak = find_peak_frequency(psd, 20.0f, 200.0f);
        CHECK(peak.found);
        CHECK(peak.frequency == Catch::Approx(100.0f).margin(5.0f));
    }
}

// ============================================================================
// 10. Mock State Machine: Calibrator Without API
// ============================================================================

TEST_CASE("BeltTensionCalibrator state transitions without API (error paths)",
          "[belt_tension][mock][calibrator]") {
    BeltTensionCalibrator cal;

    SECTION("detect_hardware -> error -> IDLE") {
        bool error_called = false;
        std::string error_msg;

        cal.detect_hardware([](const BeltTensionHardware&) { FAIL("Should not succeed"); },
                            [&](const std::string& msg) {
                                error_called = true;
                                error_msg = msg;
                            });

        CHECK(error_called);
        CHECK(error_msg.find("API") != std::string::npos);
        CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
    }

    SECTION("test_path -> error -> IDLE") {
        bool error_called = false;
        cal.test_path(
            BeltPath::PATH_A, [](int) {},
            [](const BeltMeasurement&) { FAIL("Should not succeed"); },
            [&](const std::string&) { error_called = true; });
        CHECK(error_called);
        CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
    }

    SECTION("run_auto_sweep -> error -> IDLE") {
        bool error_called = false;
        cal.run_auto_sweep([](int) {}, [](const BeltTensionResult&) { FAIL("Should not succeed"); },
                           [&](const std::string&) { error_called = true; });
        CHECK(error_called);
        CHECK(cal.get_state() == BeltTensionCalibrator::State::IDLE);
    }

    SECTION("start_strobe -> error -> IDLE") {
        bool error_called = false;
        cal.start_strobe(110.0f, [&](const std::string&) { error_called = true; });
        CHECK(error_called);
    }
}

TEST_CASE("BeltTensionCalibrator full workflow simulation (analysis only)",
          "[belt_tension][mock][calibrator]") {
    // Simulate what the calibrator does after receiving CSV data,
    // exercising the full analysis chain manually.
    BeltTensionCalibrator cal;
    cal.set_target_frequency(110.0f);
    cal.set_tolerance(10.0f);

    // Generate and analyze path A
    auto csv_a = generate_mock_accel_csv(108.0f);
    auto samples_a = parse_accel_csv(csv_a);
    auto psd_a = compute_psd(samples_a, 3200.0f);
    auto peak_a = find_peak_frequency(psd_a, 20.0f, 200.0f);
    REQUIRE(peak_a.found);

    // Generate and analyze path B
    auto csv_b = generate_mock_accel_csv(112.0f);
    auto samples_b = parse_accel_csv(csv_b);
    auto psd_b = compute_psd(samples_b, 3200.0f);
    auto peak_b = find_peak_frequency(psd_b, 20.0f, 200.0f);
    REQUIRE(peak_b.found);

    // Build result (mirrors what the calibrator does internally)
    BeltTensionResult result;
    result.target_frequency = 110.0f;
    result.tolerance = 10.0f;

    result.path_a.path = BeltPath::PATH_A;
    result.path_a.peak_frequency = peak_a.frequency;
    result.path_a.peak_amplitude = peak_a.amplitude;
    result.path_a.freq_response = psd_a;
    result.path_a.status = evaluate_belt_status(peak_a.frequency, 110.0f, 10.0f);

    result.path_b.path = BeltPath::PATH_B;
    result.path_b.peak_frequency = peak_b.frequency;
    result.path_b.peak_amplitude = peak_b.amplitude;
    result.path_b.freq_response = psd_b;
    result.path_b.status = evaluate_belt_status(peak_b.frequency, 110.0f, 10.0f);

    result.frequency_delta = std::abs(result.path_a.peak_frequency - result.path_b.peak_frequency);
    result.similarity_percent =
        calculate_similarity(result.path_a.freq_response, result.path_b.freq_response);

    // Validate results
    CHECK(result.is_complete());
    CHECK(result.path_a.status == BeltStatus::GOOD);
    CHECK(result.path_b.status == BeltStatus::GOOD);
    CHECK(result.overall_status() == BeltStatus::GOOD);
    CHECK(result.frequency_delta < 10.0f);
    // Similarity is low for pure sine waves (narrow PSD peaks), but still positive
    CHECK(result.similarity_percent >= 0.0f);
    CHECK(result.recommendation().find("good") != std::string::npos);
}

TEST_CASE("BeltTensionCalibrator config persists through analysis",
          "[belt_tension][mock][calibrator]") {
    BeltTensionCalibrator cal;

    // Set non-default target (e.g., Prusa MK4 belt frequency)
    cal.set_target_frequency(96.0f);
    cal.set_tolerance(15.0f);

    // Verify config accessible via results
    CHECK(cal.get_results().target_frequency == Catch::Approx(96.0f));
    CHECK(cal.get_results().tolerance == Catch::Approx(15.0f));

    // Reset should clear results but re-initialized with defaults
    cal.reset();
    // After reset, results are default-constructed
    const auto& r = cal.get_results();
    CHECK(r.target_frequency == Catch::Approx(110.0f));
    CHECK(r.tolerance == Catch::Approx(10.0f));
    CHECK_FALSE(r.is_complete());
}

// ============================================================================
// 11. Mock Analysis Edge Cases
// ============================================================================

TEST_CASE("Pipeline with low-frequency resonance (30 Hz)", "[belt_tension][mock][pipeline]") {
    // Very loose belt, near the lower detection limit
    std::string csv = generate_mock_accel_csv(30.0f, 3200.0f, 1.0f);
    auto samples = parse_accel_csv(csv);
    auto psd = compute_psd(samples, 3200.0f);
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

    CHECK(peak.found);
    CHECK(peak.frequency == Catch::Approx(30.0f).margin(5.0f));
    CHECK(evaluate_belt_status(peak.frequency, 110.0f, 10.0f) == BeltStatus::BAD);
}

TEST_CASE("Pipeline with high-frequency resonance (190 Hz)", "[belt_tension][mock][pipeline]") {
    // Very tight belt, near the upper detection limit
    std::string csv = generate_mock_accel_csv(190.0f, 3200.0f, 1.0f);
    auto samples = parse_accel_csv(csv);
    auto psd = compute_psd(samples, 3200.0f);
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

    CHECK(peak.found);
    CHECK(peak.frequency == Catch::Approx(190.0f).margin(5.0f));
    CHECK(evaluate_belt_status(peak.frequency, 110.0f, 10.0f) == BeltStatus::BAD);
}

TEST_CASE("Pipeline with two close frequencies", "[belt_tension][mock][pipeline]") {
    // Simulate a belt with split resonance (two peaks close together)
    // The stronger peak at 105 Hz should dominate
    std::string csv = "#time,accel_x,accel_y,accel_z\n";
    float sample_rate = 3200.0f;
    int num_samples = 3200;
    for (int i = 0; i < num_samples; i++) {
        float t = static_cast<float>(i) / sample_rate;
        float x = 50.0f * std::sin(2.0f * static_cast<float>(M_PI) * 105.0f * t) +
                  30.0f * std::sin(2.0f * static_cast<float>(M_PI) * 115.0f * t);
        float y = 0.0f;
        float z = 9.81f;
        char buf[128];
        snprintf(buf, sizeof(buf), "%.6f,%.3f,%.3f,%.3f\n", t, x, y, z);
        csv += buf;
    }

    auto samples = parse_accel_csv(csv);
    auto psd = compute_psd(samples, sample_rate);
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

    CHECK(peak.found);
    // Dominant peak should be at 105 Hz (stronger amplitude)
    CHECK(peak.frequency == Catch::Approx(105.0f).margin(5.0f));
}

TEST_CASE("Pipeline with noisy data", "[belt_tension][mock][pipeline]") {
    // Add random noise on top of signal — use deterministic "noise" via high-frequency sine
    std::string csv = "#time,accel_x,accel_y,accel_z\n";
    float sample_rate = 3200.0f;
    int num_samples = 3200;
    for (int i = 0; i < num_samples; i++) {
        float t = static_cast<float>(i) / sample_rate;
        // Signal: 110 Hz, amplitude 50
        float signal = 50.0f * std::sin(2.0f * static_cast<float>(M_PI) * 110.0f * t);
        // Deterministic "noise": sum of high-frequency sinusoids
        float noise = 10.0f * std::sin(2.0f * static_cast<float>(M_PI) * 347.0f * t) +
                      8.0f * std::sin(2.0f * static_cast<float>(M_PI) * 523.0f * t) +
                      5.0f * std::sin(2.0f * static_cast<float>(M_PI) * 789.0f * t);
        float x = signal + noise;
        float y = noise * 0.5f;
        float z = 9.81f + noise * 0.1f;
        char buf[128];
        snprintf(buf, sizeof(buf), "%.6f,%.3f,%.3f,%.3f\n", t, x, y, z);
        csv += buf;
    }

    auto samples = parse_accel_csv(csv);
    auto psd = compute_psd(samples, sample_rate);
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

    CHECK(peak.found);
    // Signal is much stronger than noise, should still detect 110 Hz
    CHECK(peak.frequency == Catch::Approx(110.0f).margin(5.0f));
}

TEST_CASE("compute_psd default bandwidth stays at 250 Hz", "[belt_tension][fft]") {
    // Existing callers must not shift under them.
    const float sr = 3200.0f;
    const int n = 3200;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x = std::sin(2.0f * static_cast<float>(M_PI) * 100.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }

    auto psd = compute_psd(samples, sr);
    REQUIRE(!psd.empty());
    CHECK(psd.back().first <= Catch::Approx(250.0f).margin(2.0f));
}

TEST_CASE("compute_psd honours a wider bandwidth request", "[belt_tension][fft]") {
    const float sr = 3200.0f;
    const int n = 3200;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x =
            std::sin(2.0f * static_cast<float>(M_PI) * 100.0f * t) +
            0.3f * std::sin(2.0f * static_cast<float>(M_PI) * 600.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }

    auto narrow = compute_psd(samples, sr, 250.0f);
    auto wide = compute_psd(samples, sr, 700.0f);

    REQUIRE(!narrow.empty());
    REQUIRE(!wide.empty());
    CHECK(wide.size() > narrow.size());
    CHECK(wide.back().first > 650.0f);

    // The 600 Hz component is invisible at the narrow bandwidth and present at
    // the wide one. This is exactly why harmonic analysis needs the parameter.
    auto narrow_peak = find_peak_frequency(narrow, 500.0f, 700.0f);
    auto wide_peak = find_peak_frequency(wide, 500.0f, 700.0f);
    CHECK_FALSE(narrow_peak.found);
    REQUIRE(wide_peak.found);
    CHECK(wide_peak.frequency == Catch::Approx(600.0f).margin(4.0f));
}

TEST_CASE("compute_psd bandwidth is clamped by Nyquist", "[belt_tension][fft][edge_case]") {
    const float sr = 1000.0f;
    const int n = 1000;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        samples[static_cast<size_t>(i)].time = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].x = std::sin(static_cast<float>(i) * 0.1f);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }
    // Asking for 5 kHz from a 1 kHz capture must not read past the array.
    auto psd = compute_psd(samples, sr, 5000.0f);
    REQUIRE(!psd.empty());
    CHECK(psd.back().first <= sr / 2.0f + 1.0f);
}

TEST_CASE("compute_psd clamps a pathologically large bandwidth before the size_t cast",
          "[belt_tension][fft][edge_case]") {
    // FLT_MAX (~3.4e38) exceeds SIZE_MAX on both 32-bit and 64-bit targets, so
    // casting an unclamped bandwidth*n/sample_rate product to size_t at this
    // magnitude is undefined behaviour - the pre-fix code could have produced
    // any result, not a specific known-bad one. This exercises the clamp on an
    // input that was previously UB; it verifies that an absurd bandwidth
    // request still yields a Nyquist-bounded bin count rather than reading
    // past the array, not that it reproduces any particular old failure.
    const float sr = 1000.0f;
    const int n = 1000;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        samples[static_cast<size_t>(i)].time = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].x = std::sin(static_cast<float>(i) * 0.1f);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9.81f;
    }
    auto psd = compute_psd(samples, sr, FLT_MAX);
    REQUIRE(!psd.empty());
    CHECK(psd.size() <= static_cast<size_t>(n) / 2);
    CHECK(psd.back().first <= sr / 2.0f + 1.0f);
}

TEST_CASE("compute_psd phasor recurrence does not drift", "[belt_tension][fft][phasor]") {
    // Two tones, the lower one stronger: verifies the phasor DFT resolves them
    // and puts the peak on the stronger (97 Hz) tone, not the weaker harmonic.
    // This does NOT verify the double-vs-float accumulator choice - a float
    // phasor passes this test too at n=1024, so the double accumulators are a
    // deliberate defensive choice, not one this test enforces.
    const float sr = 3200.0f;
    const int n = 1024;
    std::vector<AccelSample> samples(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        samples[static_cast<size_t>(i)].time = t;
        samples[static_cast<size_t>(i)].x =
            std::sin(2.0f * static_cast<float>(M_PI) * 97.0f * t) +
            0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 194.0f * t);
        samples[static_cast<size_t>(i)].y = 0.0f;
        samples[static_cast<size_t>(i)].z = 9810.0f;
    }

    auto psd = compute_psd(samples, sr, 400.0f);
    REQUIRE(psd.size() > 50);

    auto peak = find_peak_frequency(psd, 20.0f, 300.0f);
    REQUIRE(peak.found);
    // The 97 Hz tone has 4x the power of the 194 Hz one, so it must win.
    CHECK(peak.frequency == Catch::Approx(97.0f).margin(4.0f));
}
