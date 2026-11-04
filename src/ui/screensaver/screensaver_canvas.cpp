// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_canvas.h"

#include "lvgl/src/display/lv_display_private.h" // LV_INV_BUF_SIZE
#include "screensaver.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

static_assert(kSaverMaxDirtyAreas <= LV_INV_BUF_SIZE,
              "a saver frame's areas must fit LVGL's invalid-area buffer");

bool SaverCanvas::create(lv_obj_t* parent, int32_t w, int32_t h, lv_color_format_t cf) {
    // lv_canvas_set_buffer() steps rows by the aligned stride and lv_canvas_fill_bg() writes the
    // whole extent at once, so a tightly packed w * h * bytes-per-pixel buffer would under-run it.
    const uint32_t stride = screensaver_canvas_stride_bytes(w, cf);
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(h);
    auto* buf = static_cast<uint8_t*>(lv_malloc(size));
    if (!buf) {
        spdlog::error("[Screensaver] Failed to allocate a {}KB canvas buffer", size / 1024);
        return false;
    }
    buf_ = buf;
    buf_size_ = size;
    stride_ = stride;
    w_ = w;
    h_ = h;
    cf_ = cf;

    canvas_ = lv_canvas_create(parent);
    lv_obj_set_size(canvas_, w, h);
    lv_obj_set_pos(canvas_, 0, 0);
    lv_canvas_set_buffer(canvas_, buf_, w, h, cf);
    fill_black();
    return true;
}

void SaverCanvas::release() {
    // The canvas object lives until its parent's deferred deletion. Hidden first, no refresh or
    // snapshot before then draws from the buffer freed here.
    if (canvas_) {
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
        canvas_ = nullptr;
    }
    if (buf_) {
        lv_free(buf_);
        buf_ = nullptr;
    }
    buf_size_ = 0;
    stride_ = 0;
    w_ = 0;
    h_ = 0;
    cf_ = LV_COLOR_FORMAT_UNKNOWN;
    layer_dirty_.clear();
}

void SaverCanvas::fill_black() {
    if (canvas_) {
        lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    }
}

void SaverCanvas::invalidate(std::vector<DirtyRect>& areas) {
    if (!canvas_) {
        return;
    }
    reduce_dirty(areas, kSaverMaxDirtyAreas, w_, h_);
    if (areas.empty()) {
        return;
    }
    if (areas.size() == 1 && areas.front() == DirtyRect::whole(w_, h_)) {
        lv_obj_invalidate(canvas_);
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(canvas_, &coords);
    for (const DirtyRect& r : areas) {
        lv_area_t area = {r.x1, r.y1, r.x2, r.y2};
        lv_area_move(&area, coords.x1, coords.y1);
        lv_obj_invalidate_area(canvas_, &area);
    }
}

void SaverCanvas::begin_layer(lv_layer_t* layer) {
    lv_canvas_init_layer(canvas_, layer);
}

void SaverCanvas::mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    layer_dirty_.push_back({x1, y1, x2, y2});
}

void SaverCanvas::finish_layer(lv_layer_t* layer) {
    // The body of lv_canvas_finish_layer(), which ends by invalidating the whole canvas where a
    // scene step changes a few patches of it. Suppressing invalidation around
    // lv_canvas_finish_layer() is no substitute: the display's enable count is shared, and while
    // it is above 1 a single disable leaves invalidation on.
    if (layer->draw_task_head) {
        layer->all_tasks_added = true;
        lv_display_t* disp = lv_obj_get_display(canvas_);
        while (layer->draw_task_head) {
            lv_draw_dispatch_wait_for_request();
            if (!lv_draw_dispatch_layer(disp, layer)) {
                lv_draw_wait_for_finish();
                lv_draw_dispatch_request();
            }
        }
        lv_draw_unit_send_event(nullptr, LV_EVENT_SCREEN_LOAD_START, layer);
    }
    lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_DELETED, layer);

    invalidate(layer_dirty_);
    layer_dirty_.clear();
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
