// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_busy_guard.cpp
 * @brief Tests for the discretionary-gcode busy guard while homing/probing.
 *
 * A blocking non-print operation (G28, BED_MESH_CALIBRATE, QGL, PROBE_ACCURACY,
 * manual probe) holds Klipper's single-threaded gcode lock. The guard splits
 * discretionary gcode two ways (#1108):
 *   - Physical MOVES (jog/home) are REFUSED — a jog that fires minutes late is
 *     dangerous. This is the motion API's whole job and the controls API refuses
 *     a raw move too.
 *   - Benign fan/temp/LED are QUEUED fire-and-forget (they run harmlessly when the
 *     lock frees, like every other frontend), with a single "busy — will run when
 *     ready" toast per blocking episode instead of a per-command timeout.
 * Recovery/homing/probe-control and macros are never discretionary, so they pass,
 * and nothing is blocked during a real file print.
 *
 * History: bundle 7CT79XXK (Sovol SV08) saw 4 fan commands each time out at 60s
 * during a Cartographer calibration — the guard originally REJECTED them; #1108
 * changed the benign path to queue-with-one-toast so the commands aren't lost.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../busy_guard_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class BusyGuardApiFixture : public helix::BusyGuardFixture {};

} // namespace

// ============================================================================
// Benign discretionary gcode (fan/temp/LED) QUEUES while a blocking op is active
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode queues benign discretionary gcode while homing/leveling",
                 "[busy_guard][mock]") {
    // #1108: benign commands are no longer rejected — they queue in Klipper and run
    // when the lock frees. The command must actually be SENT and no error surfaced.
    SECTION("idle_timeout Printing + not a file print queues a fan command") {
        set_idle_printing(true); // homing/leveling holds the gcode lock
        set_print_state(PrintJobState::STANDBY);

        api->execute_gcode("M106 S255", nullptr,
                           [this](const MoonrakerError& err) { error_cb(err); });

        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }

    SECTION("manual probe active queues a temp command") {
        set_idle_printing(false); // idle_timeout can bounce to Ready between TESTZ
        set_manual_probe(true);

        api->execute_gcode("M104 S200", nullptr,
                           [this](const MoonrakerError& err) { error_cb(err); });

        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }
}

// ============================================================================
// The queued path settles callers through a DISTINCT "queued" outcome (#1129)
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "queued discretionary gcode settles via on_queued, never on_success",
                 "[busy_guard][mock][led]") {
    // Two failure modes bracket this path.
    //
    // Dropping BOTH callbacks (the original #1108 shape) wedges callers that pair
    // a dispatch counter with them — LedController's note_command_dispatched /
    // note_command_settled drive the led_command_in_flight subject the light
    // buttons disable on, so the buttons greyed out permanently (#1129).
    //
    // Settling via on_success is a different bug: for every other caller
    // on_success means "the printer did it". Cooldown sends
    // SET_HEATER_TEMPERATURE TARGET=0 with on_success = NOTIFY_SUCCESS("Heaters
    // off"), and temperature_service pairs its toast with a go_back() — so a
    // success settle tells the user the heaters are off, and closes the overlay,
    // while they sit at target for the rest of the calibration.
    //
    // The contract: the queued path fires ONLY on_queued, and only for callers
    // that asked for it. on_success and on_error stay untouched.
    set_idle_printing(true); // homing/leveling holds the gcode lock
    set_print_state(PrintJobState::STANDBY);

    int success_calls = 0;
    int queued_calls = 0;
    auto note_success = [&success_calls]() { success_calls++; };
    auto note_queued = [&queued_calls]() { queued_calls++; };

    SECTION("raw SET_LED through execute_gcode") {
        api->execute_gcode(
            "SET_LED LED=my_leds RED=1.00 GREEN=1.00 BLUE=1.00 SYNC=0 TRANSMIT=1", note_success,
            [this](const MoonrakerError& err) { error_cb(err); }, 0, false, note_queued);

        // Still queued fire-and-forget — the fix must not un-queue the command.
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK_FALSE(mock_client.gcode_script_history().empty());

        CHECK(queued_calls == 1);
        CHECK(success_calls == 0);
        CHECK_FALSE(error_called);
    }

    SECTION("set_led() — the real LedController route") {
        api->set_led(
            "my_leds", 1.0, 0.5, 0.25, 0.0, note_success,
            [this](const MoonrakerError& err) { error_cb(err); }, note_queued);

        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK(queued_calls == 1);
        CHECK(success_calls == 0);
        CHECK_FALSE(error_called);
    }

    SECTION("a caller that does NOT opt in is left alone entirely") {
        // Cooldown's shape: on_success = "Heaters off" toast. Nothing may fire.
        api->execute_gcode("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0", note_success,
                           [this](const MoonrakerError& err) { error_cb(err); });

        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK(success_calls == 0);
        CHECK(queued_calls == 0);
        CHECK_FALSE(error_called);
    }

    SECTION("fan command, no opt-in") {
        api->execute_gcode("M106 S255", note_success,
                           [this](const MoonrakerError& err) { error_cb(err); });

        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
        CHECK(success_calls == 0);
        CHECK(queued_calls == 0);
        CHECK_FALSE(error_called);
    }
}

TEST_CASE_METHOD(BusyGuardApiFixture, "on_queued does not fire when the command is not queued",
                 "[busy_guard][mock][led]") {
    // The opt-in must be inert on the normal path: an idle printer runs the
    // command for real and only on_success may fire.
    set_idle_printing(false);
    set_manual_probe(false);
    set_print_state(PrintJobState::STANDBY);

    int queued_calls = 0;
    api->execute_gcode(
        "SET_LED LED=my_leds RED=1.00 GREEN=1.00 BLUE=1.00 SYNC=0 TRANSMIT=1", nullptr,
        [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
        [&queued_calls]() { queued_calls++; });

    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK(queued_calls == 0);
    CHECK_FALSE(error_called);
}

TEST_CASE_METHOD(BusyGuardApiFixture, "a refused move never fires on_queued",
                 "[busy_guard][mock][led]") {
    // Refusal is an error, not a queue. Both dispositions must not blur.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    int queued_calls = 0;
    api->execute_gcode(
        "G0 X10 F3000", nullptr, [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
        [&queued_calls]() { queued_calls++; });

    CHECK(error_called);
    CHECK(queued_calls == 0);
    CHECK(mock_client.gcode_script_history().empty());
}

// ============================================================================
// A physical MOVE is still REFUSED while a blocking op is active
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture, "execute_gcode refuses a raw move while homing/leveling",
                 "[busy_guard][mock]") {
    // Even through the controls API, a G0/G1 must not queue — late-firing motion is
    // the genuinely dangerous case the guard exists to prevent. #1108.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    api->execute_gcode("G0 X10 F3000", nullptr,
                       [this](const MoonrakerError& err) { error_cb(err); });

    CHECK(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(captured_error.message == "Printer is busy — try again in a moment");
    CHECK(mock_client.gcode_script_history().empty());
}

// ============================================================================
// Discretionary gcode is NOT blocked when the printer is idle
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture, "execute_gcode allows discretionary gcode when idle",
                 "[busy_guard][mock]") {
    set_idle_printing(false);
    set_manual_probe(false);
    set_print_state(PrintJobState::STANDBY);

    api->execute_gcode("M106 S128", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}

// ============================================================================
// Discretionary gcode is NOT blocked during a real file print
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode allows discretionary gcode during an active print",
                 "[busy_guard][mock]") {
    // idle_timeout is "Printing" during a file print too, but mid-print fan/temp
    // tweaks are legitimate — Klipper queues them between moves, no 60s stall.
    set_idle_printing(true);

    SECTION("PRINTING allows fan") {
        set_print_state(PrintJobState::PRINTING);
        api->execute_gcode("M106 S255", nullptr,
                           [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }

    SECTION("PAUSED allows fan") {
        set_print_state(PrintJobState::PAUSED);
        api->execute_gcode("M106 S255", nullptr,
                           [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}

// ============================================================================
// Non-discretionary (recovery/homing) gcode is NEVER blocked by the busy guard
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode always allows non-discretionary gcode even while busy",
                 "[busy_guard][mock]") {
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    SECTION("emergency stop passes") {
        api->execute_gcode("M112", nullptr, [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }

    SECTION("manual-probe control (ACCEPT) passes so the user can finish/abort") {
        set_manual_probe(true);
        api->execute_gcode("ACCEPT", nullptr, [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}

// ============================================================================
// Self-inflicted busy (app's own jog in flight) is NOT blocked
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode allows discretionary gcode while app motion is in flight",
                 "[busy_guard][mock]") {
    // idle_timeout reports "Printing" during any move, including our own jog. When
    // the app itself has motion outstanding, the busy-ness is self-inflicted, so
    // back-to-back jogs must pass instead of self-blocking with a busy toast.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    state.app_motion_activity().note_sent();

    api->execute_gcode("M106 S255", nullptr, [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK_FALSE(mock_client.gcode_script_history().empty());

    state.app_motion_activity().note_done();
}

// ============================================================================
// The refusal reason survives all the way to the user-facing string
// ============================================================================

TEST_CASE("MoonrakerError::user_message prefers a populated message for NOT_READY",
          "[busy_guard][errors]") {
    // user_message() returned the type-based string BEFORE checking `message`,
    // so every guard's specific reason was overridden by the generic
    // "wait for initialization" text — which is actively wrong for a transient
    // blocking op the user should just retry.
    MoonrakerError busy;
    busy.type = MoonrakerErrorType::NOT_READY;
    busy.message = "Printer is busy — try again in a moment";
    CHECK(busy.user_message() == "Printer is busy — try again in a moment");

    MoonrakerError homing;
    homing.type = MoonrakerErrorType::NOT_READY;
    homing.message = "Homing is disabled while a print is in progress";
    CHECK(homing.user_message() == "Homing is disabled while a print is in progress");

    // An empty message still falls back to the generic type text.
    MoonrakerError bare;
    bare.type = MoonrakerErrorType::NOT_READY;
    CHECK(bare.user_message() == "Printer is not ready. Please wait for initialization.");

    // TIMEOUT / CONNECTION_LOST deliberately keep their curated type text: their
    // `message` fields carry diagnostic detail ("WebSocket connection lost"),
    // which is jargon beside the friendly string. Narrow fix, not a blanket
    // "message always wins".
    CHECK(MoonrakerError::connection_lost("printer.gcode.script").user_message() ==
          "Connection to printer lost.");
    CHECK(MoonrakerError::timeout("printer.gcode.script", 30000).user_message() ==
          "Request timed out. The printer may be busy.");
}

TEST_CASE_METHOD(BusyGuardApiFixture, "busy refusal reaches the user as the busy reason",
                 "[busy_guard][mock][errors]") {
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    // A move is still refused (benign fan/temp/LED now queue instead); its message
    // is what the toast renders — the whole point of the guard's message.
    api->execute_gcode("G0 X10 F3000", nullptr,
                       [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.user_message() == "Printer is busy — try again in a moment");
}

// ============================================================================
// Motion API routes through the same guard
// ============================================================================

TEST_CASE_METHOD(BusyGuardApiFixture, "motion execute_gcode refuses discretionary move while busy",
                 "[busy_guard][mock][motion]") {
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    // move_axis emits a wrapped relative move ("G91\nG0 X..\nG90") — every line
    // is discretionary, so the whole jog is refused.
    SECTION("discretionary jog blocked") {
        api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK(error_called);
        CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
        CHECK(mock_client.gcode_script_history().empty());
    }

    SECTION("jog allowed once idle") {
        set_idle_printing(false);
        api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}
