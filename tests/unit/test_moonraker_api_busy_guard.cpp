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
 * A Sovol SV08 sent 4 fan commands during a Cartographer calibration, each
 * timing out at 60s. Rejecting them loses the commands, so the benign path
 * queues them with one toast (#1108).
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/print_start_phase.h"
#include "../../include/printer_state.h"
#include "../busy_guard_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class BusyGuardApiFixture : public helix::BusyGuardFixture {};

/// Stage the print-start phase the way a live collector publish would (the
/// subject write is what is_in_print_start() reads).
void set_print_start_phase(PrinterState& state, PrintStartPhase phase) {
    lv_subject_set_int(state.print_state().get_print_start_phase_subject(),
                       static_cast<int>(phase));
}

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
// Print start holds the same gate even while print_stats says PRINTING
// ============================================================================
//
// The START macro (heat soak, mesh, probing) holds Klipper's single-threaded
// gcode lock for minutes while print_stats already reports PRINTING — the
// blocking-op predicate excludes active file prints, so it never fires in that
// window and a synchronous temp RPC stalls into its 60s timeout (bundle
// SQJ8SAL7, FlashForge AD5X). The print-start phase is the signal that covers
// exactly that window.

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "benign discretionary gcode queues while print start holds the gcode lock",
                 "[busy_guard][mock]") {
    set_print_state(PrintJobState::PRINTING);
    REQUIRE_FALSE(state.is_external_blocking_operation_active()); // the bypass this pins
    set_print_start_phase(state, PrintStartPhase::HOMING);
    REQUIRE(state.print_state().is_in_print_start());

    int success_calls = 0;
    int queued_calls = 0;
    api->execute_gcode(
        "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200",
        [&success_calls]() { success_calls++; },
        [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
        [&queued_calls]() { queued_calls++; });

    CHECK(queued_calls == 1); // queued for later, not run now
    CHECK(success_calls == 0);
    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    REQUIRE(mock_client.gcode_script_history().size() == 1);

    // The interactive temp path rides the same gate.
    api->set_temperature("extruder", 200.0, nullptr,
                         [this](const MoonrakerError& err) { error_cb(err); });
    CHECK_FALSE(error_called);
    REQUIRE(mock_client.gcode_script_history().size() == 2);

    // One toast per episode: the first send claimed it, the second claims nothing.
    CHECK_FALSE(state.calibration_state().claim_busy_queue_toast());
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "busy-queue toast latch holds through a print-start episode and re-arms after",
                 "[busy_guard][mock]") {
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::BED_MESH);

    // First benign command of the episode claims the once-per-episode toast.
    api->execute_gcode("M106 S128", nullptr, [this](const MoonrakerError& err) { error_cb(err); });
    CHECK_FALSE(error_called);
    CHECK_FALSE(state.calibration_state().claim_busy_queue_toast());

    // Status keeps flowing while the START macro is still inside the gcode lock.
    // print_stats PRINTING keeps the composite blocking predicate clear, so the
    // latch must key on the print-start arm here or it re-arms mid-episode and
    // every status tick buys back a toast.
    state.update_from_status(nlohmann::json{{"idle_timeout", {{"state", "Printing"}}}});
    CHECK(state.print_state().is_in_print_start());
    CHECK_FALSE(state.calibration_state().claim_busy_queue_toast());

    // The collector stops, the phase returns to IDLE, and the next status update
    // re-arms the latch so a later episode can announce itself again.
    set_print_start_phase(state, PrintStartPhase::IDLE);
    state.update_from_status(nlohmann::json{{"idle_timeout", {{"state", "Printing"}}}});
    CHECK_FALSE(state.print_state().is_in_print_start());
    CHECK(state.calibration_state().claim_busy_queue_toast());
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode refuses a raw move while print start holds the gcode lock",
                 "[busy_guard][mock]") {
    // Same danger as the blocking-op arm: a jog firing minutes late, after the
    // mesh finishes and the print is laying down plastic, crashes the toolhead.
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::HOMING);

    api->execute_gcode("G0 X10 F3000", nullptr,
                       [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "execute_gcode runs discretionary gcode synchronously outside print start",
                 "[busy_guard][mock]") {
    // The print-start arm must not widen the gate past the start window: mid-print
    // with the phase back at IDLE, a temp tweak is the ordinary case and takes the
    // normal synchronous path (the mock answers inline).
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::IDLE);

    int success_calls = 0;
    int queued_calls = 0;
    api->execute_gcode(
        "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210",
        [&success_calls]() { success_calls++; },
        [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
        [&queued_calls]() { queued_calls++; });

    CHECK(success_calls == 1);
    CHECK(queued_calls == 0);
    CHECK_FALSE(error_called);
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "motion API refuses a jog while print start holds the gcode lock",
                 "[busy_guard][mock][motion_guard]") {
    // The motion API's gate is the refuse-always twin of the split: a jog queued
    // behind the START macro fires minutes late, so it must be refused not_ready,
    // not sent to wait out the macro.
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::HOMING);
    REQUIRE(state.print_state().is_in_print_start());

    api->motion().move_axis('X', 10.0, 6000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(mock_client.gcode_script_history().empty());

    // Phase back to IDLE (mid-print tweaks): the same jog goes through.
    set_print_start_phase(state, PrintStartPhase::IDLE);
    REQUIRE_FALSE(state.print_state().is_in_print_start());
    error_called = false;
    mock_client.clear_gcode_script_history();

    api->motion().move_axis('X', 10.0, 6000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "bypass_busy_gate sends the print-launch block synchronously during print start",
                 "[busy_guard][mock]") {
    // PrintStartController arms the preparing job BEFORE the pre-start block is
    // sent, so is_in_print_start() is already true at dispatch. A purely
    // discretionary block (a pre_start_gcode heater template) would be queued
    // fire-and-forget and its on_success — the only trigger that launches the
    // job — would never fire. The bypass keeps the launch send synchronous.
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::HOMING);
    REQUIRE(state.print_state().is_in_print_start());

    int success_calls = 0;
    int queued_calls = 0;

    SECTION("with the bypass: synchronous path, on_success fires") {
        api->execute_gcode(
            "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200",
            [&success_calls]() { success_calls++; },
            [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
            [&queued_calls]() { queued_calls++; }, true, /*bypass_busy_gate=*/true);

        CHECK(success_calls == 1);
        CHECK(queued_calls == 0);
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }

    SECTION("without the bypass: the same block is queued fire-and-forget") {
        api->execute_gcode(
            "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200",
            [&success_calls]() { success_calls++; },
            [this](const MoonrakerError& err) { error_cb(err); }, 0, false,
            [&queued_calls]() { queued_calls++; }, true, /*bypass_busy_gate=*/false);

        CHECK(success_calls == 0);
        CHECK(queued_calls == 1);
        CHECK_FALSE(error_called);
        REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    }
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "silent discretionary send during print start leaves the busy toast unclaimed",
                 "[busy_guard][mock][led]") {
    // LedAutoState applies the state theme on every print-state change, so its
    // SET_LED lands inside the gate's window at every print start. It is not a
    // user command, and the #1108 toast exists to explain one — a silent send
    // must neither show nor claim the once-per-episode latch. (The non-silent
    // side — a user command claiming exactly once — is pinned by the first
    // print-start case above.)
    set_print_state(PrintJobState::PRINTING);
    set_print_start_phase(state, PrintStartPhase::HOMING);
    REQUIRE(state.print_state().is_in_print_start());

    api->execute_gcode("SET_LED LED=my_leds RED=1.0 GREEN=1.0 BLUE=1.0 SYNC=0 TRANSMIT=1", nullptr,
                       nullptr, /*timeout_ms=*/0, /*silent=*/true);

    // Fire-and-forget to Klipper, but the toast latch is still armed: this claim
    // succeeds only because the silent send took nothing.
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");
    CHECK(state.calibration_state().claim_busy_queue_toast());
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
    state.app_motion_activity().note_sent(
        state.calibration_state().idle_timeout_busy().printing_since() -
        std::chrono::milliseconds(100));

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

// ============================================================================
// Self-busy comes from episode ownership alone
// ============================================================================

namespace {

/// Re-observe idle_timeout going high @p ago, as a status frame would have.
IdleTimeoutBusy::clock::time_point observe_episode_start(PrinterState& state,
                                                         std::chrono::milliseconds ago) {
    auto& busy = state.calibration_state().idle_timeout_busy();
    const auto start = IdleTimeoutBusy::clock::now() - ago;
    busy.set_printing(false, start);
    busy.set_printing(true, start);
    return start;
}

} // namespace

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "chained jogs into an external episode are refused once it settles",
                 "[busy_guard][mock][motion][1108]") {
    // A bed mesh started elsewhere 1.2 s ago. A Z nudge at +0.5 s went out while
    // the guard was still settling and sits queued behind the mesh, unacked;
    // every later nudge lands while it is still in flight.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    const auto start = observe_episode_start(state, std::chrono::milliseconds(1200));
    state.app_motion_activity().note_sent(start + std::chrono::milliseconds(500));
    REQUIRE(state.app_motion_activity().recently_active());

    for (int nudge = 0; nudge < 3; ++nudge) {
        INFO("nudge " << nudge);
        error_called = false;
        api->motion().move_axis('Z', 0.1, 600.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK(error_called);
    }
    CHECK(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(BusyGuardApiFixture, "the app's own jog episode passes the jogs that follow",
                 "[busy_guard][mock][motion]") {
    // The first jog went out just before idle_timeout was seen going high.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    const auto start = observe_episode_start(state, std::chrono::milliseconds(1200));
    state.app_motion_activity().note_sent(start - std::chrono::milliseconds(100));

    SECTION("a jog button held down") {
        state.app_motion_activity().note_done(start + std::chrono::milliseconds(20));
        for (int jog = 0; jog < 5; ++jog) {
            INFO("jog " << jog);
            const size_t sent_before = mock_client.gcode_script_history().size();
            api->motion().move_axis('X', 1.0, 3000.0, nullptr,
                                    [this](const MoonrakerError& err) { error_cb(err); });
            CHECK(mock_client.gcode_script_history().size() > sent_before);
        }
        CHECK_FALSE(error_called);
    }

    SECTION("a jog right after the app's own move completes, before idle_timeout clears") {
        state.app_motion_activity().note_done(IdleTimeoutBusy::clock::now() -
                                              std::chrono::milliseconds(50));
        api->motion().move_axis('Y', 1.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "a jog stuck behind someone else's gcode disowns the app's episode",
                 "[busy_guard][mock][motion][1108]") {
    // The app's jog started the episode 4 s ago and acked. A later jog has sat
    // unacked for 2.5 s: Klipper is running someone else's command, so the
    // busy-ness is no longer the app's.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    const auto start = observe_episode_start(state, std::chrono::seconds(4));
    state.app_motion_activity().note_sent(start - std::chrono::milliseconds(100));
    state.app_motion_activity().note_done(start - std::chrono::milliseconds(80));
    state.app_motion_activity().note_sent(IdleTimeoutBusy::clock::now() -
                                          std::chrono::milliseconds(2500));

    api->motion().move_axis('Z', 0.1, 600.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });
    CHECK(error_called);
    CHECK(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "a housekeeping burst that the app's fast jogs keep up passes the next jog",
                 "[busy_guard][mock][motion]") {
    // A delayed_gcode burst set idle_timeout high 1.2 s ago. A jog at +0.3 s
    // went out before anything settled and keeps the flag up.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    const auto start = observe_episode_start(state, std::chrono::milliseconds(1200));
    state.app_motion_activity().note_sent(start + std::chrono::milliseconds(300));

    SECTION("the jog acked at once: Klipper was free, the episode is the app's") {
        state.app_motion_activity().note_done(start + std::chrono::milliseconds(310));
        api->motion().move_axis('X', 1.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK_FALSE(error_called);
        CHECK_FALSE(mock_client.gcode_script_history().empty());
    }

    SECTION("the jog never acked: it is queued behind someone else's command") {
        api->motion().move_axis('X', 1.0, 3000.0, nullptr,
                                [this](const MoonrakerError& err) { error_cb(err); });
        CHECK(error_called);
        CHECK(mock_client.gcode_script_history().empty());
    }
}

TEST_CASE_METHOD(BusyGuardApiFixture,
                 "a held jog into a housekeeping burst is adopted once the jog in flight acks",
                 "[busy_guard][mock][motion]") {
    // Hold-to-jog repeats every 150 ms without waiting for acks, so the first
    // ask after SETTLE can find the previous repeat still in flight.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);
    const auto start = observe_episode_start(state, std::chrono::milliseconds(1200));
    state.app_motion_activity().note_sent(start + std::chrono::milliseconds(300));
    state.app_motion_activity().note_done(start + std::chrono::milliseconds(310));
    state.app_motion_activity().note_sent(IdleTimeoutBusy::clock::now() -
                                          std::chrono::milliseconds(50));

    // Undecided while that repeat is in flight...
    api->motion().move_axis('X', 1.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });
    REQUIRE(error_called);

    // ...and the app's once it acks promptly.
    state.app_motion_activity().note_done();
    error_called = false;
    api->motion().move_axis('X', 1.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });
    CHECK_FALSE(error_called);
    CHECK_FALSE(mock_client.gcode_script_history().empty());
}
