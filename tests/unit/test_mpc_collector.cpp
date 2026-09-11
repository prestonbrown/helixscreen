// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mpc_collector.cpp
 * @brief Unit tests for MPCCalibrateCollector and MoonrakerAdvancedAPI::start_mpc_calibrate()
 *
 * Tests the MPCCalibrateCollector pattern and API method:
 * - MPC result parsing from multi-line gcode responses
 * - Progress reporting for each calibration phase
 * - Error handling for unknown commands and Klipper errors
 * - Atomic double-invocation prevention
 *
 * Uses mock client to simulate G-code responses from Klipper/Kalico.
 */

#include "../../include/moonraker_advanced_api.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using MPCResult = MoonrakerAdvancedAPI::MPCResult;

// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerMPCCal {
    LVGLInitializerMPCCal() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerMPCCal lvgl_init;
} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class MPCCalibrateTestFixture {
  public:
    MPCCalibrateTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        // execute_gcode() halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
    }
    ~MPCCalibrateTestFixture() {
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
    MPCResult captured_result_;
    std::string captured_error_;

    std::vector<int> progress_phases_;
    std::vector<int> progress_totals_;
    std::vector<std::string> progress_descriptions_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate collector parses complete result block",
                 "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3,
        [this](const MPCResult& result) {
            captured_result_ = result;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.5432 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.123456 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.078901 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_result_.block_heat_capacity == Catch::Approx(18.5432f).margin(0.001f));
    REQUIRE(captured_result_.sensor_responsiveness == Catch::Approx(0.123456f).margin(0.0001f));
    REQUIRE(captured_result_.ambient_transfer == Catch::Approx(0.078901f).margin(0.0001f));
    REQUIRE(captured_result_.fan_ambient_transfer == "0.12, 0.18, 0.25");
}

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate collector parses floats accurately",
                 "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 220, 0,
        [this](const MPCResult& result) {
            captured_result_ = result;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=25.1000 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.500000 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.100000 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(result_received_.load());
    REQUIRE(captured_result_.block_heat_capacity == Catch::Approx(25.1f).margin(0.01f));
    REQUIRE(captured_result_.sensor_responsiveness == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(captured_result_.ambient_transfer == Catch::Approx(0.1f).margin(0.001f));
    // No fan_ambient_transfer line sent — should remain empty
    REQUIRE(captured_result_.fan_ambient_transfer.empty());
}

TEST_CASE_METHOD(MPCCalibrateTestFixture,
                 "MPC calibrate collector parses fan_ambient_transfer string", "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 5,
        [this](const MPCResult& result) {
            captured_result_ = result;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=20.0000 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.200000 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.050000 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("fan_ambient_transfer=0.05, 0.10, 0.15, 0.20, 0.25 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(result_received_.load());
    REQUIRE(captured_result_.fan_ambient_transfer == "0.05, 0.10, 0.15, 0.20, 0.25");
}

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate collector reports progress phases",
                 "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3,
        [this](const MPCResult& result) {
            captured_result_ = result;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) { error_received_.store(true); },
        [this](int phase, int total, const std::string& desc) {
            progress_phases_.push_back(phase);
            progress_totals_.push_back(total);
            progress_descriptions_.push_back(desc);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Waiting for heater to settle");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("Performing heatup test");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("measuring power usage with 50% fan");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("measuring power usage with 100% fan");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(progress_phases_.size() == 4);
    REQUIRE(progress_phases_[0] == 1);
    REQUIRE(progress_phases_[1] == 2);
    REQUIRE(progress_phases_[2] == 3);
    REQUIRE(progress_phases_[3] == 3);
    REQUIRE(progress_descriptions_[0] == "Waiting for heater to settle");
    REQUIRE(progress_descriptions_[1] == "Performing heatup test");
    REQUIRE(progress_descriptions_[2].find("50%") != std::string::npos);
    REQUIRE(progress_descriptions_[3].find("100%") != std::string::npos);

    // Complete the collector to avoid dangling callback in mock client destruction
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.0000 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.100000 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.050000 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate collector handles unknown command error",
                 "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3, [this](const MPCResult&) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Unknown command: \"MPC_CALIBRATE\"");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("MPC_CALIBRATE") != std::string::npos);
}

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate collector handles Klipper error",
                 "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3, [this](const MPCResult&) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("!! Error: heater extruder not heating at expected rate");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("Error") != std::string::npos);
}

TEST_CASE_METHOD(MPCCalibrateTestFixture,
                 "MPC calibrate collector ignores unrelated gcode responses", "[mpc_collector]") {
    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3, [this](const MPCResult&) { result_received_.store(true); },
        [this](const MoonrakerError& err) { error_received_.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("T:200.0 /200.0 B:60.0 /60.0");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("echo: M104 S200");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());

    // Complete the collector to clean up
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.0000 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.100000 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.050000 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_CASE_METHOD(MPCCalibrateTestFixture,
                 "MPC calibrate collector does not double-invoke callbacks", "[mpc_collector]") {
    int success_count = 0;
    int error_count = 0;

    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3, [&](const MPCResult&) { ++success_count; },
        [&](const MoonrakerError&) { ++error_count; });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send first complete result
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.0000 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.100000 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.050000 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Try to send a second result or error — should be ignored
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("!! Error: something went wrong");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(success_count == 1);
    REQUIRE(error_count == 0);
}

TEST_CASE_METHOD(MPCCalibrateTestFixture,
                 "MPC calibrate survives RPC timeout and still delivers result",
                 "[mpc_collector]") {
    // Regression for #1544: the calibration outlives the RPC ceiling (PID_CALIBRATE has
    // the same property, #988). When the execute_gcode RPC times out, the collector
    // must keep listening so the later result block still completes the run.
    mock_client_.force_next_gcode_error(MoonrakerErrorType::TIMEOUT,
                                        "Request timed out after 10 min", "MPC_CALIBRATE");

    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3,
        [this](const MPCResult& result) {
            captured_result_ = result;
            result_received_.store(true);
        },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    // The RPC has already "timed out" synchronously above. Klipper finishes afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mock_client_.dispatch_gcode_response("Finished MPC calibration heater=extruder");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("block_heat_capacity=18.5432 [J/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("sensor_responsiveness=0.123456 [K/s/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("ambient_transfer=0.078901 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mock_client_.dispatch_gcode_response("fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_result_.block_heat_capacity == Catch::Approx(18.5432f).margin(0.001f));
}

TEST_CASE_METHOD(MPCCalibrateTestFixture, "MPC calibrate non-timeout RPC error still fails fast",
                 "[mpc_collector]") {
    // A genuine RPC error, not a timeout, must still tear the collector down and report
    // failure — only TIMEOUT means "keep listening".
    mock_client_.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                        "Heater extruder not configured", "MPC_CALIBRATE");

    api_->advanced().start_mpc_calibrate(
        "extruder", 200, 3, [this](const MPCResult&) { result_received_.store(true); },
        [this](const MoonrakerError& err) {
            captured_error_ = err.message;
            error_received_.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("Heater extruder not configured") != std::string::npos);
}
