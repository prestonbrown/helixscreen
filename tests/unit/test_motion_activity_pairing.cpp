// SPDX-License-Identifier: GPL-3.0-or-later
// tests/unit/test_motion_activity_pairing.cpp

/**
 * @file test_motion_activity_pairing.cpp
 * @brief Direct coverage for the note_sent()/note_done() pairing that
 *        MoonrakerMotionAPI::execute_gcode performs around discretionary sends.
 *
 * The busy guard reads AppMotionActivity to tell self-inflicted busy (our own
 * jog is executing, so idle_timeout reports "Printing") from an external
 * blocking op. That attribution is only sound while the inflight count is
 * balanced:
 *   - a leaked note_sent() makes the app believe motion is outstanding forever,
 *     permanently defeating the guard;
 *   - a missing note_sent() makes back-to-back jogs self-block with a toast;
 *   - a double note_done() under-counts concurrent sends.
 *
 * Existing coverage stamps the counter by hand and only verifies the guard
 * READS it (test_moonraker_api_busy_guard.cpp, test_printer_state_blocking_op.cpp).
 * These tests assert execute_gcode WRITES it.
 */

#include "../../include/app_motion_activity.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../busy_guard_fixture.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class MotionPairingFixture : public helix::BusyGuardFixture {
  public:
    bool motion_inflight() {
        return helix::activity_inflight(state.app_motion_activity());
    }

    /**
     * True once note_done() has fired at all: note_done() stamps the grace
     * timestamp, and an activity that was never stamped has last_done_ == 0,
     * which never reads active. Only meaningful while inflight is zero.
     */
    bool motion_note_done_fired() {
        return state.app_motion_activity().recently_active();
    }

    /// Destructive — call it last in a test.
    int drain_inflight() {
        return helix::drain_activity_inflight(state.app_motion_activity());
    }
};

} // namespace

// ============================================================================
// Balanced on both terminal paths
// ============================================================================

TEST_CASE_METHOD(MotionPairingFixture,
                 "motion execute_gcode balances the inflight count on the success path",
                 "[motion][busy_guard][mock]") {
    // move_axis emits "G91\nG0 X10 F3000\nG90" — wholly discretionary, so the
    // send is stamped. The mock dispatches gcode.script success synchronously.
    bool success_called = false;
    api->motion().move_axis(
        'X', 10.0, 3000.0, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(success_called);
    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");

    // note_done() ran (it stamps the grace timestamp) ...
    CHECK(motion_note_done_fired());
    // ... and left nothing outstanding.
    CHECK_FALSE(motion_inflight());
    CHECK(drain_inflight() == 0);
}

TEST_CASE_METHOD(MotionPairingFixture,
                 "motion execute_gcode balances the inflight count on the error path",
                 "[motion][busy_guard][mock]") {
    // Force the RPC to fail after the stamp: the error wrapper owes the same
    // single note_done() the success wrapper does, or the count leaks upward.
    mock_client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "forced RPC failure", "G0 X10");

    bool success_called = false;
    api->motion().move_axis(
        'X', 10.0, 3000.0, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK_FALSE(success_called);

    CHECK(motion_note_done_fired());
    CHECK_FALSE(motion_inflight());
    CHECK(drain_inflight() == 0);
}

// ============================================================================
// Exactly one note_done per stamped send
// ============================================================================

TEST_CASE_METHOD(MotionPairingFixture, "motion execute_gcode stamps exactly one note_done per send",
                 "[motion][busy_guard][mock]") {
    // Two unrelated sends already outstanding. A balanced send must leave the
    // count exactly where it found it: a leaked note_sent reads 3, a double
    // note_done reads 1. Only exact pairing reads 2.
    state.app_motion_activity().note_sent();
    state.app_motion_activity().note_sent();

    api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    CHECK(drain_inflight() == 2);
}

// ============================================================================
// Non-discretionary gcode is never attributed to app motion
// ============================================================================

TEST_CASE_METHOD(MotionPairingFixture, "motion execute_gcode never stamps non-discretionary gcode",
                 "[motion][busy_guard][mock]") {
    // G28 is homing: exempt from the busy guard, so it must not be attributed
    // to app motion either. Both reads false proves neither note_sent() nor
    // note_done() ran — a stamped-and-balanced send would still show
    // note_done_fired().
    bool success_called = false;
    api->motion().home_axes(
        "X", [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(success_called);
    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");

    CHECK_FALSE(motion_inflight());
    CHECK_FALSE(motion_note_done_fired());
}

// ============================================================================
// Early-return guard paths never stamp
// ============================================================================

TEST_CASE_METHOD(MotionPairingFixture,
                 "motion execute_gcode does not stamp when the busy guard refuses the send",
                 "[motion][busy_guard][mock]") {
    // An external blocking op holds the gcode lock: execute_gcode returns
    // before note_sent(). A stamp here would leak forever — nothing acks a
    // send that was never made, and the leak would defeat the guard for the
    // rest of the session.
    set_idle_printing(true);
    set_print_state(PrintJobState::STANDBY);

    api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(mock_client.gcode_script_history().empty());

    CHECK_FALSE(motion_inflight());
    CHECK_FALSE(motion_note_done_fired());
}

TEST_CASE_METHOD(MotionPairingFixture,
                 "motion execute_gcode does not stamp when klippy is not ready",
                 "[motion][busy_guard][mock]") {
    // The klippy-ready gate is the earliest return in execute_gcode.
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);

    api->motion().move_axis('X', 10.0, 3000.0, nullptr,
                            [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(mock_client.gcode_script_history().empty());

    CHECK_FALSE(motion_inflight());
    CHECK_FALSE(motion_note_done_fired());
}
