// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_print_exclude_object_manager.h
 * @brief Manages exclude object feature for G-code viewer
 *
 * Handles the user interaction flow for excluding objects during a print:
 * 1. Long-press detection on objects in the G-code viewer
 * 2. Confirmation modal with object name
 * 3. 5-second undo window with visual feedback
 * 4. Sending EXCLUDE_OBJECT command to Klipper via IMoonrakerAPI
 *
 * Syncs excluded objects from Klipper (via PrinterState observer) to handle
 * exclusions made by other clients or the web interface.
 *
 * @see docs/devel/EXCLUDE_OBJECTS.md for feature design
 */

#include "ui_exclude_object_modal.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"

namespace helix {
enum class PrintJobState;
} // namespace helix

#include <lvgl.h>
#include <string>
#include <unordered_set>

// Forward declarations
class IMoonrakerAPI;
struct MoonrakerError;
class PrintExcludeObjectManagerTestAccess;
namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief Manages the exclude object feature for PrintStatusPanel
 *
 * Extracted from PrintStatusPanel to reduce complexity. Handles:
 * - Long-press callback registration on gcode viewer
 * - Confirmation modal flow
 * - Undo timer and toast notification
 * - API calls to Klipper for exclusion
 * - Observer sync from PrinterState for external exclusions
 *
 * Usage:
 * @code
 *   auto manager = std::make_unique<PrintExcludeObjectManager>(api, printer_state, gcode_viewer);
 *   manager->init();
 *   // When done:
 *   manager->deinit();
 * @endcode
 */
class PrintExcludeObjectManager {
  public:
    /**
     * @brief Construct manager with dependencies
     *
     * @param api IMoonrakerAPI for exclude_object() calls (may be nullptr in tests)
     * @param printer_state Reference to PrinterState for excluded objects observer
     * @param gcode_viewer Pointer to gcode viewer widget for visual updates
     */
    PrintExcludeObjectManager(IMoonrakerAPI* api, PrinterState& printer_state,
                              lv_obj_t* gcode_viewer);

    ~PrintExcludeObjectManager();

    // Non-copyable, non-movable
    PrintExcludeObjectManager(const PrintExcludeObjectManager&) = delete;
    PrintExcludeObjectManager& operator=(const PrintExcludeObjectManager&) = delete;
    PrintExcludeObjectManager(PrintExcludeObjectManager&&) = delete;
    PrintExcludeObjectManager& operator=(PrintExcludeObjectManager&&) = delete;

    /**
     * @brief Initialize observers and register long-press callback
     *
     * Call after construction when gcode_viewer is ready.
     * Registers the excluded objects observer on PrinterState and
     * sets up the long-press callback on the gcode viewer.
     */
    void init();

    /**
     * @brief Clean up resources
     *
     * Deletes undo timer if active, unregisters callbacks.
     * Safe to call multiple times. Should be called before destruction
     * if LVGL is still active.
     */
    void deinit();

    /**
     * @brief Handle long-press on object in G-code viewer
     *
     * Called by PrintStatusPanel when the gcode viewer detects a long-press.
     * Shows confirmation dialog if the object is not already excluded.
     *
     * @param object_name Name of the object that was long-pressed
     */
    void handle_object_long_press(const char* object_name);

    /**
     * @brief Request exclusion of an object by name (from list overlay)
     *
     * Triggers the same confirmation flow as a long-press:
     * guard checks → confirmation modal → 5s undo → API call.
     *
     * @param object_name Name of the object to exclude
     */
    void request_exclude(const std::string& object_name);

    /**
     * @brief Update the IMoonrakerAPI pointer
     *
     * @param api New API pointer (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Update the gcode viewer pointer
     *
     * Use when the gcode viewer widget is recreated.
     * Re-registers the long-press callback.
     *
     * @param gcode_viewer New gcode viewer widget
     */
    void set_gcode_viewer(lv_obj_t* gcode_viewer);

    //
    // === Testing API ===
    //

    /**
     * @brief Get the set of excluded objects
     * @return Reference to excluded objects set
     */
    const std::unordered_set<std::string>& get_excluded_objects() const {
        return excluded_objects_;
    }

    /**
     * @brief Get the pending exclude object name
     * @return Object name pending exclusion, or empty if none
     */
    const std::string& get_pending_object() const {
        return pending_exclude_object_;
    }

    /**
     * @brief Check if an undo timer is currently active
     * @return true if timer is pending
     */
    bool has_pending_timer() const {
        return exclude_undo_timer_ != nullptr;
    }

    /**
     * @brief Clear excluded objects state
     *
     * Called when a new print starts to reset the exclusion state.
     */
    void clear_excluded_objects() {
        excluded_objects_.clear();
        awaiting_confirmation_.clear();
        pending_exclude_object_.clear();
        if (exclude_undo_timer_) {
            lv_timer_delete(exclude_undo_timer_);
            exclude_undo_timer_ = nullptr;
        }
    }

  private:
    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_;
    PrinterState& printer_state_;
    lv_obj_t* gcode_viewer_;

    //
    // === State ===
    //

    /// Objects already excluded (sent to Klipper, cannot be undone)
    std::unordered_set<std::string> excluded_objects_;

    /// Objects for which we have dispatched EXCLUDE_OBJECT but not yet seen the status
    /// subscription confirm them. Used to tell apart "RPC in flight" from "genuinely failed"
    /// when an RPC-level TIMEOUT arrives (Klipper may still run the queued gcode).
    ///
    /// Main-thread only — populated in `exclude_undo_timer_cb` (LVGL timer, main thread),
    /// cleared from `tok.defer` bodies (UpdateQueue drained on main) and from
    /// `on_excluded_objects_changed` (observe_int_sync defers to UpdateQueue per L048).
    /// No mutex needed.
    std::unordered_set<std::string> awaiting_confirmation_;

    /// Object pending exclusion (in undo window, not yet sent to Klipper)
    std::string pending_exclude_object_;

    /// Timer for undo window (5 seconds before sending EXCLUDE_OBJECT to Klipper)
    lv_timer_t* exclude_undo_timer_{nullptr};

    /// Exclude object confirmation modal (RAII - auto-hides when destroyed)
    ExcludeObjectModal exclude_modal_;

    /// Observer for excluded objects changes from PrinterState
    ObserverGuard excluded_objects_observer_;

    /// Observer for print-state transitions. When the print ends (STANDBY/COMPLETE/
    /// CANCELLED/ERROR) with un-confirmed exclusions still pending, we silently drop
    /// the optimistic visual — either Klipper ran them (in which case they'd already
    /// be in excluded_objects_) or the user gave up on the print. No toast: a cancel
    /// is usually intentional and a notification about stale excludes would be noise.
    ObserverGuard print_state_observer_;

    /// Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;

    /// Track if init() was called
    bool initialized_{false};

    //
    // === Internal Handlers ===
    //

    /**
     * @brief Handle confirmation of object exclusion
     *
     * Starts the undo window timer and shows undo toast.
     */
    void handle_exclude_confirmed();

    /**
     * @brief Handle cancellation of exclusion dialog
     */
    void handle_exclude_cancelled();

    /**
     * @brief Handle undo button press on toast
     *
     * Cancels pending exclusion and reverts visual state.
     */
    void handle_exclude_undo();

    /**
     * @brief Called when excluded objects change in PrinterState
     *
     * Syncs our local excluded set with Klipper's excluded objects.
     * Updates gcode viewer visual state.
     */
    void on_excluded_objects_changed();

    /**
     * @brief Called when the print-state enum transitions.
     *
     * If the new state is a terminal state (STANDBY/COMPLETE/CANCELLED/ERROR) and
     * we're still holding optimistic visuals for unconfirmed exclusions, drop them
     * silently. Prevents a stuck red-ghosted object from lingering into the next
     * print if Klipper's status push never arrived (crash, disconnect, etc).
     */
    void on_print_state_changed(helix::PrintJobState state);

    /**
     * @brief Handle an RPC error from EXCLUDE_OBJECT.
     *
     * TIMEOUT errors are advisory (log, keep optimistic visual, keep awaiting entry —
     * Klipper may still run the queued gcode). Other error types revert the visual,
     * clear awaiting, and toast the user. Invoked from the error lambda in
     * `exclude_undo_timer_cb`.
     */
    void on_exclude_rpc_error(const std::string& object_name, const MoonrakerError& err);

    friend class ::PrintExcludeObjectManagerTestAccess;

    //
    // === Static Callbacks ===
    //

    /**
     * @brief Static callback for gcode viewer long-press
     */
    static void on_object_long_pressed(lv_obj_t* viewer, const char* object_name, void* user_data);

    // excluded_objects_observer_cb migrated to lambda in init()

    /**
     * @brief Timer callback when undo window expires
     *
     * Sends EXCLUDE_OBJECT to Klipper via API.
     */
    static void exclude_undo_timer_cb(lv_timer_t* timer);
};

} // namespace helix::ui
