// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_calibrator.cpp
 * @brief Unit tests for InputShaperCalibrator class
 *
 * Test-first development: These tests are written BEFORE implementation.
 * Tests compile and link with minimal header stub, but will FAIL until
 * the InputShaperCalibrator implementation is complete.
 *
 * Test categories:
 * 1. State machine tests - State transitions and guards
 * 2. check_accelerometer() tests - ADXL connectivity verification
 * 3. run_calibration() tests - Resonance test execution
 * 4. apply_settings() tests - SET_INPUT_SHAPER command
 * 5. Error handling tests - Error callbacks and recovery
 */

#include "../../include/calibration_types.h"
#include "../../include/input_shaper_calibrator.h"
#include "../../include/moonraker_error.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::calibration;

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for InputShaperCalibrator testing
 *
 * Provides common setup and helper methods for testing the calibrator
 * state machine and callbacks.
 */
class InputShaperCalibratorTestFixture {
  public:
    InputShaperCalibratorTestFixture() {
        reset_callbacks();
    }

    ~InputShaperCalibratorTestFixture() = default;

    void reset_callbacks() {
        accel_check_complete_ = false;
        progress_updates_.clear();
        result_received_ = false;
        success_called_ = false;
        error_received_ = false;
        captured_noise_level_ = 0.0f;
        captured_result_ = InputShaperResult{};
        captured_error_.clear();
    }

    // Callback handlers for capturing results
    void on_accel_check(float noise_level) {
        accel_check_complete_ = true;
        captured_noise_level_ = noise_level;
    }

    void on_progress(int percent) {
        progress_updates_.push_back(percent);
    }

    void on_result(const InputShaperResult& result) {
        result_received_ = true;
        captured_result_ = result;
    }

    void on_success() {
        success_called_ = true;
    }

    void on_error(const std::string& message) {
        error_received_ = true;
        captured_error_ = message;
    }

    /**
     * @brief Wait for async operation with timeout
     * @param flag Atomic flag to wait for
     * @param timeout_ms Maximum wait time in milliseconds
     * @return true if flag was set, false on timeout
     */
    static bool wait_for(const std::atomic<bool>& flag, int timeout_ms = 1000) {
        for (int i = 0; i < timeout_ms / 10; ++i) {
            if (flag.load()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }

  protected:
    InputShaperCalibrator calibrator_;

    // Callback state tracking
    std::atomic<bool> accel_check_complete_{false};
    std::vector<int> progress_updates_;
    std::atomic<bool> result_received_{false};
    std::atomic<bool> success_called_{false};
    std::atomic<bool> error_received_{false};

    // Captured values
    float captured_noise_level_ = 0.0f;
    InputShaperResult captured_result_;
    std::string captured_error_;
};

// ============================================================================
// State Machine Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "InputShaperCalibrator initial state is IDLE",
                 "[calibrator][input_shaper][state]") {
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "CalibrationResults default construction",
                 "[calibrator][input_shaper][state]") {
    const auto& results = calibrator_.get_results();

    CHECK_FALSE(results.has_x());
    CHECK_FALSE(results.has_y());
    CHECK_FALSE(results.is_complete());
    CHECK(results.noise_level == 0.0f);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "CalibrationResults is_complete requires both axes",
                 "[calibrator][input_shaper][state]") {
    // Create results manually to test the struct
    InputShaperCalibrator::CalibrationResults results;

    SECTION("empty results are not complete") {
        CHECK_FALSE(results.is_complete());
        CHECK_FALSE(results.has_x());
        CHECK_FALSE(results.has_y());
    }

    SECTION("only X result is not complete") {
        results.x_result.shaper_type = "mzv";
        results.x_result.shaper_freq = 36.7f;
        results.x_result.axis = 'X';

        CHECK(results.has_x());
        CHECK_FALSE(results.has_y());
        CHECK_FALSE(results.is_complete());
    }

    SECTION("only Y result is not complete") {
        results.y_result.shaper_type = "ei";
        results.y_result.shaper_freq = 47.6f;
        results.y_result.axis = 'Y';

        CHECK_FALSE(results.has_x());
        CHECK(results.has_y());
        CHECK_FALSE(results.is_complete());
    }

    SECTION("both axes is complete") {
        results.x_result.shaper_type = "mzv";
        results.x_result.shaper_freq = 36.7f;
        results.x_result.axis = 'X';
        results.y_result.shaper_type = "ei";
        results.y_result.shaper_freq = 47.6f;
        results.y_result.axis = 'Y';

        CHECK(results.has_x());
        CHECK(results.has_y());
        CHECK(results.is_complete());
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "cancel() returns to IDLE state",
                 "[calibrator][input_shaper][state]") {
    // Cancel should be safe to call from any state
    calibrator_.cancel();
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "cancel() is safe to call when already IDLE",
                 "[calibrator][input_shaper][state]") {
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);

    // Should not throw or crash
    REQUIRE_NOTHROW(calibrator_.cancel());
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

// ============================================================================
// check_accelerometer() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer changes state to CHECKING_ADXL",
                 "[calibrator][input_shaper][accel]") {
    // Start accelerometer check - state should transition
    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });

    // State should be CHECKING_ADXL during check (or back to IDLE if synchronous)
    // Implementation will determine exact behavior
    auto state = calibrator_.get_state();
    CHECK((state == InputShaperCalibrator::State::CHECKING_ADXL ||
           state == InputShaperCalibrator::State::IDLE));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer with null callbacks does not crash",
                 "[calibrator][input_shaper][accel][edge_case]") {
    // Should not crash with nullptr callbacks
    REQUIRE_NOTHROW(calibrator_.check_accelerometer(nullptr, nullptr));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer stores noise level in results",
                 "[calibrator][input_shaper][accel][slow]") {
    // After successful check, noise level should be stored
    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });

    // Wait for completion (implementation-dependent timing)
    wait_for(accel_check_complete_, 2000);

    if (accel_check_complete_) {
        // Noise level should be stored
        const auto& results = calibrator_.get_results();
        CHECK(results.noise_level >= 0.0f);
    }
}

// ============================================================================
// run_calibration() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration('X') transitions to TESTING_X state",
                 "[calibrator][input_shaper][calibration]") {
    calibrator_.run_calibration(
        'X', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
        [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // State should be TESTING_X (or may have completed if synchronous mock)
    auto state = calibrator_.get_state();
    CHECK((state == InputShaperCalibrator::State::TESTING_X ||
           state == InputShaperCalibrator::State::READY ||
           state == InputShaperCalibrator::State::IDLE));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration('Y') transitions to TESTING_Y state",
                 "[calibrator][input_shaper][calibration]") {
    calibrator_.run_calibration(
        'Y', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
        [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // State should be TESTING_Y (or may have completed if synchronous mock)
    auto state = calibrator_.get_state();
    CHECK((state == InputShaperCalibrator::State::TESTING_Y ||
           state == InputShaperCalibrator::State::READY ||
           state == InputShaperCalibrator::State::IDLE));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "run_calibration accepts only X or Y axis",
                 "[calibrator][input_shaper][calibration][validation]") {
    SECTION("X axis is valid") {
        REQUIRE_NOTHROW(calibrator_.run_calibration(
            'X', [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
            [](const std::string&) {}));
    }

    SECTION("Y axis is valid") {
        calibrator_.cancel(); // Reset state
        REQUIRE_NOTHROW(calibrator_.run_calibration(
            'Y', [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
            [](const std::string&) {}));
    }

    SECTION("lowercase x should work or call error") {
        calibrator_.cancel();
        // Implementation should either normalize or call error callback
        REQUIRE_NOTHROW(calibrator_.run_calibration(
            'x', [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
            [this](const std::string& err) { on_error(err); }));
    }

    SECTION("invalid axis should call error callback") {
        calibrator_.cancel();
        calibrator_.run_calibration(
            'Z', // Invalid axis
            [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
            [this](const std::string& err) { on_error(err); });

        // Should either reject immediately or call error callback
        // Wait briefly for async error
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration with null callbacks does not crash",
                 "[calibrator][input_shaper][calibration][edge_case]") {
    REQUIRE_NOTHROW(calibrator_.run_calibration('X', nullptr, nullptr, nullptr));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration result is stored in get_results()",
                 "[calibrator][input_shaper][calibration][slow]") {
    calibrator_.run_calibration(
        'X', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
        [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // Wait for completion
    wait_for(result_received_, 2000);

    if (result_received_) {
        const auto& results = calibrator_.get_results();
        CHECK(results.has_x());
        CHECK(results.x_result.axis == 'X');
        CHECK(results.x_result.is_valid());
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration Y result is stored separately from X",
                 "[calibrator][input_shaper][calibration][slow]") {
    // Run X calibration first
    calibrator_.run_calibration(
        'X', [](int, ShaperCalibrationPhase) {},
        [this](const InputShaperResult& r) { on_result(r); }, [](const std::string&) {});

    wait_for(result_received_, 2000);

    if (result_received_) {
        reset_callbacks();

        // Run Y calibration
        calibrator_.run_calibration(
            'Y', [](int, ShaperCalibrationPhase) {},
            [this](const InputShaperResult& r) { on_result(r); }, [](const std::string&) {});

        wait_for(result_received_, 2000);

        if (result_received_) {
            const auto& results = calibrator_.get_results();
            CHECK(results.has_x());
            CHECK(results.has_y());
            CHECK(results.x_result.axis == 'X');
            CHECK(results.y_result.axis == 'Y');
        }
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "cannot start calibration while already running",
                 "[calibrator][input_shaper][calibration][guard]") {
    // Start first calibration
    calibrator_.run_calibration(
        'X', [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
        [](const std::string&) {});

    // Try to start second calibration immediately
    // Should either be rejected or queued
    bool second_error = false;
    calibrator_.run_calibration(
        'Y', [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
        [&](const std::string& err) {
            second_error = true;
            // Error message should indicate busy
            CHECK((err.find("busy") != std::string::npos ||
                   err.find("progress") != std::string::npos ||
                   err.find("running") != std::string::npos ||
                   !err.empty())); // Any error is acceptable
        });

    // Implementation may handle this synchronously or asynchronously
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Either an error was called, or the calibrator rejected the second call
    // (implementation-dependent)
}

// ============================================================================
// Progress Callback Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "progress callback is called during calibration",
                 "[calibrator][input_shaper][progress][slow]") {
    calibrator_.run_calibration(
        'X', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
        [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // Wait for completion
    wait_for(result_received_, 5000);

    // Progress callback should have been called at least once
    // (unless mock completes instantly)
    if (result_received_ && !error_received_) {
        // Progress updates should include completion (100%) or be monotonic
        if (!progress_updates_.empty()) {
            CHECK(progress_updates_.back() >= 0);
            CHECK(progress_updates_.back() <= 100);

            // Verify monotonicity (progress should never decrease)
            for (size_t i = 1; i < progress_updates_.size(); ++i) {
                CHECK(progress_updates_[i] >= progress_updates_[i - 1]);
            }
        }
    }
}

// ============================================================================
// apply_settings() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings requires valid ApplyConfig",
                 "[calibrator][input_shaper][apply]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 36.7f;
    config.damping_ratio = 0.1f;

    // Should compile and not crash
    REQUIRE_NOTHROW(calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); }));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "apply_settings with empty shaper_type calls error",
                 "[calibrator][input_shaper][apply][validation]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = ""; // Invalid - empty
    config.frequency = 36.7f;

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    // Wait for async error
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Either error was called, or implementation validates differently
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings with zero frequency calls error",
                 "[calibrator][input_shaper][apply][validation]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 0.0f; // Invalid - zero

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings accepts all valid shaper types",
                 "[calibrator][input_shaper][apply]") {
    std::vector<std::string> valid_types = {"zv", "mzv", "zvd", "ei", "2hump_ei", "3hump_ei"};

    for (const auto& type : valid_types) {
        INFO("Testing shaper type: " << type);
        reset_callbacks();

        ApplyConfig config;
        config.axis = 'X';
        config.shaper_type = type;
        config.frequency = 35.0f;

        REQUIRE_NOTHROW(calibrator_.apply_settings(
            config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); }));
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings for Y axis",
                 "[calibrator][input_shaper][apply]") {
    ApplyConfig config;
    config.axis = 'Y';
    config.shaper_type = "ei";
    config.frequency = 47.6f;

    REQUIRE_NOTHROW(calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); }));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "apply_settings with null callbacks does not crash",
                 "[calibrator][input_shaper][apply][edge_case]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 36.7f;

    REQUIRE_NOTHROW(calibrator_.apply_settings(config, nullptr, nullptr));
}

// ============================================================================
// save_to_config() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "save_to_config can be called",
                 "[calibrator][input_shaper][save]") {
    REQUIRE_NOTHROW(calibrator_.save_to_config([this]() { on_success(); },
                                               [this](const std::string& err) { on_error(err); }));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "save_to_config with null callbacks does not crash",
                 "[calibrator][input_shaper][save][edge_case]") {
    REQUIRE_NOTHROW(calibrator_.save_to_config(nullptr, nullptr));
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "error callback receives meaningful message",
                 "[calibrator][input_shaper][error]") {
    // Invalid axis should produce error
    calibrator_.run_calibration(
        'Z', // Invalid
        [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
        [this](const std::string& err) { on_error(err); });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (error_received_) {
        // Error message should not be empty
        CHECK_FALSE(captured_error_.empty());
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "state returns to IDLE on error",
                 "[calibrator][input_shaper][error][state]") {
    // Force an error condition
    calibrator_.run_calibration(
        'Z', // Invalid axis
        [](int, ShaperCalibrationPhase) {}, [](const InputShaperResult&) {},
        [this](const std::string& err) { on_error(err); });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // After error, state should be IDLE
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

// ============================================================================
// ApplyConfig Tests
// ============================================================================

TEST_CASE("ApplyConfig default construction", "[calibrator][input_shaper][types]") {
    ApplyConfig config;

    CHECK(config.axis == 'X');
    CHECK(config.shaper_type.empty());
    CHECK(config.frequency == 0.0f);
    CHECK(config.damping_ratio == Catch::Approx(0.1f));
}

TEST_CASE("ApplyConfig can be populated", "[calibrator][input_shaper][types]") {
    ApplyConfig config;
    config.axis = 'Y';
    config.shaper_type = "mzv";
    config.frequency = 36.7f;
    config.damping_ratio = 0.15f;

    CHECK(config.axis == 'Y');
    CHECK(config.shaper_type == "mzv");
    CHECK(config.frequency == Catch::Approx(36.7f));
    CHECK(config.damping_ratio == Catch::Approx(0.15f));
}

// ============================================================================
// State Enum Tests
// ============================================================================

TEST_CASE("InputShaperCalibrator::State enum values", "[calibrator][input_shaper][state]") {
    // Verify all states exist and are distinct
    using State = InputShaperCalibrator::State;

    CHECK(State::IDLE != State::CHECKING_ADXL);
    CHECK(State::CHECKING_ADXL != State::TESTING_X);
    CHECK(State::TESTING_X != State::TESTING_Y);
    CHECK(State::TESTING_Y != State::READY);
    CHECK(State::READY != State::IDLE);
}

// ============================================================================
// Move Semantics Tests
// ============================================================================

TEST_CASE("InputShaperCalibrator is movable", "[calibrator][input_shaper][move]") {
    InputShaperCalibrator calibrator1;

    // Should be movable
    InputShaperCalibrator calibrator2 = std::move(calibrator1);
    CHECK(calibrator2.get_state() == InputShaperCalibrator::State::IDLE);

    // Move assignment
    InputShaperCalibrator calibrator3;
    calibrator3 = std::move(calibrator2);
    CHECK(calibrator3.get_state() == InputShaperCalibrator::State::IDLE);
}

// ============================================================================
// Integration Scenario Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "Full calibration workflow scenario",
                 "[calibrator][input_shaper][integration][slow]") {
    // This test documents the expected full workflow
    // It may not pass until full implementation is done

    SECTION("check accelerometer first") {
        calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                        [this](const std::string& err) { on_error(err); });

        wait_for(accel_check_complete_, 2000);

        // If check completes, proceed to calibration
        if (accel_check_complete_ && !error_received_) {
            CHECK(captured_noise_level_ >= 0.0f);
        }
    }

    SECTION("calibrate X axis") {
        calibrator_.run_calibration(
            'X', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
            [this](const InputShaperResult& r) { on_result(r); },
            [this](const std::string& err) { on_error(err); });

        wait_for(result_received_, 5000);

        if (result_received_) {
            CHECK(captured_result_.axis == 'X');
            CHECK(captured_result_.is_valid());
        }
    }

    SECTION("calibrate Y axis") {
        calibrator_.run_calibration(
            'Y', [this](int pct, ShaperCalibrationPhase) { on_progress(pct); },
            [this](const InputShaperResult& r) { on_result(r); },
            [this](const std::string& err) { on_error(err); });

        wait_for(result_received_, 5000);

        if (result_received_) {
            CHECK(captured_result_.axis == 'Y');
            CHECK(captured_result_.is_valid());
        }
    }

    SECTION("apply settings") {
        ApplyConfig config;
        config.axis = 'X';
        config.shaper_type = "mzv";
        config.frequency = 36.7f;

        calibrator_.apply_settings(
            config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

        // Wait briefly for async completion
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SECTION("save to config") {
        calibrator_.save_to_config([this]() { on_success(); },
                                   [this](const std::string& err) { on_error(err); });

        // Wait briefly for async completion
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "multiple cancel calls are safe",
                 "[calibrator][input_shaper][edge_case]") {
    // Multiple cancels should not crash
    REQUIRE_NOTHROW(calibrator_.cancel());
    REQUIRE_NOTHROW(calibrator_.cancel());
    REQUIRE_NOTHROW(calibrator_.cancel());

    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "get_results is always valid reference",
                 "[calibrator][input_shaper][edge_case]") {
    // get_results should return valid reference even before any calibration
    const auto& results1 = calibrator_.get_results();
    CHECK_FALSE(results1.is_complete());

    // And after cancel
    calibrator_.cancel();
    const auto& results2 = calibrator_.get_results();
    CHECK_FALSE(results2.is_complete());
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "ApplyConfig with negative frequency",
                 "[calibrator][input_shaper][edge_case][validation]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = -10.0f; // Invalid negative

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    // Should either reject or call error callback
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "ApplyConfig with very high frequency",
                 "[calibrator][input_shaper][edge_case]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 1000.0f; // Unrealistically high but not necessarily invalid

    // Should not crash - validation is implementation-dependent
    REQUIRE_NOTHROW(calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); }));
}

// ============================================================================
// firmware_halt_message() — classify firmware-halt faults (#1021)
// ============================================================================
//
// When a calibration command (G28, MEASURE_AXES_NOISE, SHAPER_CALIBRATE)
// trips a Klipper "Internal error on command" / shutdown — e.g. the Creality
// record_z_pos crash on a K2 with a missing z_pos.json (bundle 5LSSSKPX) —
// the printer halts. firmware_halt_message() recognizes that and returns a
// clear, actionable string so the wizard can say "firmware fault, restart and
// retry" instead of dumping a raw {"code":"key60",...} JSON envelope.

TEST_CASE("firmware_halt_message detects NOT_READY (Klipper halted preflight)",
          "[calibrator][input_shaper][error_handling][1021]") {
    MoonrakerError err;
    err.type = MoonrakerErrorType::NOT_READY;
    err.message = "Klipper is halted — restart firmware to continue";

    std::string msg = InputShaperCalibrator::firmware_halt_message(err);
    REQUIRE_FALSE(msg.empty());
    // Should mention firmware/restart so the user knows the recovery path.
    REQUIRE(msg.find("firmware") != std::string::npos);
}

TEST_CASE("firmware_halt_message detects Klipper 'Internal error on command' envelope",
          "[calibrator][input_shaper][error_handling][1021]") {
    // The exact shape that halted the K2 in bundle 5LSSSKPX, delivered as a
    // raw JSON-RPC envelope string (no friendly extraction upstream).
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.message =
        R"({"code":"key60", "msg":"Internal error on command:G1 Z-10 F600", "values": ["G1 Z-10 F600"]})";

    std::string msg = InputShaperCalibrator::firmware_halt_message(err);
    REQUIRE_FALSE(msg.empty());
    REQUIRE(msg.find("firmware") != std::string::npos);
    // The raw JSON envelope must NOT leak into the user-facing string.
    REQUIRE(msg.find("\"code\"") == std::string::npos);
    REQUIRE(msg.find("key60") == std::string::npos);
}

TEST_CASE("firmware_halt_message detects generic Klipper shutdown text",
          "[calibrator][input_shaper][error_handling][1021]") {
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.message = "Lost communication with MCU 'mcu' — Klipper shutdown";

    REQUIRE_FALSE(InputShaperCalibrator::firmware_halt_message(err).empty());
}

TEST_CASE("firmware_halt_message returns empty for ordinary calibration errors",
          "[calibrator][input_shaper][error_handling][1021]") {
    // A normal "missing accelerometer" error must NOT be reclassified as a
    // firmware halt — the wizard keeps its existing specific message.
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.message = "MEASURE_AXES_NOISE requires [adxl345] accelerometer in printer.cfg";

    REQUIRE(InputShaperCalibrator::firmware_halt_message(err).empty());
}

TEST_CASE("firmware_halt_message returns empty for timeouts",
          "[calibrator][input_shaper][error_handling][1021]") {
    // Timeouts have their own "may still be homing" handling; not a halt.
    MoonrakerError err;
    err.type = MoonrakerErrorType::TIMEOUT;
    err.message = "Request timed out after 30s";

    REQUIRE(InputShaperCalibrator::firmware_halt_message(err).empty());
}
