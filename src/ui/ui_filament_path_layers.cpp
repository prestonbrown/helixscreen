// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layered canvas machinery for the filament_path_canvas widget.
//
// The widget hosts two lv_canvas children (static topology + state overlay)
// backed by cached ARGB8888 draw_bufs. LVGL composites them natively; the
// per-frame animation pass paints separately in DRAW_POST so animation ticks
// never repaint the heavyweight tube geometry. Setters mark static_dirty /
// overlay_dirty via layered_mark_dirty(), which schedules layered_refresh() to
// (re)allocate buffers on resize and repaint whichever surfaces are dirty.
// See ui_filament_path_internal.h for the full architecture.

#include "ui_filament_path_internal.h"

#include <spdlog/spdlog.h>

namespace helix::ui::fpath {

namespace {

// The topology renderers paint entry-lane segments above the widget's top edge
// (entry_y = y_off + height × -0.12, gestures up at the spool grid). Canvas
// children are extended above the widget by this much so absolute-coord
// draws land in the buffer instead of being clipped.
constexpr float CANVAS_TOP_OVERHANG_RATIO = 0.15f;

int32_t layered_overhang(int32_t widget_h) {
    return LV_MAX(50, static_cast<int32_t>(widget_h * CANVAS_TOP_OVERHANG_RATIO));
}

void layered_destroy_buffers(FilamentPathData* data) {
    if (data->layers.static_buf) {
        lv_draw_buf_destroy(data->layers.static_buf);
        data->layers.static_buf = nullptr;
    }
    if (data->layers.overlay_buf) {
        lv_draw_buf_destroy(data->layers.overlay_buf);
        data->layers.overlay_buf = nullptr;
    }
}

// (Re)allocate canvas buffers to match widget dims. Returns true on success.
// Buffers swapped in BEFORE destroying old ones — `lv_canvas_set_draw_buf`
// reads the old header to drop the cached image source.
bool layered_ensure_buffers(FilamentPathData* data, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0)
        return false;
    if (data->layers.canvas_w == w && data->layers.canvas_h == h && data->layers.static_buf &&
        data->layers.overlay_buf) {
        return true;
    }

    auto* new_static = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    auto* new_overlay = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!new_static || !new_overlay) {
        if (new_static)
            lv_draw_buf_destroy(new_static);
        if (new_overlay)
            lv_draw_buf_destroy(new_overlay);
        return false;
    }

    auto* old_static = data->layers.static_buf;
    auto* old_overlay = data->layers.overlay_buf;
    data->layers.static_buf = new_static;
    data->layers.overlay_buf = new_overlay;
    if (data->layers.static_canvas)
        lv_canvas_set_draw_buf(data->layers.static_canvas, new_static);
    if (data->layers.overlay_canvas)
        lv_canvas_set_draw_buf(data->layers.overlay_canvas, new_overlay);
    if (old_static)
        lv_draw_buf_destroy(old_static);
    if (old_overlay)
        lv_draw_buf_destroy(old_overlay);

    data->layers.canvas_w = w;
    data->layers.canvas_h = h;
    data->layers.static_dirty = true;
    data->layers.overlay_dirty = true;
    return true;
}

// Static layer renderer — idle topology only. Currently clears transparent;
// per-topology static painting can move here once split out of the overlay.
void layered_render_static(lv_obj_t* /*obj*/, FilamentPathData* data) {
    if (!data->layers.static_canvas)
        return;
    lv_canvas_fill_bg(data->layers.static_canvas, lv_color_black(), LV_OPA_TRANSP);
}

// Build buf_area covering the widget bounds + top overhang. The canvas
// widget's own coords may be stale right after `lv_obj_set_size` (layout
// recompute is deferred), so derive the screen-space area from the widget
// instead — the underlying draw_buf was sized to match.
lv_area_t layered_compute_buf_area(lv_obj_t* obj, int32_t overhang) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    a.y1 -= overhang;
    return a;
}

// Overlay layer renderer — the full state-tied topology content.
void layered_render_overlay(lv_obj_t* obj, FilamentPathData* data) {
    if (!data->layers.overlay_canvas)
        return;
    lv_canvas_fill_bg(data->layers.overlay_canvas, lv_color_black(), LV_OPA_TRANSP);

    int32_t overhang = layered_overhang(lv_obj_get_height(obj));
    lv_area_t buf_area = layered_compute_buf_area(obj, overhang);

    lv_layer_t layer;
    lv_canvas_init_layer(data->layers.overlay_canvas, &layer);
    // Override buf_area so the topology renderer's absolute-display-coord draws
    // map to the right buffer pixels (LVGL maps pixel = coord - buf_area.x1).
    // Default `lv_canvas_init_layer` uses buffer-local (0,0)→(w,h), which
    // clips anything outside that range when fed absolute screen coords.
    layer.buf_area = buf_area;
    layer._clip_area = buf_area;
    layer.phy_clip_area = buf_area;
    render_overlay_content(obj, &layer, data);
    lv_canvas_finish_layer(data->layers.overlay_canvas, &layer);
}

// Deferred refresh — runs OUTSIDE the LVGL render pass, so it's safe to call
// lv_canvas_init_layer / finish_layer (which invalidate the canvas, illegal
// during rendering). Scheduled through LayerState::refresh_timer; see
// layered_mark_dirty() below for how a burst of setters collapses onto it.
void layered_refresh(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data || !data->layers.static_canvas)
        return;

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0)
        return; // layout not finished yet — SIZE_CHANGED will retry

    int32_t overhang = layered_overhang(h);
    int32_t total_h = h + overhang;

    if (w != data->layers.canvas_w || total_h != data->layers.canvas_h) {
        if (!layered_ensure_buffers(data, w, total_h))
            return;
        lv_obj_set_size(data->layers.static_canvas, w, total_h);
        lv_obj_set_size(data->layers.overlay_canvas, w, total_h);
        lv_obj_set_pos(data->layers.static_canvas, 0, -overhang);
        lv_obj_set_pos(data->layers.overlay_canvas, 0, -overhang);
        // Force layout recompute so canvas's content_coords reflect the new
        // size immediately (otherwise the canvas's own coords stay stale
        // until LVGL's next layout pass — would clip subsequent draws).
        lv_obj_update_layout(data->layers.static_canvas);
        lv_obj_update_layout(data->layers.overlay_canvas);
    }

    if (data->layers.static_dirty) {
        layered_render_static(obj, data);
        data->layers.static_dirty = false;
    }
    if (data->layers.overlay_dirty) {
        layered_render_overlay(obj, data);
        data->layers.overlay_dirty = false;
    }
}

} // namespace

// Mark which layered surfaces need a repaint and schedule an async refresh.
// Use this from setters instead of bare `lv_obj_invalidate(obj)` so the
// dirty flags get set before refresh runs AND the canvas refresh actually
// gets scheduled. When layered is off (no canvases), this just performs a
// widget invalidate.
//
// LV_EVENT_INVALIDATE_AREA is a display-level event (not dispatched to
// objects), so we cannot piggyback on lv_obj_invalidate to schedule canvas
// refresh — we schedule it directly. schedule_once() drops the request when a
// refresh is already pending, so the ten setters a state update pushes in one
// tick arm one timer and repaint once. (lv_async_call would not: it allocates
// an info struct and a timer per call, with no dedup on cb+user_data.)
//
// Animation callbacks call lv_obj_invalidate(obj) directly without going
// through this helper — their per-frame paint happens via the DRAW_POST
// animation overlay; no canvas content changed, so no refresh is needed.
void layered_mark_dirty(lv_obj_t* obj, bool static_dirty, bool overlay_dirty) {
    auto* data = get_data(obj);
    if (data) {
        if (static_dirty)
            data->layers.static_dirty = true;
        if (overlay_dirty)
            data->layers.overlay_dirty = true;
        // The active-path cache becomes stale on any state change that could
        // affect lane geometry — flag it for refresh on next state-tied draw.
        if (overlay_dirty)
            data->path_cache.valid = false;
        if (data->layers.static_canvas)
            data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
    }
    lv_obj_invalidate(obj);
}

// Create the two canvas children, configure styles, schedule the first render.
bool layered_setup_canvases(lv_obj_t* obj, FilamentPathData* data) {
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0)
        w = DEFAULT_WIDTH;
    if (h <= 0)
        h = DEFAULT_HEIGHT;
    int32_t overhang = layered_overhang(h);
    int32_t total_h = h + overhang;

    if (!layered_ensure_buffers(data, w, total_h))
        return false;

    // Canvases extend above the widget — needs OVERFLOW_VISIBLE on parent so
    // LVGL doesn't clip them to the widget's bounds.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    data->layers.static_canvas = lv_canvas_create(obj);
    data->layers.overlay_canvas = lv_canvas_create(obj);
    if (!data->layers.static_canvas || !data->layers.overlay_canvas) {
        if (data->layers.static_canvas)
            lv_obj_delete(data->layers.static_canvas);
        if (data->layers.overlay_canvas)
            lv_obj_delete(data->layers.overlay_canvas);
        data->layers.static_canvas = nullptr;
        data->layers.overlay_canvas = nullptr;
        layered_destroy_buffers(data);
        return false;
    }

    for (auto* c : {data->layers.static_canvas, data->layers.overlay_canvas}) {
        lv_obj_set_size(c, w, total_h);
        lv_obj_set_pos(c, 0, -overhang);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_canvas_set_draw_buf(data->layers.static_canvas, data->layers.static_buf);
    lv_canvas_set_draw_buf(data->layers.overlay_canvas, data->layers.overlay_buf);
    lv_canvas_fill_bg(data->layers.static_canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_canvas_fill_bg(data->layers.overlay_canvas, lv_color_black(), LV_OPA_TRANSP);

    // Schedule initial render — layout may not be complete yet at create
    // time; the deferred callback retries when layout has settled.
    data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
    return true;
}

// The initial refresh scheduled at create time runs before layout assigns the
// widget a real size; layered_refresh() then early-returns on its w<=0 guard
// and nothing else retries it, leaving the canvases permanently blank. When
// layout finally gives the widget a non-zero size, re-mark both layers dirty
// and re-schedule the refresh so it paints. layered_refresh() handles the
// canvas buffer (re)allocation for the new size itself.
void layered_size_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    if (lv_obj_get_width(obj) <= 0 || lv_obj_get_height(obj) <= 0)
        return;
    layered_mark_dirty(obj, true, true);
}

// Widget teardown: cancel any pending refresh (it would fire with a stale obj)
// and free the canvas buffers. The lv_canvas children themselves are deleted by
// LVGL as the parent tears down. ~LayerState cancels the timer too — this is the
// explicit half of the pair, so teardown order stays readable at the call site.
void layered_teardown(lv_obj_t* obj, FilamentPathData* data) {
    LV_UNUSED(obj);
    data->layers.refresh_timer.cancel();
    layered_destroy_buffers(data);
    data->layers.static_canvas = nullptr;
    data->layers.overlay_canvas = nullptr;
}

} // namespace helix::ui::fpath
