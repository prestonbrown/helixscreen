// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_progress_bar.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

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

    main_stop = std::clamp<int32_t>(main_stop, 0, 254);
    grad_stop = std::clamp<int32_t>(grad_stop, main_stop + 1, 255);

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

/// Where the bar's fill ends, in px: everything right of it is the unfilled span.
int32_t fill_end_x(lv_obj_t* bar, const lv_area_t& track) {
    const int32_t min_val = lv_bar_get_min_value(bar);
    const int32_t range = lv_bar_get_max_value(bar) - min_val;
    const int32_t val =
        std::clamp<int32_t>(lv_bar_get_value(bar) - min_val, 0, std::max<int32_t>(range, 0));
    if (range <= 0) {
        return track.x1;
    }
    return track.x1 +
           static_cast<int32_t>(static_cast<int64_t>(lv_area_get_width(&track)) * val / range);
}

/// Keeps an estimate bar redrawing while it is on screen with an unfilled span
/// to sweep. It stops itself otherwise, and the next draw that has something to
/// sweep starts it again, so nothing ticks for a bar nobody can see or a full
/// one. LVGL deletes it with the bar.
void sweep_tick(void* var, int32_t /*value*/) {
    auto* bar = static_cast<lv_obj_t*>(var);
    if (!lv_obj_is_visible(bar) || lv_bar_get_value(bar) >= lv_bar_get_max_value(bar)) {
        lv_anim_delete(bar, sweep_tick);
        lv_obj_invalidate(bar);
        return;
    }
    lv_obj_invalidate(bar);
}

void ensure_sweep_running(lv_obj_t* bar) {
    if (lv_anim_get(bar, sweep_tick)) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, sweep_tick);
    // A value per millisecond: LVGL calls the exec callback only when the value
    // changes, so a coarser range would redraw a few times a pass.
    lv_anim_set_values(&a, 0, static_cast<int32_t>(helix::ui::kProgressSweepPeriodMs));
    lv_anim_set_duration(&a, helix::ui::kProgressSweepPeriodMs);
    // Started from inside a draw; applying the first value now would invalidate
    // the bar mid-render.
    lv_anim_set_early_apply(&a, false);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/// The estimate bar's liveness cue: a soft band of the theme text colour that
/// sweeps the unfilled span, left to right, pass after pass. The value is an
/// estimate that can sit still through a long phase, so the motion has to come
/// from the bar itself; the fill is left alone because it is what has been done.
void on_progress_bar_draw_sweep(lv_event_t* e) {
    auto* bar = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!bar || !layer) {
        return;
    }

    lv_area_t track;
    lv_obj_get_coords(bar, &track);
    const int32_t span_x1 = fill_end_x(bar, track);
    const helix::ui::SweepBand band =
        helix::ui::progress_sweep_band(span_x1, track.x2 - span_x1 + 1, lv_tick_get());
    if (band.x2 < band.x1) {
        return;
    }

    if (band.strength > 0) {
        const lv_area_t area{band.x1, track.y1, band.x2, track.y2};
        // The theme's text colour reads on the track in light and dark themes alike.
        const lv_color_t c = theme_manager_get_color("text");
        const auto peak = static_cast<lv_opa_t>(LV_OPA_30 * band.strength / 255);
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = lv_obj_get_style_radius(bar, LV_PART_MAIN);
        dsc.bg_grad.dir = LV_GRAD_DIR_HOR;
        dsc.bg_grad.stops_count = 3;
        dsc.bg_grad.stops[0] = {c, LV_OPA_TRANSP, 0};
        dsc.bg_grad.stops[1] = {c, peak, 128};
        dsc.bg_grad.stops[2] = {c, LV_OPA_TRANSP, 255};
        dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &dsc, &area);
    }

    ensure_sweep_running(bar);
}

} // namespace

void ui_progress_bar_init() {
    lv_xml_register_event_cb(nullptr, "on_progress_bar_draw", on_progress_bar_draw);
    lv_xml_register_event_cb(nullptr, "on_progress_bar_draw_sweep", on_progress_bar_draw_sweep);
    spdlog::debug("[progress_bar] Registered progress bar callbacks");
}
