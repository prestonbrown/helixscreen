// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_effects.h"

#include "backdrop_blur.h"
#include "display_settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void create_ripple(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, int start_size, int end_size,
                   int32_t duration_ms) {
    if (!parent) {
        return;
    }

    // Create circle object for ripple effect
    lv_obj_t* ripple = lv_obj_create(parent);
    lv_obj_remove_style_all(ripple);

    // Initial size (small circle)
    lv_obj_set_size(ripple, start_size, start_size);
    lv_obj_set_style_radius(ripple, LV_RADIUS_CIRCLE, 0);

    // Style: primary color, semi-transparent
    lv_obj_set_style_bg_color(ripple, theme_manager_get_color("primary"), 0);
    lv_obj_set_style_bg_opa(ripple, LV_OPA_50, 0);
    lv_obj_set_style_border_width(ripple, 0, 0);

    // Take out of flex layout so position works, and make non-clickable
    lv_obj_add_flag(ripple, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(ripple, LV_OBJ_FLAG_CLICKABLE);

    // Position centered on touch point
    lv_obj_set_pos(ripple, x - start_size / 2, y - start_size / 2);

    // Animation 1: Scale (grow) — native resize keeps the circle edge crisp.
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, ripple);
    lv_anim_set_values(&scale_anim, start_size, end_size);
    lv_anim_set_duration(&scale_anim, duration_ms);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&scale_anim, [](void* var, int32_t size) {
        auto* obj = static_cast<lv_obj_t*>(var);
        // Read the size/pos we last set (style values are immediate; laid-out
        // coords lag a frame).
        int32_t old_size = lv_obj_get_style_width(obj, LV_PART_MAIN);
        int32_t cur_x = lv_obj_get_style_x(obj, LV_PART_MAIN);
        int32_t cur_y = lv_obj_get_style_y(obj, LV_PART_MAIN);
        lv_obj_set_size(obj, size, size);
        // Keep the CENTER fixed. Computing the new top-left as
        // cur + old_size/2 - size/2 telescopes across frames (the old_size/2 term
        // cancels the previous frame's -size/2), so integer rounding does NOT
        // accumulate — unlike subtracting an incremental delta, which slid the
        // ripple off the touch point over the grow (#1082).
        lv_obj_set_pos(obj, cur_x + old_size / 2 - size / 2, cur_y + old_size / 2 - size / 2);
    });
    lv_anim_start(&scale_anim);

    // Animation 2: Fade out
    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, ripple);
    lv_anim_set_values(&fade_anim, LV_OPA_50, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, duration_ms);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* var, int32_t opa) {
        lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(opa), 0);
    });
    lv_anim_set_completed_cb(&fade_anim, [](lv_anim_t* a) {
        // Delete ripple object when animation completes
        // Validate first — parent deletion may have already freed this widget
        lv_obj_t* widget = static_cast<lv_obj_t*>(a->var);
        if (widget && lv_obj_is_valid(widget)) {
            // Defer deletion — this callback fires inside lv_timer_handler,
            // and synchronous deletion corrupts LVGL's event linked list
            lv_obj_delete_async(widget);
        }
    });
    lv_anim_start(&fade_anim);
}

void create_touch_marker(lv_obj_t* parent, lv_coord_t x, lv_coord_t y) {
    if (!parent) {
        return;
    }

    // Marker geometry + timing. The dot holds fully visible for HOLD_MS, then
    // fades over FADE_MS — long enough to read (unlike the quick ripple), short
    // enough that successive presses each get their own clear dot (#1082).
    constexpr int DOT_SIZE = 18;
    constexpr int32_t HOLD_MS = 1500;
    constexpr int32_t FADE_MS = 600;

    // Expanding ripple — identical feedback to a normal touch, centered on (x, y).
    create_ripple(parent, x, y);

    // Lingering center dot at the exact same point.
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, theme_manager_get_color("danger"), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    // White ring keeps the dot legible over any background or crosshair color.
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dot, x - DOT_SIZE / 2, y - DOT_SIZE / 2);

    // Hold (anim delay), then fade out and self-delete.
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, dot);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&fade, HOLD_MS);
    lv_anim_set_duration(&fade, FADE_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade, [](void* var, int32_t opa) {
        // Fade the fill AND the white ring together so the whole dot disappears.
        auto* o = static_cast<lv_obj_t*>(var);
        lv_obj_set_style_bg_opa(o, static_cast<lv_opa_t>(opa), 0);
        lv_obj_set_style_border_opa(o, static_cast<lv_opa_t>(opa), 0);
    });
    lv_anim_set_completed_cb(&fade, [](lv_anim_t* a) {
        // Validate first — teardown may have already freed this widget. Defer the
        // delete: this fires inside lv_timer_handler and a sync delete corrupts
        // LVGL's event linked list.
        lv_obj_t* w = static_cast<lv_obj_t*>(a->var);
        if (w && lv_obj_is_valid(w)) {
            lv_obj_delete_async(w);
        }
    });
    lv_anim_start(&fade);
}

void flash_object(lv_obj_t* obj, int32_t duration_ms, bool force) {
    if (!obj) {
        return;
    }

    if (!force && !helix::DisplaySettingsManager::instance().get_animations_enabled()) {
        return;
    }

    // Flash bright red and scale up from center, then ease back to normal
    lv_obj_set_style_text_color(obj, lv_color_hex(0xFF3333), 0);

    // Set transform pivot to center so scaling is symmetrical
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);

    // Animate scale from 1.5x → 1.0x (256 = 100% in LVGL scale units)
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, obj);
    lv_anim_set_values(&scale_anim, 384, 256);
    lv_anim_set_duration(&scale_anim, duration_ms);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&scale_anim, [](void* var, int32_t scale) {
        auto* o = static_cast<lv_obj_t*>(var);
        lv_obj_set_style_transform_scale(o, scale, 0);
    });
    lv_anim_set_completed_cb(&scale_anim, [](lv_anim_t* a) {
        auto* o = static_cast<lv_obj_t*>(a->var);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_X, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_Y, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_PIVOT_X, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_PIVOT_Y, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(o, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
    });
    lv_anim_start(&scale_anim);
}

namespace {
lv_obj_t* s_always_on_top = nullptr;
} // namespace

void set_always_on_top(lv_obj_t* obj) {
    s_always_on_top = obj;
}

lv_obj_t* always_on_top() {
    return s_always_on_top;
}

void bring_to_front(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    lv_obj_move_foreground(obj);
    if (s_always_on_top && s_always_on_top != obj && lv_obj_is_valid(s_always_on_top) &&
        lv_obj_get_parent(s_always_on_top) == lv_obj_get_parent(obj)) {
        lv_obj_move_foreground(s_always_on_top);
    }
}

bool is_screen_chrome(const lv_obj_t* child) {
    return child && child == s_always_on_top;
}

lv_obj_t* create_fullscreen_backdrop(lv_obj_t* parent, lv_opa_t opacity) {
    if (!parent) {
        spdlog::error("[UI Effects] Cannot create backdrop: parent is null");
        return nullptr;
    }

    // Try blurred backdrop first (snapshot + blur + image widget)
    lv_obj_t* blurred = create_blurred_backdrop(parent, opacity);
    if (blurred) {
        spdlog::trace("[UI Effects] Created blurred backdrop with opacity {}", opacity);
        return blurred;
    }

    lv_obj_t* backdrop = create_dim_layer(parent, opacity);
    spdlog::trace("[UI Effects] Created plain backdrop with opacity {}", opacity);
    return backdrop;
}

void defocus_tree(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    lv_group_t* group = lv_group_get_default();
    if (!group) {
        return;
    }
    // Remove children first (bottom-up) to avoid focus shifts during traversal
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        defocus_tree(lv_obj_get_child(obj, i));
    }
    lv_group_remove_obj(obj);
}

} // namespace helix::ui
