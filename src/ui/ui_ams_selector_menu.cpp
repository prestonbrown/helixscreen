// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_selector_menu.h"

#include "ui_callback_helpers.h"

#include "ams_backend.h"
#include "ams_types.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Static member initialization
bool AmsSelectorMenu::callbacks_registered_ = false;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsSelectorMenu::AmsSelectorMenu() {
    spdlog::debug("[AmsSelectorMenu] Constructed");
}

AmsSelectorMenu::~AmsSelectorMenu() {
    spdlog::trace("[AmsSelectorMenu] Destroyed");
}

AmsSelectorMenu::AmsSelectorMenu(AmsSelectorMenu&& other) noexcept
    : ContextMenu(std::move(other)), action_callback_(std::move(other.action_callback_)),
      backend_(other.backend_) {
    other.backend_ = nullptr;
}

AmsSelectorMenu& AmsSelectorMenu::operator=(AmsSelectorMenu&& other) noexcept {
    if (this != &other) {
        // Let base class handle its state, including the active-menu registry
        ContextMenu::operator=(std::move(other));

        action_callback_ = std::move(other.action_callback_);
        backend_ = other.backend_;

        other.backend_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void AmsSelectorMenu::set_action_callback(ActionCallback callback) {
    action_callback_ = std::move(callback);
}

bool AmsSelectorMenu::show_at(lv_obj_t* parent, lv_obj_t* anchor, lv_point_t click_pt,
                              AmsBackend* backend) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Store state BEFORE base class calls on_created
    backend_ = backend;

    set_click_point(click_pt);

    // Base class handles: XML creation, on_created callback, positioning, and
    // claiming the active-menu slot the static callbacks resolve through.
    bool result = show_near_widget(parent, -1, anchor);

    spdlog::debug("[AmsSelectorMenu] Shown");
    return result;
}

// ============================================================================
// ContextMenu override
// ============================================================================

void AmsSelectorMenu::on_created(lv_obj_t* menu_obj) {
    // Hide the servo row on hub-based (Type B) / virtual-selector topologies.
    // Only LINEAR (selector) systems have a physical servo to position.
    bool has_servo = backend_ && backend_->get_topology() == PathTopology::LINEAR;
    if (!has_servo) {
        lv_obj_t* servo_row = lv_obj_find_by_name(menu_obj, "servo_row");
        if (servo_row) {
            lv_obj_add_flag(servo_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Initialize the gear-sync switch.
    //
    // There is no live gear-motor-sync subject today (gear_sync is a send-on-tap
    // device action via MMU_SYNC_GEAR_MOTOR, with no read-back of the runtime
    // state). Default the switch OFF and treat each toggle as send-on-tap: tapping
    // it ON dispatches GEAR_SYNC_ON, tapping it OFF dispatches GEAR_SYNC_OFF.
    lv_obj_t* gear_switch = lv_obj_find_by_name(menu_obj, "gear_sync_switch");
    if (gear_switch) {
        lv_obj_remove_state(gear_switch, LV_STATE_CHECKED);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsSelectorMenu::dispatch_selector_action(SelectorAction action) {
    ActionCallback callback_copy = action_callback_;

    hide();

    if (callback_copy) {
        callback_copy(action);
    }
}

void AmsSelectorMenu::on_backdrop_clicked() {
    spdlog::debug("[AmsSelectorMenu] Backdrop clicked");
    dispatch_selector_action(SelectorAction::CANCELLED);
}

void AmsSelectorMenu::handle_home() {
    spdlog::info("[AmsSelectorMenu] Home selector requested");
    dispatch_selector_action(SelectorAction::HOME);
}

void AmsSelectorMenu::handle_check_slots() {
    spdlog::info("[AmsSelectorMenu] Check slots requested");
    dispatch_selector_action(SelectorAction::CHECK_SLOTS);
}

void AmsSelectorMenu::handle_servo_up() {
    spdlog::info("[AmsSelectorMenu] Servo up requested");
    dispatch_selector_action(SelectorAction::SERVO_UP);
}

void AmsSelectorMenu::handle_servo_move() {
    spdlog::info("[AmsSelectorMenu] Servo move requested");
    dispatch_selector_action(SelectorAction::SERVO_MOVE);
}

void AmsSelectorMenu::handle_servo_down() {
    spdlog::info("[AmsSelectorMenu] Servo down requested");
    dispatch_selector_action(SelectorAction::SERVO_DOWN);
}

void AmsSelectorMenu::handle_jog_prev() {
    spdlog::info("[AmsSelectorMenu] Jog prev requested");
    dispatch_selector_action(SelectorAction::JOG_PREV);
}

void AmsSelectorMenu::handle_jog_next() {
    spdlog::info("[AmsSelectorMenu] Jog next requested");
    dispatch_selector_action(SelectorAction::JOG_NEXT);
}

void AmsSelectorMenu::handle_gear_sync() {
    if (!menu()) {
        return;
    }
    lv_obj_t* gear_switch = lv_obj_find_by_name(menu(), "gear_sync_switch");
    bool checked = gear_switch && lv_obj_has_state(gear_switch, LV_STATE_CHECKED);
    spdlog::info("[AmsSelectorMenu] Gear sync toggled: {}", checked ? "on" : "off");
    dispatch_selector_action(checked ? SelectorAction::GEAR_SYNC_ON
                                     : SelectorAction::GEAR_SYNC_OFF);
}

void AmsSelectorMenu::handle_recover() {
    spdlog::info("[AmsSelectorMenu] Recover requested");
    dispatch_selector_action(SelectorAction::RECOVER);
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsSelectorMenu::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"ams_selector_home_cb", on_home_cb},
        {"ams_selector_check_cb", on_check_cb},
        {"ams_selector_servo_up_cb", on_servo_up_cb},
        {"ams_selector_servo_move_cb", on_servo_move_cb},
        {"ams_selector_servo_down_cb", on_servo_down_cb},
        {"ams_selector_jog_prev_cb", on_jog_prev_cb},
        {"ams_selector_jog_next_cb", on_jog_next_cb},
        {"ams_selector_gear_sync_cb", on_gear_sync_cb},
        {"ams_selector_recover_cb", on_recover_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsSelectorMenu] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via ContextMenu::active())
// ============================================================================

AmsSelectorMenu* AmsSelectorMenu::get_active_instance() {
    auto* self = ContextMenu::active_as<AmsSelectorMenu>();
    if (!self) {
        spdlog::warn("[AmsSelectorMenu] No active instance for event");
    }
    return self;
}

void AmsSelectorMenu::on_home_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_home();
    }
}

void AmsSelectorMenu::on_check_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_check_slots();
    }
}

void AmsSelectorMenu::on_servo_up_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_servo_up();
    }
}

void AmsSelectorMenu::on_servo_move_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_servo_move();
    }
}

void AmsSelectorMenu::on_servo_down_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_servo_down();
    }
}

void AmsSelectorMenu::on_jog_prev_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_jog_prev();
    }
}

void AmsSelectorMenu::on_jog_next_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_jog_next();
    }
}

void AmsSelectorMenu::on_gear_sync_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gear_sync();
    }
}

void AmsSelectorMenu::on_recover_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_recover();
    }
}

} // namespace helix::ui
