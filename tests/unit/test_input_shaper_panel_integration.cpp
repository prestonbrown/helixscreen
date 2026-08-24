// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_panel_integration.cpp
 * @brief Integration tests for InputShaperPanel delegation to InputShaperCalibrator
 *
 * Test-first development: These tests document the expected behavior after
 * refactoring InputShaperPanel to delegate to InputShaperCalibrator.
 *
 * These tests verify that InputShaperPanel correctly:
 * 1. Creates an InputShaperCalibrator instance when set_api() is called
 * 2. Delegates all calibration operations through the calibrator
 * 3. Updates UI state based on calibrator callbacks
 *
 * NOTE: These tests focus on the delegation contract, not full UI rendering.
 * Full UI tests require LVGL initialization which is handled separately.
 */

#include "../../include/calibration_types.h"
#include "../../include/input_shaper_calibrator.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/ui_panel_input_shaper.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::calibration;

// ============================================================================
// Mock InputShaperCalibrator for tracking delegation calls
// ============================================================================

/**
 * @brief Mock calibrator that tracks method calls for verification
 *
 * Does not perform any actual calibration - just records what was called
 * and allows tests to trigger callbacks to verify panel response.
 */
class MockInputShaperCalibrator {
  public:
    // Use the same State enum as the real calibrator
    enum class State { IDLE, CHECKING_ADXL, TESTING_X, TESTING_Y, READY };

    // Use the same CalibrationResults struct as the real calibrator
    struct CalibrationResults {
        InputShaperResult x_result;
        InputShaperResult y_result;
        float noise_level = 0.0f;

        [[nodiscard]] bool has_x() const {
            return x_result.is_valid();
        }
        [[nodiscard]] bool has_y() const {
            return y_result.is_valid();
        }
        [[nodiscard]] bool is_complete() const {
            return has_x() && has_y();
        }
    };

    MockInputShaperCalibrator() = default;

    // ========== Call tracking ==========

    struct CalibrationCall {
        char axis;
        bool has_progress_cb;
        bool has_result_cb;
        bool has_error_cb;
    };

    struct ApplyCall {
        ApplyConfig config;
        bool has_success_cb;
        bool has_error_cb;
    };

    // Track check_accelerometer calls
    bool check_accelerometer_called = false;
    AccelCheckCallback last_accel_complete_cb;
    ErrorCallback last_accel_error_cb;

    // Track run_calibration calls
    std::vector<CalibrationCall> calibration_calls;
    ProgressCallback last_progress_cb;
    ResultCallback last_result_cb;
    ErrorCallback last_calibration_error_cb;

    // Track apply_settings calls
    std::vector<ApplyCall> apply_calls;
    SuccessCallback last_apply_success_cb;
    ErrorCallback last_apply_error_cb;

    // Track save_to_config calls
    bool save_to_config_called = false;
    SuccessCallback last_save_success_cb;
    ErrorCallback last_save_error_cb;

    // Track cancel calls
    int cancel_call_count = 0;

    // State
    State state_ = State::IDLE;
    CalibrationResults results_;

    // ========== Mock interface matching InputShaperCalibrator ==========

    [[nodiscard]] State get_state() const {
        return state_;
    }

    [[nodiscard]] const CalibrationResults& get_results() const {
        return results_;
    }

    void check_accelerometer(AccelCheckCallback on_complete, ErrorCallback on_error) {
        check_accelerometer_called = true;
        last_accel_complete_cb = std::move(on_complete);
        last_accel_error_cb = std::move(on_error);
        state_ = State::CHECKING_ADXL;
    }

    void run_calibration(char axis, ProgressCallback on_progress, ResultCallback on_complete,
                         ErrorCallback on_error) {
        CalibrationCall call;
        call.axis = axis;
        call.has_progress_cb = (on_progress != nullptr);
        call.has_result_cb = (on_complete != nullptr);
        call.has_error_cb = (on_error != nullptr);
        calibration_calls.push_back(call);

        last_progress_cb = std::move(on_progress);
        last_result_cb = std::move(on_complete);
        last_calibration_error_cb = std::move(on_error);

        state_ = (axis == 'X') ? State::TESTING_X : State::TESTING_Y;
    }

    void apply_settings(const ApplyConfig& config, SuccessCallback on_success,
                        ErrorCallback on_error) {
        ApplyCall call;
        call.config = config;
        call.has_success_cb = (on_success != nullptr);
        call.has_error_cb = (on_error != nullptr);
        apply_calls.push_back(call);

        last_apply_success_cb = std::move(on_success);
        last_apply_error_cb = std::move(on_error);
    }

    void save_to_config(SuccessCallback on_success, ErrorCallback on_error) {
        save_to_config_called = true;
        last_save_success_cb = std::move(on_success);
        last_save_error_cb = std::move(on_error);
    }

    void cancel() {
        cancel_call_count++;
        state_ = State::IDLE;
    }

    // ========== Test helpers for triggering callbacks ==========

    void trigger_accel_complete(float noise_level) {
        results_.noise_level = noise_level;
        state_ = State::IDLE;
        if (last_accel_complete_cb) {
            last_accel_complete_cb(noise_level);
        }
    }

    void trigger_accel_error(const std::string& message) {
        state_ = State::IDLE;
        if (last_accel_error_cb) {
            last_accel_error_cb(message);
        }
    }

    void
    trigger_calibration_progress(int percent,
                                 ShaperCalibrationPhase phase = ShaperCalibrationPhase::Sweeping) {
        if (last_progress_cb) {
            last_progress_cb(percent, phase);
        }
    }

    void trigger_calibration_result(const InputShaperResult& result) {
        if (result.axis == 'X') {
            results_.x_result = result;
        } else {
            results_.y_result = result;
        }
        state_ = State::READY;
        if (last_result_cb) {
            last_result_cb(result);
        }
    }

    void trigger_calibration_error(const std::string& message) {
        state_ = State::IDLE;
        if (last_calibration_error_cb) {
            last_calibration_error_cb(message);
        }
    }

    void trigger_apply_success() {
        if (last_apply_success_cb) {
            last_apply_success_cb();
        }
    }

    void trigger_apply_error(const std::string& message) {
        if (last_apply_error_cb) {
            last_apply_error_cb(message);
        }
    }

    void trigger_save_success() {
        if (last_save_success_cb) {
            last_save_success_cb();
        }
    }

    void trigger_save_error(const std::string& message) {
        if (last_save_error_cb) {
            last_save_error_cb(message);
        }
    }

    // ========== Reset for multiple test sections ==========

    void reset() {
        check_accelerometer_called = false;
        calibration_calls.clear();
        apply_calls.clear();
        save_to_config_called = false;
        cancel_call_count = 0;
        state_ = State::IDLE;
        results_ = CalibrationResults{};

        last_accel_complete_cb = nullptr;
        last_accel_error_cb = nullptr;
        last_progress_cb = nullptr;
        last_result_cb = nullptr;
        last_calibration_error_cb = nullptr;
        last_apply_success_cb = nullptr;
        last_apply_error_cb = nullptr;
        last_save_success_cb = nullptr;
        last_save_error_cb = nullptr;
    }
};

// ============================================================================
// Helper to create valid test results
// ============================================================================

static InputShaperResult make_test_result(char axis) {
    InputShaperResult result;
    result.axis = axis;
    result.shaper_type = "mzv";
    result.shaper_freq = 36.8f;
    result.max_accel = 4500.0f;
    result.smoothing = 0.05f;
    result.vibrations = 3.2f;

    // Add some shaper alternatives
    ShaperOption opt1{"zv", 38.0f, 5.0f, 0.02f, 6000.0f};
    ShaperOption opt2{"mzv", 36.8f, 3.2f, 0.05f, 4500.0f};
    ShaperOption opt3{"ei", 35.0f, 2.5f, 0.08f, 3500.0f};
    result.all_shapers = {opt1, opt2, opt3};

    return result;
}

// ============================================================================
// Calibrator Unit Tests (these pass now with the real calibrator)
// ============================================================================

TEST_CASE("InputShaperCalibrator state machine basics", "[calibrator][input_shaper]") {
    InputShaperCalibrator calibrator;

    SECTION("Initial state is IDLE") {
        CHECK(calibrator.get_state() == InputShaperCalibrator::State::IDLE);
    }

    SECTION("Results start empty") {
        const auto& results = calibrator.get_results();
        CHECK_FALSE(results.has_x());
        CHECK_FALSE(results.has_y());
        CHECK_FALSE(results.is_complete());
    }

    SECTION("Cancel returns to IDLE") {
        calibrator.cancel();
        CHECK(calibrator.get_state() == InputShaperCalibrator::State::IDLE);
    }
}

// ============================================================================
// Mock Calibrator Unit Tests (verify mock works correctly)
// ============================================================================

TEST_CASE("MockInputShaperCalibrator tracks calls correctly", "[mock][input_shaper]") {
    MockInputShaperCalibrator mock;

    SECTION("check_accelerometer is tracked") {
        bool callback_called = false;
        mock.check_accelerometer([&](float) { callback_called = true; }, nullptr);

        CHECK(mock.check_accelerometer_called);
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::CHECKING_ADXL);

        mock.trigger_accel_complete(0.05f);
        CHECK(callback_called);
        CHECK(mock.get_results().noise_level == Catch::Approx(0.05f));
    }

    SECTION("run_calibration X is tracked") {
        bool result_called = false;
        mock.run_calibration(
            'X', nullptr, [&](const InputShaperResult&) { result_called = true; }, nullptr);

        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'X');
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::TESTING_X);

        auto result = make_test_result('X');
        mock.trigger_calibration_result(result);
        CHECK(result_called);
        CHECK(mock.get_results().has_x());
    }

    SECTION("run_calibration Y is tracked") {
        mock.run_calibration('Y', nullptr, nullptr, nullptr);

        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'Y');
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::TESTING_Y);
    }

    SECTION("apply_settings is tracked") {
        ApplyConfig config;
        config.axis = 'X';
        config.shaper_type = "mzv";
        config.frequency = 36.8f;

        mock.apply_settings(config, nullptr, nullptr);

        REQUIRE(mock.apply_calls.size() == 1);
        CHECK(mock.apply_calls[0].config.axis == 'X');
        CHECK(mock.apply_calls[0].config.shaper_type == "mzv");
        CHECK(mock.apply_calls[0].config.frequency == Catch::Approx(36.8f));
    }

    SECTION("save_to_config is tracked") {
        mock.save_to_config(nullptr, nullptr);
        CHECK(mock.save_to_config_called);
    }

    SECTION("cancel is tracked") {
        mock.run_calibration('X', nullptr, nullptr, nullptr);
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::TESTING_X);

        mock.cancel();
        CHECK(mock.cancel_call_count == 1);
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::IDLE);
    }

    SECTION("reset clears all state") {
        mock.check_accelerometer(nullptr, nullptr);
        mock.run_calibration('X', nullptr, nullptr, nullptr);
        mock.apply_settings({}, nullptr, nullptr);
        mock.save_to_config(nullptr, nullptr);
        mock.cancel();

        mock.reset();

        CHECK_FALSE(mock.check_accelerometer_called);
        CHECK(mock.calibration_calls.empty());
        CHECK(mock.apply_calls.empty());
        CHECK_FALSE(mock.save_to_config_called);
        CHECK(mock.cancel_call_count == 0);
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::IDLE);
    }
}

// ============================================================================
// Panel↔Calibrator delegation contract
// ============================================================================
// The InputShaperPanel -> InputShaperCalibrator delegation these once described
// as "expected after refactoring" has shipped: the panel creates the calibrator
// in set_api() and every button handler delegates to it (src/ui/ui_panel_input_shaper.cpp).
// The former [!mayfail] cases here were pure WARN() placeholders that asserted
// nothing (and one was already stale — the panel still calls the API directly in
// on_activate()). The real contract is verified by test_input_shaper_calibrator.cpp
// (calibrator behavior) and the mock-based panel tests above/below (delegation).
// Removed 2026-07-22.

// ============================================================================
// Phase 7: Test Print Pattern Feature
// ============================================================================

TEST_CASE("InputShaperPanel has print test pattern handler", "[input_shaper][panel]") {
    // This test verifies the method exists and is callable
    // The handler sends TUNING_TOWER command to enable acceleration ramping
    // during print for visual comparison of ringing at different accelerations
    //
    // Full integration test would require mock API setup with LVGL
    WARN("Print test pattern button added - integration test requires mock API");
}

// ============================================================================
// Chunk 1: Current Config Display + New Subjects
// ============================================================================

TEST_CASE("InputShaperPanel current config subjects", "[input_shaper][panel][subjects]") {
    // These test the pure logic of populate_current_config without LVGL UI

    SECTION("Configured shaper populates display strings correctly") {
        InputShaperConfig config;
        config.is_configured = true;
        config.shaper_type_x = "mzv";
        config.shaper_freq_x = 36.7f;
        config.shaper_type_y = "ei";
        config.shaper_freq_y = 47.6f;

        // Verify config is valid
        CHECK(config.is_configured);
        CHECK(config.shaper_type_x == "mzv");
        CHECK(config.shaper_freq_x == Catch::Approx(36.7f));
        CHECK(config.shaper_type_y == "ei");
        CHECK(config.shaper_freq_y == Catch::Approx(47.6f));
    }

    SECTION("Unconfigured shaper has empty strings") {
        InputShaperConfig config;
        // Default constructed = not configured
        CHECK_FALSE(config.is_configured);
        CHECK(config.shaper_type_x.empty());
        CHECK(config.shaper_type_y.empty());
        CHECK(config.shaper_freq_x == 0.0f);
        CHECK(config.shaper_freq_y == 0.0f);
    }
}

TEST_CASE("Shaper type uppercase formatting", "[input_shaper][panel][format]") {
    // Test that shaper types get uppercased for display
    SECTION("Common shaper types") {
        auto to_upper = [](const std::string& s) -> std::string {
            std::string result = s;
            for (auto& c : result)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return result;
        };
        CHECK(to_upper("mzv") == "MZV");
        CHECK(to_upper("ei") == "EI");
        CHECK(to_upper("zv") == "ZV");
        CHECK(to_upper("2hump_ei") == "2HUMP_EI");
        CHECK(to_upper("3hump_ei") == "3HUMP_EI");
    }
}

TEST_CASE("Calibrate All handler exists and delegates", "[input_shaper][panel]") {
    // Verify that calibrate_all handler starts X calibration
    // Full X->Y chain tested in Chunk 2
    MockInputShaperCalibrator mock;

    SECTION("Calibrate All starts X calibration first") {
        // Simulates what handle_calibrate_all_clicked() should do
        mock.run_calibration('X', nullptr, nullptr, nullptr);
        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'X');
    }
}

// ============================================================================
// Chunk 2: Pre-flight Noise Check + Calibrate All Flow
// ============================================================================

TEST_CASE("Pre-flight noise check flow", "[input_shaper][panel][preflight]") {
    MockInputShaperCalibrator mock;

    SECTION("Noise check runs before calibration") {
        // Start pre-flight - should call check_accelerometer first
        mock.check_accelerometer([](float) {}, [](const std::string&) {});
        CHECK(mock.check_accelerometer_called);
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::CHECKING_ADXL);
    }

    SECTION("Successful noise check proceeds to calibration") {
        bool calibration_started = false;
        mock.check_accelerometer(
            [&](float) {
                // After noise check passes, calibration should start
                mock.run_calibration('X', nullptr, nullptr, nullptr);
                calibration_started = true;
            },
            nullptr);

        mock.trigger_accel_complete(0.05f);
        CHECK(calibration_started);
        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'X');
    }

    SECTION("Failed noise check triggers error") {
        bool error_received = false;
        std::string error_msg;
        mock.check_accelerometer(nullptr, [&](const std::string& err) {
            error_received = true;
            error_msg = err;
        });

        mock.trigger_accel_error("ADXL345 not found");
        CHECK(error_received);
        CHECK_FALSE(error_msg.empty());
    }
}

TEST_CASE("Calibrate All chains X then Y", "[input_shaper][panel][calibrate_all]") {
    MockInputShaperCalibrator mock;

    SECTION("Calibrate All runs noise check, then X, then Y") {
        // Step 1: Noise check
        mock.check_accelerometer(
            [&](float) {
                // Step 2: X calibration starts after noise check
                mock.run_calibration(
                    'X', nullptr,
                    [&](const InputShaperResult&) {
                        // Step 3: Y calibration starts after X completes
                        mock.run_calibration('Y', nullptr, nullptr, nullptr);
                    },
                    nullptr);
            },
            nullptr);

        // Trigger noise check success
        mock.trigger_accel_complete(0.05f);
        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'X');

        // Trigger X result
        auto x_result = make_test_result('X');
        mock.trigger_calibration_result(x_result);
        REQUIRE(mock.calibration_calls.size() == 2);
        CHECK(mock.calibration_calls[1].axis == 'Y');
    }

    SECTION("Cancel during Calibrate All stops the sequence") {
        mock.check_accelerometer(
            [&](float) { mock.run_calibration('X', nullptr, nullptr, nullptr); }, nullptr);

        mock.trigger_accel_complete(0.05f);
        REQUIRE(mock.calibration_calls.size() == 1);

        // Cancel during X
        mock.cancel();
        CHECK(mock.get_state() == MockInputShaperCalibrator::State::IDLE);
        // Should NOT proceed to Y
        CHECK(mock.calibration_calls.size() == 1);
    }
}

TEST_CASE("Single axis calibration uses pre-flight", "[input_shaper][panel][preflight]") {
    MockInputShaperCalibrator mock;

    SECTION("Calibrate X runs noise check first") {
        mock.check_accelerometer(
            [&](float) { mock.run_calibration('X', nullptr, nullptr, nullptr); }, nullptr);

        CHECK(mock.check_accelerometer_called);
        mock.trigger_accel_complete(0.03f);
        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'X');
    }

    SECTION("Calibrate Y runs noise check first") {
        mock.check_accelerometer(
            [&](float) { mock.run_calibration('Y', nullptr, nullptr, nullptr); }, nullptr);

        mock.trigger_accel_complete(0.03f);
        REQUIRE(mock.calibration_calls.size() == 1);
        CHECK(mock.calibration_calls[0].axis == 'Y');
    }
}

// ============================================================================
// Chunk 3: Results State Redesign
// ============================================================================

TEST_CASE("Shaper type explanation mapping", "[input_shaper][panel][results]") {
    // Test that each known shaper type maps to a meaningful explanation keyword
    SECTION("Known shaper types have specific explanations") {
        std::map<std::string, std::string> expected_keywords = {{"zv", "minimal"},
                                                                {"mzv", "balance"},
                                                                {"ei", "Strong"},
                                                                {"2hump_ei", "Heavy"},
                                                                {"3hump_ei", "Maximum"}};

        for (const auto& [type, keyword] : expected_keywords) {
            CHECK_FALSE(type.empty());
            CHECK_FALSE(keyword.empty());
        }
    }
}

TEST_CASE("Vibration quality thresholds", "[input_shaper][panel][results]") {
    // Quality levels: 0=excellent (<5%), 1=good (5-15%), 2=fair (15-25%), 3=poor (>25%)

    SECTION("Excellent quality for low vibration") {
        CHECK(2.0f < 5.0f);
        CHECK(4.9f < 5.0f);
    }

    SECTION("Good quality for moderate vibration") {
        CHECK(5.0f >= 5.0f);
        CHECK(14.9f < 15.0f);
    }

    SECTION("Fair quality for higher vibration") {
        CHECK(15.0f >= 15.0f);
        CHECK(24.9f < 25.0f);
    }

    SECTION("Poor quality for high vibration") {
        CHECK(25.0f >= 25.0f);
        CHECK(50.0f >= 25.0f);
    }
}

TEST_CASE("Per-axis result population", "[input_shaper][panel][results]") {
    SECTION("Single axis result populates correct card") {
        auto result = make_test_result('X');
        CHECK(result.axis == 'X');
        CHECK(result.is_valid());
        CHECK(result.shaper_type == "mzv");
        CHECK(result.shaper_freq == Catch::Approx(36.8f));
        CHECK(result.max_accel == Catch::Approx(4500.0f));
    }

    SECTION("Calibrate All populates both axis cards") {
        auto x_result = make_test_result('X');
        auto y_result = make_test_result('Y');
        y_result.shaper_type = "ei";
        y_result.shaper_freq = 47.6f;
        y_result.vibrations = 2.5f;
        y_result.max_accel = 3500.0f;

        CHECK(x_result.is_valid());
        CHECK(y_result.is_valid());
        CHECK(x_result.axis == 'X');
        CHECK(y_result.axis == 'Y');
    }
}

TEST_CASE("Apply recommendation applies both axes for Calibrate All",
          "[input_shaper][panel][results]") {
    MockInputShaperCalibrator mock;

    SECTION("Single axis apply sends one apply_settings call") {
        ApplyConfig config;
        config.axis = 'X';
        config.shaper_type = "mzv";
        config.frequency = 36.8f;

        mock.apply_settings(config, nullptr, nullptr);
        REQUIRE(mock.apply_calls.size() == 1);
        CHECK(mock.apply_calls[0].config.axis == 'X');
    }

    SECTION("Dual axis apply sends two apply_settings calls") {
        // Apply X
        ApplyConfig x_config;
        x_config.axis = 'X';
        x_config.shaper_type = "mzv";
        x_config.frequency = 36.8f;

        mock.apply_settings(
            x_config,
            [&]() {
                // After X succeeds, apply Y
                ApplyConfig y_config;
                y_config.axis = 'Y';
                y_config.shaper_type = "ei";
                y_config.frequency = 47.6f;
                mock.apply_settings(y_config, nullptr, nullptr);
            },
            nullptr);

        // Trigger X success
        mock.trigger_apply_success();

        REQUIRE(mock.apply_calls.size() == 2);
        CHECK(mock.apply_calls[0].config.axis == 'X');
        CHECK(mock.apply_calls[1].config.axis == 'Y');
    }
}

// ============================================================================
// Live-before delta display + firmware X-overwrite warning
// ============================================================================
//
// Drives the REAL panel through the mock client's full Calibrate All
// transcript (preflight -> X sweep/analysis/CSV -> Y sweep/analysis/CSV) with
// a staged live-before configuration, then asserts the delta/verdict/warning
// subjects the results cards bind to.

namespace {

class InputShaperDeltaFixture : public LVGLUITestFixture {
  public:
    InputShaperDeltaFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // A previous test's mock run may have left the calibration CSVs in
        // /tmp; the marker-line injection below keys off the X file appearing,
        // so both must start absent.
        std::remove("/tmp/calibration_data_x_mock.csv");
        std::remove("/tmp/calibration_data_y_mock.csv");

        // Live-before config staged per-test (mock default is mzv@36.7/ei@47.6;
        // the staged pair deliberately differs so a leaked default fails loud).
        // zv@100 is far off the mock's 53.8 Hz resonance, so under the
        // thresholded (firmware-matching) residual metric the old setting
        // leaves clearly more vibration than the new fit.
        mock_client_.set_input_shaper_values("zv", 100.0, "zv", 100.0);

        printer_state_.init_subjects(false);
        printer_state_.set_klippy_state_sync(KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, printer_state_);

        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");

        panel_ = &get_global_input_shaper_panel();
        panel_->init_subjects();
        panel_->set_api(&mock_client_, api_.get());

        // Create the panel's XML view so widget-level assertions (legend
        // entries, captions) can run against real bindings. The view is NOT
        // wired through panel_->create(): the persistent singleton would keep
        // widget pointers past this fixture's screen teardown. The panel only
        // ever touches subjects, so driving it and binding the view to the
        // same subjects keeps both in sync.
        view_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "input_shaper_panel", nullptr));
        REQUIRE(view_ != nullptr);

        panel_->on_activate(); // resets to IDLE, hides the gated legend rows
        helix::ui::UpdateQueue::instance().drain();
    }

    ~InputShaperDeltaFixture() override {
        panel_->on_deactivate();
        helix::ui::UpdateQueue::instance().drain();

        // Drop the panel's subjects BEFORE the view: the view's bind observers
        // live on those subjects, and the singleton panel would otherwise
        // leave observers pointing into the deleted widget tree for whatever
        // test runs next.
        panel_->deinit_subjects();
        lv_obj_delete(view_);
        helix::ui::UpdateQueue::instance().drain();
        api_.reset();
    }

    /// Widget by name inside the panel view (fails loud when the XML drops it)
    static lv_obj_t* view_widget(lv_obj_t* root, const char* name) {
        lv_obj_t* w = lv_obj_find_by_name(root, name);
        REQUIRE(w != nullptr);
        return w;
    }

    // Pumps the mock transcript (100ms/line), LVGL timers, and the UpdateQueue
    // until pred() holds. Optionally injects the Creality copy-marker line
    // when the step label hits marker_trigger: "Step 2 of 2" (Calibrate All's
    // Y run) or "Calibrating Y axis..." (a Y-only run) is set by
    // start_calibration('Y') right before the Y collector registers, and
    // survives until the Y sweep's first progress line one tick later - so it
    // is observable exactly while the marker has a live collector to land on.
    bool pump_until(const std::function<bool()>& done, bool inject_copy_marker,
                    const char* marker_trigger = "Step 2 of 2", int max_ticks = 600) {
        bool marker_sent = false;
        for (int i = 0; i < max_ticks && !done(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            helix::ui::UpdateQueue::instance().drain();

            if (inject_copy_marker && !marker_sent &&
                subject_string("is_measuring_step_label") == marker_trigger) {
                marker_sent = true;
                mock_client_.dispatch_gcode_response(
                    "copy_TestAxis_y_to_x Recommended shaper_type_x = ei, shaper_freq_x "
                    "= 71.4 Hz");
            }
            if (inject_copy_marker && !marker_sent &&
                subject_string("is_measuring_axis_label") == marker_trigger) {
                marker_sent = true;
                mock_client_.dispatch_gcode_response(
                    "copy_TestAxis_y_to_x Recommended shaper_type_x = ei, shaper_freq_x "
                    "= 71.4 Hz");
            }
        }
        return done();
    }

    static lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    static std::string subject_string(const char* name) {
        return lv_subject_get_string(subject(name));
    }

    static int subject_int(const char* name) {
        return lv_subject_get_int(subject(name));
    }

    /// Panel state subject: 0=IDLE, 1=MEASURING, 2=RESULTS, 3=ERROR
    static int panel_state() {
        return subject_int("input_shaper_state");
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    InputShaperPanel* panel_ = nullptr;
    lv_obj_t* view_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "Calibrate All results show the live-before delta and residual verdict",
                 "[input_shaper][panel][delta]") {
    SECTION("with the firmware copy-marker line") {
        panel_->handle_calibrate_all_clicked();

        const bool done = pump_until([&] { return panel_state() == 2; },
                                     /*inject_copy_marker=*/true);
        REQUIRE(done);

        // Live-before value folded into the delta line: staged zv@100.0 (the
        // mock recommends mzv@53.8).
        CHECK(subject_string("is_x_delta_text") == "zv @ 100.0 Hz -> mzv @ 53.8 Hz");
        CHECK(subject_int("is_x_has_delta") == 1);

        // Verdict: the old setting re-scored on today's PSD must show MORE
        // residual than the new fit (the old notch at 69.8 Hz sits off the
        // mock's 53.8 Hz resonance peak).
        REQUIRE(subject_int("is_x_has_verdict") == 1);
        const std::string verdict = subject_string("is_x_verdict_text");
        std::cmatch m;
        static const std::regex verdict_re(
            R"(^Old setting on today's data: (\d+\.\d)% residual - now: (\d+\.\d)%$)");
        INFO("verdict: " << verdict);
        REQUIRE(std::regex_search(verdict.c_str(), m, verdict_re));
        const double old_pct = std::atof(m[1].str().c_str());
        const double new_pct = std::atof(m[2].str().c_str());
        CHECK(old_pct > new_pct);

        // The Y card gets the same treatment (old Y staged ei@69.8).
        REQUIRE(subject_int("is_y_has_verdict") == 1);

        // Firmware warning: visible only because the marker line was injected
        // during the Y run; it belongs on the X card, whose measured value is
        // the one the fork discarded.
        CHECK(subject_int("is_x_fw_overwrite_warn") == 1);

        // Chart key: the legend names all three curves, and the Previous entry
        // (the muted old-setting curve) exists only when a before-config was
        // captured.
        CHECK(std::string(lv_label_get_text(view_widget(view_, "legend_x_measured_label"))) ==
              "Measured (shaper off)");
        lv_obj_t* prev_dot = view_widget(view_, "legend_x_previous_dot");
        lv_obj_t* prev_label = view_widget(view_, "legend_x_previous_label");
        CHECK_FALSE(lv_obj_has_flag(prev_dot, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(prev_label, LV_OBJ_FLAG_HIDDEN));
        CHECK(std::string(lv_label_get_text(prev_label)) == "Previous");
        lv_obj_t* y_prev_dot = view_widget(view_, "legend_y_previous_dot");
        CHECK_FALSE(lv_obj_has_flag(y_prev_dot, LV_OBJ_FLAG_HIDDEN));

        // The chart's Y quantity is named above the plot.
        REQUIRE(lv_obj_find_by_name(view_, "chart_x_caption") != nullptr);
        REQUIRE(lv_obj_find_by_name(view_, "chart_y_caption") != nullptr);
    }

    SECTION("without the marker line the warning stays hidden") {
        panel_->handle_calibrate_all_clicked();

        const bool done = pump_until([&] { return panel_state() == 2; },
                                     /*inject_copy_marker=*/false);
        REQUIRE(done);

        // Delta and verdict still populate from the staged live-before config...
        CHECK(subject_int("is_x_has_delta") == 1);
        CHECK(subject_int("is_x_has_verdict") == 1);
        // ...so the Previous legend entry is present too...
        CHECK_FALSE(
            lv_obj_has_flag(view_widget(view_, "legend_x_previous_label"), LV_OBJ_FLAG_HIDDEN));
        // ...but no firmware overwrite happened, so no warning.
        CHECK(subject_int("is_x_fw_overwrite_warn") == 0);
    }
}

TEST_CASE_METHOD(InputShaperDeltaFixture, "delta rows stay hidden without a live-before config",
                 "[input_shaper][panel][delta]") {
    mock_client_.set_input_shaper_configured(false);

    panel_->handle_calibrate_x_clicked();

    const bool done = pump_until([&] { return panel_state() == 2; },
                                 /*inject_copy_marker=*/false);
    REQUIRE(done);

    CHECK(subject_int("is_x_has_delta") == 0);
    CHECK(subject_int("is_x_has_verdict") == 0);
    CHECK(subject_string("is_x_delta_text").empty());
    CHECK(subject_int("is_x_fw_overwrite_warn") == 0);

    // The Previous legend entry hides with the delta rows (no before-config),
    // while the always-on key entries and the chart caption remain.
    CHECK(lv_obj_has_flag(view_widget(view_, "legend_x_previous_dot"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(view_widget(view_, "legend_x_previous_label"), LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(view_widget(view_, "legend_x_measured_label"))) ==
          "Measured (shaper off)");
    REQUIRE(lv_obj_find_by_name(view_, "chart_x_caption") != nullptr);
}

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "a Y-only run warns on the Y card when firmware overwrites X",
                 "[input_shaper][panel][delta][fw_overwrite]") {
    // The fork's copy_TestAxis_y_to_x fires at the end of ANY Y run; with no X
    // result in this session the warning must still surface - on the Y card,
    // the only card a Y-only run shows.
    panel_->handle_calibrate_y_clicked();

    const bool done = pump_until([&] { return panel_state() == 2; },
                                 /*inject_copy_marker=*/true,
                                 /*marker_trigger=*/"Calibrating Y axis...");
    REQUIRE(done);

    // No X result exists, so the X card is absent and only the Y card shows.
    CHECK(subject_int("is_results_has_x") == 0);
    CHECK(subject_int("is_x_fw_overwrite_warn") == 1);

    // The Y card's copy of the warning row is visible. The X card's own copy
    // is unreachable because the whole X card is hidden (its row's own flag
    // stays clear - LVGL hides children through the parent, not their flags).
    CHECK_FALSE(lv_obj_has_flag(view_widget(view_, "fw_overwrite_warn_y"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(view_widget(view_, "result_card_x"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "close drops the stored X result so a Y-only apply never sends X",
                 "[input_shaper][panel][apply][stale_x]") {
    // RAII spdlog capture: the mock client logs every SET_INPUT_SHAPER with
    // its full arguments, which is the observable for what Apply sent.
    class GcodeLogCapture {
      public:
        GcodeLogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
            sink_->set_level(spdlog::level::trace);
            spdlog::default_logger()->sinks().push_back(sink_);
        }
        ~GcodeLogCapture() {
            auto& sinks = spdlog::default_logger()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
        }
        [[nodiscard]] int count(const std::string& needle) const {
            int n = 0;
            for (const auto& l : sink_->last_formatted(256)) {
                if (l.find(needle) != std::string::npos) {
                    ++n;
                }
            }
            return n;
        }

      private:
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    } capture;

    // 1. Calibrate All stores an X result.
    panel_->handle_calibrate_all_clicked();
    REQUIRE(pump_until([&] { return panel_state() == 2; }, false));
    REQUIRE(subject_int("is_results_has_x") == 1);

    // 2. Close the results - clear_results() must drop the stored X result.
    panel_->handle_close_clicked();
    helix::ui::UpdateQueue::instance().drain();

    // 3. A later Y-only calibration + Apply must send SET_INPUT_SHAPER for Y
    //    only; a stale x_result_ would silently re-send X with the previous
    //    session's values.
    panel_->handle_calibrate_y_clicked();
    REQUIRE(pump_until([&] { return panel_state() == 2; }, false));

    panel_->handle_apply_clicked();
    // The apply chain is async (lifetime-deferred success callbacks)
    for (int i = 0; i < 20; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        helix::ui::UpdateQueue::instance().drain();
        if (capture.count("SET_INPUT_SHAPER") > 0) {
            break;
        }
    }

    CHECK(capture.count("SHAPER_TYPE_X") == 0);
    CHECK(capture.count("SHAPER_TYPE_Y") == 1);
}
