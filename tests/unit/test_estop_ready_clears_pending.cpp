// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A shutdown that lands inside a suppression window is LATCHED rather than
// dropped, and a re-check surfaces it when the window ends (#1345). That latch
// needs an exit for the case it was built to tolerate: klippy coming back.
//
// The klippy_state READY observer clears pending_recovery_reason_ for exactly
// that reason - anything the window was holding describes a state that no
// longer exists, and leaving it set makes the re-check raise a full-screen
// recovery dialog for a shutdown the printer already recovered from, seconds
// after it came back.
//
// Covered nowhere else: the existing latch tests drive show_recovery_for()
// and deinit_subjects(), never a klippy READY through the observer, and the
// observer tests that do deliver READY assert on recovery_reason_ rather than
// on the latch.

#include "ui_emergency_stop.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {
using Access = EmergencyStopOverlayTestAccess;
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "A klippy READY drops the shutdown the window was holding",
                 "[recovery][suppress][1345]") {
    auto& estop = EmergencyStopOverlay::instance();
    Access::reset_recovery_reason(estop);
    Access::reset_suppression(estop);
    Access::reset_pending_recovery_reason(estop);
    // The restart guard is sampled by the READY handler and declines the
    // re-check on its own; leaving it armed would make the final assertion
    // pass whether or not the latch was cleared.
    Access::set_restart_in_progress(estop, false);

    // Wired exactly as Application::init_panel_subjects() does it.
    estop.init(state(), api());
    estop.create();

    // create() re-arms the deliberate first-fire skip, so burn it with a
    // transition the recovery path ignores. Without this the READY below is
    // the skipped fire and the test would pass against a missing clear.
    state().set_klippy_state_sync(helix::KlippyState::STARTUP);
    process_lvgl(20);
    helix::ui::UpdateQueue::instance().drain();

    // The host goes down inside an intentional-restart window: swallowed for
    // now, latched, with a one-shot re-check armed for the far end.
    estop.suppress_recovery_dialog(1000);
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(Access::pending_recovery_reason(estop) == RecoveryReason::SHUTDOWN);
    REQUIRE(Access::recovery_reason(estop) == RecoveryReason::NONE);
    // The re-check really is armed, so "nothing appears" below is a decision
    // the code made rather than a timer that was never scheduled.
    REQUIRE(Access::recheck_timer_armed(estop));

    // Klipper comes back well inside the window.
    state().set_klippy_state_sync(helix::KlippyState::READY);
    process_lvgl(20);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(Access::pending_recovery_reason(estop) == RecoveryReason::NONE);

    // Past the window. The re-check fires with nothing behind it, so the user
    // is not shown a recovery dialog for a printer that is already running.
    process_lvgl(1500);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(Access::recovery_reason(estop) == RecoveryReason::NONE);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") == nullptr);

    estop.deinit_subjects();
    Access::reset_suppression(estop);
    Access::reset_pending_recovery_reason(estop);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Without a klippy READY the latched shutdown still surfaces",
                 "[recovery][suppress][1345]") {
    // The control for the test above. If the latch were cleared by something
    // other than the READY - the window expiring, the re-check running, a stray
    // reset - the first test would pass with the observer's clear deleted, and
    // this one would fail.
    auto& estop = EmergencyStopOverlay::instance();
    Access::reset_recovery_reason(estop);
    Access::reset_suppression(estop);
    Access::reset_pending_recovery_reason(estop);
    Access::set_restart_in_progress(estop, false);

    estop.init(state(), api());
    estop.create();

    state().set_klippy_state_sync(helix::KlippyState::STARTUP);
    process_lvgl(20);
    helix::ui::UpdateQueue::instance().drain();

    estop.suppress_recovery_dialog(1000);
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(Access::pending_recovery_reason(estop) == RecoveryReason::SHUTDOWN);

    // No READY is ever delivered - the printer stayed down.
    process_lvgl(1500);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(Access::recovery_reason(estop) == RecoveryReason::SHUTDOWN);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") != nullptr);

    estop.deinit_subjects();
    Access::reset_recovery_reason(estop);
    Access::reset_suppression(estop);
    Access::reset_pending_recovery_reason(estop);
    helix::ui::UpdateQueue::instance().drain();
}
