// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Suppression logic tests (lightweight, just need LVGL tick)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Recovery suppression - basic timing", "[recovery][suppress]") {
    auto& estop = EmergencyStopOverlay::instance();

    SECTION("not suppressed by default") {
        REQUIRE_FALSE(estop.is_recovery_suppressed());
    }

    SECTION("suppressed after calling suppress_recovery_dialog") {
        estop.suppress_recovery_dialog(5000);
        REQUIRE(estop.is_recovery_suppressed());
    }

    SECTION("suppression expires after duration") {
        estop.suppress_recovery_dialog(10); // 10ms
        REQUIRE(estop.is_recovery_suppressed());

        // Advance LVGL tick past suppression window
        // Only need tick advancement (not timer processing) for time-based check
        lv_tick_inc(50);
        REQUIRE_FALSE(estop.is_recovery_suppressed());
    }
}

// ============================================================================
// A suppressed shutdown must not be a swallowed shutdown (#1345)
// ============================================================================
//
// suppress_recovery_dialog() exists so an intentional restart does not flash a
// recovery dialog on its way down. Both producers of that dialog are edge- or
// event-driven - the klippy_state observer fires on a transition, and
// MoonrakerEventRoute::RecoveryShutdown on an event - and a host that goes down
// and STAYS down produces exactly one of each. Land both inside the window and
// the dialog is not delayed, it is gone: nothing ever re-raises it.
//
// RecoverySuppression::LONG's own comment already states the rule this
// restores - "Prefer a spurious dialog over a silently-eaten shutdown" - and
// every caller of the suppression is exposed to it, not just the macro path
// that made it reachable from a home-screen button.

TEST_CASE_METHOD(LVGLTestFixture,
                 "A shutdown that lands inside a suppression window surfaces when it expires",
                 "[recovery][suppress][1345]") {
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);

    // A macro that takes the host down arms this before Klipper disappears.
    estop.suppress_recovery_dialog(50);
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    helix::ui::UpdateQueue::instance().drain();

    // Correctly swallowed while the host is still expected back.
    REQUIRE(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::NONE);

    // It never came back. Past the window the shutdown is real and the user has
    // to be told - this is the assertion that fails without the re-check.
    // process_lvgl(), not lv_tick_inc() + lv_timer_handler(): the raw handler's
    // do-while loop does not terminate under the test harness (see
    // ui_test_utils.h lv_timer_handler_safe()), and the fixture helper is what
    // advances the virtual clock and runs ready timers together.
    process_lvgl(100);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(estop.is_recovery_suppressed());
    REQUIRE(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::SHUTDOWN);

    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}

TEST_CASE_METHOD(LVGLTestFixture, "An expiring window with no shutdown behind it raises nothing",
                 "[recovery][suppress][1345]") {
    // The guard on the test above: the re-check must surface a shutdown that
    // actually arrived, not manufacture one at the end of every suppression
    // window. A firmware restart that completes cleanly suppresses, reports
    // nothing, and expires silently.
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);

    estop.suppress_recovery_dialog(50);

    process_lvgl(100);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::NONE);

    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A re-check a guard declines keeps the shutdown latched instead of eating it",
                 "[recovery][suppress][1345]") {
    // The other half of the latch's contract. Surfacing the reason when the
    // window expires is only non-lossy if the reason survives a re-check that
    // show_recovery_for_main() refuses: the latch is the ONLY copy by then, so
    // consuming it ahead of the guards drops the shutdown exactly the way an
    // un-latched suppression window used to.
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
    EmergencyStopOverlayTestAccess::reset_pending_recovery_reason(estop);

    // The recovery dialog's Restart button sets this, and only a klippy READY
    // clears it. An MCU that comes back up in a hard shutdown never delivers one
    // - a Klipper RESTART cannot clear an MCU shutdown - so the guard is still
    // true when the window ends, and stays true.
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, true);

    estop.suppress_recovery_dialog(50);
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(EmergencyStopOverlayTestAccess::pending_recovery_reason(estop) ==
            RecoveryReason::SHUTDOWN);

    // Window expires, the re-check fires, the restart guard declines it.
    process_lvgl(100);

    CHECK(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::NONE);
    REQUIRE(EmergencyStopOverlayTestAccess::pending_recovery_reason(estop) ==
            RecoveryReason::SHUTDOWN);

    // The restart resolves - or the user stops waiting on it - and the retry
    // that the decline armed has to surface what was held.
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);
    process_lvgl(3000);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::SHUTDOWN);
    CHECK(EmergencyStopOverlayTestAccess::pending_recovery_reason(estop) == RecoveryReason::NONE);

    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
    EmergencyStopOverlayTestAccess::reset_pending_recovery_reason(estop);
}

// ============================================================================
// The restart guard has to end by itself
// ============================================================================
//
// restart_klipper() and firmware_restart() arm a restart window and no
// suppression window, so a SHUTDOWN they produce is declined outright by
// show_recovery_for_main() - never latched, never re-checked. That is correct
// while the restart is genuinely in flight and catastrophic once it is not: the
// window used to be a bare bool whose only exit was the klippy-READY handler,
// and a printer whose MCU comes back up in a hard shutdown never sends a READY
// (a Klipper RESTART cannot clear an MCU shutdown - the K2's behaviour). The
// flag stayed true and every later recovery dialog was dropped at the guard,
// leaving a halted printer looking idle.

TEST_CASE_METHOD(LVGLTestFixture, "The restart guard suppresses a shutdown inside its window",
                 "[recovery][suppress]") {
    // The half that must NOT regress: an expiry that fires early, or a guard
    // reduced to a no-op, both show a recovery dialog for the SHUTDOWN every
    // restart passes through on its way down.
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
    EmergencyStopOverlayTestAccess::reset_pending_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, true);

    REQUIRE(estop.is_expected_restart());

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    CHECK(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::NONE);
    // Well inside the window it is still in flight, so the dialog stays away.
    lv_tick_inc(RecoverySuppression::RESTART_FLAG_TIMEOUT / 2);
    CHECK(estop.is_expected_restart());

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);
    REQUIRE(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::NONE);

    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A restart that never reports READY stops suppressing the recovery dialog",
                 "[recovery][suppress]") {
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
    EmergencyStopOverlayTestAccess::reset_pending_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, true);

    // No klippy READY is ever delivered here, so nothing in production clears
    // the window - the backstop is the only thing that can end it.
    lv_tick_inc(RecoverySuppression::RESTART_FLAG_TIMEOUT + 1000);
    CHECK_FALSE(estop.is_expected_restart());

    // And the printer is still down, which the user now has to be told.
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);
    REQUIRE(EmergencyStopOverlayTestAccess::recovery_reason(estop) == RecoveryReason::SHUTDOWN);

    EmergencyStopOverlayTestAccess::reset_recovery_reason(estop);
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);
}

TEST_CASE_METHOD(LVGLTestFixture, "Expected restart - tracks SAVE_CONFIG suppression window",
                 "[recovery][suppress]") {
    // is_expected_restart() is the window the status icon, nav manager, and
    // panel widget manager consult to treat a transient Klipper SHUTDOWN as a
    // restart rather than an error.
    auto& estop = EmergencyStopOverlay::instance();

    SECTION("not an expected restart by default") {
        REQUIRE_FALSE(estop.is_expected_restart());
    }

    SECTION("true while a SAVE_CONFIG suppression window is active") {
        estop.suppress_recovery_dialog(5000);
        REQUIRE(estop.is_expected_restart());
    }

    SECTION("false once the window expires") {
        estop.suppress_recovery_dialog(10); // 10ms
        REQUIRE(estop.is_expected_restart());
        lv_tick_inc(50);
        REQUIRE_FALSE(estop.is_expected_restart());
    }
}

// ============================================================================
// Suppression window sizing (must not swallow a genuine late shutdown)
// ============================================================================
//
// Creality's stock K2 code chains a SECOND config write + Klipper restart after
// a SAVE_CONFIG settles (motor_control_wrapper.py writes the CFS Tn_data via
// CXSAVE_CONFIG). Widening LONG to 60000 to cover that was tried and REVERTED:
// a real unrecoverable shutdown was observed landing 57s after SAVE_CONFIG, and
// because the suppression check is edge-triggered on the klippy state
// transition, a 60s window would have eaten it and shown the user nothing.
//
// These tests pin the window SHORT. A spurious recovery dialog for a chained
// restart is the accepted cost of never hiding a real shutdown.

TEST_CASE_METHOD(LVGLTestFixture,
                 "Recovery suppression - LONG window closes well before a late shutdown",
                 "[recovery][suppress]") {
    auto& estop = EmergencyStopOverlay::instance();

    estop.suppress_recovery_dialog(RecoverySuppression::LONG);
    REQUIRE(estop.is_recovery_suppressed());

    // Still suppressed across the immediate SAVE_CONFIG restart it exists for.
    lv_tick_inc(RecoverySuppression::LONG / 2);
    REQUIRE(estop.is_recovery_suppressed());

    // A genuine shutdown at ~57s (the observed real-world case) MUST reach the
    // user. This assertion fails if LONG is widened to cover chained restarts.
    lv_tick_inc(57000 - (RecoverySuppression::LONG / 2));
    INFO("RecoverySuppression::LONG = " << RecoverySuppression::LONG);
    REQUIRE_FALSE(estop.is_recovery_suppressed());
    REQUIRE_FALSE(estop.is_expected_restart());
}

TEST_CASE("Recovery suppression - LONG stays short enough to surface a real shutdown",
          "[recovery][suppress]") {
    // Ordering invariant across the tiers.
    REQUIRE(RecoverySuppression::SHORT < RecoverySuppression::NORMAL);
    REQUIRE(RecoverySuppression::NORMAL < RecoverySuppression::LONG);

    // Hard ceiling: a real unrecoverable shutdown was seen 57s after
    // SAVE_CONFIG. The window must close comfortably before that, or the
    // edge-triggered check swallows it. Do NOT raise this to chase chained
    // config writes — that regression is why this bound exists.
    REQUIRE(RecoverySuppression::LONG < 30000);
}

// ============================================================================
// Recovery reason enum coverage
// ============================================================================

TEST_CASE("RecoveryReason enum values", "[recovery]") {
    REQUIRE(static_cast<int>(RecoveryReason::NONE) == 0);
    REQUIRE(RecoveryReason::SHUTDOWN != RecoveryReason::DISCONNECTED);
    REQUIRE(RecoveryReason::SHUTDOWN != RecoveryReason::NONE);
    REQUIRE(RecoveryReason::DISCONNECTED != RecoveryReason::NONE);
}

// ============================================================================
// Full integration tests (need XML components, subjects, PrinterState)
// ============================================================================
//
// TODO(macOS): These LVGLUITestFixture-based recovery tests SIGSEGV consistently
// on macOS nightly runs (first observed 2026-04-19, run 24622323383). The crash
// manifests during the first test case that instantiates LVGLUITestFixture in
// this file — subsequent tests can't run because the binary dies with exit 139.
// The failure does NOT reproduce on Linux (passes in make test-run and in the
// Ubuntu parallel shard job). Cannot debug without a Mac; gating on __APPLE__
// until root-caused. Likely relates to one of: SDL2 display setup on kqueue,
// lv_i18n locale load (see `Failed to set lv_i18n locale to 'en'` warnings in
// the macOS log), or fixture teardown ordering after the preceding
// test_timezone_env assertion failure.
#ifndef __APPLE__

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - SHUTDOWN shows dialog",
                 "[recovery][integration]") {
    auto& estop = EmergencyStopOverlay::instance();

    // Trigger SHUTDOWN via show_recovery_for (bypasses observer, tests the method directly)
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50); // Allow async callback to execute

    // Dialog should be visible - find it by the backdrop name
    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    // Title should say "Printer Shutdown"
    lv_obj_t* title = lv_obj_find_by_name(dialog, "recovery_title");
    REQUIRE(title != nullptr);
    REQUIRE(std::string(lv_label_get_text(title)).find("Shutdown") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - DISCONNECTED shows dialog",
                 "[recovery][integration]") {
    auto& estop = EmergencyStopOverlay::instance();

    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    // Title should say "Disconnected"
    lv_obj_t* title = lv_obj_find_by_name(dialog, "recovery_title");
    REQUIRE(title != nullptr);
    REQUIRE(std::string(lv_label_get_text(title)).find("Disconnected") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - deduplication",
                 "[recovery][integration]") {
    auto& estop = EmergencyStopOverlay::instance();

    // Show SHUTDOWN first
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* first_dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(first_dialog != nullptr);

    // Try DISCONNECTED - should NOT create a second dialog
    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    // Count recovery dialogs - should only be one (search recursively)
    int count = 0;
    uint32_t child_cnt = lv_obj_get_child_count(lv_screen_active());
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* backdrop = lv_obj_get_child(lv_screen_active(), i);
        if (!backdrop)
            continue;
        // Modal backdrops are direct children of screen; check if they contain our dialog
        lv_obj_t* card = lv_obj_find_by_name(backdrop, "klipper_recovery_card");
        if (card) {
            count++;
        }
    }
    REQUIRE(count == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - returns after a backdrop dismiss",
                 "[recovery][integration]") {
    // helix::ui::modal_show() unconditionally wires Modal::backdrop_click_cb, so
    // a tap outside the card dismisses the recovery dialog without ever reaching
    // dismiss_recovery_dialog(). EmergencyStopOverlay is then holding a pointer to
    // a modal that has left the ModalStack, and the NEXT shutdown must still
    // reach the user rather than being spent clearing that stale pointer.
    auto& estop = EmergencyStopOverlay::instance();

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);

    // Drive the real dismissal: the click handler lives on the backdrop and only
    // acts when the event target IS the backdrop, which is exactly what
    // lv_obj_send_event() on the backdrop produces.
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);

    // The 150ms exit animation plus the deferred backdrop deletion that its
    // completion callback schedules.
    process_lvgl(400);

    REQUIRE(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") == nullptr);
    // Pointer comparison only — the stack entry is gone, so no dereference.
    REQUIRE(ModalStack::instance().backdrop_for(dialog) == nullptr);

    // Klipper shuts down again. A second dialog must appear on THIS event, not
    // on the one after it.
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    REQUIRE(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") != nullptr);
}

namespace {
/// Clears the suppression deadline on scope exit.
///
/// EmergencyStopOverlay is a process-wide singleton and Catch2 runs the whole
/// suite in one process, so a suppression window left armed leaks into every
/// later test in this file — the fixture only advances the tick ~50ms per test,
/// nowhere near a multi-second window. A trailing cleanup call is not enough
/// either: a failed REQUIRE throws and unwinds straight past it.
struct SuppressionScope {
    ~SuppressionScope() {
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - suppression prevents showing",
                 "[recovery][integration]") {
    auto& estop = EmergencyStopOverlay::instance();

    // Suppress for 5 seconds — cleared on scope exit, see SuppressionScope
    SuppressionScope suppression_cleanup;
    estop.suppress_recovery_dialog(5000);

    // Try both reasons - neither should show
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog == nullptr);

    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Unified recovery dialog - buttons present",
                 "[recovery][integration]") {
    auto& estop = EmergencyStopOverlay::instance();

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    // All three buttons should exist. The two restart actions live inside a
    // modal_button_row, so they carry that component's btn_primary/btn_secondary
    // names; recovery_restart_actions is the wrapper whose hidden flag gates them.
    lv_obj_t* restart_btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(restart_btn != nullptr);

    lv_obj_t* firmware_btn = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(firmware_btn != nullptr);

    lv_obj_t* dismiss_btn = lv_obj_find_by_name(dialog, "recovery_dismiss_btn");
    REQUIRE(dismiss_btn != nullptr);

    lv_obj_t* restart_row = lv_obj_find_by_name(dialog, "recovery_restart_actions");
    REQUIRE(restart_row != nullptr);
}

// ============================================================================
// Button state tests (restart buttons hidden when DISCONNECTED)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - SHUTDOWN shows all buttons visible",
                 "[recovery][buttons]") {
    auto& estop = EmergencyStopOverlay::instance();

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* restart_row = lv_obj_find_by_name(dialog, "recovery_restart_actions");
    lv_obj_t* dismiss_btn = lv_obj_find_by_name(dialog, "recovery_dismiss_btn");

    REQUIRE(restart_row != nullptr);
    REQUIRE(dismiss_btn != nullptr);

    // All buttons visible for SHUTDOWN (can restart)
    REQUIRE_FALSE(lv_obj_has_flag(restart_row, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - DISCONNECTED hides restart buttons",
                 "[recovery][buttons]") {
    auto& estop = EmergencyStopOverlay::instance();

    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* restart_row = lv_obj_find_by_name(dialog, "recovery_restart_actions");
    lv_obj_t* dismiss_btn = lv_obj_find_by_name(dialog, "recovery_dismiss_btn");

    REQUIRE(restart_row != nullptr);
    REQUIRE(dismiss_btn != nullptr);

    // Restart row hidden when disconnected (can't restart). Hiding the wrapper takes
    // modal_button_row's leading divider with it, so no orphaned rule is left behind.
    REQUIRE(lv_obj_has_flag(restart_row, LV_OBJ_FLAG_HIDDEN));

    // Dismiss always visible
    REQUIRE_FALSE(lv_obj_has_flag(dismiss_btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - SHUTDOWN then DISCONNECTED updates buttons",
                 "[recovery][buttons]") {
    auto& estop = EmergencyStopOverlay::instance();

    // Show SHUTDOWN first - all buttons visible
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* restart_row = lv_obj_find_by_name(dialog, "recovery_restart_actions");
    REQUIRE(restart_row != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(restart_row, LV_OBJ_FLAG_HIDDEN));

    // Connection drops - DISCONNECTED fires, buttons should update
    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    // Restart row should now be hidden
    REQUIRE(lv_obj_has_flag(restart_row, LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// State message display tests
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - SHUTDOWN shows state_message from webhooks",
                 "[recovery][state_message]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // Simulate Klipper state_message arriving via webhooks subscription
    const std::string error_msg = "flashforge_loadcell: Max force exceeded. Last weight was: 912g\n"
                                  "Once the underlying issue is corrected, use the\n"
                                  "\"FIRMWARE_RESTART\" command to reset the firmware.";
    ps.set_klippy_state_message(error_msg);

    // Trigger SHUTDOWN recovery dialog — the dialog content is set in the
    // async callback which reads printer_state_->get_klippy_state_message()
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    // Message should contain the actual Klipper error, not the generic text
    lv_obj_t* message = lv_obj_find_by_name(dialog, "recovery_message");
    REQUIRE(message != nullptr);
    std::string displayed = lv_label_get_text(message);
    REQUIRE(displayed.find("Max force exceeded") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Recovery dialog - SHUTDOWN without state_message shows generic",
                 "[recovery][state_message]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // Ensure no stale state_message from previous test
    ps.set_klippy_state_message("");

    // No state_message set — should show generic text
    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* message = lv_obj_find_by_name(dialog, "recovery_message");
    REQUIRE(message != nullptr);
    std::string displayed = lv_label_get_text(message);
    // Generic message mentions "shutdown" or "emergency stop"
    REQUIRE(displayed.find("shutdown") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - DISCONNECTED ignores state_message",
                 "[recovery][state_message]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // Even with a state_message set, DISCONNECTED should show its own text
    ps.set_klippy_state_message("some error");

    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    lv_obj_t* message = lv_obj_find_by_name(dialog, "recovery_message");
    REQUIRE(message != nullptr);
    std::string displayed = lv_label_get_text(message);
    // Should show the disconnect-specific message, not the error
    REQUIRE(displayed.find("some error") == std::string::npos);
    REQUIRE(displayed.find("disconnected") != std::string::npos);
}

// ============================================================================
// JSON state_message decoding
//
// Klipper (notably K2 builds) emits shutdown reasons as a JSON envelope rather
// than prose. Showing the envelope verbatim put a literal `{"code":"key1",
// "msg":"..."}` in front of the user. The dialog routes state_message through
// GcodeErrorRouter::clean_error_text() so the msg text is displayed and the code
// is split out into its own header slot.
// ============================================================================

namespace {

// Reads the dialog's message + code labels after a recovery show. Returns the
// message text; writes the code label text (or "" when the label is hidden).
std::string recovery_texts(lv_obj_t* dialog, std::string& out_code) {
    lv_obj_t* message = lv_obj_find_by_name(dialog, "recovery_message");
    REQUIRE(message != nullptr);
    lv_obj_t* code = lv_obj_find_by_name(dialog, "recovery_code");
    REQUIRE(code != nullptr);
    out_code = lv_obj_has_flag(code, LV_OBJ_FLAG_HIDDEN) ? "" : lv_label_get_text(code);
    return lv_label_get_text(message);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - pure JSON state_message shows msg not raw",
                 "[recovery][state_message][json]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    ps.set_klippy_state_message(
        R"({"code":"key1", "msg":"Internal error during ready callback: No active exception to reraise"})");

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    std::string code;
    std::string displayed = recovery_texts(dialog, code);

    // The prose survives...
    REQUIRE(displayed.find("Internal error during ready callback") != std::string::npos);
    // ...and none of the JSON scaffolding reaches the label.
    REQUIRE(displayed.find("{") == std::string::npos);
    REQUIRE(displayed.find("\"msg\"") == std::string::npos);
    REQUIRE(displayed.find("key1") == std::string::npos);
    // The code moves to its own slot.
    REQUIRE(code == "key1");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - embedded JSON state_message is decoded",
                 "[recovery][state_message][json]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // K2 shape: prose prefix with the envelope spliced in after a bang. key9001 is
    // deliberately absent from the CFS table, so this exercises the plain msg path.
    ps.set_klippy_state_message(
        R"(Internal error during connect: !{"code":"key9001","msg":"MCU 'mcu' shutdown"})");

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    std::string code;
    std::string displayed = recovery_texts(dialog, code);

    REQUIRE(displayed.find("MCU 'mcu' shutdown") != std::string::npos);
    REQUIRE(displayed.find("{") == std::string::npos);
    REQUIRE(code == "key9001");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - known CFS code gets curated text",
                 "[recovery][state_message][json]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // key298 has a CFS_ERROR_TABLE entry, so the decoder replaces Klipper's terse msg
    // with the curated message + recovery hint. That substitution is the point of
    // routing through clean_error_text() rather than reimplementing a JSON strip.
    ps.set_klippy_state_message(R"({"code":"key298","msg":"MCU 'mcu' shutdown"})");

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    std::string code;
    std::string displayed = recovery_texts(dialog, code);

    REQUIRE(displayed.find("MCU bridge daemon is shut down") != std::string::npos);
    REQUIRE(displayed.find("Firmware Restart") != std::string::npos);
    REQUIRE(displayed.find("{") == std::string::npos);
    REQUIRE(code == "key298");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - plain-prose state_message is left alone",
                 "[recovery][state_message][json]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // Guards against an over-eager decoder mangling the common non-JSON case.
    const std::string prose = "flashforge_loadcell: Max force exceeded. Last weight was: 912g";
    ps.set_klippy_state_message(prose);

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    std::string code;
    std::string displayed = recovery_texts(dialog, code);

    REQUIRE(displayed.find("Max force exceeded") != std::string::npos);
    // No code, so the header slot stays hidden.
    REQUIRE(code.empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog - truncated JSON falls back to raw text",
                 "[recovery][state_message][json]") {
    auto& estop = EmergencyStopOverlay::instance();
    auto& ps = get_printer_state();
    estop.init(ps, nullptr);

    // Unterminated envelope: brace-balancing never closes. The user must still see
    // something, so the raw string is shown rather than an empty dialog.
    ps.set_klippy_state_message(R"({"code":"key1", "msg":"No active exception to reraise)");

    estop.show_recovery_for(RecoveryReason::SHUTDOWN);
    process_lvgl(100);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);

    std::string code;
    std::string displayed = recovery_texts(dialog, code);

    REQUIRE(displayed.find("No active exception to reraise") != std::string::npos);
    REQUIRE(code.empty());
}

namespace {
// Packs cannot be unregistered (LVGL has no remove API), so the probe locale
// below outlives this case - but nothing else ever selects it, and restoring
// the identity locale keeps the rest of the shard on English lookups.
class ScopedIdentityLanguage {
  public:
    ~ScopedIdentityLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Recovery dialog translates its content exactly once",
                 "[recovery][integration][i18n]") {
    auto& estop = EmergencyStopOverlay::instance();
    ScopedIdentityLanguage restore_lang;

    // A probe locale whose DISCONNECTED-title translation is itself another
    // real tag. Wrapping the recovery strings in lv_tr() twice sends the
    // already-translated text into the second lookup, and when that text
    // happens to be a tag in its own right the dialog shows the WRONG string
    // instead of merely falling back to the right one (bundle CSLYH92R showed
    // the harmless half: every refresh logged "tag is not found" for strings
    // that were already French).
    lv_translation_pack_t* pack = lv_translation_add_dynamic();
    REQUIRE(lv_translation_add_language(pack, "zz-once") == LV_RESULT_OK);
    int32_t lang_idx = lv_translation_get_language_index(pack, "zz-once");
    REQUIRE(lang_idx >= 0);
    lv_translation_tag_dsc_t* title_tag =
        lv_translation_add_tag(pack, "Printer Firmware Disconnected");
    REQUIRE(title_tag != nullptr);
    REQUIRE(lv_translation_set_tag_translation(pack, title_tag, lang_idx, "Printer Shutdown") ==
            LV_RESULT_OK);
    lv_translation_tag_dsc_t* chained_tag = lv_translation_add_tag(pack, "Printer Shutdown");
    REQUIRE(chained_tag != nullptr);
    REQUIRE(lv_translation_set_tag_translation(pack, chained_tag, lang_idx, "SECOND PASS") ==
            LV_RESULT_OK);

    lv_translation_set_language("zz-once");

    estop.show_recovery_for(RecoveryReason::DISCONNECTED);
    process_lvgl(50);

    lv_obj_t* dialog = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(dialog != nullptr);
    lv_obj_t* title = lv_obj_find_by_name(dialog, "recovery_title");
    REQUIRE(title != nullptr);
    // One translation lands on the title's own string; two land on "SECOND PASS".
    REQUIRE(std::string(lv_label_get_text(title)) == "Printer Shutdown");
}

#endif // __APPLE__
