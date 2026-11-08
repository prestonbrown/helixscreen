// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_recovery_dialog_threading.cpp
 * @brief show_recovery_for() must not touch UI state on the caller's thread.
 *
 * Its callers are background threads: MoonrakerClient's event handler runs on
 * the libhv event loop (KLIPPY_DISCONNECTED / KLIPPY_SHUTDOWN), and
 * AbortManager reaches it the same way — abort_manager.cpp even documents
 * relying on it to defer for them.
 *
 * Dialog *creation* was always deferred. What was not: the preamble read and
 * wrote recovery_dialog_ and recovery_reason_ and queried ModalStack directly
 * on the calling thread, racing the main thread doing the same during teardown.
 *
 * Asserting "a dialog appeared" cannot catch that — it appears either way,
 * because the inner hop was always there. recovery_reason_ is the discriminator:
 * it must still be untouched when the background call returns, and settle only
 * once the queue drains.
 *
 * This test is only meaningful because ui_emergency_stop.o is linked into the
 * test binary. A hand-written copy in ui_test_utils.cpp would make anything
 * asserted here validate the copy instead of the shipped code.
 */

#include "ui_emergency_stop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"

#include <lvgl.h>
#include <thread>

#include "../catch_amalgamated.hpp"

using Access = EmergencyStopOverlayTestAccess;

TEST_CASE_METHOD(LVGLUITestFixture, "show_recovery_for defers all UI state off the caller's thread",
                 "[recovery][threading]") {
    auto& estop = EmergencyStopOverlay::instance();
    Access::reset_recovery_reason(estop);
    Access::reset_suppression(estop); // don't inherit a window from an earlier test
    REQUIRE(Access::recovery_reason(estop) == RecoveryReason::NONE);

    // Exactly how MoonrakerClient's event handler reaches this.
    std::thread bg([&estop]() { estop.show_recovery_for(RecoveryReason::SHUTDOWN); });
    bg.join();

    // The call has fully returned on the background thread. Nothing it owns may
    // have moved yet — reading SHUTDOWN here means the preamble mutated shared
    // state off the main thread.
    CHECK(Access::recovery_reason(estop) == RecoveryReason::NONE);

    process_lvgl(50);

    // ...and the work must still actually happen, not merely be dropped.
    CHECK(Access::recovery_reason(estop) == RecoveryReason::SHUTDOWN);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "show_recovery_for still suppresses on the caller's thread",
                 "[recovery][threading]") {
    // Suppression stays on the calling thread deliberately: the deadline is
    // atomic, and an intentional restart bursts these events. Queueing each one
    // only to discard it on the main thread is waste, so a suppressed call must
    // remain free — and must not queue anything.
    auto& estop = EmergencyStopOverlay::instance();
    Access::reset_recovery_reason(estop);
    estop.suppress_recovery_dialog(recovery_suppression::SHORT);

    std::thread bg([&estop]() { estop.show_recovery_for(RecoveryReason::SHUTDOWN); });
    bg.join();

    process_lvgl(50);

    CHECK(Access::recovery_reason(estop) == RecoveryReason::NONE);
    CHECK(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") == nullptr);

    // The overlay is a process-wide singleton: leaving a live 5s window here
    // suppresses whichever test Catch2 happens to run next.
    Access::reset_suppression(estop);
}
