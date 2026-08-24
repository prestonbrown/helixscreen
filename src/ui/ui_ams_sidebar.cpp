// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_sidebar.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_manual_pull_prompt.h"
#include "ui_step_progress.h"
#include "ui_temperature_utils.h"

#include "active_material_provider.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_dispatch.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"
#include "temperature_controller.h"
#include "toolhead_homing.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

/// UpdateQueue tags for the guarded home-confirm callbacks in
/// handle_load_with_preheat(). String literals: the skip counter interns by
/// pointer identity.
constexpr const char* HOME_CONFIRM_LOAD_TAG = "AmsOperationSidebar::home_confirm_load";
constexpr const char* HOME_CONFIRM_LOAD_DECLINE_TAG =
    "AmsOperationSidebar::home_confirm_load_decline";

/**
 * @brief Drives the sidebar Unload button's disabled state (1 = disabled).
 *
 * Process-wide rather than an AmsOperationSidebar member: the sidebar is a
 * unique_ptr on the AMS / AMS Overview panel and is destroyed every time that
 * panel closes, while the XML tree bound to this subject is torn down
 * asynchronously. An instance-owned subject would be deinit'd out from under a
 * live binding. Registered once, deinit'd via StaticSubjectRegistry (which runs
 * before lv_deinit()), same shape as ScrewsTiltShareModal's row subjects.
 */
lv_subject_t s_unload_disabled;
bool s_unload_disabled_initialized = false;

void init_unload_gating_subject() {
    if (s_unload_disabled_initialized) {
        return;
    }
    // Start disabled: nothing is known to be loaded until the first refresh.
    lv_subject_init_int(&s_unload_disabled, 1);
    lv_xml_register_subject(nullptr, "ams_sidebar_unload_disabled", &s_unload_disabled);
    s_unload_disabled_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("AmsSidebarUnloadGating", []() {
        if (s_unload_disabled_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_unload_disabled);
            s_unload_disabled_initialized = false;
            spdlog::trace("[AmsSidebar] Unload gating subject deinitialized");
        }
    });
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsOperationSidebar::AmsOperationSidebar(PrinterState& ps) : printer_state_(ps) {
    spdlog::debug("[AmsSidebar] Constructed");
}

AmsOperationSidebar::~AmsOperationSidebar() {
    cleanup();
    spdlog::debug("[AmsSidebar] Destroyed");
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsOperationSidebar::register_callbacks_static() {
    // Must exist before ams_sidebar.xml is parsed — btn_unload binds it. Same
    // "before the parser sees it" contract as the callbacks below, so it is
    // registered from the same hook.
    init_unload_gating_subject();

    register_xml_callbacks({
        {"ams_sidebar_bypass_toggled", on_bypass_toggled_cb},
        {"ams_sidebar_unload_clicked", on_unload_clicked_cb},
        {"ams_sidebar_reset_clicked", on_reset_clicked_cb},
        {"ams_sidebar_check_gates_clicked", on_check_gates_clicked_cb},
        {"ams_sidebar_settings_clicked", on_settings_clicked_cb},
    });
}

// ============================================================================
// Static Callback Routing (parent chain traversal)
// ============================================================================

AmsOperationSidebar* AmsOperationSidebar::get_instance_from_event(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Find the ams_sidebar component root by name, then get our instance from its user_data.
    // Cannot walk parents checking any user_data — ui_button and other widgets set their own
    // user_data, which would be miscast as AmsOperationSidebar* (L069).
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && strcmp(name, "ams_operation_sidebar") == 0) {
            void* user_data = lv_obj_get_user_data(obj);
            if (user_data) {
                return static_cast<AmsOperationSidebar*>(user_data);
            }
        }
        obj = lv_obj_get_parent(obj);
    }

    spdlog::warn("[AmsSidebar] Could not find instance from event target");
    return nullptr;
}

// ============================================================================
// Static XML Callbacks
// ============================================================================

void AmsOperationSidebar::on_bypass_toggled_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_bypass_toggle();
    }
}

void AmsOperationSidebar::on_unload_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_unload();
    }
}

void AmsOperationSidebar::on_reset_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_reset();
    }
}

void AmsOperationSidebar::on_check_gates_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_check_gates();
    }
}

void AmsOperationSidebar::on_settings_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsSidebar] on_settings_clicked");

    spdlog::info("[AmsSidebar] Opening AMS Device Operations overlay");

    auto& overlay = helix::ui::get_ams_device_operations_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }

    auto* event_target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* parent = lv_obj_get_screen(event_target);
    overlay.show(parent);

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Setup
// ============================================================================

bool AmsOperationSidebar::setup(lv_obj_t* panel) {
    if (!panel) {
        spdlog::error("[AmsSidebar] NULL panel");
        return false;
    }

    sidebar_root_ = lv_obj_find_by_name(panel, "ams_operation_sidebar");
    if (!sidebar_root_) {
        spdlog::error("[AmsSidebar] sidebar component not found in panel");
        return false;
    }

    // Store this pointer for static callback routing
    lv_obj_set_user_data(sidebar_root_, this);

    // Setup step progress
    setup_step_progress();

    // Setup clog detection meter
    clog_meter_ = std::make_unique<UiClogMeter>(sidebar_root_);

    // Hide settings button if no device sections
    update_settings_visibility();

    active_ = true;

    // Independent stall watchdog for the indeterminate "Working…" state (#1065
    // row 14). Runs on the main loop; the callback no-ops unless an op is active.
    if (!stall_watchdog_timer_) {
        stall_watchdog_timer_ = lv_timer_create(stall_watchdog_cb, STALL_WATCHDOG_PERIOD_MS, this);
    }

    sync_reset_button_label();
    update_check_gates_visibility();
    refresh_unload_gating();
    spdlog::debug("[AmsSidebar] Setup complete");
    return true;
}

void AmsOperationSidebar::stall_watchdog_cb(lv_timer_t* timer) {
    auto* self = static_cast<AmsOperationSidebar*>(lv_timer_get_user_data(timer));
    if (!self || !self->active_) {
        return;
    }
    // Only poke the backend while a filament op is in progress — an idle AMS
    // panel needs no watchdog. AmsAction::IDLE == 0.
    lv_subject_t* action = AmsState::instance().get_ams_action_subject();
    if (action && lv_subject_get_int(action) == 0) {
        return;
    }
    // A blocking load/unload macro can starve the WebSocket status feed on the
    // 2-core AD5X, freezing the live-temp readout and the feed-driven stall
    // check together. Drive the check on this independent clock:
    // sync_from_backend() -> get_system_info() -> check_action_timeout() flips
    // ams_operation_indeterminate, and indeterminate_observer_ swaps the Heat
    // step to "Working…" (#1065 row 14). This is a no-op when a healthy feed is
    // already keeping the flag clear.
    AmsState::instance().sync_from_backend();
}

void AmsOperationSidebar::setup_step_progress() {
    step_progress_container_ = lv_obj_find_by_name(sidebar_root_, "progress_stepper_container");
    if (!step_progress_container_) {
        spdlog::warn("[AmsSidebar] progress_stepper_container not found");
        return;
    }

    // Create initial step progress widget (fresh load by default)
    recreate_step_progress_for_operation(StepOperationType::LOAD_FRESH);

    spdlog::debug("[AmsSidebar] Step progress widget created");
}

// ============================================================================
// Observers
// ============================================================================

void AmsOperationSidebar::init_observers() {
    // Action observer: drives step progress and load completion detection
    action_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsOperationSidebar* self, int action_int) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            auto action = static_cast<AmsAction>(action_int);
            spdlog::debug("[AmsSidebar] Action changed: {} (prev={})", ams_action_to_string(action),
                          ams_action_to_string(self->prev_ams_action_));

            // Detect LOADING -> IDLE or LOADING -> ERROR for post-load cooling
            if (self->prev_ams_action_ == AmsAction::LOADING &&
                (action == AmsAction::IDLE || action == AmsAction::ERROR)) {
                self->handle_load_complete();
            }

            // Same UNLOADING -> IDLE/ERROR edge closes out the manual-pull
            // prompt. No-op unless handle_unload() armed it, and unless the
            // toolhead sensor already spoke at the earlier, truer moment.
            if (self->prev_ams_action_ == AmsAction::UNLOADING) {
                if (action == AmsAction::IDLE) {
                    helix::ui::manual_pull_unload_finished();
                } else if (action == AmsAction::ERROR) {
                    helix::ui::disarm_manual_pull_prompt();
                }
            }

            // No bypass feed here: the shared BypassToggleController now
            // observes the ams_action subject itself (armed only while a
            // pending unload→enable chain runs), so this sidebar instance
            // feeding its own controller would process the edge twice.

            // Update step progress (BEFORE updating prev_ams_action_)
            self->update_action_display(action);

            // AmsSystemInfo::is_busy() is "action is neither IDLE nor ERROR", so
            // every edge here changes the Unload button's gating.
            self->refresh_unload_gating();

            self->prev_ams_action_ = action;
        },
        AmsState::instance().get_subjects_lifetime());

    // Current slot observer: updates loaded card display and reset button label
    current_slot_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_current_slot_subject(), this,
        [](AmsOperationSidebar* self, int /*slot_index*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->update_current_loaded_display();
            self->sync_reset_button_label();
            self->update_check_gates_visibility();
            self->refresh_unload_gating();
        },
        AmsState::instance().get_subjects_lifetime());

    // The two terms the sidebar never had. ams_filament_loaded is what the XML
    // used to bind on its own; print state is the one whose absence let Unload
    // dispatch mid-print and eat a backend refusal.
    //
    // print_lifecycle: PRINTING -> PAUSED is a gating edge (a pause UNGATES Unload
    // on every backend but AD5X, and print_active reads 1 across both so it never
    // fires there), and Idle -> Preparing is one too — read_unload_gating_state()
    // now refuses during a host-side pre-print block, which the raw enum does not
    // move on at all. PrinterState is a separate singleton whose subjects tests
    // deinit between cases, so its observer takes the lifetime token (#705);
    // AmsState's does not.
    filament_loaded_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_filament_loaded_subject(), this,
        [](AmsOperationSidebar* self, int) { self->refresh_unload_gating(); },
        AmsState::instance().get_subjects_lifetime());
    print_state_observer_ = observe_int_sync<AmsOperationSidebar>(
        printer_state_.get_print_lifecycle_subject(), this,
        [](AmsOperationSidebar* self, int) { self->refresh_unload_gating(); },
        printer_state_.get_static_print_subjects_lifetime());

    // Active backend observer: re-syncs reset button label when the user switches backend tabs
    active_backend_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_active_backend_subject(), this,
        [](AmsOperationSidebar* self, int /*active_index*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->sync_reset_button_label();
            self->update_check_gates_visibility();
        },
        AmsState::instance().get_subjects_lifetime());

    // Bypass spool color observer: refreshes loaded card when external spool changes
    bypass_spool_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](AmsOperationSidebar* self, int /*color_rgb*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->update_current_loaded_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // Color observer: reactively updates loaded card swatch color
    color_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_current_color_subject(), this,
        [](AmsOperationSidebar* self, int color_int) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            lv_obj_t* swatch = lv_obj_find_by_name(self->sidebar_root_, "loaded_swatch");
            if (swatch) {
                lv_color_t color = lv_color_hex(static_cast<uint32_t>(color_int));
                lv_obj_set_style_bg_color(swatch, color, 0);
                lv_obj_set_style_border_color(swatch, color, 0);
            }
        },
        AmsState::instance().get_subjects_lifetime());

    // Extruder temp observer: checks pending preheat load + refreshes heat step
    extruder_temp_observer_ = observe_int_sync<AmsOperationSidebar>(
        printer_state_.get_active_extruder_temp_subject(), this,
        [](AmsOperationSidebar* self, int /*temp_deci*/) {
            if (!self->active_)
                return;
            self->check_pending_load();
            self->refresh_heat_step_display();
        },
        printer_state_.get_subjects_lifetime());

    // Extruder target observer: refreshes heat step when target temp changes
    // (the macro raises the target before any visible action change)
    extruder_target_observer_ = observe_int_sync<AmsOperationSidebar>(
        printer_state_.get_active_extruder_target_subject(), this,
        [](AmsOperationSidebar* self, int /*target_deci*/) {
            if (!self->active_)
                return;
            self->refresh_heat_step_display();
        },
        printer_state_.get_subjects_lifetime());

    // Indeterminate "Working…" observer: when the backend flags a stalled
    // progress feed (frozen live-temp number), re-render the Heat step so it
    // swaps between the live temp readout and the busy "Working…" label
    // (#1065 row 14). Static singleton subject — plain ObserverGuard.
    indeterminate_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_ams_operation_indeterminate_subject(), this,
        [](AmsOperationSidebar* self, int /*indeterminate*/) {
            if (!self->active_)
                return;
            self->refresh_heat_step_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // The backend-driven step-index observer (step_index_observer_) is created
    // lazily in recreate_step_progress_for_operation() once the active backend's
    // step-index subject is known — the subject differs per backend (firmware
    // phase for the U1, narration toolchange-step for AFC-style backends) and
    // may not exist for coarse backends (legacy AmsAction fallback).
}

// ============================================================================
// Cleanup
// ============================================================================

void AmsOperationSidebar::cleanup() {
    // Clear active flag FIRST to prevent observer callbacks from using freed widgets
    active_ = false;

    // Delete the stall watchdog before anything else so its main-thread callback
    // can't fire mid-teardown (it never outlives the sidebar this way).
    if (stall_watchdog_timer_) {
        lv_timer_delete(stall_watchdog_timer_);
        stall_watchdog_timer_ = nullptr;
    }

    // Nullify widget refs BEFORE resetting observers — any cascading observer
    // callbacks that slip through the active_ guard will see null pointers and
    // bail out, preventing use-after-free on deleted LVGL objects.
    if (sidebar_root_) {
        lv_obj_set_user_data(sidebar_root_, nullptr);
    }
    sidebar_root_ = nullptr;
    step_progress_ = nullptr;
    step_progress_container_ = nullptr;

    // Reset ALL observers unconditionally. Keeping extruder_temp_observer_ alive
    // across panel switches is unsafe — the sidebar may be destroyed while the
    // observer still holds a raw pointer to it.
    action_observer_.reset();
    current_slot_observer_.reset();
    active_backend_observer_.reset();
    bypass_spool_observer_.reset();
    color_observer_.reset();
    step_index_observer_.reset(); // [L085] reset(), never release()
    extruder_temp_observer_.reset();
    extruder_target_observer_.reset();
    indeterminate_observer_.reset();
    filament_loaded_observer_.reset();
    print_state_observer_.reset();

    // Reset extracted modules AFTER observers — they may have their own observers
    // that reference widget pointers; resetting before our observers could
    // trigger callbacks on already-null widget pointers.
    clog_meter_.reset();

    // Clear all pending state. clear_home_preconfirmed() undoes a confirmed
    // but now-abandoned pre-load home prompt (panel closing mid-preheat) so
    // consent doesn't leak into a later, unrelated operation on this backend
    // -- idempotent no-op when nothing was armed.
    bypass_toggle_.cancel_pending();
    pending_load_slot_ = -1;
    pending_load_target_temp_ = 0;
    ui_initiated_heat_ = false;
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        backend->clear_home_preconfirmed();
    }
    prev_ams_action_ = AmsAction::IDLE;
    step_index_subject_ = nullptr;
    live_temp_step_index_ = -1;
    current_step_model_.steps.clear();

    spdlog::debug("[AmsSidebar] Cleaned up");
}

// ============================================================================
// Sync from State (call on panel activate)
// ============================================================================

void AmsOperationSidebar::sync_from_state() {
    if (!sidebar_root_) {
        return;
    }

    // Sync step progress with current action
    auto action =
        static_cast<AmsAction>(lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
    update_step_progress(action);

    // If we're in a UI-managed preheat, restore visual feedback
    if (pending_load_slot_ >= 0 && pending_load_target_temp_ > 0) {
        show_preheat_feedback(pending_load_slot_, pending_load_target_temp_);
    }

    // Sync loaded card display
    update_current_loaded_display();

    // Update settings visibility (backend may have changed)
    update_settings_visibility();
    update_check_gates_visibility();
    sync_reset_button_label();
    refresh_unload_gating();
}

// ============================================================================
// Settings Visibility
// ============================================================================

void AmsOperationSidebar::update_settings_visibility() {
    if (!sidebar_root_) {
        return;
    }

    auto* backend = AmsState::instance().get_backend(0);
    lv_obj_t* btn_settings = lv_obj_find_by_name(sidebar_root_, "btn_settings");
    if (btn_settings && backend) {
        auto sections = backend->get_device_sections();
        if (sections.empty()) {
            lv_obj_add_flag(btn_settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(btn_settings, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Check Gates Visibility
// ============================================================================

void AmsOperationSidebar::update_check_gates_visibility() {
    if (!active_ || !sidebar_root_) {
        return;
    }
    lv_obj_t* btn = lv_obj_find_by_name(sidebar_root_, "btn_check_gates");
    if (!btn) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->supports_gate_check()) {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Reset Button Label
// ============================================================================

void AmsOperationSidebar::sync_reset_button_label() {
    if (!active_ || !sidebar_root_) {
        return;
    }
    lv_obj_t* btn_reset = lv_obj_find_by_name(sidebar_root_, "btn_reset");
    if (!btn_reset) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    std::string label = backend ? backend->reset_button_label() : std::string("Reset");
    ui_button_set_text(btn_reset, lv_tr(label.c_str()));
}

// ============================================================================
// Current Loaded Display
// ============================================================================

void AmsOperationSidebar::update_current_loaded_display() {
    if (!sidebar_root_) {
        return;
    }

    // Subjects updated reactively by sync_from_backend(); color swatch driven by color_observer_
}

// ============================================================================
// Action Display
// ============================================================================

void AmsOperationSidebar::update_action_display(AmsAction action) {
    // Sidebar-only action display: step progress
    // Path canvas heat glow and error modal stay in the host panel
    update_step_progress(action);
}

// ============================================================================
// Step Progress
// ============================================================================

void AmsOperationSidebar::recreate_step_progress_for_operation(StepOperationType op_type) {
    if (!active_ || !step_progress_container_) {
        return;
    }

    // Delete existing step progress widget if any
    safe_delete_obj(step_progress_);
    heat_label_showing_temp_ = false; // fresh widget has plain "Heat nozzle" label

    current_operation_type_ = op_type;

    // Reset the backend-driven step driver; re-established below if the backend
    // supplies a specialized step model. Drop the old observer first.
    step_index_observer_.reset();
    step_index_subject_ = nullptr;
    live_temp_step_index_ = -1;
    current_step_model_.steps.clear();

    // Expose the active operation to AmsState so the narration router can
    // resolve `//` phase narration lines to step indices for THIS operation.
    AmsState::instance().set_active_step_operation(op_type);

    AmsBackend* backend = AmsState::instance().get_backend();

    // Preferred path: build the step bar from the backend's ordered step model.
    // The backend owns ALL operation-specific knowledge — firmware phases
    // (Snapmaker U1), narration phases (AFC), etc. The sidebar renders generically
    // and observes the backend-supplied index subject. No backend-type checks here.
    if (backend) {
        current_step_model_ = backend->get_operation_step_model(op_type);
        if (!current_step_model_.steps.empty()) {
            std::vector<ui_step_t> steps;
            steps.reserve(current_step_model_.steps.size());
            int idx = 0;
            for (const auto& s : current_step_model_.steps) {
                steps.push_back({lv_tr(s.label.c_str()), helix::StepState::Pending});
                if (s.live_temp) {
                    live_temp_step_index_ = idx;
                }
                ++idx;
            }
            current_step_count_ = static_cast<int>(steps.size());
            step_progress_ =
                ui_step_progress_create(step_progress_container_, steps.data(), current_step_count_,
                                        false, "ams_step_progress");
            if (step_progress_) {
                // Observe the backend-supplied current-step subject. The subject
                // is always a STATIC singleton (firmware phase or narration step),
                // so a member ObserverGuard with no SubjectLifetime is correct.
                step_index_subject_ = backend->get_operation_step_index_subject(op_type);
                if (step_index_subject_) {
                    // #1046 I-1: for narration-driven bars, seed the shared step
                    // subject to step 0 (heat) at operation start — BEFORE observing,
                    // so the initial observer fire highlights heat immediately and the
                    // bar never looks dead if the matcher misses the opening `//` line.
                    // This also discards any early narration index resolved against a
                    // prior operation's template before this recreate ran (#1046 M-1).
                    // Firmware-phase subjects (e.g. Snapmaker) mirror live hardware
                    // and MUST NOT be seeded — gate on the shared narration subject.
                    if (step_index_subject_ == AmsState::instance().get_toolchange_step_subject()) {
                        lv_subject_set_int(step_index_subject_, 0);
                    }
                    step_index_observer_ = observe_int_sync<AmsOperationSidebar>(
                        step_index_subject_, this, [](AmsOperationSidebar* self, int index) {
                            if (!self->active_ || !self->step_progress_)
                                return;
                            self->apply_backend_step_index(index);
                        });
                }
                spdlog::debug("[AmsSidebar] Created backend step bar: {} steps for op_type={} "
                              "(index_subject={})",
                              current_step_count_, static_cast<int>(op_type),
                              step_index_subject_ ? "set" : "null");
                return; // backend model drives the bar — skip the legacy switch
            }
            spdlog::error("[AmsSidebar] Failed to create backend step bar; falling back to "
                          "legacy switch for op_type={}",
                          static_cast<int>(op_type));
            current_step_model_.steps.clear();
        }
    }

    // Legacy path (backends with no step model — no regression): build the
    // hardcoded Heat/Feed/Purge stepper driven by the coarse AmsAction enum.
    // The Heat step is always index 0 here; mark it as the live-temp step.
    live_temp_step_index_ = 0;

    // Get capabilities from backend for dynamic labels
    TipMethod tip_method = TipMethod::CUT;
    bool supports_purge = false;
    if (backend) {
        AmsSystemInfo info = backend->get_system_info();
        tip_method = info.tip_method;
        supports_purge = info.supports_purge;
    }
    const char* tip_step_label = tip_method_step_label(tip_method);

    // Backends that neither cut nor form a tip (TipMethod::NONE — e.g. the
    // Snapmaker U1) have no discrete tip phase; omit that step from the
    // LOAD_SWAP / UNLOAD steppers so the labels match the firmware sequence.
    const bool has_tip_step = (tip_method != TipMethod::NONE);
    current_op_has_tip_step_ = has_tip_step;

    switch (op_type) {
    case StepOperationType::LOAD_FRESH: {
        if (supports_purge) {
            ui_step_t steps[] = {
                {lv_tr("Heat nozzle"), StepState::Pending},
                {lv_tr("Feed filament"), StepState::Pending},
                {lv_tr("Purge"), StepState::Pending},
            };
            current_step_count_ = 3;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 3, false,
                                                     "ams_step_progress");
        } else {
            ui_step_t steps[] = {
                {lv_tr("Heat nozzle"), StepState::Pending},
                {lv_tr("Feed filament"), StepState::Pending},
            };
            current_step_count_ = 2;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 2, false,
                                                     "ams_step_progress");
        }
        break;
    }
    case StepOperationType::LOAD_SWAP: {
        ui_step_t steps[4];
        int n = 0;
        steps[n++] = {lv_tr("Heat nozzle"), StepState::Pending};
        if (has_tip_step)
            steps[n++] = {tip_step_label, StepState::Pending};
        steps[n++] = {lv_tr("Feed filament"), StepState::Pending};
        if (supports_purge)
            steps[n++] = {lv_tr("Purge"), StepState::Pending};
        current_step_count_ = n;
        step_progress_ =
            ui_step_progress_create(step_progress_container_, steps, n, false, "ams_step_progress");
        break;
    }
    case StepOperationType::UNLOAD: {
        ui_step_t steps[3];
        int n = 0;
        steps[n++] = {lv_tr("Heat nozzle"), StepState::Pending};
        if (has_tip_step)
            steps[n++] = {tip_step_label, StepState::Pending};
        steps[n++] = {lv_tr("Retract"), StepState::Pending};
        current_step_count_ = n;
        step_progress_ =
            ui_step_progress_create(step_progress_container_, steps, n, false, "ams_step_progress");
        break;
    }
    }

    if (!step_progress_) {
        spdlog::error("[AmsSidebar] Failed to create step progress for op_type={}",
                      static_cast<int>(op_type));
    } else {
        spdlog::debug("[AmsSidebar] Created step progress: {} steps for op_type={}",
                      current_step_count_, static_cast<int>(op_type));
    }
}

void AmsOperationSidebar::apply_backend_step_index(int index) {
    if (!active_ || !step_progress_) {
        return;
    }
    // index -1 = no active step (firmware idle / narration cleared). The action
    // observer's show/hide logic handles container visibility at end-of-op; hold
    // the bar here so a transient -1 between steps doesn't flicker the highlight.
    if (index < 0 || index >= current_step_count_) {
        return;
    }
    spdlog::debug("[AmsSidebar] Backend step index {} (op_type={})", index,
                  static_cast<int>(current_operation_type_));
    ui_step_progress_set_current(step_progress_, index);

    // Refresh the live "<label> X / Y°C" readout on the live-temp step (declared
    // by the backend model, e.g. the Snapmaker Heat step). Driven both here (step
    // transitions) and by the extruder temp/target observers (live updates while
    // sitting on the live-temp step).
    refresh_live_temp_step_label(index);
}

void AmsOperationSidebar::refresh_live_temp_step_label(int current_index) {
    if (!step_progress_ || live_temp_step_index_ < 0) {
        return;
    }
    if (current_index == live_temp_step_index_) {
        const char* base_label =
            (live_temp_step_index_ < static_cast<int>(current_step_model_.steps.size()))
                ? lv_tr(current_step_model_.steps[live_temp_step_index_].label.c_str())
                : lv_tr("Heat nozzle");
        char label_buf[64];
        // When the backend flags the operation indeterminate, the live-temp feed
        // has frozen and the "225/230°C" number reads as a hang — swap it for an
        // indeterminate "Working…" busy label so it reads BUSY, not STUCK
        // (#1065 row 14). Restored to the live readout the moment the flag clears.
        bool indeterminate =
            lv_subject_get_int(AmsState::instance().get_ams_operation_indeterminate_subject()) != 0;
        if (indeterminate) {
            snprintf(label_buf, sizeof(label_buf), "%s %s", base_label, lv_tr("Working..."));
        } else {
            int current_deci =
                lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
            int target_deci =
                lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
            char temp_buf[32];
            temperature::format_temperature_pair(temperature::deci_to_degrees(current_deci),
                                                 temperature::deci_to_degrees(target_deci),
                                                 temp_buf, sizeof(temp_buf));
            snprintf(label_buf, sizeof(label_buf), "%s %s", base_label, temp_buf);
        }
        ui_step_progress_set_label(step_progress_, live_temp_step_index_, label_buf);
        heat_label_showing_temp_ = true;
        spdlog::debug("[AmsSidebar] Live-temp step label: {}", label_buf);
    } else if (heat_label_showing_temp_) {
        // Left the live-temp step — restore the static label.
        const char* base_label =
            (live_temp_step_index_ < static_cast<int>(current_step_model_.steps.size()))
                ? lv_tr(current_step_model_.steps[live_temp_step_index_].label.c_str())
                : lv_tr("Heat nozzle");
        ui_step_progress_set_label(step_progress_, live_temp_step_index_, base_label);
        heat_label_showing_temp_ = false;
    }
}

int AmsOperationSidebar::get_step_index_for_action(AmsAction action, StepOperationType op_type) {
    switch (op_type) {
    case StepOperationType::LOAD_FRESH:
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::LOADING:
            return 1;
        case AmsAction::PURGING:
            return 2;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }

    case StepOperationType::LOAD_SWAP: {
        // With no tip step the trailing steps shift up by one
        // (Heat=0, Feed=1, [Purge=2] vs Heat=0, Tip=1, Feed=2, [Purge=3]).
        const int feed_idx = current_op_has_tip_step_ ? 2 : 1;
        const int purge_idx = current_op_has_tip_step_ ? 3 : 2;
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::CUTTING:
        case AmsAction::FORMING_TIP:
        case AmsAction::UNLOADING:
            // No discrete tip step → keep the Heat step active through the
            // brief retract-old phase rather than pointing at a step that
            // doesn't exist.
            return current_op_has_tip_step_ ? 1 : 0;
        case AmsAction::LOADING:
            return feed_idx;
        case AmsAction::PURGING:
            return purge_idx;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }
    }

    case StepOperationType::UNLOAD: {
        // Without a tip step the Retract step moves from index 2 to index 1.
        const int retract_idx = current_op_has_tip_step_ ? 2 : 1;
        switch (action) {
        case AmsAction::HEATING:
            return 0;
        case AmsAction::CUTTING:
        case AmsAction::FORMING_TIP:
            return current_op_has_tip_step_ ? 1 : retract_idx;
        case AmsAction::UNLOADING:
            return retract_idx;
        case AmsAction::IDLE:
            return -1;
        default:
            return -1;
        }
    }
    }
    return -1;
}

void AmsOperationSidebar::start_operation(StepOperationType op_type, int target_slot) {
    // The active backend supplies the step model (and its driving index subject)
    // in recreate_step_progress_for_operation — no backend-type remapping here.
    spdlog::info("[AmsSidebar] Starting operation: type={}, target_slot={}",
                 static_cast<int>(op_type), target_slot);

    target_load_slot_ = target_slot;

    // Set pending target slot early for pulse animation
    AmsState::instance().set_pending_target_slot(target_slot);

    // Set action to HEATING immediately — triggers XML binding to hide buttons
    AmsState::instance().set_action(AmsAction::HEATING);

    // Show the container BEFORE building the step widget so the create-time
    // layout pass runs against a visible (non-collapsed) container. The step
    // connectors also relayout on SIZE_CHANGED, but revealing first keeps the
    // very first paint correct.
    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }

    // Create step progress with correct steps
    recreate_step_progress_for_operation(op_type);
}

void AmsOperationSidebar::fail_started_operation(const AmsError& error) {
    spdlog::warn("[AmsSidebar] Operation dispatch failed: {} ({})", error.user_msg,
                 error.technical_msg);
    helix::ui::notify_ams_error(error, lv_tr("Filament operation failed"));
    target_load_slot_ = -1;
    AmsState::instance().set_pending_target_slot(-1);
    // Backend never left IDLE; pull its truth back into the UI so the action
    // buttons reappear and the step bar hides.
    AmsState::instance().sync_from_backend();
}

void AmsOperationSidebar::update_step_progress(AmsAction action) {
    if (!active_ || !step_progress_container_) {
        return;
    }

    // Heuristic detection for externally-started operations
    bool is_external = (target_load_slot_ < 0);
    bool filament_loaded = false;
    if (is_external) {
        AmsBackend* backend = AmsState::instance().get_backend();
        if (backend) {
            AmsSystemInfo info = backend->get_system_info();
            filament_loaded = (info.current_slot >= 0);
        }
    }

    auto detection = detect_step_operation(action, prev_ams_action_, current_operation_type_,
                                           is_external, filament_loaded);
    if (detection.should_recreate) {
        StepOperationType new_op = detection.op_type;
        if (new_op == StepOperationType::LOAD_SWAP &&
            current_operation_type_ == StepOperationType::UNLOAD) {
            spdlog::debug("[AmsSidebar] Upgrading UNLOAD → LOAD_SWAP");
        }
        recreate_step_progress_for_operation(new_op);
        // The coarse jump_to_step hint only applies to the legacy AmsAction model.
        // When a backend step model drives the bar, its index subject is the sole
        // step driver — skip the hint so it doesn't fight the backend.
        if (!step_index_subject_ && detection.jump_to_step >= 0 && step_progress_) {
            ui_step_progress_set_current(step_progress_, detection.jump_to_step);
        }
    }

    if (!step_progress_) {
        return;
    }

    // Show/hide container based on action
    bool show_progress = (action == AmsAction::HEATING || action == AmsAction::LOADING ||
                          action == AmsAction::PURGING || action == AmsAction::CUTTING ||
                          action == AmsAction::FORMING_TIP || action == AmsAction::UNLOADING);

    if (show_progress) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
        target_load_slot_ = -1;
        if (heat_label_showing_temp_) {
            int reset_idx = (live_temp_step_index_ >= 0) ? live_temp_step_index_ : 0;
            ui_step_progress_set_label(step_progress_, reset_idx, lv_tr("Heat nozzle"));
            heat_label_showing_temp_ = false;
        }
        return;
    }

    // Backend-driven step bar (firmware phase OR narration): the backend-supplied
    // index subject is the sole step driver — the coarse AmsAction→index map and
    // heat-anchor below are for the legacy 2-step bar and must NOT run here (they
    // would fight the index observer). Apply the current value and return.
    if (step_index_subject_) {
        int index = lv_subject_get_int(step_index_subject_);
        apply_backend_step_index(index);
        return;
    }

    int step_index = get_step_index_for_action(action, current_operation_type_);

    // Physical-state anchor: backends emit LOADING/UNLOADING/etc. at gcode dispatch,
    // before the printer physically leaves the heating phase. Hold the indicator at
    // step 0 with a live "X / Y°C" label until the extruder reaches its target.
    if (is_extruder_below_target()) {
        step_index = 0;
        int current_deci = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        int target_deci = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        char temp_buf[32];
        temperature::format_temperature_pair(temperature::deci_to_degrees(current_deci),
                                             temperature::deci_to_degrees(target_deci), temp_buf,
                                             sizeof(temp_buf));
        char label_buf[64];
        snprintf(label_buf, sizeof(label_buf), "%s %s", lv_tr("Heat nozzle"), temp_buf);
        ui_step_progress_set_label(step_progress_, 0, label_buf);
        heat_label_showing_temp_ = true;
    } else if (heat_label_showing_temp_) {
        ui_step_progress_set_label(step_progress_, 0, lv_tr("Heat nozzle"));
        heat_label_showing_temp_ = false;
    }

    if (step_index >= 0) {
        ui_step_progress_set_current(step_progress_, step_index);
    }
}

bool AmsOperationSidebar::is_extruder_below_target() const {
    int target_deci = lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
    if (target_deci <= 0) {
        return false;
    }
    int current_deci = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    // 5°C threshold matches check_pending_load() at line ~795
    constexpr int TEMP_THRESHOLD_DECI = 50;
    return current_deci < (target_deci - TEMP_THRESHOLD_DECI);
}

void AmsOperationSidebar::refresh_heat_step_display() {
    if (!active_ || !step_progress_) {
        return;
    }
    int action_int = lv_subject_get_int(AmsState::instance().get_ams_action_subject());
    update_step_progress(static_cast<AmsAction>(action_int));
}

// ============================================================================
// Action Handlers
// ============================================================================

helix::ui::OpButtonState AmsOperationSidebar::read_unload_gating_state() const {
    helix::ui::OpButtonState state;

    AmsBackend* backend = AmsState::instance().get_backend();

    // print_blocks_filament_op(), not the raw print_active subject: PRINTING
    // always refuses, but a PAUSED print now ALLOWS the unload on every backend
    // whose filament macro does not home itself (only AD5X IFS does). Gating on
    // print_active would keep this button greyed through the pause that is the
    // entire recovery workflow — and the sidebar had no print term at all
    // before, so it went straight from "always tappable" to "correct" only if
    // this asks the same question the backend does.
    const auto lifecycle = printer_state_.get_print_lifecycle();
    state.print_blocks_op = helix::ui::print_blocks_filament_op(
        lifecycle, backend && backend->filament_ops_self_home());

    if (backend) {
        // AmsSystemInfo::is_busy() — the same predicate check_preconditions()
        // refuses on, instead of a fourth open-coded `action != IDLE && != ERROR`.
        state.system_busy = backend->get_system_info().is_busy();
    }

    // This button means "unload whatever is active", so the aggregate loaded flag
    // is its availability — the same signal the XML used to bind directly.
    lv_subject_t* loaded = AmsState::instance().get_filament_loaded_subject();
    state.unload_available = loaded && lv_subject_get_int(loaded) == 1;

    // Always the heated toolhead unload. The cold lane ops (Eject / Recover),
    // which stay reachable mid-print, live on the AMS context menu.
    state.unload_is_cold_lane_op = false;
    return state;
}

void AmsOperationSidebar::refresh_unload_gating() {
    if (!s_unload_disabled_initialized) {
        return;
    }
    const auto gating = helix::ui::compute_op_button_gating(read_unload_gating_state());
    lv_subject_set_int(&s_unload_disabled, gating.unload_disabled ? 1 : 0);
}

void AmsOperationSidebar::handle_unload() {
    // Active-slot unload (sidebar Unload button). Delegate to the slot overload
    // with slot_index = -1 so the stepper-build + backend-call path lives in one
    // place. The IFS backend's unload_filament() ignores the slot index and
    // sends the current-channel toolhead unload (IFS_REMOVE_CURRENT_PRUTOK).
    handle_unload(-1);
}

void AmsOperationSidebar::handle_unload(int slot_index) {
    spdlog::info("[AmsSidebar] Unload requested (slot={})", slot_index);

    // The button is bound to ams_sidebar_unload_disabled, but a tap can still
    // land in the window between a print starting and the subject settling, and
    // handle_unload(slot) is also the context menu's dispatch entry. Refuse here
    // with copy the user can act on rather than forwarding a guaranteed backend
    // rejection ("Cannot run filament operation while printing" raised while
    // merely PAUSED was a live field report).
    const auto gating_state = read_unload_gating_state();
    if (gating_state.system_busy) {
        spdlog::info("[AmsSidebar] Unload refused — an AMS operation is already running");
        NOTIFY_WARNING(lv_tr("Wait for the current filament operation to finish"));
        return;
    }
    if (gating_state.print_blocks_op) {
        AmsBackend* gating_backend = AmsState::instance().get_backend();
        const bool self_homes = gating_backend && gating_backend->filament_ops_self_home();
        spdlog::info("[AmsSidebar] Unload refused — a print owns the toolhead (self_homes={})",
                     self_homes);
        if (self_homes) {
            // AD5X IFS: the unload macro homes itself, so pausing does not help
            // — recommending it would send the user into the exact loadcell-Z
            // collision the backend guard exists to prevent.
            NOTIFY_WARNING(lv_tr("Cannot unload while a print is active"));
        } else {
            // PRINTING on every other backend. A PAUSED print here would not
            // have reached this branch at all, so pausing IS the recovery.
            NOTIFY_WARNING(lv_tr("Pause the print first, then load, unload, or change filament"));
        }
        return;
    }

    AmsSystemInfo info;
    // -1: plan_unload() never reads caps.needs_unload_before_load, and the slot
    // this unload is about is only resolvable from the info this call fills in.
    const helix::ui::BackendCaps caps = read_backend_caps(info, /*target_slot=*/-1);

    // Which slot the unload is *about*. -1 from the sidebar's own Unload button
    // means "whatever the firmware has active"; the context menu passes a slot.
    const int target_slot = (slot_index >= 0) ? slot_index : info.current_slot;

    // plan_unload() gates tier 1 on the backend merely existing — deliberately
    // asymmetric with plan_load(), because bypass unload stays on the backend:
    // AFC calls the user's unload macro itself when bypass is enabled, and
    // routing it to tier 2 here would run that macro twice.
    bool loaded = false;
    if (caps.present) {
        AmsBackend* backend = AmsState::instance().get_backend();
        loaded = backend && helix::ui::unload_target_is_loaded(
                                target_slot, backend->slot_is_actively_loaded(target_slot),
                                backend->slot_has_filament_at_toolhead(target_slot),
                                info.current_slot == target_slot, info.filament_loaded);
    }

    const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    const helix::ui::FilamentOpPlan plan =
        helix::ui::plan_unload(caps, target_slot, loaded, !macro_info.is_empty(),
                               macro_info.get_source() == MacroSource::CONFIGURED);

    if (plan.tier == helix::ui::FilamentTier::Refused) {
        // NothingLoaded is plan_unload's only refusal.
        spdlog::info("[AmsSidebar] Unload refused — nothing loaded (slot={})", target_slot);
        NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
        return;
    }

    // Filament is being pulled — drop the swap-preheat latch so the next load
    // computes its hold-temp fresh instead of inheriting this material's target.
    // Cleared once we know something will actually be dispatched, never on a
    // refusal.
    printer_state_.clear_nozzle_load_latch();

    if (plan.tier != helix::ui::FilamentTier::AmsBackend) {
        dispatch_unload_outside_backend(plan);
        return;
    }

    // Build the UNLOAD stepper first (HEATING + correct step list).
    if (target_slot >= 0) {
        start_operation(StepOperationType::UNLOAD, target_slot);
    }

    // Pass the caller's slot through unchanged, NOT plan.ams_arg: the sidebar's
    // own Unload button means "the active one" and the IFS backend keys on that
    // -1 to send its current-channel toolhead unload.
    AmsBackend* backend = AmsState::instance().get_backend();

    // Nothing reels a bypass spool back down a lane, so the user finishes by
    // hand. Armed before dispatch so the toolhead sensor's clear edge is already
    // watched when the retract starts; the action observer closes it out.
    //
    // Deliberately scoped to the tier-1 path: dispatch_unload_outside_backend()
    // returns above, and that no-AMS case belongs to FilamentPanel, which owns a
    // real per-tier completion signal. The sidebar has only this action edge, and
    // with no backend the action never moves.
    const bool needs_pull =
        helix::ui::unload_needs_manual_pull(/*backend_present=*/true, target_slot);
    if (needs_pull) {
        helix::ui::arm_manual_pull_prompt();
    }

    AmsError error = backend->unload_filament(slot_index);
    if (error.result != AmsResult::SUCCESS) {
        if (needs_pull) {
            helix::ui::disarm_manual_pull_prompt();
        }
        helix::ui::notify_ams_error(error);
    }
}

void AmsOperationSidebar::handle_reset() {
    spdlog::info("[AmsSidebar] Reset requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    // Clear any latched fault before re-prepping. AFC_RESET alone leaves
    // printer.AFC.message populated, so the sidebar kept showing an error the
    // user had just asked to reset away.
    AmsError cleared = backend->clear_fault(backend->get_system_info().current_slot);
    if (!cleared.success()) {
        spdlog::warn("[AmsSidebar] Fault clear during reset failed: {}", cleared.user_msg);
    }

    AmsError error = backend->reset();
    if (error.result != AmsResult::SUCCESS) {
        helix::ui::notify_ams_error(error, lv_tr("Reset failed"));
    }
}

void AmsOperationSidebar::handle_check_gates() {
    spdlog::info("[AmsSidebar] Check all gates requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    AmsError error = backend->check_all_gates();
    if (error.result != AmsResult::SUCCESS) {
        helix::ui::notify_ams_error(error, lv_tr("Check slots failed"));
    }
}

void AmsOperationSidebar::handle_bypass_toggle() {
    bypass_toggle_.toggle();

    // ui_switch flips its own CHECKED state before the handler runs, so a
    // refusal (print active, hardware sensor, backend precondition) leaves the
    // switch claiming a bypass state the backend never entered. Republish and
    // force the binding to re-apply — lv_subject_set_int does not notify when
    // the value is unchanged, which is precisely the refusal case. Same two
    // lines as the Device Operations switch.
    AmsState::instance().sync_from_backend();
    lv_subject_notify(AmsState::instance().get_bypass_active_subject());
}

// ============================================================================
// Preheat Logic
// ============================================================================

int AmsOperationSidebar::get_load_temp_for_slot(int slot_index) {
    // The slot-vs-external-spool precedence and the nozzle_recommended() choice
    // both come from resolve_load_preheat_material(), shared with
    // FilamentPanel::resolve_preheat_temp(). The two surfaces preheating the
    // same lane to two different temperatures is the bug that rule exists to end.
    AmsBackend* backend = AmsState::instance().get_backend();
    SlotInfo slot;
    const SlotInfo* slot_ptr = nullptr;
    if (backend && slot_index >= 0) {
        slot = backend->get_slot_info(slot_index);
        slot_ptr = &slot;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    const auto resolved = helix::ui::resolve_load_preheat_material(
        slot_index, slot_ptr, ext.has_value() ? &ext.value() : nullptr);

    if (resolved) {
        return resolved->temp_c;
    }
    return AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP;
}

void AmsOperationSidebar::handle_load_with_preheat(int slot_index) {
    // The three-tier routing (AMS backend → configured macro → raw gcode) and
    // the load-vs-swap rule both come from plan_load(), the one answer every
    // dispatch surface shares. This sidebar used to be the only place the
    // already-mounted guard lived; it is now FilamentRefusal::AlreadyMounted.
    AmsSystemInfo info;
    const helix::ui::BackendCaps caps = read_backend_caps(info, slot_index);

    const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    const helix::ui::FilamentOpPlan plan =
        helix::ui::plan_load(info, caps, slot_index, !macro_info.is_empty(),
                             macro_info.get_source() == MacroSource::CONFIGURED);

    if (plan.tier == helix::ui::FilamentTier::Refused) {
        // Silent on THIS surface. The AMS panel already highlights the mounted
        // slot and greys the ones that cannot be picked, so a toast here would
        // narrate what the grid is showing. The Filament panel, where the user
        // pressed a button with no other feedback, does toast.
        spdlog::debug("[AmsSidebar] Load of slot {} refused ({})", slot_index,
                      plan.refusal == helix::ui::FilamentRefusal::AlreadyMounted
                          ? "already mounted"
                          : "no slot selected");
        return;
    }

    if (plan.tier != helix::ui::FilamentTier::AmsBackend) {
        dispatch_load_outside_backend(plan);
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();

    // Tool changers keep their fast path: no UI preheat and no optimistic
    // HEATING stepper. SELECT_TOOL owns its own heat sequence, and the backend
    // sets SELECTING at dispatch and resolves it on the macro ack (#1183) — an
    // optimistic HEATING would fight that. Only the *decision* is shared.
    if (caps.is_tool_changer) {
        dispatch_backend_load(plan, slot_index);
        return;
    }

    // Determine operation type BEFORE calling the backend. plan.ams_call carries
    // the load-vs-swap answer (needs_unload_before_load, centralized so the UI
    // and backend agree — K1 CFS reports a preloaded cassette slot with an empty
    // nozzle and a SWAP there would cut nothing and stall at the cut step,
    // #968).
    start_operation(plan.ams_call == helix::ui::AmsCall::Load ? StepOperationType::LOAD_FRESH
                                                              : StepOperationType::LOAD_SWAP,
                    slot_index);

    // If backend handles heating automatically, just call load directly
    if (backend->supports_auto_heat_on_load()) {
        ui_initiated_heat_ = false;
        dispatch_backend_load(plan, slot_index);
        return;
    }

    // Otherwise, UI handles preheat
    int target = get_load_temp_for_slot(slot_index);

    int current_deci = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current = temperature::deci_to_degrees(current_deci);

    // Swap-preheat: the effective load temp is the hotter of the requested
    // material temp and the latched last-nonzero nozzle target, so a nozzle that
    // cooled below the previous material's temp still reheats to purge it. Fold the
    // latch into the skip/wait decision; the controller applies the same max()
    // (against latch AND actual) when we send, via keep_previous_hot.
    int latch =
        static_cast<int>(std::lround(printer_state_.get_active_extruder_last_nonzero_target()));
    int effective_target = std::max(target, latch);

    constexpr int TEMP_THRESHOLD = 5;
    if (current >= (effective_target - TEMP_THRESHOLD)) {
        ui_initiated_heat_ = false;
        dispatch_backend_load(plan, slot_index);
        return;
    }

    // Start preheating. get_temperature_controller() captured by value below
    // since it's a plain accessor, not a stashed pointer across the async gap.
    auto start_preheat = [this, slot_index, target, effective_target, latch]() {
        pending_load_slot_ = slot_index;
        pending_load_target_temp_ = effective_target;
        ui_initiated_heat_ = true;

        if (auto* c = get_temperature_controller()) {
            c->set_target(helix::HeaterType::Nozzle, static_cast<double>(target),
                          {.toast = false, .keep_previous_hot = true});
        }

        show_preheat_feedback(slot_index, effective_target);

        spdlog::info(
            "[AmsSidebar] Starting preheat to {}C (requested {}, latch {}) for slot {} load",
            effective_target, target, latch, slot_index);
    };

    // Ask "home printer first?" BEFORE the preheat, not after: the physical
    // G28 still fires later, inside AmsSubscriptionBackend::ensure_homed_then()
    // right before the tier-1 dispatch (unchanged) -- only the confirmation
    // moves earlier, so a decline never wastes a preheat cycle.
    if (!helix::toolhead_is_homed(printer_state_) &&
        !(backend && backend->delegates_homing_to_printer())) {
        spdlog::info("[AmsSidebar] Toolhead not homed -- asking before starting preheat for "
                     "slot {} load",
                     slot_index);
        // The shared MacroParamModal comment above explains why this sidebar
        // is NOT immortal (dies with the AMS panel) -- route through
        // lifetime_.token()/defer() rather than a bare captured [this].
        auto token = lifetime_.token();
        helix::ui::request_home_confirmation(
            [this, token, start_preheat]() {
                token.defer(HOME_CONFIRM_LOAD_TAG, [this, start_preheat]() {
                    if (AmsBackend* backend = AmsState::instance().get_backend()) {
                        backend->arm_home_preconfirmed();
                    }
                    start_preheat();
                });
            },
            [this, token]() {
                token.defer(HOME_CONFIRM_LOAD_DECLINE_TAG, [this]() {
                    spdlog::info("[AmsSidebar] User declined pre-load home; no heat commanded");
                });
            });
        return;
    }

    start_preheat();
}

void AmsOperationSidebar::check_pending_load() {
    if (pending_load_slot_ < 0) {
        return;
    }

    int current_deci = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current = temperature::deci_to_degrees(current_deci);

    // Update display with current temperature while waiting
    char temp_buf[32];
    temperature::format_temperature_pair(current, pending_load_target_temp_, temp_buf,
                                         sizeof(temp_buf));
    AmsState::instance().set_action_detail(temp_buf);

    constexpr int TEMP_THRESHOLD = 5;

    if (current >= (pending_load_target_temp_ - TEMP_THRESHOLD)) {
        int slot = pending_load_slot_;
        pending_load_slot_ = -1;
        pending_load_target_temp_ = 0;

        // Re-plan against live state rather than replaying the plan computed
        // before the preheat: the firmware may have picked up or dropped a tool
        // while the nozzle came up to temperature, which flips load-vs-swap.
        AmsSystemInfo preheat_info;
        const helix::ui::BackendCaps caps = read_backend_caps(preheat_info, slot);
        const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
        const helix::ui::FilamentOpPlan plan =
            helix::ui::plan_load(preheat_info, caps, slot, !macro_info.is_empty(),
                                 macro_info.get_source() == MacroSource::CONFIGURED);

        if (plan.tier != helix::ui::FilamentTier::AmsBackend) {
            // The preheat only ever starts on the tier-1 path, so anything else
            // means the backend changed under us. Surface it in the log and let
            // the stepper unwind rather than dispatching a guess.
            spdlog::warn("[AmsSidebar] Preheat complete but slot {} no longer routes to the "
                         "backend (tier={}) — not dispatching",
                         slot, static_cast<int>(plan.tier));
            return;
        }
        spdlog::info("[AmsSidebar] Preheat complete, dispatching load for slot {}", slot);
        dispatch_backend_load(plan, slot);
    }
}

// ============================================================================
// Shared Dispatch Plan
// ============================================================================

helix::ui::BackendCaps AmsOperationSidebar::read_backend_caps(AmsSystemInfo& info_out,
                                                              int target_slot) const {
    helix::ui::BackendCaps caps;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return caps;
    }
    info_out = backend->get_system_info();
    caps.present = true;
    caps.requires_slot_selection_for_load = backend->requires_slot_selection_for_load();
    caps.needs_unload_before_load = backend->needs_unload_before_load(info_out, target_slot);
    caps.is_tool_changer = backend->get_type() == AmsType::TOOL_CHANGER;
    return caps;
}

void AmsOperationSidebar::dispatch_backend_load(const helix::ui::FilamentOpPlan& plan,
                                                int slot_index) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[AmsSidebar] Backend vanished before load dispatch (slot {})", slot_index);
        return;
    }

    AmsError error;
    switch (plan.ams_call) {
    case helix::ui::AmsCall::ChangeTool:
        spdlog::info("[AmsSidebar] Filament seated — swapping to slot {} via tool change T{}",
                     slot_index, plan.ams_arg);
        error = backend->change_tool(plan.ams_arg);
        break;
    case helix::ui::AmsCall::Load:
    default: // plan_load yields no other tier-1 call
        spdlog::info("[AmsSidebar] Loading slot {} directly", plan.ams_arg);
        error = backend->load_filament(plan.ams_arg);
        break;
    }

    if (!error.success()) {
        fail_started_operation(error);
    }
}

// ============================================================================
// Tiers 2 and 3 — configured macro, then raw gcode
// ============================================================================

namespace {
/// UpdateQueue tags for the guarded param-modal callbacks. String literals: the
/// skip counter interns by pointer identity.
constexpr const char* LOAD_MACRO_TAG = "AmsOperationSidebar::load_macro";
constexpr const char* UNLOAD_MACRO_TAG = "AmsOperationSidebar::unload_macro";
} // namespace

void AmsOperationSidebar::dispatch_load_outside_backend(const helix::ui::FilamentOpPlan& plan) {
    if (plan.tier == helix::ui::FilamentTier::RawGcode) {
        spdlog::info("[AmsSidebar] No backend and no load macro — raw gcode fallback");
        send_filament_fallback_gcode(/*is_load=*/true);
        return;
    }

    const std::string macro_name =
        StandardMacros::instance().get(StandardMacroSlot::LoadFilament).get_macro();
    // The shared MacroParamModal retains this callback past dismissal, and this
    // sidebar dies with the AMS panel. token.defer() re-checks the generation on
    // the main thread before touching `this`; a Run press after the panel closed
    // is dropped (and counted) instead of running against freed memory.
    auto token = lifetime_.token();
    helix::ui::dispatch_filament_macro(
        macro_name, helix::ui::ParamPolicy::Prompt,
        [this, token](const helix::MacroParamResult& result) {
            token.defer(LOAD_MACRO_TAG, [this, params = result.params]() {
                send_standard_filament_macro(/*is_load=*/true, params);
            });
        });
}

void AmsOperationSidebar::dispatch_unload_outside_backend(const helix::ui::FilamentOpPlan& plan) {
    if (plan.tier == helix::ui::FilamentTier::RawGcode) {
        spdlog::info("[AmsSidebar] No backend and no unload macro — raw gcode fallback");
        send_filament_fallback_gcode(/*is_load=*/false);
        return;
    }

    const std::string macro_name =
        StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).get_macro();
    auto token = lifetime_.token();
    helix::ui::dispatch_filament_macro(
        macro_name, helix::ui::ParamPolicy::Prompt,
        [this, token](const helix::MacroParamResult& result) {
            token.defer(UNLOAD_MACRO_TAG, [this, params = result.params]() {
                send_standard_filament_macro(/*is_load=*/false, params);
            });
        });
}

void AmsOperationSidebar::send_standard_filament_macro(
    bool is_load, const std::map<std::string, std::string>& params) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    const StandardMacroSlot slot =
        is_load ? StandardMacroSlot::LoadFilament : StandardMacroSlot::UnloadFilament;
    const char* label = is_load ? "Load" : "Unload";
    spdlog::info("[AmsSidebar] Running standard {} macro with {} parameter(s)", label,
                 params.size());
    StandardMacros::instance().execute(
        slot, api, params, [label]() { spdlog::info("[AmsSidebar] {} macro started", label); },
        [is_load](const MoonrakerError& err) {
            spdlog::error("[AmsSidebar] Filament macro failed: {}", err.message);
            if (is_load) {
                NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_message());
            } else {
                NOTIFY_ERROR(lv_tr("Failed to unload: {}"), err.user_message());
            }
        });
}

void AmsOperationSidebar::send_filament_fallback_gcode(bool is_load) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    const std::string gcode = is_load ? helix::ui::filament_load_fallback_gcode()
                                      : helix::ui::filament_unload_fallback_gcode();
    api->execute_gcode(
        gcode, []() {},
        [is_load](const MoonrakerError& err) {
            spdlog::error("[AmsSidebar] Fallback gcode failed: {}", err.message);
            if (is_load) {
                NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_message());
            } else {
                NOTIFY_ERROR(lv_tr("Failed to unload: {}"), err.user_message());
            }
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

void AmsOperationSidebar::handle_load_complete() {
    if (ui_initiated_heat_) {
        PostOpCooldownManager::instance().schedule();
        spdlog::info("[AmsSidebar] Load complete, scheduling cooldown (UI-initiated heat)");
        ui_initiated_heat_ = false;
    }
}

void AmsOperationSidebar::show_preheat_feedback(int slot_index, int target_temp) {
    LV_UNUSED(slot_index);

    int current_deci = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
    int current_temp = temperature::deci_to_degrees(current_deci);

    char temp_buf[32];
    temperature::format_temperature_pair(current_temp, target_temp, temp_buf, sizeof(temp_buf));
    AmsState::instance().set_action_detail(temp_buf);

    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (step_progress_) {
        ui_step_progress_set_current(step_progress_, 0);
    }

    spdlog::debug("[AmsSidebar] Showing preheat feedback: {}", temp_buf);
}

} // namespace helix::ui
