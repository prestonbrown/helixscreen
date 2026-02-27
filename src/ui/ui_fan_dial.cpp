// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fan_dial.h"

#include "ui_fan_arc_resize.h"
#include "ui_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui/fan_spin_animation.h"
#include "ui/ui_event_trampoline.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <utility>

using namespace helix;

// ============================================================================
// FanDial Implementation
// ============================================================================

FanDial::FanDial(lv_obj_t* parent, const std::string& name, const std::string& fan_id,
                 int initial_speed)
    : name_(name), fan_id_(fan_id), current_speed_(initial_speed) {
    // Build attributes array for XML creation
    char initial_value_str[16];
    snprintf(initial_value_str, sizeof(initial_value_str), "%d", initial_speed);

    const char* attrs[] = {"fan_name",      name.c_str(),      "fan_id", fan_id.c_str(),
                           "initial_value", initial_value_str, nullptr};

    // Create widget from XML component
    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "fan_dial", attrs));
    if (!root_) {
        spdlog::error("[FanDial] Failed to create fan_dial component for '{}'", name);
        return;
    }

    // Find child widgets by name
    arc_ = lv_obj_find_by_name(root_, "dial_arc");
    speed_label_ = lv_obj_find_by_name(root_, "speed_label");
    fan_icon_ = lv_obj_find_by_name(root_, "fan_icon");
    btn_off_ = lv_obj_find_by_name(root_, "btn_off");
    btn_on_ = lv_obj_find_by_name(root_, "btn_on");

    if (!arc_ || !speed_label_ || !btn_off_ || !btn_on_) {
        spdlog::error("[FanDial] Failed to find child widgets for '{}': arc={} label={} "
                      "off={} on={}",
                      name, arc_ != nullptr, speed_label_ != nullptr, btn_off_ != nullptr,
                      btn_on_ != nullptr);
        return;
    }

    // Set rotation pivot on fan icon for spin animation
    if (fan_icon_) {
        lv_obj_set_style_transform_pivot_x(fan_icon_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(fan_icon_, LV_PCT(50), 0);

        // Make the fan icon clickable (for opening the fan overlay)
        lv_obj_add_flag(fan_icon_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(fan_icon_, on_icon_clicked, LV_EVENT_CLICKED, this);
    }

    // Add event callbacks with this pointer as user data
    lv_obj_add_event_cb(arc_, on_arc_value_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(btn_off_, on_off_clicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(btn_on_, on_on_clicked, LV_EVENT_CLICKED, this);

    // Attach auto-resize callback for dynamic arc scaling
    helix::ui::fan_arc_attach_auto_resize(root_);

    // Set initial speed display, button states, and fan animation
    update_speed_label(initial_speed);
    update_button_states(initial_speed);
    update_knob_glow(initial_speed);
    update_fan_animation(initial_speed);

    spdlog::trace("[FanDial] Created '{}' (id={}) with initial speed {}%", name, fan_id,
                  initial_speed);
}

FanDial::~FanDial() {
    // Stop any running animation BEFORE destruction.
    // The animation's var points to `this` (FanDial*), not the lv_obj_t*,
    // so lv_obj_delete() on the widget does NOT clean it up.
    // Without this, a pending anim_timer tick will call label_anim_exec_cb
    // on freed memory, crashing in lv_obj_set_style_* → lv_obj_get_parent.
    lv_anim_delete(this, label_anim_exec_cb);
    // Stop fan icon spin animation (uses icon obj as var, not this)
    stop_spin(fan_icon_);

    // Remove event callbacks to prevent stale 'this' dispatch if events
    // are still pending in the LVGL event queue after widget deletion
    if (arc_)
        lv_obj_remove_event_cb(arc_, on_arc_value_changed);
    if (btn_off_)
        lv_obj_remove_event_cb(btn_off_, on_off_clicked);
    if (btn_on_)
        lv_obj_remove_event_cb(btn_on_, on_on_clicked);
    if (fan_icon_)
        lv_obj_remove_event_cb(fan_icon_, on_icon_clicked);

    spdlog::trace("[FanDial] Destroyed '{}'", name_);
}

FanDial::FanDial(FanDial&& other) noexcept
    : root_(other.root_), arc_(other.arc_), speed_label_(other.speed_label_),
      fan_icon_(other.fan_icon_), btn_off_(other.btn_off_), btn_on_(other.btn_on_),
      name_(std::move(other.name_)), fan_id_(std::move(other.fan_id_)),
      current_speed_(other.current_speed_), on_speed_changed_(std::move(other.on_speed_changed_)),
      on_icon_clicked_(std::move(other.on_icon_clicked_)), syncing_(other.syncing_),
      last_user_input_(other.last_user_input_) {
    // Clear the source pointers so other's destructor is a no-op
    other.root_ = nullptr;
    other.arc_ = nullptr;
    other.speed_label_ = nullptr;
    other.fan_icon_ = nullptr;
    other.btn_off_ = nullptr;
    other.btn_on_ = nullptr;

    // Re-target label animation var from old instance to this one
    lv_anim_delete(&other, label_anim_exec_cb);

    // Update event callback user_data to point to this instance
    if (arc_) {
        lv_obj_remove_event_cb(arc_, on_arc_value_changed);
        lv_obj_add_event_cb(arc_, on_arc_value_changed, LV_EVENT_VALUE_CHANGED, this);
    }
    if (btn_off_) {
        lv_obj_remove_event_cb(btn_off_, on_off_clicked);
        lv_obj_add_event_cb(btn_off_, on_off_clicked, LV_EVENT_CLICKED, this);
    }
    if (btn_on_) {
        lv_obj_remove_event_cb(btn_on_, on_on_clicked);
        lv_obj_add_event_cb(btn_on_, on_on_clicked, LV_EVENT_CLICKED, this);
    }
    if (fan_icon_) {
        lv_obj_remove_event_cb(fan_icon_, on_icon_clicked);
        lv_obj_add_event_cb(fan_icon_, on_icon_clicked, LV_EVENT_CLICKED, this);
    }
}

FanDial& FanDial::operator=(FanDial&& other) noexcept {
    if (this != &other) {
        // Clean up current resources: stop animations and remove callbacks first
        lv_anim_delete(this, label_anim_exec_cb);
        stop_spin(fan_icon_);
        if (arc_)
            lv_obj_remove_event_cb(arc_, on_arc_value_changed);
        if (btn_off_)
            lv_obj_remove_event_cb(btn_off_, on_off_clicked);
        if (btn_on_)
            lv_obj_remove_event_cb(btn_on_, on_on_clicked);
        if (fan_icon_)
            lv_obj_remove_event_cb(fan_icon_, on_icon_clicked);
        // Child widgets are destroyed with root_
        helix::ui::safe_delete(root_);

        // Move resources
        root_ = other.root_;
        arc_ = other.arc_;
        speed_label_ = other.speed_label_;
        fan_icon_ = other.fan_icon_;
        btn_off_ = other.btn_off_;
        btn_on_ = other.btn_on_;
        name_ = std::move(other.name_);
        fan_id_ = std::move(other.fan_id_);
        current_speed_ = other.current_speed_;
        on_speed_changed_ = std::move(other.on_speed_changed_);
        on_icon_clicked_ = std::move(other.on_icon_clicked_);
        syncing_ = other.syncing_;
        last_user_input_ = other.last_user_input_;

        // Stop animations on the source before clearing its pointers
        lv_anim_delete(&other, label_anim_exec_cb);
        stop_spin(other.fan_icon_);

        // Clear source pointers so other's destructor is a no-op
        other.root_ = nullptr;
        other.arc_ = nullptr;
        other.speed_label_ = nullptr;
        other.fan_icon_ = nullptr;
        other.btn_off_ = nullptr;
        other.btn_on_ = nullptr;

        // Update event callback user_data to point to this instance
        if (arc_) {
            lv_obj_remove_event_cb(arc_, on_arc_value_changed);
            lv_obj_add_event_cb(arc_, on_arc_value_changed, LV_EVENT_VALUE_CHANGED, this);
        }
        if (btn_off_) {
            lv_obj_remove_event_cb(btn_off_, on_off_clicked);
            lv_obj_add_event_cb(btn_off_, on_off_clicked, LV_EVENT_CLICKED, this);
        }
        if (btn_on_) {
            lv_obj_remove_event_cb(btn_on_, on_on_clicked);
            lv_obj_add_event_cb(btn_on_, on_on_clicked, LV_EVENT_CLICKED, this);
        }
        if (fan_icon_) {
            lv_obj_remove_event_cb(fan_icon_, on_icon_clicked);
            lv_obj_add_event_cb(fan_icon_, on_icon_clicked, LV_EVENT_CLICKED, this);
        }
    }
    return *this;
}

void FanDial::set_speed(int percent) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    // Suppress external updates while user is actively dragging the arc
    if (arc_ && (lv_obj_get_state(arc_) & LV_STATE_PRESSED)) {
        spdlog::trace("[FanDial] '{}' suppressed set_speed({}%) - arc is pressed", name_, percent);
        return;
    }

    // Suppress external updates for a short window after the user releases the dial,
    // so stale Moonraker values don't snap the dial back before confirmation arrives
    constexpr uint32_t suppression_ms = 1500;
    if (last_user_input_ > 0 && (lv_tick_get() - last_user_input_) < suppression_ms) {
        spdlog::trace("[FanDial] '{}' suppressed set_speed({}%) - within {}ms of last input", name_,
                      percent, suppression_ms);
        return;
    }

    current_speed_ = percent;

    // Set syncing flag to prevent callback loop
    syncing_ = true;

    // Update arc value
    if (arc_) {
        lv_arc_set_value(arc_, percent);
    }

    // Update label, button states, and fan animation
    update_speed_label(percent);
    update_button_states(percent);
    update_knob_glow(percent);
    update_fan_animation(percent);

    syncing_ = false;

    spdlog::trace("[FanDial] '{}' set_speed({}%)", name_, percent);
}

int FanDial::get_speed() const {
    return current_speed_;
}

void FanDial::set_on_speed_changed(SpeedCallback callback) {
    on_speed_changed_ = std::move(callback);
}

void FanDial::set_on_icon_clicked(IconClickCallback callback) {
    on_icon_clicked_ = std::move(callback);
}

void FanDial::handle_icon_clicked() {
    if (on_icon_clicked_) {
        on_icon_clicked_(fan_id_);
    }
}

void FanDial::update_button_states(int percent) {
    if (btn_off_) {
        if (percent == 0) {
            lv_obj_add_state(btn_off_, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(btn_off_, LV_STATE_DISABLED);
        }
    }
    if (btn_on_) {
        if (percent == 100) {
            lv_obj_add_state(btn_on_, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(btn_on_, LV_STATE_DISABLED);
        }
    }
}

void FanDial::update_knob_glow(int percent) {
    if (!arc_)
        return;

    if (percent > 0) {
        // Color gradient: tertiary → secondary → primary (shifts with speed)
        lv_color_t color;
        if (percent <= 50) {
            uint8_t mix = static_cast<uint8_t>(percent * 255 / 50);
            color = lv_color_mix(theme_manager_get_color("secondary"),
                                 theme_manager_get_color("tertiary"), mix);
        } else {
            uint8_t mix = static_cast<uint8_t>((percent - 50) * 255 / 50);
            color = lv_color_mix(theme_manager_get_color("primary"),
                                 theme_manager_get_color("secondary"), mix);
        }

        // Quadratic opacity: 0%→0, 50%→35, 100%→140
        constexpr int MAX_OPA = 140;
        int opa = (percent * percent * MAX_OPA) / 10000;

        // Knob shadow — scale with actual arc indicator width
        int32_t arc_w = lv_obj_get_style_arc_width(arc_, LV_PART_INDICATOR);
        int shadow_w = (arc_w * 2 + (percent * arc_w * 8) / 100) / 10;
        int spread = (shadow_w * percent) / 500;
        lv_obj_set_style_shadow_width(arc_, shadow_w, LV_PART_KNOB);
        lv_obj_set_style_shadow_spread(arc_, spread, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(arc_, color, LV_PART_KNOB);
        int knob_opa = std::min(opa + 60, 255);
        lv_obj_set_style_shadow_opa(arc_, static_cast<lv_opa_t>(knob_opa), LV_PART_KNOB);
    } else {
        lv_obj_set_style_shadow_width(arc_, 0, LV_PART_KNOB);
        lv_obj_set_style_shadow_opa(arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    }
}

void FanDial::update_speed_label(int percent) {
    if (!speed_label_)
        return;

    if (percent == 0) {
        lv_label_set_text(speed_label_, lv_tr("Off"));
    } else {
        char buf[8];
        helix::format::format_percent(percent, buf, sizeof(buf));
        lv_label_set_text(speed_label_, buf);
    }
}

void FanDial::handle_arc_changed() {
    if (syncing_)
        return;

    if (!arc_)
        return;

    last_user_input_ = lv_tick_get();

    int value = lv_arc_get_value(arc_);
    current_speed_ = value;
    update_speed_label(value);
    update_button_states(value);
    update_knob_glow(value);
    update_fan_animation(value);

    if (on_speed_changed_) {
        on_speed_changed_(fan_id_, value);
    }

    spdlog::trace("[FanDial] '{}' arc changed to {}%", name_, value);
}

void FanDial::label_anim_exec_cb(void* var, int32_t value) {
    auto* self = static_cast<FanDial*>(var);
    int percent = static_cast<int>(value);
    self->update_speed_label(percent);
    self->update_button_states(percent);
    self->update_knob_glow(percent);
    if (self->arc_) {
        lv_arc_set_value(self->arc_, value);
    }
}

void FanDial::anim_completed_cb(lv_anim_t* anim) {
    auto* self = static_cast<FanDial*>(anim->var);
    if (self) {
        self->syncing_ = false;
    }
}

void FanDial::animate_speed_label(int from, int to) {
    // Skip animation when value unchanged or animations disabled
    if (from == to || !DisplaySettingsManager::instance().get_animations_enabled()) {
        update_speed_label(to);
        if (arc_) {
            lv_arc_set_value(arc_, to);
        }
        syncing_ = false;
        return;
    }

    // Cancel any existing animation
    lv_anim_delete(this, label_anim_exec_cb);

    // Keep syncing_ true for the entire animation to suppress arc change callbacks
    syncing_ = true;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, 400);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, label_anim_exec_cb);
    lv_anim_set_completed_cb(&anim, anim_completed_cb);
    lv_anim_start(&anim);
}

void FanDial::handle_off_clicked() {
    last_user_input_ = lv_tick_get();
    int prev_speed = current_speed_;
    current_speed_ = 0;
    animate_speed_label(prev_speed, 0);
    update_fan_animation(0);

    if (on_speed_changed_) {
        on_speed_changed_(fan_id_, 0);
    }

    spdlog::debug("[FanDial] '{}' Off button clicked", name_);
}

void FanDial::handle_on_clicked() {
    last_user_input_ = lv_tick_get();
    int prev_speed = current_speed_;
    current_speed_ = 100;
    animate_speed_label(prev_speed, 100);
    update_fan_animation(100);

    if (on_speed_changed_) {
        on_speed_changed_(fan_id_, 100);
    }

    spdlog::debug("[FanDial] '{}' On button clicked", name_);
}

// ============================================================================
// Fan Icon Spin Animation
// ============================================================================

void FanDial::set_read_only(bool read_only) {
    if (!arc_)
        return;

    if (read_only) {
        // Disable arc interaction
        lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);

        // Hide the knob
        lv_obj_set_style_bg_opa(arc_, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_shadow_opa(arc_, LV_OPA_TRANSP, LV_PART_KNOB);

        // Muted indicator color (matches fan_status_card.xml)
        lv_color_t muted = theme_manager_get_color("text_muted");
        lv_obj_set_style_arc_color(arc_, muted, LV_PART_INDICATOR);

        // Hide Off/On buttons, show "Auto" label instead
        if (btn_off_)
            lv_obj_add_flag(btn_off_, LV_OBJ_FLAG_HIDDEN);
        if (btn_on_)
            lv_obj_add_flag(btn_on_, LV_OBJ_FLAG_HIDDEN);

        // Add "Auto" indicator in the button row
        lv_obj_t* btn_row = lv_obj_find_by_name(root_, "button_row");
        if (btn_row) {
            lv_obj_t* auto_label = lv_label_create(btn_row);
            lv_label_set_text(auto_label, lv_tr("Auto"));
            lv_obj_set_style_text_color(auto_label, theme_manager_get_color("text_muted"), 0);
            const lv_font_t* font = theme_manager_get_font("font_xs");
            if (font)
                lv_obj_set_style_text_font(auto_label, font, 0);
        }

        // Clear the speed callback so no commands are sent
        on_speed_changed_ = nullptr;
    } else {
        lv_obj_add_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(arc_, LV_OPA_COVER, LV_PART_KNOB);
        lv_color_t primary = theme_manager_get_color("primary");
        lv_obj_set_style_arc_color(arc_, primary, LV_PART_INDICATOR);
        if (btn_off_)
            lv_obj_remove_flag(btn_off_, LV_OBJ_FLAG_HIDDEN);
        if (btn_on_)
            lv_obj_remove_flag(btn_on_, LV_OBJ_FLAG_HIDDEN);
    }

    spdlog::debug("[FanDial] '{}' set_read_only({})", name_, read_only);
}

void FanDial::refresh_animation() {
    update_fan_animation(current_speed_);
}

void FanDial::update_fan_animation(int speed_pct) {
    if (!fan_icon_)
        return;

    if (!DisplaySettingsManager::instance().get_animations_enabled() || speed_pct <= 0) {
        stop_spin(fan_icon_);
    } else {
        start_spin(fan_icon_, speed_pct);
    }
}

void FanDial::spin_anim_cb(void* var, int32_t value) {
    helix::ui::fan_spin_anim_cb(var, value);
}

void FanDial::stop_spin(lv_obj_t* icon) {
    helix::ui::fan_spin_stop(icon);
}

void FanDial::start_spin(lv_obj_t* icon, int speed_pct) {
    helix::ui::fan_spin_start(icon, speed_pct);
}

// ============================================================================
// Static Event Trampolines
// ============================================================================

DEFINE_EVENT_TRAMPOLINE_SIMPLE(FanDial, on_arc_value_changed, handle_arc_changed)
DEFINE_EVENT_TRAMPOLINE_SIMPLE(FanDial, on_off_clicked, handle_off_clicked)
DEFINE_EVENT_TRAMPOLINE_SIMPLE(FanDial, on_on_clicked, handle_on_clicked)
DEFINE_EVENT_TRAMPOLINE_SIMPLE(FanDial, on_icon_clicked, handle_icon_clicked)

// ============================================================================
// XML Callback Registration
// ============================================================================

// These are no-op placeholders for XML event callbacks
// The actual event handling is done via lv_obj_add_event_cb in the constructor
// with user_data pointing to the FanDial instance

static void xml_fan_dial_value_changed(lv_event_t* /*e*/) {
    // No-op: actual handling is via C++ event callbacks with user_data
}

static void xml_fan_dial_off_clicked(lv_event_t* /*e*/) {
    // No-op: actual handling is via C++ event callbacks with user_data
}

static void xml_fan_dial_on_clicked(lv_event_t* /*e*/) {
    // No-op: actual handling is via C++ event callbacks with user_data
}

void register_fan_dial_callbacks() {
    // Register XML callbacks (required for XML parsing, but we use C++ callbacks)
    lv_xml_register_event_cb(nullptr, "on_fan_dial_value_changed", xml_fan_dial_value_changed);
    lv_xml_register_event_cb(nullptr, "on_fan_dial_off_clicked", xml_fan_dial_off_clicked);
    lv_xml_register_event_cb(nullptr, "on_fan_dial_on_clicked", xml_fan_dial_on_clicked);

    spdlog::trace("[FanDial] Registered XML event callbacks");
}
