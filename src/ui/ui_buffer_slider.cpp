// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_buffer_slider.h"

#include "ui_timer_guard.h"

#include "ams_state.h"
#include "buffer_reading.h"
#include "buffer_slider_geometry.h"
#include "observer_factory.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

namespace {
constexpr uint32_t kTraceRedrawMs = 1000;

lv_point_precise_t point(int32_t x, int32_t y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

/// @p b, in the slider's own pixels, placed in the content area @p a.
lv_area_t area(const lv_area_t& a, const BufferBox& b) {
    return {a.x1 + b.x, a.y1 + b.y, a.x1 + b.x + b.w - 1, a.y1 + b.y + b.h - 1};
}
} // namespace

UiBufferSlider::UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj, int trace_unit)
    : slider_obj_(slider_obj), trace_obj_(trace_obj), trace_unit_(trace_unit) {
    if (!slider_obj_) {
        spdlog::error("[BufferSlider] No object to draw the slider into");
        trace_obj_ = nullptr;
        return;
    }
    // Draw and delete hooks only: XML authors and sizes both objects.
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN,
                                this); // DECLARATIVE_OK: draw hook has no XML form
            lv_obj_add_event_cb(obj, on_deleted, LV_EVENT_DELETE,
                                this); // DECLARATIVE_OK: delete hook has no XML form
        }
    }
    if (trace_obj_) {
        trace_timer_.reset(lv_timer_create(on_trace_timer, kTraceRedrawMs, this));
    }
}

void UiBufferSlider::follow_system_reading() {
    auto& ams = AmsState::instance();
    const auto lifetime = ams.get_subjects_lifetime();
    // Immediate: the handlers only store the reading and invalidate.
    bias_observer_ = observe<int>(
        ams.get_buffer_bias_pct_subject(), this,
        [](UiBufferSlider* self, int pct) { self->set_reading(pct / 100.0f, self->status_); },
        lifetime, Dispatch::Immediate);
    status_observer_ = observe<int>(
        ams.get_buffer_status_subject(), this,
        [](UiBufferSlider* self, int status) {
            self->set_reading(self->bias_, static_cast<ClogMeterStatus>(status));
        },
        lifetime, Dispatch::Immediate);
}

UiBufferSlider::~UiBufferSlider() {
    bias_observer_.reset();
    status_observer_.reset();
    trace_timer_.reset();
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_remove_event_cb_with_user_data(obj, on_draw, this);
            lv_obj_remove_event_cb_with_user_data(obj, on_deleted, this);
        }
    }
}

void UiBufferSlider::set_reading(float bias, ClogMeterStatus status) {
    bias_ = bias;
    status_ = status;
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_invalidate(obj);
        }
    }
}

void UiBufferSlider::on_draw(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!self || !layer) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->draw_slider(layer);
    } else if (target == self->trace_obj_) {
        self->draw_trace(layer);
    }
}

void UiBufferSlider::on_deleted(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->slider_obj_ = nullptr;
    }
    if (target == self->trace_obj_) {
        self->trace_obj_ = nullptr;
        self->trace_timer_.reset();
    }
}

void UiBufferSlider::on_trace_timer(lv_timer_t* timer) {
    auto* self = static_cast<UiBufferSlider*>(lv_timer_get_user_data(timer));
    if (self && self->trace_obj_) {
        ++self->trace_ticks_;
        lv_obj_invalidate(self->trace_obj_);
    }
}

void UiBufferSlider::draw_slider(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(slider_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const BufferSliderGeometry g = buffer_slider_geometry(bias_, w, h);
    const lv_color_t ground = theme_manager_get_color("screen_bg");
    const lv_color_t muted = theme_manager_get_color("text_muted");

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = ground;
    fill.opa = LV_OPA_COVER;
    fill.radius = g.housing_radius;
    const lv_area_t housing = area(a, g.housing);
    lv_draw_fill(layer, &fill, &housing);

    fill.color = theme_manager_get_color("danger");
    fill.opa = LV_OPA_20;
    fill.radius = g.radius;
    for (const BufferBox& stop : {g.danger_top, g.danger_bottom}) {
        const lv_area_t zone = area(a, stop);
        lv_draw_fill(layer, &fill, &zone);
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.width = 1;
    line.dash_width = 3;
    line.dash_gap = 2;
    const lv_area_t window = area(a, g.target);
    const lv_point_precise_t corners[] = {point(window.x1, window.y1), point(window.x2, window.y1),
                                          point(window.x2, window.y2), point(window.x1, window.y2)};
    for (int i = 0; i < 4; ++i) {
        line.p1 = corners[i];
        line.p2 = corners[(i + 1) % 4];
        lv_draw_line(layer, &line);
    }

    lv_draw_border_dsc_t stroke;
    lv_draw_border_dsc_init(&stroke);
    stroke.color = theme_manager_get_color("text_subtle");
    stroke.width = 1;
    stroke.radius = g.housing_radius;
    lv_draw_border(layer, &stroke, &housing);

    lv_draw_line_dsc_t strand;
    lv_draw_line_dsc_init(&strand);
    strand.color = theme_manager_get_color("secondary");
    strand.width = g.strand_w;
    strand.p1 = point(a.x1 + g.strand_x, a.y1);
    strand.p2 = point(a.x1 + g.strand_x, a.y2);
    lv_draw_line(layer, &strand);

    fill.color = theme_manager_get_color(buffer_status_token(status_));
    fill.opa = LV_OPA_COVER;
    const lv_area_t block = area(a, g.block);
    lv_draw_fill(layer, &fill, &block);
    stroke.color = ground;
    stroke.radius = g.radius;
    lv_draw_border(layer, &stroke, &block);

    lv_draw_line_dsc_t grip;
    lv_draw_line_dsc_init(&grip);
    grip.color = ground;
    grip.opa = LV_OPA_40;
    grip.width = 1;
    for (int i = 0; i < g.grip_count; ++i) {
        grip.p1 = point(a.x1 + g.grip_x1, a.y1 + g.grip_y[i]);
        grip.p2 = point(a.x1 + g.grip_x2, a.y1 + g.grip_y[i]);
        lv_draw_line(layer, &grip);
    }
}

void UiBufferSlider::draw_trace(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(trace_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const lv_color_t muted = theme_manager_get_color("text_muted");

    const int64_t now = buffer_clock_ms();
    const auto window = AmsState::instance().buffer_trace(trace_unit_).window(now);

    // The target line is dashed where the minute is recorded and dotted where
    // it is not yet, so the area always spans the full window.
    const int32_t unrecorded_x = buffer_trace_unrecorded_x(window, now, w);
    const int32_t target_y = a.y1 + buffer_trace_y(0.0f, h);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.opa = LV_OPA_30;
    line.width = 1;
    line.dash_width = 4;
    line.dash_gap = 3;
    if (unrecorded_x > 0) {
        line.p1 = point(a.x1, target_y);
        line.p2 = point(a.x1 + unrecorded_x, target_y);
        lv_draw_line(layer, &line);
    }
    if (unrecorded_x < w) {
        line.dash_width = 1;
        line.dash_gap = 3;
        line.p1 = point(a.x1 + unrecorded_x, target_y);
        line.p2 = point(a.x2, target_y);
        lv_draw_line(layer, &line);
    }

    const auto lines = buffer_trace_polylines(window, now, w, h);
    lv_draw_line_dsc_t trace;
    lv_draw_line_dsc_init(&trace);
    trace.width = 2;
    trace.round_start = 1;
    trace.round_end = 1;
    const lv_color_t status_color[3] = {
        muted, theme_manager_get_color(buffer_status_token(ClogMeterStatus::Warning)),
        theme_manager_get_color(buffer_status_token(ClogMeterStatus::Fault))};
    for (const auto& run : lines) {
        for (std::size_t i = 1; i < run.size(); ++i) {
            trace.color =
                status_color[static_cast<int>(buffer_trace_segment_status(run[i - 1], run[i]))];
            trace.p1 = point(a.x1 + run[i - 1].x, a.y1 + run[i - 1].y);
            trace.p2 = point(a.x1 + run[i].x, a.y1 + run[i].y);
            lv_draw_line(layer, &trace);
        }
    }

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);

    // The newest reading, in the block's colour, where the trace leaves "now".
    if (!lines.empty() && lines.front().front().x == 0) {
        const int32_t r = std::max<int32_t>(2, theme_manager_get_spacing("space_xxs"));
        const int32_t y = a.y1 + lines.front().front().y;
        fill.color = theme_manager_get_color(buffer_status_token(status_));
        fill.opa = LV_OPA_COVER;
        fill.radius = LV_RADIUS_CIRCLE;
        const lv_area_t dot = {a.x1 - r, y - r, a.x1 + r, y + r};
        lv_draw_fill(layer, &fill, &dot);
    }
}

} // namespace helix::ui
