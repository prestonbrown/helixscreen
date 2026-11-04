// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_overlay.h"

#include "ui_utils.h"

namespace helix::ui {

void SaverOverlay::create() {
    if (obj_) {
        return;
    }
    obj_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(obj_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj_, 0, 0);
    lv_obj_set_style_pad_all(obj_, 0, 0);
    lv_obj_set_style_radius(obj_, 0, 0);
    // Clickable, so the waking touch lands here rather than on the panel beneath; LVGL still
    // counts it as activity.
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
}

void SaverOverlay::make_transparent() {
    if (obj_) {
        lv_obj_set_style_bg_opa(obj_, LV_OPA_TRANSP, 0);
    }
}

void SaverOverlay::destroy() {
    // A saver stops from the main loop's wake path, next to lv_timer_handler ticks, where a
    // synchronous delete of an overlay with children corrupts LVGL's event list (#316).
    safe_delete_deferred(obj_);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
