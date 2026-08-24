// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace {

/// Update gradient stops on each draw so the blend zone tracks the fill.
/// The blend zone is a fixed 15% of the full bar width, positioned at the
/// leading edge of the fill. Stops are on a 0-255 scale relative to full bar width.
void on_progress_bar_draw(lv_event_t* e) {
    auto* bar = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!bar) {
        return;
    }

    int32_t val = lv_bar_get_value(bar);
    int32_t min_val = lv_bar_get_min_value(bar);
    int32_t max_val = lv_bar_get_max_value(bar);
    int32_t range = max_val - min_val;
    if (range <= 0 || val <= min_val) {
        return;
    }

    float fill = static_cast<float>(val - min_val) / static_cast<float>(range);

    // Fixed 15% of full bar width as blend zone at the leading edge
    float blend_start = std::max(0.0f, fill - 0.15f);
    int32_t main_stop = static_cast<int32_t>(blend_start * 255.0f);
    int32_t grad_stop = static_cast<int32_t>(fill * 255.0f);

    main_stop = std::clamp<int32_t>(main_stop, 0, 254); // AUDIT OVERRIDE: int32_t=long on Xtensa
    grad_stop = std::clamp<int32_t>(grad_stop, main_stop + 1, 255); // AUDIT OVERRIDE

    // Only update if changed — setting styles during draw triggers invalidation
    int32_t cur_main = lv_obj_get_style_bg_main_stop(bar, LV_PART_INDICATOR);
    int32_t cur_grad = lv_obj_get_style_bg_grad_stop(bar, LV_PART_INDICATOR);
    if (cur_main == main_stop && cur_grad == grad_stop) {
        return;
    }

    // Suppress style refresh during draw phase to prevent lv_inv_area assertions.
    // Same pattern LVGL uses internally in lv_obj_class_create_obj() and lv_obj_delete().
    lv_obj_enable_style_refresh(false);
    lv_obj_set_style_bg_main_stop(bar, main_stop, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_stop(bar, grad_stop, LV_PART_INDICATOR);
    lv_obj_enable_style_refresh(true);
}

} // namespace

void ui_progress_bar_init() {
    lv_xml_register_event_cb(nullptr, "on_progress_bar_draw", on_progress_bar_draw);
    spdlog::debug("[progress_bar] Registered progress bar callbacks");
}
