// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_error_ownership_more.cpp
 * @brief Second batch of "who reports this failed gcode" sites.
 *
 * Companion to test_gcode_error_ownership.cpp, same contract: a callback that
 * only writes a spdlog line must NOT claim the user-visible report, because
 * claiming it records the message through
 * rpc_error_correlation::record_caller_handled() and GcodeErrorRouter then
 * suppresses its own toast for Klipper's matching `!!` broadcast — leaving the
 * failure reported by nobody.
 *
 * Assertions are on the dedup record itself (was_recently_handled), since that
 * record IS the mechanism that mutes the router. Every case forces a real
 * rejection and checks the gcode actually went out, so a `false` here can never
 * be the vacuous "nothing was sent" answer.
 *
 * The mock's printer.gcode.script handler runs synchronously inside
 * send_jsonrpc(), so the record (if any) exists by the time the call returns.
 */

#include "ui_panel_calibration_zoffset.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "led/led_controller.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "rpc_error_correlation.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "test_helpers/zoffset_calibration_test_access.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Clears the process-wide correlation window on both sides of a test so a
/// record left by an earlier case can never be mistaken for this one's.
class OwnershipMoreFixture : public LVGLTestFixture {
  public:
    OwnershipMoreFixture() {
        helix::rpc_error_correlation::clear_for_test();
    }
    ~OwnershipMoreFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        helix::rpc_error_correlation::clear_for_test();
    }
};

constexpr const char* REJECTION = "Printer is not ready";

/// ZOffsetCalibrationPanel::adjust_z() branches on the PROCESS-WIDE
/// PrinterState's strategy, not on a local one. Reset pins it to
/// PROBE_CALIBRATE so the TESTZ arm is the one under test regardless of what
/// an earlier test left behind.
helix::PrinterState& probe_calibrate_global_state() {
    helix::PrinterState& state = get_printer_state();
    helix::PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    return state;
}

} // namespace

// ============================================================================
// Z-offset calibration. Every send in the panel logs and nothing more — the
// paper-test flow reports failure through its own state machine, not through
// these callbacks.
// ============================================================================

TEST_CASE_METHOD(OwnershipMoreFixture, "Z-offset TESTZ nudge leaves the `!!` router free",
                 "[error-center][gcode-ownership][zoffset]") {
    helix::PrinterState& state = probe_calibrate_global_state();
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);

    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, REJECTION, "TESTZ");
    ZOffsetCalibrationTestAccess::adjust_z(panel, -0.025f);

    // The nudge really went out and really was rejected.
    REQUIRE(client.last_send_method() == "printer.gcode.script");
    REQUIRE(client.last_send_script() == "TESTZ Z=-0.025");
    CHECK_FALSE(client.current_send_intent().surfaces_errors);

    CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(REJECTION));
}

TEST_CASE_METHOD(OwnershipMoreFixture, "Z-offset bed-off send leaves the `!!` router free",
                 "[error-center][gcode-ownership][zoffset]") {
    helix::PrinterState& state = probe_calibrate_global_state();
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    ZOffsetCalibrationPanel panel;
    panel.set_api(&api);
    ZOffsetCalibrationTestAccess::mark_bed_warmed(panel);

    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, REJECTION, "M140");
    ZOffsetCalibrationTestAccess::turn_off_bed(panel);

    REQUIRE(client.last_send_script() == "M140 S0");
    CHECK_FALSE(client.current_send_intent().surfaces_errors);

    CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(REJECTION));
}

// ============================================================================
// LED backends. Every send wraps the caller's on_error in a non-null adapter
// lambda, so ownership has to be read from the CALLER's handler before the
// wrap — the same shape as helix::ensure_homed_then().
// ============================================================================

TEST_CASE_METHOD(OwnershipMoreFixture, "LED strip writes claim ownership only for a real handler",
                 "[error-center][gcode-ownership][led]") {
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    helix::led::NativeBackend backend;
    backend.set_api(&api);

    SECTION("no caller handler — the adapter lambda must not claim it") {
        client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, REJECTION, "SET_LED");
        backend.set_color("neopixel bar", 1.0, 0.0, 0.0, 0.0);

        REQUIRE(client.last_send_script().find("SET_LED LED=\"bar\"") == 0);
        CHECK_FALSE(client.current_send_intent().surfaces_errors);
        CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(REJECTION));
    }

    SECTION("caller handler present — the `!!` copy dedups against it") {
        client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, REJECTION, "SET_LED");
        std::string seen;
        backend.set_color("neopixel bar", 1.0, 0.0, 0.0, 0.0, nullptr,
                          [&seen](const std::string& msg) { seen = msg; });

        REQUIRE(client.last_send_script().find("SET_LED LED=\"bar\"") == 0);
        CHECK(client.current_send_intent().surfaces_errors);
        CHECK(helix::rpc_error_correlation::was_recently_handled(REJECTION));
        CHECK(seen == REJECTION);
    }
}

TEST_CASE_METHOD(OwnershipMoreFixture, "LED effect activation can opt out for a log-only handler",
                 "[error-center][gcode-ownership][led]") {
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    helix::led::LedEffectBackend backend;
    backend.set_api(&api);

    // LedControlOverlay's shape: a non-null handler that only writes a log line,
    // so it explicitly declines ownership rather than inheriting the
    // "non-null means the user was told" presumption.
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, REJECTION, "SET_LED_EFFECT");
    std::string seen;
    backend.activate_effect(
        "led_effect rainbow", nullptr, [&seen](const std::string& msg) { seen = msg; },
        /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);

    REQUIRE(client.last_send_script() == "SET_LED_EFFECT EFFECT=rainbow");
    CHECK(seen == REJECTION); // the handler still ran; it just does not report
    CHECK_FALSE(client.current_send_intent().surfaces_errors);
    CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(REJECTION));
}
