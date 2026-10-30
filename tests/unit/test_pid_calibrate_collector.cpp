// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pid_calibrate_collector.cpp
 * @brief Unit tests for PIDCalibrateCollector and MoonrakerAPI::start_pid_calibrate()
 *
 * Tests the PIDCalibrateCollector pattern and API method:
 * - PID result parsing from gcode responses
 * - Error handling for unknown commands and Klipper errors
 * - Bed heater calibration
 *
 * Uses mock client to simulate G-code responses from Klipper.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"

#include <atomic>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class PIDCalibrateTestFixture : public LVGLTestFixture {
  public:
    PIDCalibrateTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        // execute_gcode() halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
    }
    ~PIDCalibrateTestFixture() {
        // Run what the collectors queued while this fixture's PrinterState is
        // still alive: the idle-fallback callbacks close over state_, so leaving
        // them for the next fixture's drain reaches a dead object.
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

        api_.reset();
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;

    std::atomic<bool> result_received_{false};
    std::atomic<bool> error_received_{false};
    float captured_kp_ = 0, captured_ki_ = 0, captured_kd_ = 0;
    std::string captured_error_;

    std::vector<int> progress_samples_;
    std::vector<float> progress_tolerances_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate collector parses results",
                 "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    // Simulate Klipper PID output
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_kp_ == Catch::Approx(22.865f).margin(0.001f));
    REQUIRE(captured_ki_ == Catch::Approx(1.292f).margin(0.001f));
    REQUIRE(captured_kd_ == Catch::Approx(101.178f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate collector handles errors",
                 "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200, [this](float, float, float) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    mock_client_.dispatch_gcode_response("!! Error: heater extruder not heating at expected rate");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("Error") != std::string::npos);
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate handles unknown command",
                 "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200, [this](float, float, float) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    mock_client_.dispatch_gcode_response("Unknown command: \"PID_CALIBRATE\"");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate bed heater", "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "heater_bed", 60,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); });

    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=73.517 pid_Ki=1.132 pid_Kd=1194.093");

    REQUIRE(result_received_.load());
    REQUIRE(captured_kp_ == Catch::Approx(73.517f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate collector fires progress callback",
                 "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); },
        [this](int sample, float tolerance) {
            progress_samples_.push_back(sample);
            progress_tolerances_.push_back(tolerance);
        });

    mock_client_.dispatch_gcode_response("sample:1 pwm:0.5 asymmetry:0.2 tolerance:n/a");
    mock_client_.dispatch_gcode_response("sample:2 pwm:0.48 asymmetry:0.15 tolerance:0.045");

    REQUIRE(progress_samples_.size() == 2);
    REQUIRE(progress_samples_[0] == 1);
    REQUIRE(progress_samples_[1] == 2);
    REQUIRE(progress_tolerances_[0] == -1.0f);
    REQUIRE(progress_tolerances_[1] == Catch::Approx(0.045f));

    // Complete the collector to avoid dangling callback in mock client destruction
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate progress then result", "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); },
        [this](int sample, float tolerance) {
            progress_samples_.push_back(sample);
            progress_tolerances_.push_back(tolerance);
        });

    mock_client_.dispatch_gcode_response("sample:1 pwm:0.5 asymmetry:0.2 tolerance:n/a");
    mock_client_.dispatch_gcode_response("sample:2 pwm:0.48 asymmetry:0.15 tolerance:0.045");
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");

    REQUIRE(progress_samples_.size() == 2);
    REQUIRE(result_received_.load());
    REQUIRE(captured_kp_ == Catch::Approx(22.865f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate no progress after completion",
                 "[pid_calibrate]") {
    api_->advanced().start_pid_calibrate(
        "extruder", 200, [this](float kp, float ki, float kd) { result_received_.store(true); },
        [this](const MoonrakerError& err) { error_received_.store(true); },
        [this](int sample, float tolerance) { progress_samples_.push_back(sample); });

    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");
    mock_client_.dispatch_gcode_response("sample:3 pwm:0.5 asymmetry:0.2 tolerance:0.01");

    REQUIRE(result_received_.load());
    REQUIRE(progress_samples_.empty());
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate backward compat without progress",
                 "[pid_calibrate]") {
    // Call without progress callback (nullptr default)
    api_->advanced().start_pid_calibrate(
        "extruder", 200,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); });

    mock_client_.dispatch_gcode_response("sample:1 pwm:0.5 asymmetry:0.2 tolerance:n/a");
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_kp_ == Catch::Approx(22.865f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture,
                 "PID calibrate survives RPC timeout and still delivers result",
                 "[pid_calibrate]") {
    // Regression for #988: slow-cooling beds run longer than the RPC timeout. When the
    // printer.gcode.script RPC times out, the collector must keep listening so the later
    // "PID parameters:" result line still completes the calibration successfully.
    mock_client_.force_next_gcode_error(MoonrakerErrorType::TIMEOUT,
                                        "Request timed out after 20 min", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60,
        [this](float kp, float ki, float kd) {
            captured_kp_ = kp;
            captured_ki_ = ki;
            captured_kd_ = kd;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    // The RPC has already "timed out" synchronously above. Klipper finishes afterwards.
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=73.517 pid_Ki=1.132 pid_Kd=1194.093");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_kp_ == Catch::Approx(73.517f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate RPC timeout does not fire error callback",
                 "[pid_calibrate]") {
    // The RPC timeout alone must NOT surface a hard failure to the UI — the collector is
    // the authority for completion, and the calibration may still be running on Klipper.
    mock_client_.force_next_gcode_error(MoonrakerErrorType::TIMEOUT,
                                        "Request timed out after 20 min", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "heater_bed", 60, [this](float, float, float) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    REQUIRE_FALSE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "PID calibrate non-timeout RPC error still fails fast",
                 "[pid_calibrate]") {
    // A genuine RPC error (not a timeout) must still tear down the collector and report
    // failure — only TIMEOUT is treated as "keep listening".
    mock_client_.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                        "Heater extruder not configured", "PID_CALIBRATE");

    api_->advanced().start_pid_calibrate(
        "extruder", 200, [this](float, float, float) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());

    // After a hard error the collector is unregistered: a stray result line is ignored.
    result_received_.store(false);
    mock_client_.dispatch_gcode_response(
        "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178");
    REQUIRE_FALSE(result_received_.load());
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "get_heater_pid_values returns extruder values via API",
                 "[pid_calibrate]") {
    std::atomic<bool> cb_fired{false};
    float kp = 0, ki = 0, kd = 0;

    api_->advanced().get_heater_pid_values(
        "extruder",
        [&](float p, float i, float d) {
            kp = p;
            ki = i;
            kd = d;
            cb_fired.store(true);
        },
        [&](const MoonrakerError& err) {
            spdlog::error("get_heater_pid_values failed: {}", err.message);
            cb_fired.store(true);
        });

    REQUIRE(cb_fired.load());
    REQUIRE(kp == Catch::Approx(22.865f).margin(0.001f));
    REQUIRE(ki == Catch::Approx(1.292f).margin(0.001f));
    REQUIRE(kd == Catch::Approx(101.178f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "get_heater_pid_values returns bed values via API",
                 "[pid_calibrate]") {
    std::atomic<bool> cb_fired{false};
    float kp = 0;

    api_->advanced().get_heater_pid_values(
        "heater_bed",
        [&](float p, float, float) {
            kp = p;
            cb_fired.store(true);
        },
        [&](const MoonrakerError&) { cb_fired.store(true); });

    REQUIRE(cb_fired.load());
    REQUIRE(kp == Catch::Approx(73.517f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "Mock configfile returns extruder PID values",
                 "[pid_calibrate]") {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    std::atomic<bool> cb_fired{false};
    float kp = 0, ki = 0, kd = 0;

    mock_client_.send_jsonrpc(
        "printer.objects.query", params,
        [&](json response) {
            const json& settings = response["result"]["status"]["configfile"]["settings"];
            REQUIRE(settings.contains("extruder"));
            const json& ext = settings["extruder"];
            kp = ext["pid_kp"].get<float>();
            ki = ext["pid_ki"].get<float>();
            kd = ext["pid_kd"].get<float>();
            cb_fired.store(true);
        },
        [&](const MoonrakerError&) { cb_fired.store(true); });

    REQUIRE(cb_fired.load());
    REQUIRE(kp == Catch::Approx(22.865f).margin(0.001f));
    REQUIRE(ki == Catch::Approx(1.292f).margin(0.001f));
    REQUIRE(kd == Catch::Approx(101.178f).margin(0.001f));
}

TEST_CASE_METHOD(PIDCalibrateTestFixture, "Mock configfile returns bed PID values",
                 "[pid_calibrate]") {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    std::atomic<bool> cb_fired{false};
    float kp = 0, ki = 0, kd = 0;

    mock_client_.send_jsonrpc(
        "printer.objects.query", params,
        [&](json response) {
            const json& settings = response["result"]["status"]["configfile"]["settings"];
            REQUIRE(settings.contains("heater_bed"));
            const json& bed = settings["heater_bed"];
            kp = bed["pid_kp"].get<float>();
            ki = bed["pid_ki"].get<float>();
            kd = bed["pid_kd"].get<float>();
            cb_fired.store(true);
        },
        [&](const MoonrakerError&) { cb_fired.store(true); });

    REQUIRE(cb_fired.load());
    REQUIRE(kp == Catch::Approx(73.517f).margin(0.001f));
    REQUIRE(ki == Catch::Approx(1.132f).margin(0.001f));
    REQUIRE(kd == Catch::Approx(1194.093f).margin(0.001f));
}
