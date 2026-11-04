// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_drag_surface.h"

namespace helix::ui {

void mark_drag_surface(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    lv_obj_add_flag(obj, kDragSurfaceFlag);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    // LVGL's scroll search stops at an object that neither scrolls nor chains, so a
    // drag starting here never reaches the home carousel's page strip.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    // A drag that leaves the surface keeps reporting to it until release.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

bool is_drag_surface_hit(lv_obj_t* target, lv_obj_t* stop_at) {
    for (lv_obj_t* obj = target; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kDragSurfaceFlag) || lv_obj_has_class(obj, &lv_arc_class) ||
            lv_obj_has_class(obj, &lv_slider_class)) {
            return true;
        }
        if (obj == stop_at) {
            break;
        }
    }
    return false;
}

} // namespace helix::ui
