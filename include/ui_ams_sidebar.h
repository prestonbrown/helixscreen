// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_bypass_toggle_controller.h"
#include "ui_clog_meter.h"
#include "ui_observer_guard.h"

#include "ams_backend.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "async_lifetime_guard.h"
#include "filament_op_dispatch.h"
#include "filament_op_slot_resolver.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_event_t;
typedef struct _lv_event_t lv_event_t;

namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief Shared AMS sidebar component for operation status and controls
 *
 * Manages the right-column sidebar used by both AmsPanel and AmsOverviewPanel.
 * Contains: current loaded card, status display, step progress stepper,
 * action buttons (unload/reset/settings/bypass), and dryer card.
 *
 * Uses user_data callback routing pattern (same as AmsDryerCard).
 * Static callbacks traverse parent chain to find the AmsOperationSidebar instance.
 */
class AmsOperationSidebar {
  public:
    explicit AmsOperationSidebar(PrinterState& ps);
    ~AmsOperationSidebar();

    // Non-copyable, non-movable
    AmsOperationSidebar(const AmsOperationSidebar&) = delete;
    AmsOperationSidebar& operator=(const AmsOperationSidebar&) = delete;

    /**
     * @brief Find sidebar widget in panel, set user_data, setup dryer card
     * @param panel Root panel object containing the ams_sidebar component
     * @return true if sidebar found and initialized
     */
    bool setup(lv_obj_t* panel);

    /**
     * @brief Register action/current_slot/extruder_temp observers
     */
    void init_observers();

    /**
     * @brief Clear observers and widget refs
     *
     * Unconditionally resets ALL observers and nullifies widget pointers.
     * Widget pointers are cleared before observers to prevent cascading
     * observer callbacks from accessing freed LVGL objects.
     */
    void cleanup();

    /**
     * @brief Sync step progress and swatch from current state (call on panel activate)
     */
    void sync_from_state();

    /**
     * @brief Start an operation with known type and target slot
     *
     * Called BEFORE backend operation to set up step progress and pulse animation.
     * Sets action to HEATING and shows step progress immediately.
     */
    void start_operation(StepOperationType op_type, int target_slot);

    /**
     * @brief Revert a start_operation() whose backend dispatch failed
     *
     * start_operation() optimistically sets the AmsState action to HEATING and
     * arms the pulse animation before the backend call. If that call returns an
     * error, the backend never left IDLE — surface the error and resync the UI
     * from the backend so the sidebar doesn't freeze in a phantom "Heating"
     * (Vger1700, bundle Z5V4K3NL: dispatch error was silently discarded).
     */
    void fail_started_operation(const AmsError& error);

    /**
     * @brief Handle load request with automatic preheat if needed
     */
    void handle_load_with_preheat(int slot_index);

    /**
     * @brief Handle unload of a specific slot from the context menu.
     *
     * Routes through start_operation(UNLOAD) so the vertical step widget is
     * built correctly (mirrors how handle_load_with_preheat routes LOAD).
     * Without this the context-menu UNLOAD path called the backend directly,
     * leaving auto-detect to mis-build the stepper as LOAD_SWAP.
     */
    void handle_unload(int slot_index);

    /**
     * @brief Update the loaded card swatch color and info
     */
    void update_current_loaded_display();

    /**
     * @brief Hide settings button if backend has no device sections
     */
    void update_settings_visibility();

    /**
     * @brief Show/hide Check gates button based on whether the active backend supports it.
     */
    void update_check_gates_visibility();

    /**
     * @brief Set btn_reset's label from the active backend (e.g. "Home" for Happy Hare).
     */
    void sync_reset_button_label();

    /**
     * @brief Register XML event callbacks (call once before XML parsing)
     */
    static void register_callbacks_static();

  private:
    // Dependencies
    PrinterState& printer_state_;

    // Expires every outstanding callback the moment this sidebar dies.
    //
    // Load for you: AmsPanel::clear_panel_reference() destroys the sidebar when
    // the AMS panel closes, but the shared MacroParamModal keeps the callback it
    // was handed until the NEXT show_for_*() overwrites it — dismissing the
    // modal does not clear it. So "open the param modal from the AMS panel,
    // close the panel, press Run" reaches a freed `this` without this guard.
    // Every callback handed to the modal goes through token().defer(), which
    // re-checks the generation on the main thread before touching members.
    helix::AsyncLifetimeGuard lifetime_;

    // Widget references
    lv_obj_t* sidebar_root_ = nullptr;
    lv_obj_t* step_progress_ = nullptr;
    lv_obj_t* step_progress_container_ = nullptr;

    // Extracted UI modules
    std::unique_ptr<UiClogMeter> clog_meter_;

    // Bypass spool observer (updates sidebar if needed)
    ObserverGuard bypass_spool_observer_;

    // Observers
    ObserverGuard action_observer_;
    ObserverGuard current_slot_observer_;
    ObserverGuard active_backend_observer_;
    ObserverGuard extruder_temp_observer_;
    ObserverGuard extruder_target_observer_;
    ObserverGuard color_observer_;
    // Watches AmsState's ams_operation_indeterminate subject (#1065 row 14). When
    // 1 the live "Heat 225/230" readout is frozen by a starved status feed, so
    // the Heat step swaps to an indeterminate "Working…" busy label instead of a
    // number that reads as a hang. Static-lifetime singleton subject (like
    // ams_operation_phase) — a plain ObserverGuard with no SubjectLifetime is
    // correct; cleaned up via reset() in cleanup().
    ObserverGuard indeterminate_observer_;

    // Drives the step bar's current step when the active backend supplies a
    // specialized step model (get_operation_step_index_subject). The subject is
    // backend-supplied and always a STATIC singleton (firmware-phase subject for
    // the U1, narration toolchange-step subject for AFC-style backends), so a
    // member ObserverGuard with no SubjectLifetime is correct — cleaned up via
    // reset() in cleanup(). When null the sidebar uses the legacy coarse
    // AmsAction→index fallback.
    ObserverGuard step_index_observer_;
    // The subject the step_index_observer_ watches (nullptr => legacy fallback).
    // Set in recreate_step_progress_for_operation from the active backend.
    lv_subject_t* step_index_subject_ = nullptr;
    // Keeps the current backend step model alive so live_temp / phase lookups
    // work for the active operation. Empty => legacy coarse model.
    AmsBackend::OperationStepModel current_step_model_;

    // Bypass toggle policy (guards, unload-first chain, print refusal)
    BypassToggleController bypass_toggle_;

    // Preheat state
    int pending_load_slot_ = -1;
    int pending_load_target_temp_ = 0;
    bool ui_initiated_heat_ = false;
    AmsAction prev_ams_action_ = AmsAction::IDLE;

    // Lifecycle flag — set in setup(), cleared in cleanup().
    // Guards widget operations against use-after-free on dangling lv_obj_t* pointers.
    bool active_ = false;

    // Independent main-thread clock for the indeterminate detector (#1065 row 14).
    // On the 2-core AD5X a blocking load/unload macro can starve the WebSocket
    // status feed, freezing the live-temp readout AND the backend's feed-driven
    // stall check together. This timer periodically calls sync_from_backend()
    // while an op is active, so get_system_info() -> check_action_timeout() flips
    // ams_operation_indeterminate on its own clock and the Heat step swaps to
    // "Working…". Created in setup(), deleted in cleanup() (both main-thread), so
    // it never outlives the sidebar. Runs on the LVGL main loop — if the loop
    // itself stalls it won't fire, but the loop keeps rendering (the number is
    // visibly frozen), so it does.
    lv_timer_t* stall_watchdog_timer_ = nullptr;
    static constexpr uint32_t STALL_WATCHDOG_PERIOD_MS = 1500;
    static void stall_watchdog_cb(lv_timer_t* timer);

    // Step progress state
    StepOperationType current_operation_type_ = StepOperationType::LOAD_FRESH;
    int current_step_count_ = 4;
    int target_load_slot_ = -1;
    bool heat_label_showing_temp_ = false;
    // Index of the step whose label shows a live "<label> cur/target°C" readout
    // (OperationStep::live_temp), or -1 if the active model has none. Set when a
    // backend-supplied step model is built; for the legacy coarse model the Heat
    // step is always index 0.
    int live_temp_step_index_ = -1;
    // Whether the current LOAD_SWAP/UNLOAD stepper includes a discrete tip
    // (cut / tip-form) step. False for backends with TipMethod::NONE. Drives the
    // step-index map in get_step_index_for_action so trailing steps don't shift.
    bool current_op_has_tip_step_ = true;

    // Step progress methods
    void setup_step_progress();
    void recreate_step_progress_for_operation(StepOperationType op_type);
    void update_step_progress(AmsAction action);
    int get_step_index_for_action(AmsAction action, StepOperationType op_type);

    // Apply a backend-supplied step index (-1=none, clamped to model size) to the
    // current step bar and refresh the live-temp step label. Used when the active
    // backend supplied a step-index subject (firmware phase or narration).
    void apply_backend_step_index(int index);

    // Update the live-temp step (live_temp_step_index_) label: a live
    // "<label> X / Y°C" readout while that step is current, reverting to the
    // static label otherwise. Driven by apply_backend_step_index and the
    // extruder temp/target observers (live updates while heating).
    void refresh_live_temp_step_label(int current_index);

    // Re-evaluate step display when extruder temp/target changes (called by observers).
    // Physical heating state overrides AmsAction for step indicator: backends emit
    // LOADING optimistically at gcode dispatch (CFS, ACE, AD5x), and even firmware-driven
    // backends can fire the next phase before the printer leaves heating.
    void refresh_heat_step_display();
    bool is_extruder_below_target() const;

    // Preheat methods
    int get_load_temp_for_slot(int slot_index);
    void check_pending_load();
    void handle_load_complete();
    void show_preheat_feedback(int slot_index, int target_temp);

    // ---- Shared dispatch plan (filament_op_dispatch.h) ----------------------
    // Read the live backend's capabilities into the planner's plain-value form.
    // `info_out` receives the system info the caps were derived from so callers
    // that need current_slot / slot mapping don't re-fetch it.
    //
    // `target_slot` is the slot a Load is aimed at: needs_unload_before_load()
    // is a per-lane question, so the same backend answers differently for a
    // direct-fed lane and a hub-routed one on a MIXED unit. Unload callers pass
    // -1 — plan_unload() never reads the field.
    [[nodiscard]] helix::ui::BackendCaps read_backend_caps(AmsSystemInfo& info_out,
                                                           int target_slot) const;

    // Execute a tier-1 (AmsBackend) plan. Reports a failed dispatch through
    // fail_started_operation() so a start_operation() that never took never
    // leaves the sidebar frozen in a phantom "Heating".
    void dispatch_backend_load(const helix::ui::FilamentOpPlan& plan, int slot_index);

    // ---- Tiers 2 and 3 (configured macro, then raw gcode) ------------------
    // Reached when there is no AMS backend, or when bypass hands the load to the
    // user's LOAD_FILAMENT macro. Neither tier has an AMS operation to narrate,
    // so they run without the stepper / pending-target-slot bookkeeping.
    void dispatch_load_outside_backend(const helix::ui::FilamentOpPlan& plan);
    void dispatch_unload_outside_backend(const helix::ui::FilamentOpPlan& plan);
    void send_standard_filament_macro(bool is_load,
                                      const std::map<std::string, std::string>& params);
    void send_filament_fallback_gcode(bool is_load);

    // ---- Unload button gating (filament_op_slot_resolver.h) -----------------
    // Recompute the sidebar Unload button's enabled state from the SAME
    // compute_op_button_gating() rule the filament panel and the AMS context
    // menu use, and publish it on ams_sidebar_unload_disabled.
    //
    // The sidebar had no print-state term at all, so its Unload stayed tappable
    // through a print or a runout pause, dispatched, and ate the backend's
    // "Cannot run filament operation while printing" refusal.
    void refresh_unload_gating();

    // Live inputs for refresh_unload_gating(), also consulted by handle_unload()
    // so the dispatch cannot run when the button should have been greyed.
    [[nodiscard]] helix::ui::OpButtonState read_unload_gating_state() const;

    // Observers feeding refresh_unload_gating(). ams_action / current_slot are
    // already watched above; these two are the terms the sidebar never had.
    ObserverGuard filament_loaded_observer_;
    ObserverGuard print_state_observer_;

    // Action handlers
    void handle_unload();
    void handle_reset();
    void handle_check_gates();
    void handle_bypass_toggle();

    // Action display (sidebar-relevant parts only)
    void update_action_display(AmsAction action);

    // Static callback routing
    static AmsOperationSidebar* get_instance_from_event(lv_event_t* e);

    // Static XML callbacks
    static void on_bypass_toggled_cb(lv_event_t* e);
    static void on_unload_clicked_cb(lv_event_t* e);
    static void on_reset_clicked_cb(lv_event_t* e);
    static void on_check_gates_clicked_cb(lv_event_t* e);
    static void on_settings_clicked_cb(lv_event_t* e);
};

} // namespace helix::ui
