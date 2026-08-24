// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_navigation.h"

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {

// Track previous state to detect inactive→active print transitions
// RAW_PRINT_STATE_OK: navigation's activation edge - see is_active_print_state().
//
// FIRST-TICK CONTRACT: observe fires once at registration, so this is re-seeded
// to the CURRENT state in init_print_start_navigation_observer() before
// subscribing - otherwise connecting to an already-running printer would look
// like a print that just started. The deliberate "already active" case is
// handled separately by the level check there (#1099 power-loss recovery).
// Four sites in this tree answer that first tick differently and all four are
// correct; read the local one before copying any of them.
static PrintJobState prev_print_state = PrintJobState::STANDBY;

// RAW_PRINT_STATE_OK: navigation's activation edge. On a lifecycle that
// includes host-side Preparing this would push the status panel for a job the
// printer has not accepted, duplicating the optimistic push in
// ui_panel_print_select.cpp.
bool is_active_print_state(PrintJobState s) {
    return s == PrintJobState::PRINTING || s == PrintJobState::PAUSED;
}

bool print_start_nav_should_navigate(PrintJobState prev, PrintJobState current) {
    return !is_active_print_state(prev) && is_active_print_state(current);
}

// Queue the print status overlay push on the UI thread. Shared by the
// state-change observer (which may fire on the WebSocket background thread)
// and the init-time level check — push_overlay() creates LVGL widgets which
// must run on the UI thread, so both paths defer via queue_update.
//
// Skip if print status is already on the nav stack. The observer fires on
// every inactive→active transition, including the completion→retry cycle
// (finished print → error/standby → new print) where the user may still be
// viewing print status from the previous job. Without this guard, every
// retry produced a "[NavigationManager] Overlay ... already in stack"
// warning (bundle J3WD5GQJ saw 9 of these in 11 hours). The
// is_panel_in_stack() check has to run on the UI thread, so it lives
// inside the queue_update lambda.
static void queue_push_print_status_overlay() {
    helix::ui::queue_update([]() {
        auto* cached = PrintStatusPanel::get_cached_overlay();
        if (cached && NavigationManager::instance().is_panel_in_stack(cached)) {
            spdlog::debug("[PrintStartNav] Print status already on stack — skip auto-nav");
            return;
        }
        PrintStatusPanel::push_overlay(lv_display_get_screen_active(nullptr));
    });
}

// Callback for print state changes - auto-navigates to print status when a print
// job becomes active. The gate fires on any inactive→active edge, not just
// →PRINTING: firmware power-loss recovery surfaces the restored job as PAUSED at
// initial connect (#1099), and that deserves the same auto-navigation as a normal
// print start. Active→active edges (pause, resume) never navigate, so a user who
// deliberately left the print status screen mid-print isn't yanked back on resume.
//
// This observer fires synchronously from lv_subject_set_int which may be called on
// the WebSocket background thread. All LVGL widget creation MUST happen on the UI
// thread, so we defer push_overlay via queue_update. This fixes the thumbnail race
// condition (#450) where widgets created on the background thread were in an
// inconsistent state when deferred observer callbacks tried to update them.
static void on_print_state_changed_for_navigation(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;
    // PRINT_STATE_CAST_OK: `subject` IS print_state_enum, and this file is a
    // deliberate keep-raw site - navigation must not fire on Idle -> Preparing.
    auto current = static_cast<PrintJobState>(lv_subject_get_int(subject));

    spdlog::trace("[PrintStartNav] State change: {} -> {}", static_cast<int>(prev_print_state),
                  static_cast<int>(current));

    if (print_start_nav_should_navigate(prev_print_state, current)) {
        // Don't auto-navigate while the setup wizard is running
        if (is_wizard_active()) {
            spdlog::debug(
                "[PrintStartNav] Wizard active, suppressing auto-navigation to print status");
            prev_print_state = current;
            return;
        }

        spdlog::info("[PrintStartNav] Auto-navigating to print status (print job active)");
        queue_push_print_status_overlay();
    }

    prev_print_state = current;
}

ObserverGuard init_print_start_navigation_observer() {
    // Initialize prev_print_state to current state to prevent false trigger on startup
    // RAW_PRINT_STATE_OK: see the first-tick contract on prev_print_state.
    prev_print_state = static_cast<PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    spdlog::debug("[PrintStartNav] Observer registered (initial state={})",
                  static_cast<int>(prev_print_state));

    // Level check: if the print state is ALREADY active when the observer is
    // registered, the edge observer never fires — the subject was set before we
    // subscribed. This happens after firmware power-loss recovery, where the
    // restored job can be PAUSED/PRINTING by the time initial connect completes
    // (#1099). Navigate now so the user lands on the running job.
    if (is_active_print_state(prev_print_state) && !is_wizard_active()) {
        spdlog::info("[PrintStartNav] Print already active at init (state={}) — "
                     "auto-navigating to print status",
                     static_cast<int>(prev_print_state));
        queue_push_print_status_overlay();
    }

    // RAW_PRINT_STATE_OK: navigation must NOT fire on Idle -> Preparing.
    return ObserverGuard(get_printer_state().get_print_state_enum_subject(),
                         on_print_state_changed_for_navigation, nullptr);
}

} // namespace helix
