// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file busy_guard_fixture.h
 * @brief Shared setup for the tests that exercise the discretionary-gcode busy guard.
 *
 * Three test files (`test_moonraker_api_busy_guard.cpp`,
 * `test_motion_activity_pairing.cpp`, `test_busy_toast_app_initiated.cpp`) all
 * need the same thing: a READY printer with a connected mock client and a real
 * `MoonrakerAPI` over it, parked in a non-blocking state. They had grown three
 * byte-identical copies of the constructor, destructor, `set_print_state()`,
 * `set_idle_printing()`, `error_cb()` and the member set — which is why a single
 * change to what feeds `is_blocking_operation_active()` needed an edit in each
 * one, and why it was easy to miss the third.
 */

#include "../include/moonraker_api.h"
#include "../include/moonraker_client_mock.h"
#include "../include/printer_state.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/printer_state_test_access.h"

#include <chrono>
#include <memory>

namespace helix {

/**
 * @brief A READY printer, connected mock client, and a real MoonrakerAPI over it.
 *
 * Starts parked in the non-blocking state (STANDBY, idle_timeout clear) so a
 * send reaches the client unless the test arranges otherwise.
 */
class BusyGuardFixture : public LVGLTestFixture {
  public:
    BusyGuardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        // ORDERING IS LOAD-BEARING: klippy subjects initialize to SHUTDOWN, so
        // this call is a transition that resets the volatile subjects. It must
        // come before idle_timeout is set, or the reset wipes it and the test
        // silently exercises the non-busy path (see test_led_controller.cpp).
        // It is also what stops execute_gcode()'s klippy-halted gate from
        // rejecting everything.
        state.set_klippy_state_sync(KlippyState::READY);
        set_print_state(PrintJobState::STANDBY);
        set_idle_printing(false);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~BusyGuardFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    void set_print_state(PrintJobState s) {
        lv_subject_set_int(state.get_print_state_enum_subject(), static_cast<int>(s));
    }

    /**
     * @brief Report a SUSTAINED idle_timeout "Printing", i.e. a blocking op already under way.
     *
     * Writing the subject alone no longer reaches the guard — it reads
     * `IdleTimeoutBusy`, which requires the flag to hold for `SETTLE` first. The
     * settle behaviour itself is covered by test_idle_timeout_busy.cpp; every
     * case in these files means an op that has already outlasted it.
     */
    void set_idle_printing(bool on) {
        PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, on);
    }

    void set_manual_probe(bool on) {
        lv_subject_set_int(state.get_manual_probe_active_subject(), on ? 1 : 0);
    }

    /// Make an external blocking op (calibration / console macro) look active.
    void begin_blocking_episode() {
        set_print_state(PrintJobState::STANDBY);
        set_idle_printing(true);
    }

    void error_cb(const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

    bool error_called = false;
    MoonrakerError captured_error;
};

/**
 * @brief Whether an activity tracker has anything in flight, ignoring its grace window.
 *
 * `recently_active(now)` is `inflight_ > 0 || (now - last_done_ < GRACE_WINDOW)`.
 * Passing a `now` past the grace window retires the second term, collapsing the
 * call to a pure `inflight_ > 0` read. Neither AppMotionActivity nor
 * AppMacroActivity exposes an inflight getter; this is the supported way to
 * isolate the counter (the same trick test_app_motion_activity.cpp uses).
 */
template <typename Activity> bool activity_inflight(Activity& activity) {
    return activity.recently_active(Activity::clock::now() + Activity::GRACE_WINDOW +
                                    std::chrono::seconds(1));
}

/**
 * @brief Exact inflight count for an activity tracker.
 *
 * Uses the only read the public API allows: retire the counter one `note_done()`
 * at a time and count the steps. Destructive — call it last in a test.
 */
template <typename Activity> int drain_activity_inflight(Activity& activity) {
    int n = 0;
    while (activity_inflight(activity) && n < 100) {
        activity.note_done();
        ++n;
    }
    return n;
}

} // namespace helix
