// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_calibrator.cpp
 * @brief Unit tests for InputShaperCalibrator's calibration state machine
 *
 * The calibrator is driven through a real MoonrakerAPI backed by
 * MoonrakerClientMock, so every case exercises the state machine instead of the
 * "no API available" early returns (src/calibration/input_shaper_calibrator.cpp
 * :102, :174, :305, :336). A default-constructed calibrator leaves api_ null and
 * makes those cases assert nothing at all.
 *
 * ensure_homed_then() (src/printer/toolhead_homing.cpp:38) reads the GLOBAL
 * PrinterState's homed_axes subject, so the fixture seeds it with "xyz": the
 * continuation then runs synchronously and the calibrator reaches the API on the
 * caller's thread. Cases that need to observe a state the calibrator only holds
 * while a command is outstanding (CHECKING_ADXL) call unhome() first — the G28
 * ensure_homed_then() then sends is held by HomingGateAPI until the test
 * releases or fails it.
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
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_error.h"
#include "../../include/printer_state.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::calibration;

namespace {

/**
 * @brief MoonrakerAPI over the mock client that HOLDS the G28 ensure_homed_then() sends
 *
 * Everything except "G28" passes straight through to the real implementation,
 * so the mock's SHAPER_CALIBRATE / MEASURE_AXES_NOISE / SET_INPUT_SHAPER
 * transcripts still drive the calibrator. Holding G28 gives a test a window in
 * which the calibrator has committed to a state but the command has not yet
 * come back — the only way to observe CHECKING_ADXL from outside — and a
 * first-class way to fail a run without inventing a fake error path.
 */
class HomingGateAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;

    void execute_gcode(const std::string& gcode, IMoonrakerAPI::SuccessCallback on_success,
                       IMoonrakerAPI::ErrorCallback on_error, uint32_t timeout_ms = 0,
                       bool silent = false, IMoonrakerAPI::SuccessCallback on_queued = nullptr,
                       bool caller_surfaces_errors = true) override {
        if (gcode == "G28") {
            ++g28_count_;
            g28_success_ = std::move(on_success);
            g28_error_ = std::move(on_error);
            return; // held: the test decides when homing finishes
        }
        MoonrakerAPI::execute_gcode(gcode, std::move(on_success), std::move(on_error), timeout_ms,
                                    silent, std::move(on_queued), caller_surfaces_errors);
    }

    [[nodiscard]] bool g28_pending() const {
        return static_cast<bool>(g28_success_);
    }

    [[nodiscard]] int g28_count() const {
        return g28_count_;
    }

    /// Answer the held G28 with success, letting the calibrator proceed.
    void finish_homing() {
        auto cb = std::move(g28_success_);
        g28_success_ = nullptr;
        g28_error_ = nullptr;
        if (cb) {
            cb();
        }
    }

    /// Fail the held G28 the way Klipper does when homing aborts.
    void fail_homing(const std::string& message) {
        auto cb = std::move(g28_error_);
        g28_success_ = nullptr;
        g28_error_ = nullptr;
        if (cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = message;
            cb(err);
        }
    }

  private:
    IMoonrakerAPI::SuccessCallback g28_success_;
    IMoonrakerAPI::ErrorCallback g28_error_;
    int g28_count_ = 0;
};

} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for InputShaperCalibrator testing
 *
 * Owns the mock client, the API the calibrator talks through, and the callback
 * capture the assertions read.
 */
class InputShaperCalibratorTestFixture : public LVGLTestFixture {
  public:
    InputShaperCalibratorTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // A previous mock run in THIS process may have left the calibration
        // CSVs behind; the collector keys its "chart available" decision off
        // them. Scoped to our own PID so the removal cannot reach a concurrent
        // shard's fixture (see MoonrakerClientMock::shaper_csv_path).
        MoonrakerClientMock::remove_shaper_csvs();

        printer_state_.init_subjects(false);
        // execute_gcode()'s halted gate would otherwise reject every command.
        printer_state_.set_klippy_state_sync(KlippyState::READY);
        api_ = std::make_unique<HomingGateAPI>(mock_client_, printer_state_);

        // ensure_homed_then() reads the GLOBAL PrinterState, not the one the
        // API was built with, so seed that one too. "xyz" = fully homed, which
        // makes the continuation run synchronously.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");

        calibrator_ = InputShaperCalibrator(api_.get());
        // Nothing the fixture itself did should show up in the transcript the
        // apply/save cases assert on.
        mock_client_.clear_gcode_script_history();
        reset_callbacks();
    }

    ~InputShaperCalibratorTestFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        MoonrakerClientMock::remove_shaper_csvs();
    }

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

    /// One report from the calibrator's progress callback. The phase is kept
    /// because the percentage alone cannot distinguish "still sweeping" from
    /// "Klipper is fitting shapers" — the reason ProgressCallback carries it
    /// (include/input_shaper_calibrator.h:49-52).
    struct ProgressEvent {
        int percent;
        ShaperCalibrationPhase phase;
    };

    void on_progress(int percent, ShaperCalibrationPhase phase) {
        progress_updates_.push_back({percent, phase});
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
     * @brief Report the toolhead as un-homed so ensure_homed_then() sends a G28
     *
     * HomingGateAPI holds that G28, so the calibrator stays in the state it
     * committed to until release_homing() / fail_homing() is called.
     */
    static void unhome() {
        lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "");
    }

    void release_homing() {
        api_->finish_homing();
        // ensure_homed_then() wraps its callbacks in AsyncLifetimeGuard::bg_cb(),
        // which routes through the UpdateQueue rather than running inline.
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail_homing(const std::string& message) {
        api_->fail_homing(message);
        helix::ui::UpdateQueue::instance().drain();
    }

    /**
     * @brief Pump the mock's transcript timers and the UpdateQueue until @p pred holds
     *
     * The mock replays SHAPER_CALIBRATE as ~43 gcode_response lines on a 100ms
     * repeating timer, so a resonance run needs the tick advanced, not slept on.
     */
    bool pump_until(const std::function<bool()>& pred, int max_ticks = 400) {
        for (int i = 0; i < max_ticks && !pred(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            helix::ui::UpdateQueue::instance().drain();
        }
        return pred();
    }

    /// Run one full resonance calibration on @p axis and wait for the result.
    bool calibrate(char axis) {
        calibrator_.run_calibration(
            axis, [this](int pct, ShaperCalibrationPhase ph) { on_progress(pct, ph); },
            [this](const InputShaperResult& r) { on_result(r); },
            [this](const std::string& err) { on_error(err); });
        return pump_until([this] { return result_received_.load() || error_received_.load(); });
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState printer_state_;
    std::unique_ptr<HomingGateAPI> api_;
    InputShaperCalibrator calibrator_;

    // Callback state tracking
    std::atomic<bool> accel_check_complete_{false};
    std::vector<ProgressEvent> progress_updates_;
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
    REQUIRE_FALSE(calibrator_.is_active());
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

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "cancel() returns an in-progress run to IDLE",
                 "[calibrator][input_shaper][state]") {
    calibrator_.run_calibration('X', nullptr, nullptr,
                                [this](const std::string& err) { on_error(err); });
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);
    REQUIRE(calibrator_.is_active());

    calibrator_.cancel();

    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
    REQUIRE_FALSE(calibrator_.is_active());
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
    // Un-homed, so ensure_homed_then() has to send a G28 the fixture holds.
    // That is the window in which CHECKING_ADXL is observable from outside.
    unhome();

    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });

    REQUIRE(api_->g28_pending());
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::CHECKING_ADXL);
    REQUIRE(calibrator_.is_active());
    REQUIRE_FALSE(accel_check_complete_);

    // Homing lands, the noise measurement runs, and the calibrator releases.
    release_homing();

    REQUIRE(accel_check_complete_);
    REQUIRE_FALSE(error_received_);
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer with null callbacks does not crash",
                 "[calibrator][input_shaper][accel][edge_case]") {
    // Runs the whole measurement chain with nowhere to report it.
    REQUIRE_NOTHROW(calibrator_.check_accelerometer(nullptr, nullptr));
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
    // The measurement still landed in the results container.
    REQUIRE(calibrator_.get_results().noise_level > 0.0f);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer stores noise level in results",
                 "[calibrator][input_shaper][accel]") {
    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });

    REQUIRE(accel_check_complete_);
    REQUIRE_FALSE(error_received_);

    // The mock reports 12.345678 (x), 15.678901 (y); NoiseCheckCollector reports
    // max(x, y) as the overall level (src/api/moonraker_advanced_api.cpp:1530).
    CHECK(captured_noise_level_ == Catch::Approx(15.678901f).margin(0.001f));
    // ...and the calibrator stores exactly what it reported
    // (src/calibration/input_shaper_calibrator.cpp:120).
    CHECK(calibrator_.get_results().noise_level == Catch::Approx(captured_noise_level_));
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "check_accelerometer surfaces a missing accelerometer as an error",
                 "[calibrator][input_shaper][accel][error]") {
    mock_client_.set_accelerometer_available(false);

    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });

    REQUIRE(error_received_);
    REQUIRE_FALSE(accel_check_complete_);
    CHECK(captured_error_.find("adxl345") != std::string::npos);
    // Error unwinds the state machine (input_shaper_calibrator.cpp:132).
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

// ============================================================================
// run_calibration() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration transitions to the axis's TESTING state",
                 "[calibrator][input_shaper][calibration]") {
    const char axis = GENERATE('X', 'Y');
    const auto expected = (axis == 'X') ? InputShaperCalibrator::State::TESTING_X
                                        : InputShaperCalibrator::State::TESTING_Y;
    INFO("axis: " << axis);

    calibrator_.run_calibration(
        axis, [this](int pct, ShaperCalibrationPhase ph) { on_progress(pct, ph); },
        [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // The mock replays SHAPER_CALIBRATE over a timer, so the run is still open.
    REQUIRE(calibrator_.get_state() == expected);
    REQUIRE(calibrator_.is_active());
    REQUIRE_FALSE(result_received_);
    REQUIRE_FALSE(error_received_);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "run_calibration normalizes a lowercase axis",
                 "[calibrator][input_shaper][calibration][validation]") {
    calibrator_.run_calibration(
        'x', nullptr, [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    // toupper() in run_calibration (input_shaper_calibrator.cpp:161) means
    // lowercase is a valid axis, not an error.
    REQUIRE_FALSE(error_received_);
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);

    REQUIRE(pump_until([this] { return result_received_.load(); }));
    CHECK(captured_result_.axis == 'X');
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "run_calibration rejects an axis other than X/Y",
                 "[calibrator][input_shaper][calibration][validation]") {
    const char axis = GENERATE('Z', 'A', '\0');
    INFO("axis: " << static_cast<int>(axis));

    calibrator_.run_calibration(
        axis, nullptr, [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    REQUIRE(error_received_);
    CHECK(captured_error_.find("Invalid axis") != std::string::npos);
    CHECK(captured_error_.find("must be X or Y") != std::string::npos);
    CHECK_FALSE(result_received_);
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration with null callbacks does not crash",
                 "[calibrator][input_shaper][calibration][edge_case]") {
    REQUIRE_NOTHROW(calibrator_.run_calibration('X', nullptr, nullptr, nullptr));
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);

    // The run still completes and still stores its result with no callbacks.
    REQUIRE(pump_until([this] { return calibrator_.get_results().has_x(); }));
    CHECK(calibrator_.get_results().x_result.axis == 'X');
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration result is stored in get_results()",
                 "[calibrator][input_shaper][calibration]") {
    REQUIRE(calibrate('X'));
    REQUIRE(result_received_);
    REQUIRE_FALSE(error_received_);

    const auto& results = calibrator_.get_results();
    CHECK(results.has_x());
    CHECK(results.x_result.axis == 'X');
    CHECK(results.x_result.is_valid());
    // The stored copy is the one handed to the callback, not a rebuilt stub.
    CHECK(results.x_result.shaper_type == captured_result_.shaper_type);
    CHECK(results.x_result.shaper_freq == Catch::Approx(captured_result_.shaper_freq));
    // Y untouched: one axis does not complete the calibration
    // (input_shaper_calibrator.cpp:215-223).
    CHECK_FALSE(results.has_y());
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "run_calibration Y result is stored separately from X",
                 "[calibrator][input_shaper][calibration]") {
    REQUIRE(calibrate('X'));
    REQUIRE(result_received_);
    reset_callbacks();

    REQUIRE(calibrate('Y'));
    REQUIRE(result_received_);
    REQUIRE_FALSE(error_received_);

    const auto& results = calibrator_.get_results();
    CHECK(results.has_x());
    CHECK(results.has_y());
    CHECK(results.x_result.axis == 'X');
    CHECK(results.y_result.axis == 'Y');
    CHECK(results.is_complete());
    // Both axes present promotes the machine to READY (calibrator.cpp:216).
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::READY);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "cannot start calibration while already running",
                 "[calibrator][input_shaper][calibration][guard]") {
    calibrator_.run_calibration('X', nullptr, nullptr, nullptr);
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);

    bool second_error = false;
    std::string second_message;
    calibrator_.run_calibration(
        'Y', nullptr, [this](const InputShaperResult& r) { on_result(r); },
        [&](const std::string& err) {
            second_error = true;
            second_message = err;
        });

    // The concurrency guard rejects the second run (calibrator.cpp:183-189).
    REQUIRE(second_error);
    CHECK(second_message.find("already in progress") != std::string::npos);
    // ...and does not disturb the run already going.
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);
    CHECK_FALSE(result_received_);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "a finished run leaves the calibrator able to start the other axis",
                 "[calibrator][input_shaper][calibration][guard]") {
    REQUIRE(calibrate('X'));
    reset_callbacks();

    // IDLE after one axis, so the guard must let Y through.
    calibrator_.run_calibration('Y', nullptr, nullptr,
                                [this](const std::string& err) { on_error(err); });
    CHECK_FALSE(error_received_);
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_Y);
}

// ============================================================================
// Progress Callback Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "progress callback is called during calibration",
                 "[calibrator][input_shaper][progress]") {
    REQUIRE(calibrate('X'));
    REQUIRE(result_received_);
    REQUIRE_FALSE(error_received_);

    // The calibrator forwards the API's progress verbatim (calibrator.cpp:200-204).
    REQUIRE(progress_updates_.size() > 1);

    // The run ends on a Complete/100 report, emitted just before the result
    // (moonraker_advanced_api.cpp:1304).
    CHECK(progress_updates_.back().percent == 100);
    CHECK(progress_updates_.back().phase == ShaperCalibrationPhase::Complete);

    bool saw_sweeping = false;
    bool saw_analyzing = false;
    int last_sweep_percent = -1;
    for (size_t i = 0; i < progress_updates_.size(); ++i) {
        const auto& ev = progress_updates_[i];
        INFO("progress[" << i << "] = " << ev.percent << " phase=" << static_cast<int>(ev.phase));
        CHECK(ev.percent >= 0);
        CHECK(ev.percent <= 100);
        if (ev.phase == ShaperCalibrationPhase::Sweeping) {
            saw_sweeping = true;
            // The sweep percentage tracks frequency, so it never goes backwards.
            CHECK(ev.percent >= last_sweep_percent);
            last_sweep_percent = ev.percent;
        } else if (ev.phase == ShaperCalibrationPhase::Analyzing) {
            saw_analyzing = true;
        }
    }

    // Both phases are reported: the UI switches from a bar to a spinner on the
    // Analyzing report, which carries no meaningful percentage of its own.
    CHECK(saw_sweeping);
    CHECK(saw_analyzing);
}

// ============================================================================
// apply_settings() Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings sends SET_INPUT_SHAPER",
                 "[calibrator][input_shaper][apply]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 36.7f;
    config.damping_ratio = 0.1f;

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    REQUIRE(success_called_);
    REQUIRE_FALSE(error_received_);

    const auto& history = mock_client_.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    const std::string& sent = history.back();
    INFO("sent: " << sent);
    CHECK(sent.find("SET_INPUT_SHAPER") != std::string::npos);
    CHECK(sent.find("SHAPER_TYPE_X=mzv") != std::string::npos);
    CHECK(sent.find("SHAPER_FREQ_X=36.7") != std::string::npos);
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

    // Validation runs before anything is sent (calibrator.cpp:286-292).
    REQUIRE(error_received_);
    REQUIRE_FALSE(success_called_);
    CHECK(captured_error_.find("shaper_type") != std::string::npos);
    CHECK(mock_client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "apply_settings rejects a non-positive frequency",
                 "[calibrator][input_shaper][apply][validation]") {
    const float frequency = GENERATE(0.0f, -10.0f);
    INFO("frequency: " << frequency);

    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = frequency;

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    // calibrator.cpp:295-302 — a zero or negative frequency never reaches the printer.
    REQUIRE(error_received_);
    REQUIRE_FALSE(success_called_);
    CHECK(captured_error_.find("frequency") != std::string::npos);
    CHECK(mock_client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings accepts all valid shaper types",
                 "[calibrator][input_shaper][apply]") {
    std::vector<std::string> valid_types = {"zv", "mzv", "zvd", "ei", "2hump_ei", "3hump_ei"};

    for (const auto& type : valid_types) {
        INFO("Testing shaper type: " << type);
        reset_callbacks();
        mock_client_.clear_gcode_script_history();

        ApplyConfig config;
        config.axis = 'X';
        config.shaper_type = type;
        config.frequency = 35.0f;

        calibrator_.apply_settings(
            config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

        REQUIRE(success_called_);
        REQUIRE_FALSE(error_received_);
        REQUIRE(mock_client_.gcode_script_history().size() == 1);
        CHECK(mock_client_.gcode_script_history()[0].find("SHAPER_TYPE_X=" + type) !=
              std::string::npos);
    }
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings for Y axis",
                 "[calibrator][input_shaper][apply]") {
    ApplyConfig config;
    config.axis = 'Y';
    config.shaper_type = "ei";
    config.frequency = 47.6f;

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    REQUIRE(success_called_);
    REQUIRE_FALSE(error_received_);
    REQUIRE(mock_client_.gcode_script_history().size() == 1);
    const std::string& sent = mock_client_.gcode_script_history()[0];
    INFO("sent: " << sent);
    // The Y axis must not be written into the X parameters.
    CHECK(sent.find("SHAPER_TYPE_Y=ei") != std::string::npos);
    CHECK(sent.find("SHAPER_TYPE_X") == std::string::npos);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "apply_settings with null callbacks does not crash",
                 "[calibrator][input_shaper][apply][edge_case]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 36.7f;

    REQUIRE_NOTHROW(calibrator_.apply_settings(config, nullptr, nullptr));
    REQUIRE(mock_client_.gcode_script_history().size() == 1);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "apply_settings accepts a very high frequency",
                 "[calibrator][input_shaper][edge_case]") {
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = "mzv";
    config.frequency = 1000.0f; // Unrealistically high but not rejected

    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });

    // Only <= 0 is rejected; an absurd-but-positive value goes to the printer.
    CHECK(success_called_);
    CHECK_FALSE(error_received_);
    REQUIRE(mock_client_.gcode_script_history().size() == 1);
    CHECK(mock_client_.gcode_script_history()[0].find("SHAPER_FREQ_X=1000") != std::string::npos);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "error callback receives meaningful message",
                 "[calibrator][input_shaper][error]") {
    calibrator_.run_calibration(
        'Z', // Invalid
        nullptr, [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    REQUIRE(error_received_);
    CHECK_FALSE(captured_error_.empty());
    // The rejected axis is named, so the message is actionable.
    CHECK(captured_error_.find('Z') != std::string::npos);
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture, "state returns to IDLE on error",
                 "[calibrator][input_shaper][error][state]") {
    // Reach a live TESTING_X first — asserting IDLE from IDLE proves nothing.
    unhome();
    calibrator_.run_calibration(
        'X', nullptr, [this](const InputShaperResult& r) { on_result(r); },
        [this](const std::string& err) { on_error(err); });

    REQUIRE(api_->g28_pending());
    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::TESTING_X);

    // Klipper aborts the homing move that SHAPER_CALIBRATE needs.
    fail_homing("Move out of range: 300.000 > 250.00");

    REQUIRE(error_received_);
    CHECK(captured_error_.find("Homing failed") != std::string::npos);
    CHECK_FALSE(result_received_);
    // calibrator.cpp:240-245 unwinds the machine on the homing error path.
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
    CHECK_FALSE(calibrator_.is_active());
}

TEST_CASE_METHOD(InputShaperCalibratorTestFixture,
                 "a firmware halt during homing is reported as a firmware fault",
                 "[calibrator][input_shaper][error][1021]") {
    unhome();
    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });
    REQUIRE(api_->g28_pending());

    fail_homing(R"({"code":"key60", "msg":"Internal error on command:G1 Z-10 F600"})");

    REQUIRE(error_received_);
    // homing_error_message() gives firmware-halt classification priority over
    // the generic "Homing failed" wording (calibrator.cpp:84-90).
    CHECK(captured_error_.find("firmware") != std::string::npos);
    CHECK(captured_error_.find("key60") == std::string::npos);
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
}

// ============================================================================
// Move Semantics Tests
// ============================================================================

TEST_CASE("InputShaperCalibrator is movable", "[calibrator][input_shaper][move]") {
    // Pins the unique_ptr<AsyncLifetimeGuard> indirection documented at
    // include/input_shaper_calibrator.h:286-297 — the guard itself is
    // non-movable, so a plain member would make the calibrator non-movable too.
    InputShaperCalibrator calibrator1;

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
                 "[calibrator][input_shaper][integration]") {
    // check accelerometer -> calibrate X -> calibrate Y -> apply,
    // driven end-to-end through the mock printer.
    calibrator_.check_accelerometer([this](float noise) { on_accel_check(noise); },
                                    [this](const std::string& err) { on_error(err); });
    REQUIRE(accel_check_complete_);
    REQUIRE_FALSE(error_received_);
    CHECK(captured_noise_level_ > 0.0f);
    reset_callbacks();

    REQUIRE(calibrate('X'));
    REQUIRE(result_received_);
    CHECK(captured_result_.axis == 'X');
    CHECK(captured_result_.is_valid());
    const std::string x_type = captured_result_.shaper_type;
    const float x_freq = captured_result_.shaper_freq;
    reset_callbacks();

    REQUIRE(calibrate('Y'));
    REQUIRE(result_received_);
    CHECK(captured_result_.axis == 'Y');
    CHECK(captured_result_.is_valid());
    reset_callbacks();

    REQUIRE(calibrator_.get_state() == InputShaperCalibrator::State::READY);
    REQUIRE(calibrator_.get_results().is_complete());

    mock_client_.clear_gcode_script_history();
    ApplyConfig config;
    config.axis = 'X';
    config.shaper_type = x_type;
    config.frequency = x_freq;
    calibrator_.apply_settings(
        config, [this]() { on_success(); }, [this](const std::string& err) { on_error(err); });
    REQUIRE(success_called_);
    REQUIRE_FALSE(error_received_);
    reset_callbacks();
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

    // cancel() only resets the state machine — a measured result survives it.
    REQUIRE(calibrate('X'));
    calibrator_.cancel();
    CHECK(calibrator_.get_results().has_x());
    CHECK(calibrator_.get_state() == InputShaperCalibrator::State::IDLE);
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
