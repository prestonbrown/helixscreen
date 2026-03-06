// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_clog_meter.h"

#include "ams_state.h"
#include "observer_factory.h"
#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <spdlog/spdlog.h>

namespace helix::ui {

// Arc size = percentage of card width; stroke scales with arc size
constexpr int32_t ARC_WIDTH_PCT = 18;       // arc is 18% of card width
constexpr int32_t ARC_TO_STROKE_RATIO = 12;
constexpr int32_t MIN_ARC_SIZE = 24;
constexpr int32_t MIN_STROKE_WIDTH = 2;

UiClogMeter::UiClogMeter(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[ClogMeter] NULL parent");
        return;
    }

    root_ = lv_obj_find_by_name(parent, "clog_meter");
    if (!root_) {
        spdlog::warn("[ClogMeter] clog_meter not found in parent");
        return;
    }

    arc_container_ = lv_obj_find_by_name(root_, "clog_arc_container");
    arc_ = lv_obj_find_by_name(root_, "clog_arc");
    if (!arc_container_ || !arc_) {
        spdlog::warn("[ClogMeter] clog_arc_container or clog_arc not found");
        return;
    }

    // Attach SIZE_CHANGED callback on the loaded card (root_'s parent)
    // to dynamically size the arc to fill available height
    lv_obj_t* card = lv_obj_get_parent(root_);
    if (card) {
        // SIZE_CHANGED is a layout event — cannot be registered via XML <event_cb>
        lv_obj_add_event_cb(card, on_card_size_changed, LV_EVENT_SIZE_CHANGED, this);
        resize_arc();
    }

    // Cache the XML-bound value text for hiding in fill mode
    value_text_ = lv_obj_find_by_name(root_, "clog_value_text");

    setup_observers();
    spdlog::debug("[ClogMeter] Initialized");
}

UiClogMeter::~UiClogMeter() {
    // Freeze update queue around observer teardown to prevent race with
    // WebSocket thread enqueueing deferred callbacks between drain and destroy
    auto freeze = UpdateQueue::instance().scoped_freeze();
    UpdateQueue::instance().drain();

    // Remove SIZE_CHANGED callback to prevent dangling this pointer
    lv_obj_t* card = root_ ? lv_obj_get_parent(root_) : nullptr;
    if (card) {
        lv_obj_remove_event_cb_with_user_data(card, on_card_size_changed, this);
    }

    mode_obs_.reset();
    value_obs_.reset();
    warning_obs_.reset();
    danger_obs_.reset();
    peak_obs_.reset();
    center_text_obs_.reset();
    label_left_obs_.reset();
    label_right_obs_.reset();
    root_ = nullptr;
    arc_ = nullptr;
    danger_arc_ = nullptr;
    peak_arc_ = nullptr;
    label_left_ = nullptr;
    label_right_ = nullptr;
    center_label_ = nullptr;
    safe_icon_ = nullptr;
    value_text_ = nullptr;
    spdlog::debug("[ClogMeter] Destroyed");
}

void UiClogMeter::setup_observers() {
    auto& ams = AmsState::instance();

    // Use immediate observers — these callbacks only update local state and
    // arc properties, they don't modify observer lifecycle (safe per issue #82)
    mode_obs_ = observe_int_immediate<UiClogMeter>(
        ams.get_clog_meter_mode_subject(), this,
        [](UiClogMeter* self, int mode) { self->on_mode_changed(mode); });

    value_obs_ = observe_int_immediate<UiClogMeter>(
        ams.get_clog_meter_value_subject(), this,
        [](UiClogMeter* self, int value) { self->on_value_changed(value); });

    warning_obs_ = observe_int_immediate<UiClogMeter>(
        ams.get_clog_meter_warning_subject(), this,
        [](UiClogMeter* self, int warning) { self->on_warning_changed(warning); });
}

void UiClogMeter::on_mode_changed(int mode) {
    current_mode_ = mode;

    if (!arc_) return;

    if (mode == 2) {
        // Flowguard: symmetrical mode, range -100..+100 mapped to 0..200
        lv_arc_set_range(arc_, 0, 200);
        lv_arc_set_mode(arc_, LV_ARC_MODE_SYMMETRICAL);
        lv_arc_set_value(arc_, 100); // Center
    } else {
        // Encoder/AFC/none: normal 0-100
        lv_arc_set_range(arc_, 0, 100);
        lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);
        lv_arc_set_value(arc_, 0);
    }

    update_arc_color();
    update_safe_state();
    spdlog::debug("[ClogMeter] Mode changed to {}", mode);
}

void UiClogMeter::on_value_changed(int value) {
    current_value_ = value;

    if (!arc_) return;

    if (current_mode_ == 2) {
        // Flowguard: -100..+100 → 0..200
        lv_arc_set_value(arc_, value + 100);
    } else {
        lv_arc_set_value(arc_, value);
    }

    update_arc_color();
    update_safe_state();
}

void UiClogMeter::on_warning_changed(int warning) {
    current_warning_ = warning;
    update_arc_color();

    // Update peak marker color when warning state changes
    if (peak_arc_) {
        lv_color_t color = warning ? theme_manager_get_color("danger")
                                   : theme_manager_get_color("primary");
        lv_obj_set_style_arc_color(peak_arc_, color, LV_PART_INDICATOR);
    }
}

void UiClogMeter::update_arc_color() {
    if (!arc_) return;

    lv_color_t color;
    int val = std::clamp(std::abs(current_value_), 0, 100);

    if (current_warning_) {
        // Warning/triggered state
        color = theme_manager_get_color("danger");
    } else if (current_mode_ == 1 || current_mode_ == 3) {
        // Encoder/AFC: gradient primary (safe) → warning (risky) → danger (clogged)
        // Dynamic arc color is an intentional exception to the "no C++ styling" rule
        if (val < 50) {
            color = lv_color_mix(theme_manager_get_color("warning"),
                                 theme_manager_get_color("primary"),
                                 static_cast<uint8_t>(val * 255 / 50));
        } else {
            color = lv_color_mix(theme_manager_get_color("danger"),
                                 theme_manager_get_color("warning"),
                                 static_cast<uint8_t>((val - 50) * 255 / 50));
        }
    } else {
        // Flowguard or default
        color = theme_manager_get_color("primary");
    }

    lv_obj_set_style_arc_color(arc_, color, LV_PART_INDICATOR);
}

void UiClogMeter::update_safe_state() {
    bool safe = (current_mode_ == 3 && current_value_ == 0);

    // Hide the arc entirely when safe — no info means no arc
    if (arc_) {
        if (safe) {
            lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide enhanced arcs when safe
    if (danger_arc_) {
        if (safe) {
            lv_obj_add_flag(danger_arc_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(danger_arc_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (peak_arc_) {
        if (safe) {
            lv_obj_add_flag(peak_arc_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(peak_arc_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide endpoint labels when safe
    if (label_left_) {
        if (safe) {
            lv_obj_add_flag(label_left_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(label_left_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (label_right_) {
        if (safe) {
            lv_obj_add_flag(label_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(label_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide center label (fill-mode) when safe
    if (center_label_) {
        if (safe) {
            lv_obj_add_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Mini-meter: hide XML-bound value text when safe
    if (!fill_mode_ && value_text_) {
        if (safe) {
            lv_obj_add_flag(value_text_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(value_text_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create safe icon lazily on first need
    if (safe && !safe_icon_ && arc_container_) {
        safe_icon_ = lv_label_create(arc_container_);
        lv_label_set_text(safe_icon_, ICON_CHECK_CIRCLE);
        lv_obj_set_style_text_font(safe_icon_, &mdi_icons_24, 0);
        lv_obj_set_style_text_color(safe_icon_, theme_manager_get_color("primary"), 0);
        lv_obj_align(safe_icon_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(safe_icon_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(safe_icon_, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    if (safe_icon_) {
        if (safe) {
            lv_obj_remove_flag(safe_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(safe_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void UiClogMeter::set_fill_mode(bool fill) {
    fill_mode_ = fill;
    if (fill && !danger_arc_) {
        create_enhanced_widgets();
    }
    resize_arc();
    update_safe_state();
}

void UiClogMeter::on_card_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiClogMeter*>(lv_event_get_user_data(e));
    if (self) self->resize_arc();
}

void UiClogMeter::resize_arc() {
    if (!arc_ || !arc_container_ || !root_) return;

    // Re-entrancy guard: lv_obj_update_layout() can fire SIZE_CHANGED
    if (in_resize_) return;
    in_resize_ = true;

    lv_obj_t* card = lv_obj_get_parent(root_);
    if (!card) {
        in_resize_ = false;
        return;
    }

    lv_obj_update_layout(card);

    int32_t arc_size;
    if (fill_mode_) {
        // Fill mode: use widget container dimensions, reserving space for
        // the mode text below and endpoint labels within the arc
        int32_t w = lv_obj_get_content_width(root_);
        int32_t h = lv_obj_get_content_height(root_);
        // Reserve ~20% height for mode text label below the arc
        arc_size = LV_MAX(LV_MIN(w, h * 80 / 100), MIN_ARC_SIZE);
    } else {
        // Arc size = percentage of card width (responsive to breakpoint)
        int32_t card_w = lv_obj_get_content_width(card);
        arc_size = LV_MAX(card_w * ARC_WIDTH_PCT / 100, MIN_ARC_SIZE);
    }

    // Skip if already at target size
    if (lv_obj_get_width(arc_) == arc_size && lv_obj_get_height(arc_) == arc_size) {
        in_resize_ = false;
        return;
    }

    // Size the container and arc to the computed square
    lv_obj_set_size(arc_container_, arc_size, arc_size);
    lv_obj_set_size(arc_, arc_size, arc_size);

    // Scale stroke width proportionally
    int32_t stroke = LV_MAX(arc_size / ARC_TO_STROKE_RATIO, MIN_STROKE_WIDTH);
    lv_obj_set_style_arc_width(arc_, stroke, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, stroke, LV_PART_INDICATOR);

    // Resize enhanced widgets to match main arc
    if (fill_mode_ && danger_arc_) {
        lv_obj_set_size(danger_arc_, arc_size, arc_size);
        lv_obj_set_style_arc_width(danger_arc_, stroke, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(danger_arc_, 0, LV_PART_MAIN);

        // Peak marker extends past the arc — slightly larger size + thicker stroke
        int32_t peak_overhang = LV_MAX(stroke / 2, 2);
        int32_t peak_size = arc_size + peak_overhang * 2;
        lv_obj_set_size(peak_arc_, peak_size, peak_size);
        lv_obj_set_style_arc_width(peak_arc_, stroke + peak_overhang, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(peak_arc_, 0, LV_PART_MAIN);
        // Ensure peak marker renders on top of main arc
        lv_obj_move_foreground(peak_arc_);

        // Position endpoint labels at bottom corners of arc container
        // The arc sweeps 270° from 135° (bottom-left) to 45° (bottom-right)
        if (label_left_) {
            lv_obj_align(label_left_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
        if (label_right_) {
            lv_obj_align(label_right_, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }
    }

    spdlog::debug("[ClogMeter] arc={}x{} stroke={} fill={}",
                  arc_size, arc_size, stroke, fill_mode_);
    in_resize_ = false;
}

int UiClogMeter::value_to_angle(int value) const {
    // Arc sweep: 270° from 135° (start/safe) clockwise to 45° (end/danger)
    int angle;
    if (current_mode_ == 2) {
        // Flowguard: -100..+100 mapped to full 270° sweep
        angle = 135 + ((value + 100) * 270 / 200);
    } else {
        // Normal: 0..100
        angle = 135 + (std::clamp(value, 0, 100) * 270 / 100);
    }
    return angle % 360;
}

void UiClogMeter::create_enhanced_widgets() {
    if (!arc_container_ || !arc_) return;

    // 1. Danger zone arc — behind main arc, semi-transparent danger color
    danger_arc_ = lv_arc_create(arc_container_);
    lv_obj_set_size(danger_arc_, lv_obj_get_width(arc_), lv_obj_get_height(arc_));
    lv_obj_align(danger_arc_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(danger_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(danger_arc_, 135, 45);
    lv_arc_set_range(danger_arc_, 0, 100);
    lv_arc_set_mode(danger_arc_, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_opa(danger_arc_, LV_OPA_0, LV_PART_MAIN); // Hide background track
    lv_obj_set_style_arc_color(danger_arc_, theme_manager_get_color("danger"),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(danger_arc_, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(danger_arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(danger_arc_, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(danger_arc_, LV_OPA_0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(danger_arc_, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(danger_arc_, 0, LV_PART_KNOB);
    // Move behind main arc
    lv_obj_move_to_index(danger_arc_, 0);

    // 2. Peak-hold marker — thin arc at peak position
    peak_arc_ = lv_arc_create(arc_container_);
    lv_obj_set_size(peak_arc_, lv_obj_get_width(arc_), lv_obj_get_height(arc_));
    lv_obj_align(peak_arc_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(peak_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(peak_arc_, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(peak_arc_, theme_manager_get_color("primary"),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(peak_arc_, false, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(peak_arc_, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(peak_arc_, LV_OPA_0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(peak_arc_, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(peak_arc_, 0, LV_PART_KNOB);
    // Default: hidden until we get a peak value
    lv_arc_set_angles(peak_arc_, 135, 135);

    // 3. Endpoint labels at bottom corners
    label_left_ = lv_label_create(arc_container_);
    lv_label_set_text(label_left_, "");
    lv_obj_set_style_text_font(label_left_, theme_manager_get_font("font_xs"), 0);
    lv_obj_set_style_text_color(label_left_, theme_manager_get_color("text_muted"), 0);
    lv_obj_align(label_left_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    label_right_ = lv_label_create(arc_container_);
    lv_label_set_text(label_right_, "");
    lv_obj_set_style_text_font(label_right_, theme_manager_get_font("font_xs"), 0);
    lv_obj_set_style_text_color(label_right_, theme_manager_get_color("text_muted"), 0);
    lv_obj_align(label_right_, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // 4. Enhanced center text — replaces XML-bound clog_value_text in fill mode
    if (value_text_) {
        lv_obj_add_flag(value_text_, LV_OBJ_FLAG_HIDDEN);
    }
    center_label_ = lv_label_create(arc_container_);
    lv_label_set_text(center_label_, "");
    lv_obj_set_style_text_font(center_label_, lv_theme_get_font_small(nullptr), 0);
    lv_obj_set_style_text_align(center_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(center_label_, LV_ALIGN_CENTER, 0, 0);

    // 5. Set up observers for new subjects
    auto& ams = AmsState::instance();

    danger_obs_ = observe_int_immediate<UiClogMeter>(
        ams.get_clog_meter_danger_pct_subject(), this,
        [](UiClogMeter* self, int val) { self->update_danger_zone(val); });

    peak_obs_ = observe_int_immediate<UiClogMeter>(
        ams.get_clog_meter_peak_pct_subject(), this,
        [](UiClogMeter* self, int val) { self->update_peak_marker(val); });

    center_text_obs_ = observe_string_immediate<UiClogMeter>(
        ams.get_clog_meter_center_text_subject(), this,
        [](UiClogMeter* self, const char* text) {
            if (self->center_label_) {
                lv_label_set_text(self->center_label_, text ? text : "");
            }
        });

    label_left_obs_ = observe_string_immediate<UiClogMeter>(
        ams.get_clog_meter_label_left_subject(), this,
        [](UiClogMeter* self, const char* text) {
            if (self->label_left_) {
                lv_label_set_text(self->label_left_, text);
            }
        });

    label_right_obs_ = observe_string_immediate<UiClogMeter>(
        ams.get_clog_meter_label_right_subject(), this,
        [](UiClogMeter* self, const char* text) {
            if (self->label_right_) {
                lv_label_set_text(self->label_right_, text);
            }
        });

    spdlog::debug("[ClogMeter] Enhanced widgets created — danger={} peak={} center='{}' left='{}' right='{}'",
                  lv_subject_get_int(ams.get_clog_meter_danger_pct_subject()),
                  lv_subject_get_int(ams.get_clog_meter_peak_pct_subject()),
                  lv_subject_get_string(ams.get_clog_meter_center_text_subject()),
                  lv_subject_get_string(ams.get_clog_meter_label_left_subject()),
                  lv_subject_get_string(ams.get_clog_meter_label_right_subject()));
}

void UiClogMeter::update_danger_zone(int threshold) {
    if (!danger_arc_) return;

    threshold = std::clamp(threshold, 0, 100);
    if (threshold == 0) {
        // No danger zone — hide indicator
        lv_arc_set_angles(danger_arc_, 135, 135);
        return;
    }

    // Danger zone covers from threshold% to 100% of the arc sweep
    int start_angle = value_to_angle(threshold);
    // End angle is always 45° (100% end of arc)
    lv_arc_set_angles(danger_arc_, static_cast<uint16_t>(start_angle), 45);
}

void UiClogMeter::update_peak_marker(int peak) {
    if (!peak_arc_) return;

    peak = std::clamp(peak, 0, 100);
    if (peak == 0) {
        // No peak — hide marker
        lv_arc_set_angles(peak_arc_, 135, 135);
        return;
    }

    int angle = value_to_angle(peak);
    // 5° wide marker centered on peak position
    int start = (angle - 2 + 360) % 360;
    int end = (angle + 2) % 360;

    // Guard against zero-sweep arc (start == end) which can crash the SW renderer
    if (start == end) {
        end = (start + 1) % 360;
    }

    lv_arc_set_angles(peak_arc_, static_cast<uint16_t>(start),
                      static_cast<uint16_t>(end));

    // Color based on warning state
    lv_color_t color = current_warning_ ? theme_manager_get_color("danger")
                                        : theme_manager_get_color("primary");
    lv_obj_set_style_arc_color(peak_arc_, color, LV_PART_INDICATOR);
}

} // namespace helix::ui
