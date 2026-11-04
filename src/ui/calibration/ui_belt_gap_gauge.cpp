// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_gap_gauge.h"

#include "belt_tune_session.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

struct GaugeState {
    float gap_hz = 0.0f;
    std::optional<float> previous_gap_hz;
    float tolerance_hz = 1.0f;
    float range_hz = 10.0f;
};

GaugeState* state_of(lv_obj_t* obj) {
    return static_cast<GaugeState*>(lv_obj_get_user_data(obj));
}

lv_color_t severity_color(GapSeverity severity) {
    switch (severity) {
    case GapSeverity::MATCHED:
        return theme_manager_get_color("success");
    case GapSeverity::NEAR:
        return theme_manager_get_color("warning");
    case GapSeverity::FAR:
        break;
    }
    return theme_manager_get_color("danger");
}

lv_point_precise_t pt(int32_t x, int32_t y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

void draw_arrow(lv_layer_t* layer, int32_t from_x, int32_t to_x, int32_t y, lv_color_t color,
                int32_t head) {
    const int32_t dir = to_x > from_x ? 1 : -1;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 2;
    line.opa = LV_OPA_COVER;
    line.p1 = pt(from_x, y);
    line.p2 = pt(to_x - dir * head, y);
    lv_draw_line(layer, &line);

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;
    tri.p[0] = pt(to_x, y);
    tri.p[1] = pt(to_x - dir * head, y - head / 2);
    tri.p[2] = pt(to_x - dir * head, y + head / 2);
    lv_draw_triangle(layer, &tri);
}

void on_draw(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    const GaugeState* st = state_of(obj);
    if (!layer || !st) {
        return;
    }

    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    const int32_t w = lv_area_get_width(&box);
    const int32_t h = lv_area_get_height(&box);
    if (w < 20 || h < 8) {
        return;
    }

    const int32_t needle_w = std::max<int32_t>(3, h / 10);
    // Needles overhang the track by half their width at the ends.
    const int32_t margin = needle_w;
    const int32_t track_px = w - 2 * margin;
    const int32_t track_x0 = box.x1 + margin;
    const int32_t cy = box.y1 + h / 2;
    const int32_t track_h = std::max<int32_t>(6, h / 4);
    const int32_t needle_h = std::max<int32_t>(track_h + 4, h * 3 / 4);

    const lv_color_t muted = theme_manager_get_color("text_muted");
    const lv_color_t matched = theme_manager_get_color("success");

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = muted;
    rect.bg_opa = LV_OPA_30;
    rect.radius = track_h / 2;
    lv_area_t track = {track_x0, cy - track_h / 2, track_x0 + track_px - 1, cy + track_h / 2};
    lv_draw_rect(layer, &rect, &track);

    // The matched band is symmetric about zero.
    const int32_t band_lo = belt_gap_gauge_x(-st->tolerance_hz, st->range_hz, track_px);
    const int32_t band_hi = belt_gap_gauge_x(st->tolerance_hz, st->range_hz, track_px);
    rect.bg_color = matched;
    rect.bg_opa = LV_OPA_60;
    rect.radius = 0;
    lv_area_t band = {track_x0 + band_lo, cy - track_h / 2, track_x0 + band_hi, cy + track_h / 2};
    lv_draw_rect(layer, &rect, &band);

    const auto needle_area = [&](float gap) {
        const int32_t x = track_x0 + belt_gap_gauge_x(gap, st->range_hz, track_px);
        return lv_area_t{x - needle_w / 2, cy - needle_h / 2, x + needle_w / 2, cy + needle_h / 2};
    };

    const int32_t cur_x = track_x0 + belt_gap_gauge_x(st->gap_hz, st->range_hz, track_px);
    if (st->previous_gap_hz) {
        const int32_t prev_x =
            track_x0 + belt_gap_gauge_x(*st->previous_gap_hz, st->range_hz, track_px);
        const int32_t head = std::max<int32_t>(6, h / 5);
        if (std::abs(cur_x - prev_x) > head + needle_w) {
            const int32_t dir = cur_x > prev_x ? 1 : -1;
            draw_arrow(layer, prev_x + dir * needle_w, cur_x - dir * needle_w, cy, muted, head);
        }

        lv_draw_rect_dsc_t hollow;
        lv_draw_rect_dsc_init(&hollow);
        hollow.bg_opa = LV_OPA_TRANSP;
        hollow.border_color = muted;
        hollow.border_width = 2;
        hollow.border_opa = LV_OPA_COVER;
        hollow.radius = needle_w / 2;
        lv_area_t prev_area = needle_area(*st->previous_gap_hz);
        lv_draw_rect(layer, &hollow, &prev_area);
    }

    lv_draw_rect_dsc_t filled;
    lv_draw_rect_dsc_init(&filled);
    filled.bg_color = severity_color(belt_gap_severity(st->gap_hz, st->tolerance_hz));
    filled.bg_opa = LV_OPA_COVER;
    filled.radius = needle_w / 2;
    lv_area_t cur_area = needle_area(st->gap_hz);
    lv_draw_rect(layer, &filled, &cur_area);
}

void on_delete(lv_event_t* e) {
    delete state_of(lv_event_get_target_obj(e));
}

void* gauge_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, new GaugeState());
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, nullptr);
    return obj;
}

} // namespace

int belt_gap_gauge_x(float gap_hz, float range_hz, int track_px) {
    const int last = std::max(track_px - 1, 0);
    if (!(range_hz > 0.0f) || !std::isfinite(gap_hz)) {
        return last / 2;
    }
    const float t = std::clamp(0.5f + gap_hz / (2.0f * range_hz), 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(t * static_cast<float>(last))), 0, last);
}

GapSeverity belt_gap_severity(float gap_hz, float tolerance_hz) {
    const float mag = std::fabs(gap_hz);
    if (mag <= tolerance_hz) {
        return GapSeverity::MATCHED;
    }
    if (mag <= calibration::belt_tune::kGaugeWarnHz) {
        return GapSeverity::NEAR;
    }
    return GapSeverity::FAR;
}

void ui_belt_gap_gauge_set(lv_obj_t* gauge, float gap_hz, std::optional<float> previous_gap_hz,
                           float tolerance_hz, float range_hz) {
    GaugeState* st = gauge ? state_of(gauge) : nullptr;
    if (!st) {
        return;
    }
    st->gap_hz = gap_hz;
    st->previous_gap_hz = previous_gap_hz;
    st->tolerance_hz = tolerance_hz;
    st->range_hz = range_hz;
    lv_obj_invalidate(gauge);
}

float ui_belt_gap_gauge_get_gap(lv_obj_t* gauge) {
    const GaugeState* st = gauge ? state_of(gauge) : nullptr;
    return st ? st->gap_hz : 0.0f;
}

void register_belt_gap_gauge_widget() {
    lv_xml_register_widget("belt_gap_gauge", gauge_xml_create, lv_xml_obj_apply);
    spdlog::trace("[BeltGapGauge] Widget registered with XML system");
}

} // namespace helix::ui
