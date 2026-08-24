// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_filament_runout_handler.h
 * @brief Handles filament runout guidance during print pauses
 *
 * Extracted from PrintStatusPanel to reduce complexity. Manages:
 * - Detection of filament runout condition on print pause
 * - Display of guidance modal with action buttons
 * - User interaction: load filament, unload, purge, resume, cancel
 * - State tracking to prevent repeated modal popups per pause event
 *
 * The handler owns a RunoutGuidanceModal and coordinates between:
 * - FilamentSensorManager (runout detection)
 * - StandardMacros (filament operations, resume, cancel)
 * - IMoonrakerAPI (command execution)
 *
 * @see docs/FILAMENT_RUNOUT.md for feature design
 */

#include "ui_observer_guard.h"
#include "ui_runout_guidance_modal.h"

#include "async_lifetime_guard.h"

// Forward declarations
class IMoonrakerAPI;

// Forward declare the global PrintState enum (defined in ui_panel_print_status.h)
enum class PrintState;

namespace helix::ui {

/**
 * @brief Manages filament runout guidance for PrintStatusPanel
 *
 * Extracted from PrintStatusPanel to reduce complexity. Handles:
 * - Checking for runout condition when print enters Paused state
 * - Showing guidance modal with 6 action buttons
 * - Executing filament operations via StandardMacros
 * - Tracking whether modal was shown for current pause
 *
 * Usage:
 * @code
 *   auto handler = std::make_unique<FilamentRunoutHandler>(api);
 *
 *   // On print state change:
 *   handler->on_print_state_changed(old_state, new_state);
 *
 *   // When API changes:
 *   handler->set_api(new_api);
 * @endcode
 */
class FilamentRunoutHandler {
    // Reaches dispatch_load() without a live modal — see
    // tests/test_helpers/filament_runout_handler_test_access.h.
    friend class FilamentRunoutHandlerTestAccess;

  public:
    /**
     * @brief Construct handler with dependencies
     *
     * @param api IMoonrakerAPI for macro execution (may be nullptr in tests)
     */
    explicit FilamentRunoutHandler(IMoonrakerAPI* api);

    ~FilamentRunoutHandler();

    // Non-copyable, non-movable
    FilamentRunoutHandler(const FilamentRunoutHandler&) = delete;
    FilamentRunoutHandler& operator=(const FilamentRunoutHandler&) = delete;
    FilamentRunoutHandler(FilamentRunoutHandler&&) = delete;
    FilamentRunoutHandler& operator=(FilamentRunoutHandler&&) = delete;

    /**
     * @brief Handle print state transitions
     *
     * Called by PrintStatusPanel when print state changes.
     * - On transition to Paused: checks for runout and shows modal if detected
     * - On transition to Printing: resets flag and hides modal
     *
     * @param old_state Previous print state
     * @param new_state New print state
     */
    void on_print_state_changed(::PrintState old_state, ::PrintState new_state);

    /**
     * @brief Update the IMoonrakerAPI pointer
     *
     * @param api New API pointer (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Hide the runout guidance modal if visible
     *
     * Called when panel is deactivated or navigated away from.
     */
    void hide_modal();

    //
    // === Testing API ===
    //

    /**
     * @brief Check if modal was shown for current pause event
     * @return true if modal was already shown for this pause
     */
    bool is_modal_shown_for_pause() const {
        return runout_modal_shown_for_pause_;
    }

    /**
     * @brief Check if the runout guidance modal is currently visible
     * @return true if modal is shown
     */
    bool is_modal_visible() const {
        return runout_modal_.is_visible();
    }

  private:
    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_;

    //
    // === State ===
    //

    /// Runout guidance modal (RAII - auto-hides when destroyed)
    RunoutGuidanceModal runout_modal_;

    /// Flag to track if runout modal was shown for current pause
    /// Reset when print resumes or ends, prevents repeated modal popups
    bool runout_modal_shown_for_pause_{false};

    /// Set when the user triggers Load/Unload/Purge from within the modal. While
    /// true, the sensor-driven auto-close is suppressed so a user-initiated load
    /// (which itself makes the sensor read present) keeps the dialog open for a
    /// follow-up purge. An EXTERNAL resolution (no in-dialog action) still closes.
    bool user_took_manual_action_{false};

    /// Latch for the auto-close observer (#991). The observer on the any-runout
    /// subject fires its initial read (and any startup-grace transient 0) the
    /// moment it's installed — which used to close the modal immediately after a
    /// UI restart. We only auto-close on a GENUINE runout→clear transition, so we
    /// require having first observed a confirmed runout (value==1) while the modal
    /// is open. Reset to false in show_runout_guidance_modal() so each fresh modal
    /// must re-confirm an active runout before auto-close can fire.
    bool runout_confirmed_active_{false};

    /// Observes FilamentSensorManager::get_any_runout_subject(); auto-closes the
    /// modal when the runout clears externally. Static singleton subject, so a
    /// plain ObserverGuard (no SubjectLifetime token) is correct.
    ObserverGuard runout_cleared_observer_;

    /// Observes AmsState::get_active_tool_port_present_subject() to gate the
    /// Resume button on first-gate (port) filament presence (#991). On auto-feed
    /// backends, Resume is disabled until filament is present at the active
    /// tool's port sensor. Static singleton subject — plain ObserverGuard, no
    /// SubjectLifetime token. Only installed while the modal is up on an
    /// auto-feed backend; reset in hide_runout_guidance_modal().
    ObserverGuard port_present_observer_;

    /// True while the current modal is on an auto-feed backend (Snapmaker U1),
    /// captured at show time. Gates whether the port-present observer drives the
    /// Resume block — non-auto-feed backends never gate Resume.
    bool autofeed_context_{false};

    /// Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;

    //
    // === Internal Methods ===
    //

    /**
     * @brief Check if runout condition exists and show guidance modal if appropriate
     *
     * Called when print transitions to Paused state. Checks if runout sensor
     * is available and shows no filament - if so, displays guidance modal.
     */
    void check_and_show_runout_guidance();

    /**
     * @brief Show the runout guidance modal
     *
     * Called when print pauses and runout sensor shows no filament.
     * Configures all 6 button callbacks and displays the modal.
     */
    void show_runout_guidance_modal();

    /**
     * @brief Hide and cleanup the runout guidance modal
     */
    void hide_runout_guidance_modal();

    /**
     * @brief Dispatch the dialog's "Load filament" action.
     *
     * Routes through the shared plan_load() ladder — AMS backend, then the
     * configured LOAD_FILAMENT macro, then raw gcode — so this dialog agrees
     * with the Filament panel and the AMS sidebar. Two constraints are specific
     * to this surface: parameters are never prompted for (ParamPolicy::Suppress,
     * because a param modal would stack on top of this live dialog), and a
     * refusal never navigates away (the dialog the user is standing in would be
     * torn down beneath them).
     *
     * Main thread only — called from the modal's button callback after its
     * LifetimeToken check.
     */
    void dispatch_load();

    /**
     * @brief Dispatch the dialog's "Unload filament" action.
     *
     * The plan_unload() counterpart of dispatch_load(). This button used to call
     * StandardMacros::execute() directly: no backend tier at all, no raw-gcode
     * fallback, and a "Unload macro not configured" warning on a printer whose
     * AMS backend would have handled it perfectly well.
     *
     * The unload target is the current lane, and unload_target_is_loaded()'s
     * is_current_slot arm is what keeps it reachable here — a runout clears the
     * lane's own sensor while filament is still at the head (#995 / #1199).
     *
     * Main thread only.
     */
    void dispatch_unload();

    /**
     * @brief Dispatch the dialog's "Purge" action.
     *
     * Tiers 2 and 3 only — there is no backend purge entry point, so there is no
     * plan_purge() to route through. Same ParamPolicy::Suppress and the same
     * never-navigate rule as the other two buttons.
     *
     * Main thread only.
     */
    void dispatch_purge();
};

} // namespace helix::ui
