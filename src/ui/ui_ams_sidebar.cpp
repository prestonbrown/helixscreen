// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_sidebar.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_slot_layout.h"
#include "ui_batch_filament_modal.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_manual_pull_prompt.h"
#include "ui_spool_canvas.h"
#include "ui_step_progress.h"
#include "ui_temperature_utils.h"
#include "ui_unload_lane_choice.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_op_heater.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_size.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "temperature_controller.h"
#include "tool_change_request.h"
#include "toolhead_homing.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

/// How long a held optimistic action outlives backend silence once the backend
/// has accepted the operation: the gap before its first frame, which can
/// include a G28.
constexpr std::chrono::seconds kOptimisticDispatchHold{90};

/// The backend accepted the operation: restart a held action's budget from
/// here, and sync so a backend that set its own action at dispatch drives.
void hand_operation_to_backend() {
    auto& ams = AmsState::instance();
    if (ams.optimistic_action_held()) {
        ams.hold_optimistic_action(kOptimisticDispatchHold);
    }
    ams.sync_from_backend();
}

/// The heater watches the action feed for the end of a backend op it heated
/// for. Armed before the dispatch, so a fast op cannot finish unseen.
void watch_backend(FilamentOpTicket ticket) {
    if (auto* heater = get_filament_op_heater()) {
        heater->watch_backend_completion(ticket);
    }
}

/// The backend refused the op @p ticket names, or it never reached the backend.
void backend_refused(FilamentOpTicket ticket) {
    if (auto* heater = get_filament_op_heater()) {
        heater->op_failed(ticket);
    }
}

/// An op the UI marked busy will not run: drop the target pulse, release the
/// held action and pull the backend's truth back so the buttons reappear.
void release_operation() {
    AmsState::instance().set_pending_target_slot(-1);
    AmsState::instance().release_optimistic_action();
    AmsState::instance().sync_from_backend();
}

/// A tier-1 load, re-planned against the backend as it is now: it runs after a
/// preheat the firmware may have picked up or dropped a tool during, which
/// flips load-vs-swap. Needs no sidebar, so it runs after the AMS panel closes.
void dispatch_backend_load(int slot_index, FilamentOpTicket ticket) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[AmsSidebar] Backend vanished before load dispatch (slot {})", slot_index);
        backend_refused(ticket);
        release_operation();
        return;
    }
    AmsSystemInfo info;
    const BackendCaps caps = helix::ui::read_backend_caps(backend, info, slot_index);
    const FilamentOpPlan plan = plan_live_load(info, caps, slot_index);
    if (plan.tier != FilamentTier::AmsBackend) {
        // Only the tier-1 path reaches here, so anything else means the backend
        // changed under the preheat. Unwind rather than dispatch a guess.
        spdlog::warn("[AmsSidebar] Slot {} no longer routes to the backend (tier={}) — not "
                     "dispatching",
                     slot_index, static_cast<int>(plan.tier));
        backend_refused(ticket);
        release_operation();
        return;
    }

    watch_backend(ticket);
    AmsError error;
    switch (plan.ams_call) {
    case AmsCall::ChangeTool:
        spdlog::info("[AmsSidebar] Filament seated — swapping to slot {} via tool change T{}",
                     slot_index, plan.ams_arg);
        error = backend->change_tool(plan.ams_arg);
        break;
    case AmsCall::Load:
    default: // plan_load yields no other tier-1 call
        spdlog::info("[AmsSidebar] Loading slot {} directly", plan.ams_arg);
        error = backend->load_filament(plan.ams_arg);
        break;
    }
    if (!error.success()) {
        spdlog::warn("[AmsSidebar] Operation dispatch failed: {} ({})", error.user_msg,
                     error.technical_msg);
        helix::ui::notify_ams_error(error, lv_tr("Filament operation failed"));
        backend_refused(ticket);
        release_operation();
        return;
    }
    hand_operation_to_backend();
}

/// A tier-1 unload. @p slot_index goes to the backend unchanged, NOT the plan's
/// ams_arg: the sidebar's own Unload button means "the active one" and a
/// backend can key on that -1 to unload its current channel. @p target_slot is
/// the lane the unload is about.
void dispatch_backend_unload(int slot_index, int target_slot, FilamentOpTicket ticket) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[AmsSidebar] Backend vanished before unload dispatch (slot {})", slot_index);
        backend_refused(ticket);
        release_operation();
        return;
    }
    // Filament is being pulled — drop the swap-preheat latch so the next load
    // computes its hold-temp fresh instead of inheriting this material's target.
    get_printer_state().temperature_state().clear_load_latch();

    // Nothing reels a bypass spool back down a lane, so the user finishes by
    // hand. Armed before dispatch so the toolhead sensor's clear edge is already
    // watched when the retract starts; the action observer closes it out.
    //
    // Deliberately scoped to the tier-1 path: the executor's tiers belong to
    // FilamentPanel's no-AMS case, which owns a real per-tier completion signal.
    // The sidebar has only this action edge, and with no backend the action
    // never moves.
    const bool needs_pull =
        helix::ui::unload_needs_manual_pull(/*backend_present=*/true, target_slot);
    if (needs_pull) {
        helix::ui::arm_manual_pull_prompt();
    }

    watch_backend(ticket);
    AmsError error = backend->unload_filament(slot_index);
    if (error.result != AmsResult::SUCCESS) {
        if (needs_pull) {
            helix::ui::disarm_manual_pull_prompt();
        }
        helix::ui::notify_ams_error(error);
        backend_refused(ticket);
        release_operation();
        return;
    }
    hand_operation_to_backend();
}

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
HELIX_PSRAM_BSS lv_subject_t s_unload_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_load_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_reset_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_check_gates_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_load_hidden{};
HELIX_PSRAM_BSS lv_subject_t s_unload_hidden{};
HELIX_PSRAM_BSS lv_subject_t s_unload_picks{};
HELIX_PSRAM_BSS lv_subject_t s_mount_hidden{};
HELIX_PSRAM_BSS lv_subject_t s_mount_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_park_hidden{};
HELIX_PSRAM_BSS lv_subject_t s_park_disabled{};
HELIX_PSRAM_BSS lv_subject_t s_action_reason{};
HELIX_PSRAM_BSS lv_subject_t s_action_reason_shown{};
HELIX_PSRAM_BSS char s_action_reason_buf[192];
HELIX_PSRAM_BSS lv_subject_t s_supports_reset{};
HELIX_PSRAM_BSS lv_subject_t s_cancel_shown{};
bool s_gating_subjects_initialized = false;
SubjectManager s_subjects;

/// The sidebar whose refresh last wrote the subjects above. They describe that
/// sidebar's backend, so they go back to their defaults when it goes away: a
/// sidebar built later for another backend must not inherit a hidden Unload.
const AmsOperationSidebar* s_gating_owner = nullptr;

/// The values nothing has refreshed yet.
void reset_button_gating_subjects() {
    // Disabled: nothing is known about the backend or the job until the first
    // refresh, and an over-eager button is the failure that matters here.
    lv_subject_set_int(&s_unload_disabled, 1);
    lv_subject_set_int(&s_load_disabled, 1);
    lv_subject_set_int(&s_reset_disabled, 1);
    lv_subject_set_int(&s_check_gates_disabled, 1);
    // Shown, as on most printers; setup() refreshes them before the first draw.
    lv_subject_set_int(&s_load_hidden, 0);
    lv_subject_set_int(&s_unload_hidden, 0);
    lv_subject_set_int(&s_unload_picks, 0);
    // Mount and Park appear only where a backend says its heads dock.
    lv_subject_set_int(&s_mount_hidden, 1);
    lv_subject_set_int(&s_mount_disabled, 1);
    lv_subject_set_int(&s_park_hidden, 1);
    lv_subject_set_int(&s_park_disabled, 1);
    lv_subject_copy_string(&s_action_reason, "");
    lv_subject_set_int(&s_action_reason_shown, 0);
    // Shown by default, as on most backends; refresh_button_gating() hides it
    // where Reset has nothing to clear.
    lv_subject_set_int(&s_supports_reset, 1);
    lv_subject_set_int(&s_cancel_shown, 0);
}

void init_button_gating_subjects() {
    if (s_gating_subjects_initialized) {
        return;
    }
    for (lv_subject_t* subject :
         {&s_unload_disabled, &s_load_disabled, &s_reset_disabled, &s_check_gates_disabled,
          &s_load_hidden, &s_unload_hidden, &s_unload_picks, &s_mount_hidden, &s_mount_disabled,
          &s_park_hidden, &s_park_disabled, &s_action_reason_shown, &s_supports_reset,
          &s_cancel_shown}) {
        lv_subject_init_int(subject, 0);
    }
    lv_subject_init_string(&s_action_reason, s_action_reason_buf, nullptr,
                           sizeof(s_action_reason_buf), "");
    reset_button_gating_subjects();
    s_subjects.publish("ams_sidebar_unload_disabled", &s_unload_disabled);
    s_subjects.publish("ams_sidebar_load_disabled", &s_load_disabled);
    s_subjects.publish("ams_sidebar_reset_disabled", &s_reset_disabled);
    s_subjects.publish("ams_sidebar_check_gates_disabled", &s_check_gates_disabled);
    s_subjects.publish("ams_sidebar_load_hidden", &s_load_hidden);
    s_subjects.publish("ams_sidebar_unload_hidden", &s_unload_hidden);
    s_subjects.publish("ams_sidebar_unload_picks", &s_unload_picks);
    s_subjects.publish("ams_sidebar_mount_hidden", &s_mount_hidden);
    s_subjects.publish("ams_sidebar_mount_disabled", &s_mount_disabled);
    s_subjects.publish("ams_sidebar_park_hidden", &s_park_hidden);
    s_subjects.publish("ams_sidebar_park_disabled", &s_park_disabled);
    s_subjects.publish("ams_sidebar_action_reason", &s_action_reason);
    s_subjects.publish("ams_sidebar_action_reason_shown", &s_action_reason_shown);
    s_subjects.publish("ams_sidebar_supports_reset", &s_supports_reset);
    s_subjects.publish("ams_sidebar_cancel_shown", &s_cancel_shown);
    s_gating_subjects_initialized = true;

    // The subjects are created once, from the AMS panels' process-once XML
    // registration, so a printer switch must not deinit them: nothing would
    // create them again. The switch resets their values instead.
    StaticSubjectRegistry::instance().register_deinit(
        "AmsSidebarButtonGating",
        []() {
            if (s_gating_subjects_initialized && lv_is_initialized()) {
                s_gating_owner = nullptr;
                s_subjects.deinit_all();
                s_gating_subjects_initialized = false;
                spdlog::trace("[AmsSidebar] Button gating subjects deinitialized");
            }
        },
        StaticSubjectRegistry::Lifetime::Process);
    StaticSubjectRegistry::instance().register_deinit(
        "AmsSidebarButtonGatingReset",
        []() {
            if (s_gating_subjects_initialized && lv_is_initialized()) {
                s_gating_owner = nullptr;
                reset_button_gating_subjects();
            }
        },
        StaticSubjectRegistry::Lifetime::ProcessRunOnSwitch);
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsOperationSidebar::AmsOperationSidebar(PrinterState& ps) : printer_state_(ps) {
    spdlog::debug("[AmsSidebar] Constructed");
}

std::unique_ptr<AmsOperationSidebar> AmsOperationSidebar::attach(PrinterState& ps,
                                                                 lv_obj_t* panel) {
    auto sidebar = std::make_unique<AmsOperationSidebar>(ps);
    sidebar->setup(panel);
    sidebar->init_observers();
    return sidebar;
}

AmsOperationSidebar::~AmsOperationSidebar() {
    cleanup();
    spdlog::debug("[AmsSidebar] Destroyed");
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsOperationSidebar::register_callbacks_static() {
    // Must exist before ams_sidebar.xml is parsed — the Unload buttons bind it. Same
    // "before the parser sees it" contract as the callbacks below, so it is
    // registered from the same hook.
    init_button_gating_subjects();

    register_xml_callbacks({
        {"ams_sidebar_bypass_toggled",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->handle_bypass_toggle();
             }
         }},
        {"ams_sidebar_unload_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->handle_unload();
             }
         }},
        {"ams_sidebar_reset_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->handle_reset();
             }
         }},
        {"ams_sidebar_check_gates_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->handle_check_gates();
             }
         }},
        {"ams_sidebar_settings_clicked", on_settings_clicked_cb},
        {"ams_sidebar_cancel_clicked", [](lv_event_t*) { helix::ui::abort_ams_operation(); }},
        {"ams_sidebar_load_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->handle_load();
             }
         }},
        {"ams_sidebar_mount_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 self->open_head_picker(HeadPickOp::Mount);
             }
         }},
        {"ams_sidebar_park_clicked",
         [](lv_event_t* e) {
             if (auto* self = get_instance_from_event(e)) {
                 helix::ui::request_tool_park(self->printer_state_);
             }
         }},
    });
}

void AmsOperationSidebar::place_summary(bool portrait) {
    if (!loaded_card_ || !status_section_ || !sidebar_root_)
        return;
    lv_obj_t* home = portrait && summary_slot_ ? summary_slot_ : sidebar_root_;
    if (lv_obj_get_parent(loaded_card_) == home)
        return;
    // DECLARATIVE_OK: one strip and status line, placed by orientation; the
    // widgets keep their bindings and the meters wired to them
    lv_obj_set_parent(loaded_card_, home);
    lv_obj_set_parent(status_section_, home);
    lv_obj_move_to_index(loaded_card_, 0);
    lv_obj_move_to_index(status_section_, 1);
}

// ============================================================================
// Static Callback Routing (parent chain traversal)
// ============================================================================

AmsOperationSidebar* AmsOperationSidebar::get_instance_from_event(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Find the ams_sidebar component root by name (or the portrait slot the strip and
    // status line move into), then get our instance from its user_data.
    // Cannot walk parents checking any user_data — ui_button and other widgets set their own
    // user_data, which would be miscast as AmsOperationSidebar* (L069).
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && (strcmp(name, "ams_operation_sidebar") == 0 ||
                     strcmp(name, "portrait_summary_slot") == 0)) {
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

void AmsOperationSidebar::on_settings_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsSidebar] on_settings_clicked");

    spdlog::info("[AmsSidebar] Opening AMS Device Operations overlay");

    auto& overlay = helix::ui::get_ams_device_operations_overlay();
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

    // In portrait the strip and status stand above the diagram, in the panel's
    // summary slot; they follow the orientation live.
    loaded_card_ = lv_obj_find_by_name(sidebar_root_, "current_loaded_card");
    status_section_ = lv_obj_find_by_name(sidebar_root_, "status_section");
    summary_slot_ = lv_obj_find_by_name(panel, "portrait_summary_slot");
    if (summary_slot_)
        lv_obj_set_user_data(summary_slot_, this);
    if (lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait")) {
        portrait_observer_ = observe<int>(
            portrait, this,
            [](AmsOperationSidebar* self, int is_portrait) {
                if (self->active_)
                    self->place_summary(is_portrait != 0);
            },
            subject_never_freed());
    }

    // Setup step progress
    setup_step_progress();

    // Setup clog detection meter (in the loaded strip, wherever it stands)
    lv_obj_t* strip = loaded_card_ ? loaded_card_ : sidebar_root_;
    clog_meter_ = std::make_unique<UiClogMeter>(strip);

    // The loaded card's buffer slider follows the system-level reading
    buffer_slider_ =
        std::make_unique<UiBufferSlider>(lv_obj_find_by_name(strip, "buffer_mini_slider"));
    buffer_slider_->follow_system_reading();

    if (lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait"))
        place_summary(lv_subject_get_int(portrait) != 0);

    active_ = true;

    // Independent stall watchdog for the indeterminate "Working…" state (#1065
    // row 14). Runs on the main loop; the callback no-ops unless an op is active.
    if (!stall_watchdog_timer_) {
        stall_watchdog_timer_ = lv_timer_create(stall_watchdog_cb, kStallWatchdogPeriodMs, this);
    }

    sync_reset_button_label();
    update_check_gates_visibility();
    refresh_button_gating();
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

    // The container outlives a sidebar (the AMS panel keeps its tree between
    // opens), so a stepper an earlier sidebar created is still in it.
    helix::ui::safe_clean_children(step_progress_container_);

    // Create initial step progress widget (fresh load by default)
    recreate_step_progress_for_operation(StepOperationType::LOAD_FRESH);

    spdlog::debug("[AmsSidebar] Step progress widget created");
}

// ============================================================================
// Observers
// ============================================================================

void AmsOperationSidebar::init_observers() {
    // Action observer: drives step progress and load completion detection
    action_observer_ = observe<int>(
        AmsState::instance().get_ams_action_subject(), this,
        [](AmsOperationSidebar* self, int action_int) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            auto action = static_cast<AmsAction>(action_int);
            spdlog::debug("[AmsSidebar] Action changed: {} (prev={})", ams_action_to_string(action),
                          ams_action_to_string(self->prev_ams_action_));

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

            // No bypass feed here: the shared BypassToggleController wakes
            // itself while a pending unload→enable chain runs, so this sidebar
            // instance feeding its own controller would settle the chain twice.

            // Update step progress (BEFORE updating prev_ams_action_)
            self->update_action_display(action);

            // AmsSystemInfo::is_busy() is "action is neither IDLE nor ERROR", so
            // every edge here changes the Unload button's gating.
            self->refresh_button_gating();

            self->prev_ams_action_ = action;
        },
        AmsState::instance().get_subjects_lifetime());

    // Current slot observer: updates loaded card display and reset button label
    current_slot_observer_ = observe<int>(
        AmsState::instance().get_current_slot_subject(), this,
        [](AmsOperationSidebar* self, int /*slot_index*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->update_current_loaded_display();
            self->sync_reset_button_label();
            self->update_check_gates_visibility();
            self->refresh_button_gating();
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
    filament_loaded_observer_ = observe<int>(
        AmsState::instance().get_filament_loaded_subject(), this,
        [](AmsOperationSidebar* self, int) { self->refresh_button_gating(); },
        AmsState::instance().get_subjects_lifetime());
    // A backend can stop being cancellable mid-operation (a Cancel it holds for
    // a swap), with no action edge to say so.
    can_cancel_observer_ = observe<int>(
        AmsState::instance().get_ams_can_cancel_subject(), this,
        [](AmsOperationSidebar* self, int) { self->refresh_button_gating(); },
        AmsState::instance().get_subjects_lifetime());
    print_state_observer_ = observe<int>(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [](AmsOperationSidebar* self, int) { self->refresh_button_gating(); },
        printer_state_.print_state().get_static_subjects_lifetime());

    // Active backend observer: re-syncs reset button label when the user switches backend tabs
    active_backend_observer_ = observe<int>(
        AmsState::instance().get_active_backend_subject(), this,
        [](AmsOperationSidebar* self, int /*active_index*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->sync_reset_button_label();
            self->update_check_gates_visibility();
            // reset_moves_filament() is a per-backend answer, so switching tabs
            // changes Reset's gating even with the job state unmoved.
            self->refresh_button_gating();
        },
        AmsState::instance().get_subjects_lifetime());

    // Bypass spool color observer: refreshes loaded card when external spool changes
    bypass_spool_observer_ = observe<int>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](AmsOperationSidebar* self, int /*color_rgb*/) {
            if (!self->active_ || !self->sidebar_root_)
                return;
            self->update_current_loaded_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // Paint observers: the loaded filament's colour, multi-colour list and
    // finish on the card's bar (below medium) and its spool (medium up); XML
    // picks which one shows.
    color_observer_ = observe<int>(
        AmsState::instance().get_current_color_subject(), this,
        [](AmsOperationSidebar* self, int) { self->paint_loaded_filament(); },
        AmsState::instance().get_subjects_lifetime());
    colors_observer_ = observe<const char*>(
        AmsState::instance().get_current_colors_subject(), this,
        [](AmsOperationSidebar* self, const char*) { self->paint_loaded_filament(); },
        AmsState::instance().get_subjects_lifetime());
    finish_observer_ = observe<const char*>(
        AmsState::instance().get_current_finish_subject(), this,
        [](AmsOperationSidebar* self, const char*) { self->paint_loaded_filament(); },
        AmsState::instance().get_subjects_lifetime());

    // Fill observer: how much is left on the loaded spool, in the
    // SlotInfo::display_fill_pct() encoding. -1 (no weight data) draws a full
    // spool rather than leaving the render as it is: this one spool shows
    // whatever is loaded now, so an untouched render would keep the previous
    // load's fill.
    fill_observer_ = observe<int>(
        AmsState::instance().get_current_fill_pct_subject(), this,
        [](AmsOperationSidebar* self, int pct) {
            if (!self->active_ || !self->sidebar_root_ || !self->loaded_card_)
                return;
            if (lv_obj_t* spool = lv_obj_find_by_name(self->loaded_card_, "loaded_spool")) {
                ui_spool_canvas_set_fill_level(spool, pct < 0 ? 1.0f : pct / 100.0f);
            }
        },
        AmsState::instance().get_subjects_lifetime());

    // Extruder temp observer: refreshes the heat step
    extruder_temp_observer_ = observe<int>(
        printer_state_.temperature_state().get_active_extruder_temp_subject(), this,
        [](AmsOperationSidebar* self, int /*temp_deci*/) {
            if (!self->active_)
                return;
            self->refresh_heat_step_display();
        },
        printer_state_.get_subjects_lifetime());

    // Extruder target observer: refreshes heat step when target temp changes
    // (the macro raises the target before any visible action change)
    extruder_target_observer_ = observe<int>(
        printer_state_.temperature_state().get_active_extruder_target_subject(), this,
        [](AmsOperationSidebar* self, int /*target_deci*/) {
            if (!self->active_)
                return;
            self->refresh_heat_step_display();
        },
        printer_state_.get_subjects_lifetime());

    // Indeterminate "Working…" observer: when the backend flags a stalled
    // progress feed (frozen live-temp number), re-render the Heat step so it
    // swaps between the live temp readout and the busy "Working…" label
    // (#1065 row 14). AmsState frees the subject in deinit_subjects(), so the
    // observer carries its subjects lifetime.
    indeterminate_observer_ = observe<int>(
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
    portrait_observer_.reset();
    if (summary_slot_)
        lv_obj_set_user_data(summary_slot_, nullptr);
    sidebar_root_ = nullptr;
    loaded_card_ = nullptr;
    status_section_ = nullptr;
    summary_slot_ = nullptr;
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
    colors_observer_.reset();
    finish_observer_.reset();
    fill_observer_.reset();
    step_index_observer_.reset(); // [L085] reset(), never release()
    extruder_temp_observer_.reset();
    extruder_target_observer_.reset();
    step_heater_observers_.clear();
    step_heater_version_observer_.reset();
    indeterminate_observer_.reset();
    filament_loaded_observer_.reset();
    can_cancel_observer_.reset();
    print_state_observer_.reset();

    // Reset extracted modules AFTER observers — they may have their own observers
    // that reference widget pointers; resetting before our observers could
    // trigger callbacks on already-null widget pointers.
    clog_meter_.reset();
    buffer_slider_.reset();

    if (s_gating_owner == this) {
        s_gating_owner = nullptr;
        if (s_gating_subjects_initialized) {
            reset_button_gating_subjects();
        }
    }

    // A preheat still waiting outlives this sidebar: FilamentOpHeater runs the
    // op once the nozzle is hot. Only the stepper's held action goes with us.
    bypass_toggle_.cancel_pending();
    if (pending_load_slot_ >= 0) {
        AmsState::instance().release_optimistic_action();
        AmsState::instance().sync_from_backend();
    }
    pending_load_slot_ = -1;
    pending_load_target_temp_ = 0;
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
        show_preheat_feedback(pending_load_current_temp_, pending_load_target_temp_);
    }

    // Sync loaded card display
    update_current_loaded_display();

    update_check_gates_visibility();
    sync_reset_button_label();
    refresh_button_gating();
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

void AmsOperationSidebar::paint_loaded_filament() {
    if (!active_ || !sidebar_root_ || !loaded_card_)
        return;
    const auto paint = AmsState::instance().current_paint();
    const auto finish = helix::ui::swatch_finish(paint.finish);
    if (lv_obj_t* swatch = lv_obj_find_by_name(loaded_card_, "loaded_swatch")) {
        helix::ui::apply_swatch_color(swatch, paint.rgb, paint.multi_color_hexes, finish);
    }
    if (lv_obj_t* spool = lv_obj_find_by_name(loaded_card_, "loaded_spool")) {
        helix::ui::spool_canvas_set_filament(spool, paint.rgb, paint.multi_color_hexes, finish);
    }
}

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
    // Cleared even when no bar is built: start_operation() reads the first step
    // of this model, and a previous operation's would be wrong.
    current_step_model_.steps.clear();
    if (!active_ || !step_progress_container_) {
        return;
    }

    // Delete existing step progress widget if any
    safe_delete_obj(step_progress_);
    heat_label_showing_temp_ = false; // fresh widget has plain "Heat nozzle" label

    current_operation_type_ = op_type;

    // A row where the sidebar has no height for a list (a narrow portrait panel).
    lv_subject_t* portrait_subject = lv_xml_get_subject(nullptr, "ui_is_portrait");
    const bool horizontal = helix::ui::ams_stepper_horizontal(
        portrait_subject && lv_subject_get_int(portrait_subject) != 0,
        helix::widget_size::current_breakpoint());

    // Reset the backend-driven step driver; re-established below if the backend
    // supplies a specialized step model. Drop the old observer first.
    step_index_observer_.reset();
    step_index_subject_ = nullptr;
    live_temp_step_index_ = -1;
    step_heater_observers_.clear();
    step_heater_version_observer_.reset();

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
        // A backend that reports no phases AND says the legacy bar would lie
        // gets no bar. Distinct from an empty model, which still means "fall
        // back". A tool changer has no Heat/Feed/Purge to show: it mounts a
        // toolhead, and the status line above already names what it is doing.
        if (current_step_model_.suppressed) {
            current_step_count_ = 0;
            if (step_progress_container_) {
                lv_obj_add_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
            }
            spdlog::debug("[AmsSidebar] Backend suppressed the step bar for op_type={}",
                          static_cast<int>(op_type));
            return;
        }
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
                                        horizontal, "ams_step_progress");
            if (step_progress_ && horizontal)
                ui_step_progress_set_current_label_only(step_progress_, true);
            if (step_progress_) {
                // Observe the backend-supplied current-step subject. Every
                // backend shipping today routes it through AmsState (narration
                // toolchange_step, or the firmware operation_phase the U1, the
                // AD5X IFS and the tool changer all mirror), and those die
                // together in AmsState::deinit_subjects() — so an observer on
                // one must carry that death signal or its reset() removes a
                // freed node. A backend that owns its own subject outlives that
                // teardown and must NOT get the token: suppressing a removal it
                // still needs orphans a live observer node.
                step_index_subject_ = backend->get_operation_step_index_subject(op_type);
                if (step_index_subject_) {
                    auto& ams = AmsState::instance();
                    SubjectLifetime step_lifetime;
                    if (step_index_subject_ == ams.get_toolchange_step_subject() ||
                        step_index_subject_ == ams.get_ams_operation_phase_subject()) {
                        step_lifetime = ams.get_subjects_lifetime();
                    }
                    // For narration-driven bars with no phase yet, seed the shared
                    // step subject to step 0 (heat) BEFORE observing, so the initial
                    // observer fire highlights heat and the bar never looks dead if
                    // the matcher misses the opening `//` line (#1046). A phase
                    // already narrated for this operation is kept: an externally
                    // started operation narrates before its action reaches us, and
                    // set_active_step_operation() above has already re-resolved or
                    // dropped any index from another template.
                    // Firmware-phase subjects (e.g. Snapmaker) mirror live hardware
                    // and MUST NOT be seeded — gate on the shared narration subject.
                    if (step_index_subject_ == ams.get_toolchange_step_subject() &&
                        lv_subject_get_int(step_index_subject_) < 0) {
                        lv_subject_set_int(step_index_subject_, 0);
                    }
                    step_index_observer_ = observe<int>(
                        step_index_subject_, this,
                        [](AmsOperationSidebar* self, int index) {
                            if (!self->active_ || !self->step_progress_)
                                return;
                            self->apply_backend_step_index(index);
                        },
                        step_lifetime);
                }
                watch_step_heaters();
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
    if (const auto info = backend ? AmsState::instance().system_info() : nullptr) {
        tip_method = info->tip_method;
        supports_purge = info->supports_purge;
    }
    const char* tip_step_label = lv_tr(tip_method_step_label(tip_method));

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
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 3, horizontal,
                                                     "ams_step_progress");
        } else {
            ui_step_t steps[] = {
                {lv_tr("Heat nozzle"), StepState::Pending},
                {lv_tr("Feed filament"), StepState::Pending},
            };
            current_step_count_ = 2;
            step_progress_ = ui_step_progress_create(step_progress_container_, steps, 2, horizontal,
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
        step_progress_ = ui_step_progress_create(step_progress_container_, steps, n, horizontal,
                                                 "ams_step_progress");
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
        step_progress_ = ui_step_progress_create(step_progress_container_, steps, n, horizontal,
                                                 "ams_step_progress");
        break;
    }
    }

    if (!step_progress_) {
        spdlog::error("[AmsSidebar] Failed to create step progress for op_type={}",
                      static_cast<int>(op_type));
    } else {
        // A row has no width for a label under every step: name the current one.
        if (horizontal)
            ui_step_progress_set_current_label_only(step_progress_, true);
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
    // AmsState publishes the action before the phase, so the action read here
    // is the one this step arrived with.
    if (const auto projected = current_step_model_.action_at(index)) {
        const auto assigned = static_cast<AmsAction>(
            lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
        if (*projected != assigned) {
            spdlog::debug("[AmsSidebar] Step {} projects {} but the backend assigned {}", index,
                          ams_action_to_string(*projected), ams_action_to_string(assigned));
        }
    }
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
    // A run of several tools heats once per tool, so the model can carry several
    // live-temp steps: the readout follows the one under way.
    const auto& steps = current_step_model_.steps;
    if (current_index != live_temp_step_index_ && current_index >= 0 &&
        current_index < static_cast<int>(steps.size()) && steps[current_index].live_temp) {
        if (heat_label_showing_temp_) {
            ui_step_progress_set_label(step_progress_, live_temp_step_index_,
                                       lv_tr(steps[live_temp_step_index_].label.c_str()));
            heat_label_showing_temp_ = false;
        }
        live_temp_step_index_ = current_index;
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
            // The heater this step heats, which on a machine with one per
            // tool need not be the active extruder.
            auto& temps = printer_state_.temperature_state();
            lv_subject_t* temp_subject = temps.get_active_extruder_temp_subject();
            lv_subject_t* target_subject = temps.get_active_extruder_target_subject();
            AmsBackend* backend = AmsState::instance().get_backend();
            const std::string heater =
                backend ? backend->step_heater(live_temp_step_index_) : std::string();
            if (!heater.empty()) {
                lv_subject_t* own_temp = temps.get_extruder_temp_subject(heater);
                lv_subject_t* own_target = temps.get_extruder_target_subject(heater);
                if (own_temp && own_target) {
                    temp_subject = own_temp;
                    target_subject = own_target;
                }
            }
            int current_deci = lv_subject_get_int(temp_subject);
            int target_deci = lv_subject_get_int(target_subject);
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

void AmsOperationSidebar::observe_step_heaters() {
    step_heater_observers_.clear();
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    auto& temps = printer_state_.temperature_state();
    std::vector<std::string> watched;
    for (size_t i = 0; i < current_step_model_.steps.size(); ++i) {
        if (!current_step_model_.steps[i].live_temp) {
            continue;
        }
        const std::string heater = backend->step_heater(static_cast<int>(i));
        if (heater.empty() || std::find(watched.begin(), watched.end(), heater) != watched.end()) {
            continue;
        }
        watched.push_back(heater);
        for (const bool target : {false, true}) {
            SubjectLifetime lifetime;
            lv_subject_t* subject = target ? temps.get_extruder_target_subject(heater, lifetime)
                                           : temps.get_extruder_temp_subject(heater, lifetime);
            if (!subject) {
                continue;
            }
            step_heater_observers_.push_back(observe<int>(
                subject, this,
                [](AmsOperationSidebar* self, int /*deci*/) {
                    if (!self->active_)
                        return;
                    self->refresh_heat_step_display();
                },
                lifetime));
        }
    }
}

void AmsOperationSidebar::watch_step_heaters() {
    observe_step_heaters();
    // Armed once per step bar: observe_step_heaters() never re-arms it, so its
    // own notify on attach cannot loop.
    auto& temps = printer_state_.temperature_state();
    step_heater_version_observer_ = observe<int>(
        temps.get_extruder_version_subject(), this,
        [](AmsOperationSidebar* self, int /*version*/) {
            if (!self->active_)
                return;
            self->observe_step_heaters();
            self->refresh_heat_step_display();
        },
        temps.get_subjects_lifetime());
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

    // Show the container BEFORE building the step widget so the create-time
    // layout pass runs against a visible (non-collapsed) container. The step
    // connectors also relayout on SIZE_CHANGED, but revealing first keeps the
    // very first paint correct.
    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }

    // An operation this UI starts begins now: no narration from before it
    // belongs to it.
    AmsState::instance().set_narration_phase(-1, "");

    // Create step progress with correct steps
    recreate_step_progress_for_operation(op_type);

    // Mark the operation busy right away (the XML binding hides the buttons on
    // it) with the action of the step it starts on. A backend step model says
    // which; the legacy bar's first step is always Heat.
    AmsState::instance().set_action(current_step_model_.action_at(0).value_or(AmsAction::HEATING));
    // A backend that reports IDLE until its firmware starts gets the UI's
    // action standing in for that silence instead of reading as Idle
    // (prestonbrown/helixscreen#1057).
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->holds_optimistic_action()) {
        AmsState::instance().hold_optimistic_action(kOptimisticPreheatHold);
    }
}

void AmsOperationSidebar::fail_started_operation(const AmsError& error) {
    spdlog::warn("[AmsSidebar] Operation dispatch failed: {} ({})", error.user_msg,
                 error.technical_msg);
    helix::ui::notify_ams_error(error, lv_tr("Filament operation failed"));
    abandon_started_operation();
}

void AmsOperationSidebar::abandon_started_operation() {
    target_load_slot_ = -1;
    release_operation();
}

void AmsOperationSidebar::update_step_progress(AmsAction action) {
    if (!active_ || !step_progress_container_) {
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();

    // The one question both halves of this function ask: is this action an
    // operation the bar should be following? The backend owns the answer,
    // because the action vocabulary differs by backend family - a filament
    // system heats and feeds, a tool changer selects.
    const bool is_active_action = backend ? backend->action_tracks_step_operation(action)
                                          : ams_action_is_filament_operation(action);

    // Heuristic detection for externally-started operations
    bool is_external = (target_load_slot_ < 0);
    bool filament_loaded = false;
    if (const auto info = is_external && backend ? AmsState::instance().system_info() : nullptr) {
        filament_loaded = (info->current_slot >= 0);
    }

    const bool unloads_stand_alone = backend && backend->unloads_stand_alone();

    // An operation this sidebar did not start is named by the backend when it
    // knows it: read from the action frames, a load still heating looks like a
    // swap.
    const std::optional<StepOperationType> known_op = is_external && is_active_action && backend
                                                          ? backend->running_step_operation()
                                                          : std::nullopt;
    if (known_op) {
        if (!step_progress_ || current_operation_type_ != *known_op) {
            recreate_step_progress_for_operation(*known_op);
        }
    } else if (auto detection = detect_step_operation(
                   action, prev_ams_action_, current_operation_type_, is_external, filament_loaded,
                   is_active_action, unloads_stand_alone);
               detection.should_recreate) {
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

    // Same question as the detection above, same answer. These were two separate
    // hardcoded lists that disagreed about both SELECTING and PURGING, so a tool
    // changer's bar appeared for the dock half of a swap and vanished for the
    // pick half.
    if (is_active_action) {
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
        int current_deci = lv_subject_get_int(
            printer_state_.temperature_state().get_active_extruder_temp_subject());
        int target_deci = lv_subject_get_int(
            printer_state_.temperature_state().get_active_extruder_target_subject());
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
    int target_deci =
        lv_subject_get_int(printer_state_.temperature_state().get_active_extruder_target_subject());
    if (target_deci <= 0) {
        return false;
    }
    int current_deci =
        lv_subject_get_int(printer_state_.temperature_state().get_active_extruder_temp_subject());
    return current_deci < (target_deci - helix::ui::kPreheatMarginC * 10);
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

AmsOperationSidebar::OpInputs AmsOperationSidebar::read_op_inputs() const {
    AmsBackend* backend = AmsState::instance().get_backend();
    const auto info = AmsState::instance().system_info();
    // AmsSystemInfo::is_busy(): the same predicate check_preconditions()
    // refuses on, instead of a fourth open-coded `action != IDLE && != ERROR`.
    return {/*system_busy=*/info && info->is_busy(),
            printer_state_.print_state().get_print_lifecycle(),
            /*backend_self_homes=*/backend && backend->filament_ops_self_home()};
}

helix::ui::FilamentActionSet AmsOperationSidebar::read_filament_actions() const {
    // AmsState's shared reading: every observer a sync wakes reads the same one.
    const auto info = AmsState::instance().system_info();
    return helix::ui::decide_filament_actions(helix::ui::read_filament_head_view(
        AmsState::instance().get_backend(), printer_state_, info.get()));
}

helix::ui::MachineOpGating AmsOperationSidebar::read_machine_op_gating() const {
    AmsBackend* backend = AmsState::instance().get_backend();
    return helix::ui::compute_machine_op_gating(
        helix::ui::print_blocks_filament_op(printer_state_.print_state().get_print_lifecycle(),
                                            backend && backend->filament_ops_self_home()),
        /*reset_moves_filament=*/backend && backend->reset_moves_filament());
}

void AmsOperationSidebar::refresh_button_gating() {
    if (!s_gating_subjects_initialized) {
        return;
    }
    s_gating_owner = this;
    const helix::ui::FilamentActionSet actions = read_filament_actions();
    lv_subject_set_int(&s_load_hidden, actions.load.visible ? 0 : 1);
    lv_subject_set_int(&s_load_disabled, actions.load.enabled ? 0 : 1);
    lv_subject_set_int(&s_unload_hidden, actions.unload.visible ? 0 : 1);
    lv_subject_set_int(&s_unload_disabled, actions.unload.enabled ? 0 : 1);
    lv_subject_set_int(&s_unload_picks, actions.unload_opens_picker ? 1 : 0);
    lv_subject_set_int(&s_mount_hidden, actions.mount.visible ? 0 : 1);
    lv_subject_set_int(&s_mount_disabled, actions.mount.enabled ? 0 : 1);
    lv_subject_set_int(&s_park_hidden, actions.park.visible ? 0 : 1);
    lv_subject_set_int(&s_park_disabled, actions.park.enabled ? 0 : 1);

    // One line under the grid says why an action is unavailable; the first
    // reason wins, Load before Unload.
    const std::string& reason =
        !actions.load.reason.empty() ? actions.load.reason : actions.unload.reason;
    if (reason != lv_subject_get_string(&s_action_reason)) {
        lv_subject_copy_string(&s_action_reason, reason.c_str());
    }
    lv_subject_set_int(&s_action_reason_shown, reason.empty() ? 0 : 1);

    const auto machine = read_machine_op_gating();
    lv_subject_set_int(&s_reset_disabled, machine.reset_disabled ? 1 : 0);
    lv_subject_set_int(&s_check_gates_disabled, machine.check_gates_disabled ? 1 : 0);

    AmsBackend* backend = AmsState::instance().get_backend();
    lv_subject_set_int(&s_supports_reset, backend && backend->supports_reset() ? 1 : 0);

    // Cancel stands in for the action buttons while an operation runs that the
    // backend can stop: the sidebar is where the operation is watched.
    const auto action =
        static_cast<AmsAction>(lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
    lv_subject_set_int(
        &s_cancel_shown,
        backend && ams_action_is_busy(action) && backend->can_cancel_operation() ? 1 : 0);
}

bool AmsOperationSidebar::refuse_if_busy_or_printing() const {
    // The reads behind one shared refusal: an op already running, or a print
    // owning the toolhead. The one-tap Unload and both picker directions stop
    // on the same two conditions; the messages name unloading.
    const OpInputs in = read_op_inputs();
    if (in.system_busy) {
        spdlog::info("[AmsSidebar] Filament op refused: an AMS operation is already running");
        NOTIFY_WARNING(lv_tr("Wait for the current filament operation to finish"));
        return true;
    }
    if (helix::ui::print_blocks_filament_op(in.lifecycle, in.backend_self_homes)) {
        const bool self_homes = in.backend_self_homes;
        spdlog::info("[AmsSidebar] Filament op refused: a print owns the toolhead (self_homes={})",
                     self_homes);
        if (self_homes) {
            // AD5X IFS: the unload macro homes itself, so pausing does not help
            // - recommending it would send the user into the exact loadcell-Z
            // collision the backend guard exists to prevent.
            NOTIFY_WARNING(lv_tr("Cannot unload while a print is active"));
        } else {
            // PRINTING on every other backend. A PAUSED print here would not
            // have reached this branch at all, so pausing IS the recovery.
            NOTIFY_WARNING(lv_tr("Pause the print first, then load, unload, or change filament"));
        }
        return true;
    }
    return false;
}

void AmsOperationSidebar::handle_load() {
    // The picker opens on every printer: it names the slot the load will feed
    // (each head's Assigned Tool slot), which on a one-head printer is the
    // whole question. A load of any other slot is a slot tap.
    if (refuse_if_busy_or_printing()) {
        return;
    }
    open_head_picker(HeadPickOp::Load);
}

void AmsOperationSidebar::open_head_picker(HeadPickOp op) {
    auto token = lifetime_.token();
    spdlog::info("[AmsSidebar] Opening head picker ({})",
                 op == HeadPickOp::Load ? "load" : (op == HeadPickOp::Unload ? "unload" : "mount"));
    BatchFilamentModal::show_owned(op, [this, token, op](const std::vector<HeadPickRow>& chosen) {
        if (token.expired()) {
            return;
        }
        run_picked(op, chosen);
    });
}

void AmsOperationSidebar::run_picked(HeadPickOp op, const std::vector<HeadPickRow>& chosen) {
    if (chosen.empty()) {
        return;
    }
    // A mount answers to the tool-change verdict, which confirms a paused job's
    // move instead of refusing it.
    if (op == HeadPickOp::Mount) {
        helix::ui::offer_tool_change(printer_state_, chosen.front().head);
        return;
    }
    if (refuse_if_busy_or_printing()) {
        return;
    }
    // One head takes the per-slot path, with its preheat and stepper; several
    // run as the backend's one batch.
    if (chosen.size() == 1) {
        const int slot = chosen.front().slot;
        if (op == HeadPickOp::Load) {
            handle_load_with_preheat(slot);
        } else if (op == HeadPickOp::Unload) {
            handle_unload(slot);
        }
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("Multi-Filament System not available"));
        return;
    }
    std::vector<int> slots;
    slots.reserve(chosen.size());
    for (const HeadPickRow& row : chosen) {
        slots.push_back(row.slot);
    }
    // The bar is told which operation this is: guessed from the first frame,
    // an unload of a loaded head reads as a swap and an empty one as a load.
    const StepOperationType op_type =
        op == HeadPickOp::Load ? StepOperationType::LOAD_FRESH : StepOperationType::UNLOAD;
    start_operation(op_type, slots.front());
    if (!BatchFilamentModal::dispatch_batch(*backend, slots, op == HeadPickOp::Load)) {
        abandon_started_operation();
        return;
    }
    // Rebuilt now the batch is under way: a backend sizes its step model from
    // the run it holds, which did not exist when start_operation() built it.
    recreate_step_progress_for_operation(op_type);
    hand_operation_to_backend();
}

void AmsOperationSidebar::handle_unload() {
    // With several heads this button opens the head picker in the Unload
    // direction, the same modal the Load button opens. The busy/print refusal
    // runs first: the picker must not open over a running op.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (read_filament_actions().unload_opens_picker) {
        if (refuse_if_busy_or_printing()) {
            return;
        }
        open_head_picker(HeadPickOp::Unload);
        return;
    }
    if (helix::ui::notify_if_filament_op_unavailable(backend, /*slot=*/-1, /*load=*/false)) {
        return;
    }

    // A backend that cannot name the lane in the toolhead asks for it first,
    // and the chosen lane takes the slot overload below.
    if (refuse_if_busy_or_printing()) {
        return;
    }
    auto token = lifetime_.token();
    if (helix::ui::choose_unload_lane(backend, lv_obj_find_by_name(sidebar_root_, "btn_unload"),
                                      [this, token](int slot) {
                                          if (token.expired()) {
                                              return;
                                          }
                                          handle_unload(slot, /*lane_in_toolhead=*/true);
                                      })) {
        return;
    }

    // Active-slot unload (per-toolhead backends). Delegate to the slot overload
    // with slot_index = -1 so the stepper-build + backend-call path lives in one
    // place.
    handle_unload(-1);
}

void AmsOperationSidebar::handle_unload(int slot_index, bool lane_in_toolhead) {
    spdlog::info("[AmsSidebar] Unload requested (slot={})", slot_index);
    if (helix::ui::notify_if_filament_op_unavailable(AmsState::instance().get_backend(), slot_index,
                                                     /*load=*/false)) {
        return;
    }

    // The button is bound to ams_sidebar_unload_disabled, but a tap can still
    // land in the window between a print starting and the subject settling, and
    // handle_unload(slot) is also the context menu's dispatch entry. Refuse
    // here rather than forwarding a guaranteed backend rejection, which the
    // backend raises for a paused print as well as a printing one.
    if (refuse_if_busy_or_printing()) {
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
    const bool loaded =
        lane_in_toolhead ||
        helix::ui::read_unload_target_loaded(AmsState::instance().get_backend(), info, target_slot);

    const helix::ui::FilamentOpPlan plan = helix::ui::plan_live_unload(caps, target_slot, loaded);

    if (plan.tier == helix::ui::FilamentTier::Refused) {
        // NothingLoaded is plan_unload's only refusal.
        spdlog::info("[AmsSidebar] Unload refused — nothing loaded (slot={})", target_slot);
        NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (plan.tier != helix::ui::FilamentTier::AmsBackend) {
        helix::ui::execute_filament_unload(backend, target_slot, loaded,
                                           op_surface("[AmsSidebar]", target_slot));
        return;
    }

    // Asked before the stepper is armed: a refusal leaves it to the op heating.
    if (auto* heater = get_filament_op_heater();
        heater && heater->refuse_while_heating(FilamentHeatOp::Unload, this, "[AmsSidebar]")) {
        return;
    }

    // Build the UNLOAD stepper first (HEATING + correct step list).
    if (target_slot >= 0) {
        start_operation(StepOperationType::UNLOAD, target_slot);
    }
    run_backend_op_after_preheat(
        FilamentHeatOp::Unload,
        helix::ui::preheat_skip_reason(plan, StandardMacroSlot::UnloadFilament, backend),
        target_slot, [slot_index, target_slot](FilamentOpTicket ticket) {
            dispatch_backend_unload(slot_index, target_slot, ticket);
        });
}

void AmsOperationSidebar::handle_reset() {
    spdlog::info("[AmsSidebar] Reset requested");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    // The button is bound to ams_sidebar_reset_disabled, but a tap can still
    // land in the window between a print starting and the subject settling. No
    // backend's reset() asks check_preconditions() for the print term, so
    // nothing downstream would refuse this.
    if (read_machine_op_gating().reset_disabled) {
        spdlog::info("[AmsSidebar] Reset refused — a print owns the toolhead and this "
                     "backend's reset moves filament");
        NOTIFY_WARNING(lv_tr("Pause the print first, then load, unload, or change filament"));
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

    // check_all_gates() checks only that the backend is running, so this is the
    // only place a mid-print tap can be stopped. Probing a gate parks the
    // toolhead and unloads/reloads each lane to test its sensor.
    if (read_machine_op_gating().check_gates_disabled) {
        spdlog::info("[AmsSidebar] Check slots refused — a print owns the toolhead");
        NOTIFY_WARNING(lv_tr("Pause the print first, then load, unload, or change filament"));
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

std::optional<int> AmsOperationSidebar::material_load_temp_for_slot(int slot_index) {
    // The rule FilamentOpHeater heats by, so the macro is offered the
    // temperature the preheat reaches.
    if (const auto material = preheat_material_for_slot(slot_index)) {
        return material->temp_c;
    }
    return std::nullopt;
}

std::map<std::string, std::string>
AmsOperationSidebar::macro_temp_prefill(helix::ui::FilamentMacroOp op, int slot_index) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        // No printer connection: no extrusion minimum or hotend maximum to hold a
        // temperature between.
        spdlog::debug("[AmsSidebar] No API — no temperature prefilled for slot {}", slot_index);
        return {};
    }
    const helix::ui::OpNozzle nozzle = helix::ui::resolve_op_nozzle(
        AmsState::instance().get_backend(), slot_index, printer_state_, api->get_safety_limits());
    return helix::ui::nozzle_temp_prefill(op, nozzle.target_c,
                                          material_load_temp_for_slot(slot_index), nozzle.floor_c,
                                          nozzle.ceiling_c);
}

void AmsOperationSidebar::handle_load_with_preheat(int slot_index) {
    if (helix::ui::notify_if_filament_op_unavailable(AmsState::instance().get_backend(), slot_index,
                                                     /*load=*/true)) {
        return;
    }
    // The three-tier routing (AMS backend → configured macro → raw gcode) and
    // the load-vs-swap rule both come from plan_load(), the one answer every
    // dispatch surface shares, including the already-mounted refusal.
    AmsSystemInfo info;
    const helix::ui::BackendCaps caps = read_backend_caps(info, slot_index);

    const helix::ui::FilamentOpPlan plan = helix::ui::plan_live_load(info, caps, slot_index);

    if (plan.tier == helix::ui::FilamentTier::Refused) {
        // Mostly silent on THIS surface. The AMS panel already highlights the
        // mounted slot and greys the ones that cannot be picked, so a toast here
        // would narrate what the grid is showing. The Filament panel, where the
        // user pressed a button with no other feedback, does toast.
        //
        // BypassLoaded is the exception: nothing in the grid shows that the
        // bypass spool is still threaded, so staying silent here reproduces the
        // exact "I tap Load and nothing happens" this refusal exists to end.
        if (plan.refusal == helix::ui::FilamentRefusal::BypassLoaded) {
            spdlog::info(
                "[AmsSidebar] Load of slot {} refused — bypass spool still at the toolhead",
                slot_index);
            NOTIFY_INFO(lv_tr("Remove the bypass spool from the toolhead first"));
            return;
        }
        spdlog::debug("[AmsSidebar] Load of slot {} refused (no slot selected)", slot_index);
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();

    if (plan.tier != helix::ui::FilamentTier::AmsBackend) {
        // Macro and raw gcode are the shared ladder's, verbatim — this surface
        // adds only the lifetime guard and the parameter policy.
        helix::ui::execute_filament_load(backend, slot_index,
                                         op_surface("[AmsSidebar]", slot_index));
        return;
    }

    // The stepper is armed now, before any preheat. plan.ams_call carries the
    // load-vs-swap answer (needs_unload_before_load, centralized so the UI and
    // backend agree — K1 CFS reports a preloaded cassette slot with an empty
    // nozzle and a SWAP there would cut nothing and stall at the cut step, #968).
    // Asked before the stepper is armed: a refusal leaves it to the op heating.
    if (auto* heater = get_filament_op_heater();
        heater && heater->refuse_while_heating(FilamentHeatOp::Load, this, "[AmsSidebar]")) {
        return;
    }
    start_operation(plan.ams_call == helix::ui::AmsCall::Load ? StepOperationType::LOAD_FRESH
                                                              : StepOperationType::LOAD_SWAP,
                    slot_index);

    // The backend's load homes inside its own ensure_homed_then() once the
    // nozzle is up to temperature.
    run_backend_op_after_preheat(
        FilamentHeatOp::Load,
        helix::ui::preheat_skip_reason(plan, StandardMacroSlot::LoadFilament, backend), slot_index,
        [slot_index](FilamentOpTicket ticket) { dispatch_backend_load(slot_index, ticket); });
}

void AmsOperationSidebar::run_backend_op_after_preheat(
    FilamentHeatOp op, PreheatSkip skip, int slot_index,
    helix::SharedCallback<void(FilamentOpTicket)> run) {
    FilamentOpHeater* heater = get_filament_op_heater();
    if (!heater) {
        spdlog::warn("[AmsSidebar] No FilamentOpHeater — running {} with no preheat",
                     filament_heat_op_name(op));
        run(0);
        return;
    }
    PreheatRequest req;
    req.op = op;
    req.log_tag = "[AmsSidebar]";
    req.skip = skip;
    req.slot = slot_index;
    req.owner = this;
    req.announce = false; // the Heat step shows it
    req.run = std::move(run);
    // The op this request is for owns the stepper until a later one arms it, so
    // a superseded request's end leaves the stepper alone.
    const uint64_t mine = ++stepper_owner_;
    auto token = lifetime_.token();
    req.on_progress = [this, token, slot_index, mine](const PreheatProgress& progress) {
        if (token.expired()) {
            return; // main thread: the sidebar closed, the heater carries on
        }
        switch (progress.phase) {
        case PreheatPhase::Heating:
            // The reading is the heated extruder's, which need not be the active one.
            pending_load_current_temp_ = progress.current_c;
            if (pending_load_slot_ < 0) {
                pending_load_slot_ = slot_index;
                pending_load_target_temp_ = progress.target_c;
                show_preheat_feedback(progress.current_c, progress.target_c);
            } else {
                // Only the reading moves; the Heat step is already showing.
                AmsState::instance().set_action_detail(preheat_status_text(
                    step_progress_ != nullptr, progress.current_c, progress.target_c));
            }
            return;
        case PreheatPhase::Reached:
            pending_load_slot_ = -1;
            pending_load_target_temp_ = 0;
            return;
        case PreheatPhase::Cancelled:
        case PreheatPhase::Abandoned:
            // The preheat this mirrors is over either way; only the stepper may
            // since belong to a newer request.
            pending_load_slot_ = -1;
            pending_load_target_temp_ = 0;
            if (mine == stepper_owner_) {
                abandon_started_operation();
            }
            return;
        }
    };
    if (heater->run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Cancelled) {
        // This request was not run (a re-tap withdrew the one waiting, or it
        // could not take the heater); the stepper armed for it goes too.
        abandon_started_operation();
    }
}

// ============================================================================
// Shared Dispatch Plan
// ============================================================================

helix::ui::BackendCaps AmsOperationSidebar::read_backend_caps(AmsSystemInfo& info_out,
                                                              int target_slot) const {
    return helix::ui::read_backend_caps(AmsState::instance().get_backend(), info_out, target_slot);
}

namespace {
/// UpdateQueue tag for the guarded param-modal callbacks. A string literal: the
/// skip counter interns by pointer identity.
constexpr const char* kLoadMacroTag = "AmsOperationSidebar::load_macro";
} // namespace

helix::ui::FilamentOpSurface AmsOperationSidebar::op_surface(const char* tag, int slot_index) {
    helix::ui::FilamentOpSurface surface;
    surface.log_tag = tag;
    // The user tapped a button on this panel, so a parameter modal is expected
    // rather than an ambush on top of something else.
    surface.param_policy = helix::ui::ParamPolicy::Prompt;

    surface.on_failed = [this](const helix::ui::FilamentOpPlan&, const AmsError& err,
                               bool& reported) {
        fail_started_operation(err);
        reported = true;
    };
    // No on_async_failed: this surface arms its stepper only on the backend
    // tier, so a failed macro has nothing here to unwind.

    // MacroParamModal stores its on_execute_ callback and does NOT clear it on
    // dismiss, while this sidebar dies with the AMS panel. token.defer()
    // re-checks the generation on the main thread, so a Run press after the
    // panel closed is dropped and counted rather than reaching freed memory.
    auto token = lifetime_.token();
    surface.guard = [token](helix::SharedCallback<void()> fn) mutable {
        token.defer(kLoadMacroTag, std::move(fn));
    };
    surface.owner = this;
    surface.alive = [token]() { return !token.expired(); };
    // A dispatch after a preheat reads this at request time, and the token keeps
    // a closed sidebar from ever being asked.
    surface.macro_prefill = [this, token, slot_index](helix::ui::FilamentMacroOp op) {
        return token.expired() ? std::map<std::string, std::string>{}
                               : macro_temp_prefill(op, slot_index);
    };
    return surface;
}

std::string preheat_status_text(bool step_list_shows_temp, int current_c, int target_c) {
    if (step_list_shows_temp)
        return lv_tr("Heating nozzle");
    char temp_buf[32];
    temperature::format_temperature_pair(current_c, target_c, temp_buf, sizeof(temp_buf));
    return temp_buf;
}

void AmsOperationSidebar::show_preheat_feedback(int current_temp, int target_temp) {
    const std::string status =
        preheat_status_text(step_progress_ != nullptr, current_temp, target_temp);
    AmsState::instance().set_action_detail(status);

    if (step_progress_container_) {
        lv_obj_remove_flag(step_progress_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (step_progress_) {
        ui_step_progress_set_current(step_progress_, 0);
    }

    spdlog::debug("[AmsSidebar] Showing preheat feedback: {}", status);
}

} // namespace helix::ui
