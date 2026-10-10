// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_clog_bar.h"

#include "ui_update_queue.h"
#include "ui_utils.h"

#include "clog_meter_geometry.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

UiClogBar::UiClogBar(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[ClogBar] NULL parent");
        return;
    }

    root_ = lv_obj_find_by_name(parent, "clog_bar");
    track_ = root_ ? lv_obj_find_by_name(root_, "clog_bar_track") : nullptr;
    if (!root_ || !track_) {
        spdlog::warn("[ClogBar] clog_bar or clog_bar_track not found");
        root_ = nullptr;
        return;
    }

    fill_ = lv_obj_find_by_name(track_, "clog_bar_fill");
    marker_ = lv_obj_find_by_name(track_, "clog_bar_marker");
    peak_ = lv_obj_find_by_name(track_, "clog_bar_peak");
    danger_lo_ = lv_obj_find_by_name(track_, "clog_bar_danger_lo");
    danger_hi_ = lv_obj_find_by_name(track_, "clog_bar_danger_hi");
    threshold_lo_ = lv_obj_find_by_name(track_, "clog_bar_threshold_lo");
    threshold_hi_ = lv_obj_find_by_name(track_, "clog_bar_threshold_hi");

    // The track is flex_grow'd, so its width is only known after a layout pass.
    // SIZE_CHANGED is a layout event and has no declarative equivalent.
    // DECLARATIVE_OK: measured layout — every position here is derived from the
    // track's realised pixel width.
    lv_obj_add_event_cb(track_, on_track_size_changed, LV_EVENT_SIZE_CHANGED, this);

    model_.emplace([this](const ClogMeterSample&) { relayout(); });
    relayout();
    spdlog::debug("[ClogBar] Initialized");
}

UiClogBar::~UiClogBar() {
    // Freeze the queue around teardown so a background enqueue cannot land
    // between the drain and the observer resets, the same way UiClogMeter does.
    auto freeze = UpdateQueue::instance().scoped_freeze();
    UpdateQueue::instance().drain();

    helix::ui::remove_event_cb_if_alive(track_, on_track_size_changed, this);

    // Before the widget pointers below are cleared: its callback calls
    // relayout(), which reads them.
    model_.reset();

    root_ = nullptr;
    track_ = nullptr;
    fill_ = nullptr;
    marker_ = nullptr;
    peak_ = nullptr;
    danger_lo_ = nullptr;
    danger_hi_ = nullptr;
    threshold_lo_ = nullptr;
    threshold_hi_ = nullptr;
    spdlog::debug("[ClogBar] Destroyed");
}

void UiClogBar::on_track_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiClogBar*>(lv_event_get_user_data(e));
    if (self) {
        self->relayout();
    }
}

void UiClogBar::relayout() {
    if (!track_) {
        return;
    }

    // relayout() also runs from the track's SIZE_CHANGED, which can fire
    // before the model exists (the constructor lays out once to get a first
    // paint). An unset model reads as an all-zero sample, which draws nothing.
    const ClogMeterSample s = model_ ? model_->sample() : ClogMeterSample{};

    const int track_w = lv_obj_get_content_width(track_);
    const ClogBarGeometry g = clog_bar_geometry(s.mode, s.value, s.danger_pct, s.peak_pct, track_w);

    // A zero-width piece is hidden rather than drawn: LVGL still paints a
    // rounded 0px-wide object as a sliver of its radius.
    auto place = [](lv_obj_t* obj, int x, int w) {
        if (!obj) {
            return;
        }
        if (w <= 0) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(obj, w);
        lv_obj_set_x(obj, x);
    };

    place(danger_lo_, g.danger_lo_x, g.danger_lo_w);
    place(danger_hi_, g.danger_hi_x, g.danger_hi_w);
    place(fill_, g.fill_x, g.fill_w);

    // A rule where each shaded zone begins, on top of the fill. Only drawn
    // where that zone exists: a linear mode has no low end to mark, and a
    // threshold at either extreme would put the rule under the track's edge.
    place(threshold_lo_, g.danger_lo_x + g.danger_lo_w - kClogBarTickW,
          g.danger_lo_w > 0 ? kClogBarTickW : 0);
    place(threshold_hi_, g.danger_hi_x, g.danger_hi_w > 0 ? kClogBarTickW : 0);

    // The marker only means something once there is a fill to lead, and the
    // peak tick only once a worst-case has actually been recorded.
    place(marker_, g.marker_x, g.fill_w > 0 ? kClogBarTickW : 0);
    place(peak_, g.peak_x, s.peak_pct > 0 ? kClogBarTickW : 0);

    if (fill_) {
        // DECLARATIVE_OK: the indicator colour is a function of the live
        // reading; resolve_clog_tint() is the same rule the arc paints with.
        lv_obj_set_style_bg_color(fill_, resolve_clog_tint(s.mode, s.value, s.warning), 0);
    }
}

} // namespace helix::ui
