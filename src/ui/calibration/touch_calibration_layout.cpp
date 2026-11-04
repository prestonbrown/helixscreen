// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch_calibration_layout.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

CaptureSurfaceWidgets reparent_capture_surface_fullscreen(lv_obj_t* screen_root) {
    CaptureSurfaceWidgets w;
    if (!screen_root) {
        return w;
    }

    lv_obj_t* screen = lv_screen_active();

    // Capture surface first, crosshair second, so the crosshair (a passive,
    // non-clickable marker) ends up above the invisible capture surface.
    w.capture_overlay = lv_obj_find_by_name(screen_root, "touch_capture_overlay");
    if (w.capture_overlay) {
        w.capture_original_parent = lv_obj_get_parent(w.capture_overlay);
        lv_obj_set_parent(w.capture_overlay, screen);
        lv_obj_set_size(w.capture_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(w.capture_overlay, 0, 0);
        lv_obj_add_flag(w.capture_overlay, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(w.capture_overlay);
    } else {
        spdlog::warn("[TouchCalLayout] touch_capture_overlay not found");
    }

    w.crosshair = lv_obj_find_by_name(screen_root, "crosshair");
    if (w.crosshair) {
        w.crosshair_original_parent = lv_obj_get_parent(w.crosshair);
        lv_obj_set_parent(w.crosshair, screen);
        lv_obj_add_flag(w.crosshair, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(w.crosshair);
    } else {
        spdlog::warn("[TouchCalLayout] crosshair not found");
    }

    return w;
}

RaisedControl raise_control_above_capture(lv_obj_t* search_root, const char* name) {
    RaisedControl r;
    if (!search_root || !name) {
        return r;
    }

    r.obj = lv_obj_find_by_name(search_root, name);
    if (!r.obj) {
        return r;
    }

    // Coordinates must be settled before we snapshot the on-screen rect.
    lv_obj_update_layout(lv_screen_active());

    r.original_parent = lv_obj_get_parent(r.obj);
    r.orig_w = lv_obj_get_style_width(r.obj, LV_PART_MAIN);
    r.orig_h = lv_obj_get_style_height(r.obj, LV_PART_MAIN);

    lv_area_t area;
    lv_obj_get_coords(r.obj, &area);

    lv_obj_set_parent(r.obj, lv_screen_active());
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(r.obj, area.x1, area.y1);
    lv_obj_set_size(r.obj, lv_area_get_width(&area), lv_area_get_height(&area));
    lv_obj_move_foreground(r.obj);

    return r;
}

void restore_raised_control(const RaisedControl& r) {
    if (!r.obj || !r.original_parent) {
        return;
    }
    lv_obj_set_parent(r.obj, r.original_parent);
    lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(r.obj, 0, 0);
    lv_obj_set_size(r.obj, r.orig_w, r.orig_h);
}

} // namespace helix::ui
