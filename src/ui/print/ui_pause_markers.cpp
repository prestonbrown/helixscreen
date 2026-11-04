// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pause_markers.h"

#include "core/observer_factory.h"
#include "gcode_pause_scan.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace helix::ui {

namespace {

/// Per-widget attachment: what the draw hook and the version observer need.
/// Heap-owned so LVGL's single user_data slot can carry it; freed on the
/// widget's LV_EVENT_DELETE together with its observer.
struct PauseMarkerCtx {
    helix::PrinterState* ps{nullptr};
    lv_obj_t* owner{nullptr};
    ObserverGuard version_guard;
};

void pause_markers_invalidate(PauseMarkerCtx* ctx, int /*version*/) {
    // Any change to the pause list (publish or clear) must re-render the
    // ticks; the bar's own value binding says nothing about the markers.
    if (ctx && ctx->owner) {
        lv_obj_invalidate(ctx->owner);
    }
}

/// Every tick to draw, as fractions along the bar: the pauses in the file and
/// the pause scheduled at runtime. Both pass the identity gate inside
/// PrinterPrintState, so a scan that finished after a print switch is
/// invisible.
std::vector<float> marker_fractions(PauseMarkerCtx* ctx) {
    std::vector<float> fractions;
    if (!ctx || !ctx->ps) {
        return fractions;
    }
    const auto& print = ctx->ps->print_state();
    if (print.pause_markers_match_current_file()) {
        const auto axis = print.get_pause_marker_axis();
        for (const auto& pause : print.get_scheduled_pauses()) {
            fractions.push_back(helix::gcode::display_fraction(pause, axis));
        }
    }
    if (auto runtime = print.layer_pause_marker_fraction()) {
        fractions.push_back(*runtime);
    }
    return fractions;
}

void fill_tick_dsc(lv_draw_line_dsc_t& dsc) {
    lv_draw_line_dsc_init(&dsc);
    dsc.color = theme_manager_get_color("warning");
    dsc.width = 2;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
}

// DECLARATIVE_OK: draw hook — no declarative equivalent exists. A tick at the
// pause's fraction spans the bar's height so it reads against both the track
// and the fill that may already cover it.
void bar_pause_marker_draw_cb(lv_event_t* e) {
    const auto fractions =
        marker_fractions(static_cast<PauseMarkerCtx*>(lv_event_get_user_data(e)));
    if (fractions.empty()) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* bar = lv_event_get_target_obj(e);
    if (!layer || !bar) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(bar, &coords);
    const int32_t pad_left = lv_obj_get_style_pad_left(bar, LV_PART_MAIN);
    const int32_t pad_right = lv_obj_get_style_pad_right(bar, LV_PART_MAIN);
    const int32_t track_width = lv_area_get_width(&coords) - pad_left - pad_right;
    if (track_width <= 0) {
        return;
    }

    lv_draw_line_dsc_t dsc;
    fill_tick_dsc(dsc);
    for (const float fraction : fractions) {
        dsc.p1.x =
            static_cast<float>(coords.x1 + pad_left + static_cast<int32_t>(fraction * track_width));
        dsc.p1.y = static_cast<float>(coords.y1);
        dsc.p2.x = dsc.p1.x;
        dsc.p2.y = static_cast<float>(coords.y2);
        lv_draw_line(layer, &dsc);
    }
}

// DECLARATIVE_OK: draw hook — no declarative equivalent exists. A tick is a
// short radial line crossing the track at start + fraction * sweep, so it
// stays legible over both the unfilled track and the filled arc.
void arc_pause_marker_draw_cb(lv_event_t* e) {
    const auto fractions =
        marker_fractions(static_cast<PauseMarkerCtx*>(lv_event_get_user_data(e)));
    if (fractions.empty()) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* arc = lv_event_get_target_obj(e);
    if (!layer || !arc) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(arc, &coords);
    const int32_t half_w = lv_area_get_width(&coords) / 2;
    const int32_t half_h = lv_area_get_height(&coords) / 2;
    if (half_w <= 0 || half_h <= 0) {
        return;
    }
    const float cx = static_cast<float>(coords.x1 + half_w);
    const float cy = static_cast<float>(coords.y1 + half_h);

    const int32_t arc_width = lv_obj_get_style_arc_width(arc, LV_PART_MAIN);
    const int32_t track_radius = std::min(half_w, half_h) - arc_width / 2;
    // Flush with the track's outer edge, overhanging inward — an outward
    // overhang would clip against the widget at the dial's extremes.
    const int32_t tick_inner = track_radius - arc_width / 2 - 2;
    const int32_t tick_outer = track_radius + arc_width / 2;
    if (tick_inner <= 0) {
        return;
    }

    const float start = lv_arc_get_bg_angle_start(arc);
    float sweep = std::fmod(lv_arc_get_bg_angle_end(arc) - start + 360.0f, 360.0f);
    if (sweep < 0.0f) {
        sweep += 360.0f;
    } // 270 for the dial
    if (sweep <= 0.0f) {
        return;
    }

    lv_draw_line_dsc_t dsc;
    fill_tick_dsc(dsc);
    for (const float fraction : fractions) {
        // LVGL angles: 0deg at 3 o'clock, growing clockwise, y axis down —
        // plain cos/sin with +angle lands where lv_arc draws it.
        const float angle =
            std::fmod(start + fraction * sweep, 360.0f) * (static_cast<float>(M_PI) / 180.0f);
        const float cos_a = std::cos(angle);
        const float sin_a = std::sin(angle);
        dsc.p1.x = cx + cos_a * static_cast<float>(tick_inner);
        dsc.p1.y = cy + sin_a * static_cast<float>(tick_inner);
        dsc.p2.x = cx + cos_a * static_cast<float>(tick_outer);
        dsc.p2.y = cy + sin_a * static_cast<float>(tick_outer);
        lv_draw_line(layer, &dsc);
    }
}

void pause_marker_delete_cb(lv_event_t* e) {
    auto* ctx = static_cast<PauseMarkerCtx*>(lv_event_get_user_data(e));
    if (!ctx) {
        return;
    }
    // Drop the observer before freeing the ctx it hands to its handler.
    // The subject itself outlives the widget (owned by PrinterPrintState).
    ctx->version_guard.reset();
    delete ctx;
}

/// Shared attach: one heap ctx (LVGL user_data), one DRAW_POST hook, one
/// observer on the pause-list version subject for invalidation.
void attach_pause_markers(lv_obj_t* widget, helix::PrinterState& printer_state,
                          lv_event_cb_t draw_cb) {
    if (!widget) {
        return;
    }
    auto* ctx = new PauseMarkerCtx{};
    ctx->ps = &printer_state;
    ctx->owner = widget;
    ctx->version_guard = observe<int>(
        printer_state.print_state().get_pause_markers_version_subject(), ctx,
        [](PauseMarkerCtx* self, int version) { pause_markers_invalidate(self, version); },
        printer_state.get_subjects_lifetime(), Dispatch::Immediate);
    lv_obj_add_event_cb(widget, draw_cb, LV_EVENT_DRAW_POST, ctx);
    lv_obj_add_event_cb(widget, pause_marker_delete_cb, LV_EVENT_DELETE, ctx);
    lv_obj_invalidate(widget);
}

} // anonymous namespace

void attach_bar_pause_markers(lv_obj_t* bar, helix::PrinterState& printer_state) {
    attach_pause_markers(bar, printer_state, bar_pause_marker_draw_cb);
}

void attach_arc_pause_markers(lv_obj_t* arc, helix::PrinterState& printer_state) {
    attach_pause_markers(arc, printer_state, arc_pause_marker_draw_cb);
}

} // namespace helix::ui
