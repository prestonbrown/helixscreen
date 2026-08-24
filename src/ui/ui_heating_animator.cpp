// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_heating_animator.h"

#include "ui_icon.h"
#include "ui_temperature_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

HeatingIconAnimator::~HeatingIconAnimator() {
    if (lv_is_initialized()) {
        detach();
    } else {
        pulse_active_ = false;
        icon_ = nullptr;
        // theme_observer_ is an ObserverGuard — its destructor handles cleanup
    }
}

HeatingIconAnimator::HeatingIconAnimator(HeatingIconAnimator&& other) noexcept
    : icon_(other.icon_), state_(other.state_), current_temp_(other.current_temp_),
      target_temp_(other.target_temp_), current_color_(other.current_color_),
      current_opacity_(other.current_opacity_), pulse_active_(other.pulse_active_),
      theme_observer_(std::move(other.theme_observer_)) {
    other.icon_ = nullptr;
    other.pulse_active_ = false;
}

HeatingIconAnimator& HeatingIconAnimator::operator=(HeatingIconAnimator&& other) noexcept {
    if (this != &other) {
        detach();
        icon_ = other.icon_;
        state_ = other.state_;
        current_temp_ = other.current_temp_;
        target_temp_ = other.target_temp_;
        current_color_ = other.current_color_;
        current_opacity_ = other.current_opacity_;
        pulse_active_ = other.pulse_active_;
        theme_observer_ = std::move(other.theme_observer_);
        other.icon_ = nullptr;
        other.pulse_active_ = false;
    }
    return *this;
}

void HeatingIconAnimator::attach(lv_obj_t* icon) {
    if (icon_ != nullptr) {
        detach();
    }
    icon_ = icon;
    state_ = State::Off;
    current_color_ = get_secondary_color();
    current_opacity_ = LV_OPA_COVER;
    apply_color();

    // Auto-detach when the icon widget is destroyed — prevents dangling pointer
    // when a shared TemperatureService outlives the overlay panel (issue #177)
    lv_obj_add_event_cb(icon_, icon_delete_cb, LV_EVENT_DELETE, this);

    // Subscribe to theme changes — ObserverGuard handles cleanup on detach/destroy
    lv_subject_t* theme_subject = theme_manager_get_changed_subject();
    if (theme_subject) {
        theme_observer_ = ObserverGuard(theme_subject, theme_change_cb, this);
        // trace, not debug: attach/detach pairs fire on every panel rebuild and
        // pushed ~1400 lines through a 20k-line bundle ring with nothing ever
        // diagnosed from them. The no-theme-subject branch below stays at debug —
        // that one is abnormal.
        spdlog::trace("[HeatingIconAnimator] Attached to icon with theme observer");
    } else {
        spdlog::debug("[HeatingIconAnimator] Attached to icon (no theme subject found)");
    }
}

void HeatingIconAnimator::detach() {
    if (icon_ == nullptr) {
        return;
    }

    // Cache and null icon_ FIRST — makes this function re-entrant safe.
    // If the crash handler invokes ~HeatingIconAnimator → detach() while we're
    // mid-execution, the nullptr check above will bail out immediately.
    lv_obj_t* icon = icon_;
    icon_ = nullptr;

    stop_pulse();

    // Remove our delete callback. Do NOT use lv_obj_is_valid() here — it performs
    // an O(n) recursive walk that can stack overflow on Pi. The pointer is valid
    // because detach() is only called while the widget tree is alive, and
    // icon_delete_cb would have nulled icon_ if the widget was already destroyed.
    lv_obj_remove_event_cb_with_user_data(icon, icon_delete_cb, this);

    // ObserverGuard::reset() removes the observer from the subject
    theme_observer_.reset();
    spdlog::trace("[HeatingIconAnimator] Detached");
}

void HeatingIconAnimator::update(int current_temp, int target_temp, helix::ChamberMode mode) {
    if (icon_ == nullptr) {
        return;
    }

    current_temp_ = current_temp;
    target_temp_ = target_temp;

    // Same classifier the temperature label uses, in decidegrees, and against
    // the same displayed_deci() reading — the icon sits beside the number, so it
    // has to agree with what that number rounded to. mode defaults to Heating,
    // which classify_heat_state_with_mode() passes straight through to
    // classify_heat_state() — a no-op for nozzle/bed. Only the chamber ever
    // passes Off/Maintaining.
    State new_state = helix::ui::temperature::classify_heat_state_with_mode(
        helix::ui::temperature::displayed_deci(current_temp), target_temp, mode, TEMP_TOLERANCE);

    if (new_state != state_) {
        state_ = new_state;

        // Pulse means "working toward a setpoint from below". Cooling is a
        // transient step-down; Off and Neutral (chamber Maintaining, at/below
        // its cooling ceiling) are idle — none of them should pulse.
        if (new_state == State::Heating) {
            if (!pulse_active_) {
                start_pulse();
            }
        } else {
            stop_pulse();
        }
        spdlog::trace("[HeatingIconAnimator] State: {}", static_cast<int>(new_state));
    }

    current_color_ = helix::ui::temperature::get_heating_state_color(state_);
    apply_color();
}

void HeatingIconAnimator::start_pulse() {
    if (icon_ == nullptr || pulse_active_) {
        return;
    }

    pulse_active_ = true;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, PULSE_OPA_MIN, PULSE_OPA_MAX);
    lv_anim_set_duration(&anim, PULSE_DURATION_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&anim, PULSE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, pulse_anim_cb);
    lv_anim_start(&anim);

    spdlog::trace("[HeatingIconAnimator] Pulse animation started");
}

void HeatingIconAnimator::stop_pulse() {
    if (!pulse_active_) {
        return;
    }

    pulse_active_ = false;
    lv_anim_delete(this, pulse_anim_cb);
    current_opacity_ = LV_OPA_COVER;

    spdlog::trace("[HeatingIconAnimator] Pulse animation stopped");
}

void HeatingIconAnimator::apply_color() {
    if (icon_ == nullptr) {
        return;
    }

    // Try to set color on the attached widget directly (for single icons)
    ui_icon_set_color(icon_, current_color_, current_opacity_);

    // Also recolor direct children. Dead code for every current call site — they
    // all attach to a leaf <icon> label — but kept so attaching to a wrapper
    // component (e.g. nozzle_icon.xml) tints its glyph rather than nothing.
    uint32_t child_count = lv_obj_get_child_count(icon_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(icon_, static_cast<int32_t>(i));
        if (child) {
            ui_icon_set_color(child, current_color_, current_opacity_);
        }
    }
}

lv_color_t HeatingIconAnimator::get_secondary_color() {
    // Off-state color, shared with the temperature label (text_muted, not secondary).
    return helix::ui::temperature::get_heating_state_color(0, 0, TEMP_TOLERANCE);
}

void HeatingIconAnimator::refresh_theme() {
    if (icon_ == nullptr) {
        return;
    }

    // Colors are theme tokens — re-resolve for the already-classified state_
    // (not by re-deriving it from current_temp_/target_temp_, which would lose
    // mode-awareness: a chamber in Maintaining below its ceiling would
    // misclassify back to Off/AtTemp under the plain int-based classifier).
    current_color_ = helix::ui::temperature::get_heating_state_color(state_);
    apply_color();
}

void HeatingIconAnimator::pulse_anim_cb(void* var, int32_t value) {
    auto* animator = static_cast<HeatingIconAnimator*>(var);
    if (animator && animator->icon_) {
        animator->current_opacity_ = static_cast<lv_opa_t>(value);
        animator->apply_color();
    }
}

void HeatingIconAnimator::theme_change_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* animator = static_cast<HeatingIconAnimator*>(lv_observer_get_user_data(observer));
    if (animator) {
        spdlog::trace("[HeatingIconAnimator] Theme changed, refreshing colors");
        animator->refresh_theme();
    }
}

void HeatingIconAnimator::icon_delete_cb(lv_event_t* e) {
    auto* animator = static_cast<HeatingIconAnimator*>(lv_event_get_user_data(e));
    if (!animator) {
        return;
    }

    spdlog::debug("[HeatingIconAnimator] Icon widget destroyed, auto-detaching");

    // Stop pulse animation (references icon_)
    animator->stop_pulse();

    animator->theme_observer_.reset();

    // Null out icon_ — the widget is already being destroyed, don't touch it further
    animator->icon_ = nullptr;
}
