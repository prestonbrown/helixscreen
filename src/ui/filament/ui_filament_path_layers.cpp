// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layered canvas machinery for the filament_path_canvas widget.
//
// The widget hosts one lv_canvas child backed by a cached ARGB8888 draw_buf
// holding the full topology render. LVGL composites it natively; the per-frame
// animation pass paints separately in DRAW_POST so animation ticks never
// repaint the heavyweight tube geometry. Setters call layered_mark_dirty(),
// which schedules layered_refresh() to (re)allocate the buffer on resize and
// repaint it.
// See ui_filament_path_internal.h for the full architecture.

#include "ui_filament_path_internal.h"

#include "lv_draw_buf_guard.h"

#include <spdlog/spdlog.h>

namespace helix::ui::fpath {

namespace {

// The topology renderers paint entry-lane segments above the widget's top edge
// (entry_y = y_off + height × -0.12, gestures up at the spool grid). The canvas
// child is extended above the widget by this much so absolute-coord
// draws land in the buffer instead of being clipped.
constexpr float kCanvasTopOverhangRatio = 0.15f;

int32_t layered_overhang(int32_t widget_h) {
    return LV_MAX(50, static_cast<int32_t>(widget_h * kCanvasTopOverhangRatio));
}

// A 1x1 transparent buffer the canvas points at while it owns no real one, so
// freeing the old buffer never leaves the canvas reading freed memory.
lv_draw_buf_t* empty_canvas_buf() {
    alignas(64) static uint8_t px[128] = {};
    static lv_draw_buf_t buf;
    if (buf.data == nullptr) {
        lv_draw_buf_init(&buf, 1, 1, LV_COLOR_FORMAT_ARGB8888, 0, px, sizeof(px));
    }
    return &buf;
}

void layered_destroy_buffers(FilamentPathData* data) {
    helix::safe_draw_buf_destroy(data->layers.overlay_buf, "fp_ovl");
}

constexpr int kAllocRetries = 5;

// Size the canvas buffer to w x h. Returns true when the canvas has a buffer of
// exactly that size.
//
// Reuses the current allocation when the new shape fits in it, so the resizes a
// panel goes through while its layout settles cost no allocation. Otherwise the
// old buffer is freed BEFORE the new one is allocated, so the peak is one buffer,
// not two. This all runs on the main thread outside the render pass, so no render
// can read the old buffer between the swap to the placeholder and its free.
bool layered_ensure_buffers(lv_obj_t* obj, FilamentPathData* data, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0)
        return false;
    LayerState& ls = data->layers;
    if (ls.canvas_w == w && ls.canvas_h == h && ls.overlay_buf) {
        return true;
    }

    if (ls.overlay_buf && lv_draw_buf_reshape(ls.overlay_buf, LV_COLOR_FORMAT_ARGB8888, w, h, 0)) {
        if (ls.overlay_canvas)
            lv_canvas_set_draw_buf(ls.overlay_canvas, ls.overlay_buf);
    } else {
        if (ls.overlay_canvas)
            lv_canvas_set_draw_buf(ls.overlay_canvas, empty_canvas_buf());
        layered_destroy_buffers(data);
        ls.canvas_w = 0;
        ls.canvas_h = 0;

        ls.overlay_buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
        if (!ls.overlay_buf) {
            // The canvas stays on the empty placeholder: drawing nothing beats
            // drawing a topology scaled for some other size. After the timed
            // retries, only a state change or resize asks again (one attempt per
            // coalesced refresh), and only the first failure of each run warns.
            const int kb = w * h * 4 / 1024;
            if (ls.alloc_retries_left > 0) {
                if (ls.alloc_retries_left == kAllocRetries)
                    spdlog::warn("[FilamentPath] No memory for a {}x{} path canvas ({} KB); "
                                 "retrying",
                                 w, h, kb);
                --ls.alloc_retries_left;
                ls.alloc_retry_timer.schedule_once([obj]() { layered_mark_dirty(obj); });
            } else if (ls.alloc_retries_left == 0) {
                spdlog::warn("[FilamentPath] No memory for a {}x{} path canvas ({} KB); giving up "
                             "until the next state change",
                             w, h, kb);
                ls.alloc_retries_left = -1;
            } else {
                spdlog::debug("[FilamentPath] No memory for a {}x{} path canvas ({} KB)", w, h, kb);
            }
            return false;
        }
        if (ls.overlay_canvas)
            lv_canvas_set_draw_buf(ls.overlay_canvas, ls.overlay_buf);
    }

    ls.alloc_retries_left = kAllocRetries;
    ls.canvas_w = w;
    ls.canvas_h = h;
    ls.overlay_dirty = true;
    return true;
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
    ++data->layers.render_count;
    refresh_theme_colors(data);
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
    if (!data || !data->layers.overlay_canvas)
        return;

    // A widget nobody can see is painted when it is first drawn, at whatever
    // size it has settled to by then, not at every size it passes through
    // while hidden.
    if (!lv_obj_is_visible(obj)) {
        data->layers.refresh_deferred = true;
        return;
    }
    data->layers.refresh_deferred = false;

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0)
        return; // layout not finished yet — SIZE_CHANGED will retry

    int32_t overhang = layered_overhang(h);
    int32_t total_h = h + overhang;

    if (w != data->layers.canvas_w || total_h != data->layers.canvas_h) {
        if (!layered_ensure_buffers(obj, data, w, total_h))
            return;
        lv_obj_set_size(data->layers.overlay_canvas, w, total_h);
        lv_obj_set_pos(data->layers.overlay_canvas, 0, -overhang);
        // Force layout recompute so canvas's content_coords reflect the new
        // size immediately (otherwise the canvas's own coords stay stale
        // until LVGL's next layout pass — would clip subsequent draws).
        lv_obj_update_layout(data->layers.overlay_canvas);
    }

    if (data->layers.overlay_dirty) {
        layered_render_overlay(obj, data);
        data->layers.overlay_dirty = false;
    }
}

} // namespace

// Mark the canvas for a repaint and schedule an async refresh.
// Use this from setters instead of bare `lv_obj_invalidate(obj)` so the
// dirty flag gets set before refresh runs AND the canvas refresh actually
// gets scheduled. When layered is off (no canvas), this just performs a
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
void layered_mark_dirty(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (data) {
        data->layers.overlay_dirty = true;
        // The active-path cache becomes stale on any state change that could
        // affect lane geometry — flag it for refresh on next state-tied draw.
        data->path_cache.valid = false;
        if (data->layers.overlay_canvas)
            data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
    }
    lv_obj_invalidate(obj);
}

void layered_schedule_repaint(lv_obj_t* obj, FilamentPathData* data) {
    data->layers.overlay_dirty = true;
    if (data->layers.overlay_canvas)
        data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
}

void layered_release_buffer(FilamentPathData* data) {
    LayerState& ls = data->layers;
    ls.refresh_timer.cancel();
    ls.alloc_retry_timer.cancel();
    if (ls.overlay_canvas)
        lv_canvas_set_draw_buf(ls.overlay_canvas, empty_canvas_buf());
    layered_destroy_buffers(data);
    ls.canvas_w = 0;
    ls.canvas_h = 0;
    ls.overlay_dirty = true;
    ls.refresh_deferred = true;
}

void layered_on_draw(lv_obj_t* obj, FilamentPathData* data) {
    if (data->layers.refresh_deferred)
        data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
}

// Create the canvas child, configure styles, schedule the first render. The
// canvas starts on the empty placeholder: its buffer is allocated by the first
// refresh that sees the widget's laid-out size.
namespace {

// REFR_EXT_DRAW_SIZE on the widget and on its container: room for the
// overlay canvas's overhang above the widget (user data = the widget).
void overhang_ext_draw_cb(lv_event_t* e) {
    auto* widget = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_event_set_ext_draw_size(e, layered_overhang(lv_obj_get_height(widget)));
}

} // namespace

bool layered_setup_canvases(lv_obj_t* obj, FilamentPathData* data) {
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    if (w <= 0)
        w = kDefaultWidth;
    if (h <= 0)
        h = kDefaultHeight;
    int32_t overhang = layered_overhang(h);
    int32_t total_h = h + overhang;

    // Canvases extend above the widget, into the spool box above it: LVGL
    // draws a child outside its parent only within the parent's ext draw size,
    // so the widget and its (overflow-visible) container both report the
    // overhang.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj, overhang_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, obj);
    if (lv_obj_t* parent = lv_obj_get_parent(obj))
        lv_obj_add_event_cb(parent, overhang_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, obj);

    lv_obj_t* c = lv_canvas_create(obj);
    if (!c) {
        return false;
    }
    data->layers.overlay_canvas = c;
    data->layers.alloc_retries_left = kAllocRetries;

    lv_obj_set_size(c, w, total_h);
    lv_obj_set_pos(c, 0, -overhang);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_draw_buf(c, empty_canvas_buf());

    // Schedule initial render — layout may not be complete yet at create
    // time; the deferred callback retries when layout has settled.
    data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
    return true;
}

// The initial refresh scheduled at create time runs before layout assigns the
// widget a real size; layered_refresh() then early-returns on its w<=0 guard
// and nothing else retries it, leaving the canvas permanently blank. When
// layout finally gives the widget a non-zero size, re-mark the canvas dirty
// and re-schedule the refresh so it paints. layered_refresh() handles the
// canvas buffer (re)allocation for the new size itself.
void layered_size_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    if (lv_obj_get_width(obj) <= 0 || lv_obj_get_height(obj) <= 0)
        return;
    lv_obj_refresh_ext_draw_size(obj);
    if (lv_obj_t* parent = lv_obj_get_parent(obj))
        lv_obj_refresh_ext_draw_size(parent);
    layered_mark_dirty(obj);
}

// Widget teardown: cancel any pending refresh (it would fire with a stale obj)
// and free the canvas buffer. The lv_canvas child itself is deleted by
// LVGL as the parent tears down. ~LayerState cancels the timers too — this is the
// explicit half of the pair, so teardown order stays readable at the call site.
void layered_teardown(lv_obj_t* obj, FilamentPathData* data) {
    if (lv_obj_t* parent = lv_obj_get_parent(obj))
        lv_obj_remove_event_cb_with_user_data(parent, overhang_ext_draw_cb, obj);
    data->layers.refresh_timer.cancel();
    data->layers.alloc_retry_timer.cancel();
    layered_destroy_buffers(data);
    data->layers.overlay_canvas = nullptr;
}

} // namespace helix::ui::fpath
