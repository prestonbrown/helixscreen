// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fan_dial.h"

#include "ui_fan_arc_resize.h"
#include "ui_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "ui/fan_spin_animation.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace helix;

// ============================================================================
// FanDial Implementation
// ============================================================================

namespace {
constexpr const char* kRootName = "fan_dial_root";
} // namespace

// NAMESPACE_OK: the definition of FanDial's own nested type
struct FanDial::Subjects {
    lv_subject_t speed{};
    char speed_buf[16] = {};
    lv_subject_t on{};
    lv_subject_t rpm{};
    int reading = 0; ///< last tachometer reading, shown only while speed > 0
    std::string speed_name;
    std::string on_name;
    SubjectManager manager;

    Subjects() {
        // Two dials can show one fan id (and tests build dials for the same fan),
        // so the published names come from a per-process counter instead.
        static unsigned next_id = 0;
        const std::string base = "fan_dial_" + std::to_string(next_id++);
        speed_name = base + "_speed";
        on_name = base + "_on";
        lv_subject_init_string(&speed, speed_buf, nullptr, sizeof(speed_buf), "");
        manager.publish(speed_name.c_str(), &speed);
        lv_subject_init_int(&on, 0);
        manager.publish(on_name.c_str(), &on);
        lv_subject_init_int(&rpm, 0);
        manager.register_subject(&rpm);
    }
    ~Subjects() {
        manager.deinit_all();
    }
    Subjects(const Subjects&) = delete;
    Subjects& operator=(const Subjects&) = delete;
};

FanDial::FanDial(lv_obj_t* parent, const std::string& name, const std::string& fan_id,
                 int initial_speed, bool label_bottom)
    : subjects_(std::make_unique<Subjects>()), name_(name), fan_id_(fan_id),
      current_speed_(initial_speed) {
    char initial_value_str[16];
    snprintf(initial_value_str, sizeof(initial_value_str), "%d", initial_speed);

    const char* attrs[] = {"fan_name",      name.c_str(),
                           "fan_id",        fan_id.c_str(),
                           "initial_value", initial_value_str,
                           "label_bottom",  label_bottom ? "true" : "false",
                           "speed_subject", subjects_->speed_name.c_str(),
                           "on_subject",    subjects_->on_name.c_str(),
                           nullptr};

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "fan_dial", attrs));
    if (!root_) {
        spdlog::error("[FanDial] Failed to create fan_dial component for '{}'", name);
        return;
    }
    // The XML callbacks find this dial through its root.
    lv_obj_set_user_data(root_, this);

    arc_ = helix::ui::find_required(root_, "dial_arc", "FanDial");
    fan_icon_ = helix::ui::find_required(root_, "fan_icon", "FanDial");
    onoff_switch_ = helix::ui::find_required(root_, "onoff_switch", "FanDial");

    if (!arc_ || !onoff_switch_) {
        return;
    }

    if (lv_obj_t* rpm_label = helix::ui::find_required(root_, "rpm_label", "FanDial")) {
        lv_label_bind_text(rpm_label, &subjects_->rpm, "%d RPM");
        lv_obj_bind_flag_if_eq(rpm_label, &subjects_->rpm, LV_OBJ_FLAG_HIDDEN, 0);
    }

    // Set rotation pivot on fan icon for spin animation
    if (fan_icon_) {
        lv_obj_set_style_transform_pivot_x(fan_icon_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(fan_icon_, LV_PCT(50), 0);
    }

    // The knob's padding is display-density scaled, which XML pixel values are not.
    lv_obj_set_style_pad_all(arc_, lv_dpx(4), LV_PART_KNOB);

    // Attach auto-resize callback for dynamic arc scaling
    helix::ui::fan_arc_attach_auto_resize(root_);

    // Set initial speed display, switch state, and fan animation
    update_speed_label(initial_speed);
    update_onoff_state(initial_speed);
    update_knob_glow(initial_speed);
    update_fan_animation(initial_speed);

    spdlog::trace("[FanDial] Created '{}' (id={}) with initial speed {}%", name, fan_id,
                  initial_speed);
}

FanDial::~FanDial() {
    // Cancel pending debounce send — its user_data points at this.
    cancel_pending_send();

    // Stop any running animation BEFORE destruction.
    // The animation's var points to `this` (FanDial*), not the lv_obj_t*,
    // so lv_obj_delete() on the widget does NOT clean it up.
    // Without this, a pending anim_timer tick will call label_anim_exec_cb
    // on freed memory, crashing in lv_obj_set_style_* → lv_obj_get_parent.
    lv_anim_delete(this, label_anim_exec_cb);
    // Stop fan icon spin animation (uses icon obj as var, not this)
    helix::ui::fan_spin_stop(fan_icon_);

    // An event still queued for the tree must not reach a freed dial.
    if (root_) {
        lv_obj_set_user_data(root_, nullptr);
    }

    spdlog::trace("[FanDial] Destroyed '{}'", name_);
}

FanDial::FanDial(FanDial&& other) noexcept
    : root_(other.root_.get()), arc_(other.arc_.get()), fan_icon_(other.fan_icon_.get()),
      onoff_switch_(other.onoff_switch_.get()), subjects_(std::move(other.subjects_)),
      name_(std::move(other.name_)), fan_id_(std::move(other.fan_id_)),
      current_speed_(other.current_speed_), on_speed_changed_(std::move(other.on_speed_changed_)),
      on_icon_clicked_(std::move(other.on_icon_clicked_)), syncing_(other.syncing_),
      last_user_input_(other.last_user_input_), hold_until_release_(other.hold_until_release_) {
    // Drop any pending debounce timer on the source — its user_data still points
    // at the moved-from instance and we don't bother re-targeting it (drag
    // straddling a move is not a real use case).
    other.cancel_pending_send();

    // Clear the source pointers so other's destructor is a no-op
    other.root_ = nullptr;
    other.arc_ = nullptr;
    other.fan_icon_ = nullptr;
    other.onoff_switch_ = nullptr;

    // Re-target label animation var from old instance to this one
    lv_anim_delete(&other, label_anim_exec_cb);

    if (root_) {
        lv_obj_set_user_data(root_, this);
    }
}

FanDial& FanDial::operator=(FanDial&& other) noexcept {
    if (this != &other) {
        // Cancel pending debounce on both sides — see move-ctor comment.
        cancel_pending_send();
        other.cancel_pending_send();

        // Clean up current resources: stop animations first
        lv_anim_delete(this, label_anim_exec_cb);
        helix::ui::fan_spin_stop(fan_icon_);
        // Child widgets are destroyed with root_, before the subjects they bind
        helix::ui::safe_delete(root_);

        // Move resources
        root_ = other.root_.get();
        arc_ = other.arc_.get();
        fan_icon_ = other.fan_icon_.get();
        onoff_switch_ = other.onoff_switch_.get();
        subjects_ = std::move(other.subjects_);
        name_ = std::move(other.name_);
        fan_id_ = std::move(other.fan_id_);
        current_speed_ = other.current_speed_;
        on_speed_changed_ = std::move(other.on_speed_changed_);
        on_icon_clicked_ = std::move(other.on_icon_clicked_);
        syncing_ = other.syncing_;
        hold_until_release_ = other.hold_until_release_;
        last_user_input_ = other.last_user_input_;

        // Stop animations on the source before clearing its pointers
        lv_anim_delete(&other, label_anim_exec_cb);
        helix::ui::fan_spin_stop(other.fan_icon_);

        // Clear source pointers so other's destructor is a no-op
        other.root_ = nullptr;
        other.arc_ = nullptr;
        other.fan_icon_ = nullptr;
        other.onoff_switch_ = nullptr;

        if (root_) {
            lv_obj_set_user_data(root_, this);
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

    if (arc_) {
        lv_arc_set_value(arc_, percent);
    }

    // Update label, switch state, and fan animation
    update_speed_label(percent);
    update_onoff_state(percent);
    update_knob_glow(percent);
    update_fan_animation(percent);
    refresh_rpm_label();

    syncing_ = false;

    spdlog::trace("[FanDial] '{}' set_speed({}%)", name_, percent);
}

void FanDial::restore_speed(int percent) {
    last_user_input_ = 0;
    set_speed(percent);
}

void FanDial::set_rpm(std::optional<int> rpm) {
    if (!subjects_) {
        return;
    }
    subjects_->reading = rpm.value_or(0) > 0 ? *rpm : 0;
    refresh_rpm_label();
}

void FanDial::refresh_rpm_label() {
    if (!subjects_) {
        return;
    }
    const int value = current_speed_ > 0 ? subjects_->reading : 0;
    if (lv_subject_get_int(&subjects_->rpm) != value) {
        lv_subject_set_int(&subjects_->rpm, value);
    }
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

void FanDial::update_onoff_state(int percent) {
    bool is_on = percent > 0;
    if (onoff_switch_) {
        // Suppress VALUE_CHANGED feedback while we sync visual state to current speed.
        syncing_ = true;
        if (is_on) {
            lv_obj_add_state(onoff_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(onoff_switch_, LV_STATE_CHECKED);
        }
        syncing_ = false;
    }
    if (subjects_ && lv_subject_get_int(&subjects_->on) != (is_on ? 1 : 0)) {
        lv_subject_set_int(&subjects_->on, is_on ? 1 : 0);
    }
}

// The knob glow is computed per value (a colour mix and a shadow scaled to the
// measured arc width) and runs on every frame of the speed animation, so it has
// no declarative form. Off draws no glow.
void FanDial::update_knob_glow(int percent) {
    if (!arc_)
        return;

    int shadow_w = 0;
    int spread = 0;
    lv_opa_t knob_opa = LV_OPA_TRANSP;
    lv_color_t color = theme_manager_get_color("tertiary");
    if (percent > 0) {
        // Color gradient: tertiary → secondary → primary (shifts with speed)
        if (percent <= 50) {
            uint8_t mix = static_cast<uint8_t>(percent * 255 / 50);
            color = lv_color_mix(theme_manager_get_color("secondary"), color, mix);
        } else {
            uint8_t mix = static_cast<uint8_t>((percent - 50) * 255 / 50);
            color = lv_color_mix(theme_manager_get_color("primary"),
                                 theme_manager_get_color("secondary"), mix);
        }

        // Quadratic opacity: 0%→0, 50%→35, 100%→140
        constexpr int kMaxOpa = 140;
        int opa = (percent * percent * kMaxOpa) / 10000;
        knob_opa = static_cast<lv_opa_t>(std::min(opa + 60, 255));

        // Knob shadow — scale with actual arc indicator width
        int32_t arc_w = lv_obj_get_style_arc_width(arc_, LV_PART_INDICATOR);
        shadow_w = (arc_w * 2 + (percent * arc_w * 8) / 100) / 10;
        spread = (shadow_w * percent) / 500;
    }
    // DECLARATIVE_OK: computed glow, see above
    lv_obj_set_style_shadow_width(arc_, shadow_w, LV_PART_KNOB);
    // DECLARATIVE_OK: computed glow, see above
    lv_obj_set_style_shadow_spread(arc_, spread, LV_PART_KNOB);
    // DECLARATIVE_OK: computed glow, see above
    lv_obj_set_style_shadow_color(arc_, color, LV_PART_KNOB);
    // DECLARATIVE_OK: computed glow, see above
    lv_obj_set_style_shadow_opa(arc_, knob_opa, LV_PART_KNOB);
}

void FanDial::update_speed_label(int percent) {
    if (!subjects_)
        return;

    char buf[8];
    const char* text = lv_tr(helix::format::format_fan_speed(percent, buf, sizeof(buf)));
    if (std::strcmp(lv_subject_get_string(&subjects_->speed), text) != 0) {
        lv_subject_set_string(&subjects_->speed, text);
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
    update_onoff_state(value);
    update_knob_glow(value);
    update_fan_animation(value);
    refresh_rpm_label();

    // Coalesce mid-drag bursts: stash the latest value and (re)arm a one-shot
    // 500 ms timer. Touch RELEASED flushes immediately, so dragging fast and
    // letting go still feels responsive; the timer is the safety net for when
    // the release event never arrives (e.g. AD5X touchscreen quirks).
    pending_speed_ = value;
    has_pending_send_ = true;
    constexpr uint32_t kDebounceMs = 500;
    if (debounce_timer_) {
        lv_timer_reset(debounce_timer_);
        lv_timer_set_period(debounce_timer_, kDebounceMs);
    } else {
        debounce_timer_ = lv_timer_create(on_debounce_timer, kDebounceMs, this);
        lv_timer_set_repeat_count(debounce_timer_, 1);
    }

    spdlog::trace("[FanDial] '{}' arc changed to {}% (debounced)", name_, value);
}

void FanDial::handle_arc_released() {
    // Touch released (or press lost) — flush whatever the user landed on now.
    flush_pending_send();
}

void FanDial::cancel_pending_send() {
    if (debounce_timer_) {
        lv_timer_delete(debounce_timer_);
        debounce_timer_ = nullptr;
    }
    has_pending_send_ = false;
}

void FanDial::flush_pending_send() {
    if (!has_pending_send_)
        return;
    int value = pending_speed_;
    cancel_pending_send();
    if (on_speed_changed_) {
        on_speed_changed_(fan_id_, value);
    }
    spdlog::debug("[FanDial] '{}' flushed pending speed {}%", name_, value);
}

void FanDial::on_debounce_timer(lv_timer_t* t) {
    auto* self = static_cast<FanDial*>(lv_timer_get_user_data(t));
    if (!self)
        return;
    // One-shot timer auto-deletes after firing; clear our pointer first so
    // flush_pending_send → cancel_pending_send doesn't double-delete.
    self->debounce_timer_ = nullptr;
    if (self->hold_until_release_ && self->arc_ &&
        (lv_obj_get_state(self->arc_) & LV_STATE_PRESSED)) {
        // Still held: try again later rather than send under the finger. A release that
        // never arrives still clears the pressed state, so the send is not lost.
        constexpr uint32_t kHeldRetryMs = 500;
        self->debounce_timer_ = lv_timer_create(on_debounce_timer, kHeldRetryMs, self);
        lv_timer_set_repeat_count(self->debounce_timer_, 1);
        return;
    }
    self->flush_pending_send();
}

void FanDial::label_anim_exec_cb(void* var, int32_t value) {
    auto* self = static_cast<FanDial*>(var);
    int percent = static_cast<int>(value);
    self->update_speed_label(percent);
    self->update_onoff_state(percent);
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
        update_onoff_state(to);
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

void FanDial::handle_switch_changed() {
    // Ignore the value_changed pulse we triggered ourselves while syncing
    // the visual state to current_speed_.
    if (syncing_ || !onoff_switch_)
        return;

    last_user_input_ = lv_tick_get();
    bool checked = lv_obj_has_state(onoff_switch_, LV_STATE_CHECKED);
    int prev_speed = current_speed_;
    int new_speed = checked ? 100 : 0;
    current_speed_ = new_speed;
    animate_speed_label(prev_speed, new_speed);
    update_fan_animation(new_speed);
    refresh_rpm_label();

    if (on_speed_changed_) {
        on_speed_changed_(fan_id_, new_speed);
    }

    spdlog::debug("[FanDial] '{}' switch toggled -> {}%", name_, new_speed);
}

void FanDial::refresh_animation() {
    update_fan_animation(current_speed_);
}

void FanDial::update_fan_animation(int speed_pct) {
    if (!fan_icon_)
        return;

    if (!DisplaySettingsManager::instance().get_animations_enabled() || speed_pct <= 0) {
        helix::ui::fan_spin_stop(fan_icon_);
    } else {
        helix::ui::fan_spin_start(fan_icon_, speed_pct);
    }
}

// ============================================================================
// XML Event Callbacks
// ============================================================================

FanDial* FanDial::from_event(lv_event_t* e) {
    for (auto* o = static_cast<lv_obj_t*>(lv_event_get_current_target(e)); o;
         o = lv_obj_get_parent(o)) {
        const char* obj_name = lv_obj_get_name(o);
        if (obj_name && std::strcmp(obj_name, kRootName) == 0) {
            return static_cast<FanDial*>(lv_obj_get_user_data(o));
        }
    }
    return nullptr;
}

void FanDial::on_arc_value_changed(lv_event_t* e) {
    if (auto* self = from_event(e)) {
        self->handle_arc_changed();
    }
}

void FanDial::on_arc_released(lv_event_t* e) {
    if (auto* self = from_event(e)) {
        self->handle_arc_released();
    }
}

void FanDial::on_switch_changed(lv_event_t* e) {
    if (auto* self = from_event(e)) {
        self->handle_switch_changed();
    }
}

void FanDial::on_icon_clicked(lv_event_t* e) {
    if (auto* self = from_event(e)) {
        self->handle_icon_clicked();
    }
}

void register_fan_dial_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_fan_dial_switch_changed", FanDial::on_switch_changed);
    lv_xml_register_event_cb(nullptr, "on_fan_dial_arc_changed", FanDial::on_arc_value_changed);
    lv_xml_register_event_cb(nullptr, "on_fan_dial_arc_released", FanDial::on_arc_released);
    lv_xml_register_event_cb(nullptr, "on_fan_dial_icon_clicked", FanDial::on_icon_clicked);

    spdlog::trace("[FanDial] Registered XML event callbacks");
}
