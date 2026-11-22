// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_heater_response_sketch.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace helix::ui {

namespace {

/// The curves live in a 220 x 36 box with the target at y = 18.
constexpr float kBoxW = 220.0f;
constexpr float kBoxH = 36.0f;
constexpr float kTargetY = 18.0f;
constexpr int kStepsPerSegment = 12;

struct P {
    float x;
    float y;
};

/// One cubic Bezier segment: start, two control points, end.
struct Cubic {
    P a;
    P c1;
    P c2;
    P b;
};

// PID: flat, then a dip after the fan change and two shrinking swings.
constexpr std::array<Cubic, 6> kPid = {{
    {{0, 18}, {0, 18}, {70, 18}, {70, 18}},
    {{70, 18}, {80, 18}, {86, 32}, {96, 30}},
    {{96, 30}, {106, 28}, {112, 6}, {124, 8}},
    {{124, 8}, {136, 10}, {140, 26}, {150, 24}},
    {{150, 24}, {160, 22}, {166, 14}, {176, 15}},
    {{176, 15}, {186, 16}, {196, 18}, {220, 18}},
}};

// MPC: the same fan change moves it by a few px and it is back at once.
constexpr std::array<Cubic, 4> kMpc = {{
    {{0, 18}, {0, 18}, {70, 18}, {70, 18}},
    {{70, 18}, {80, 18}, {86, 21}, {96, 21}},
    {{96, 21}, {106, 21}, {112, 18}, {124, 18}},
    {{124, 18}, {124, 18}, {220, 18}, {220, 18}},
}};

P bezier(const Cubic& c, float t) {
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3 * u * u * t;
    const float w2 = 3 * u * t * t;
    const float w3 = t * t * t;
    return {w0 * c.a.x + w1 * c.c1.x + w2 * c.c2.x + w3 * c.b.x,
            w0 * c.a.y + w1 * c.c1.y + w2 * c.c2.y + w3 * c.b.y};
}

void stroke(lv_layer_t* layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color,
            int32_t width, int32_t dash) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    if (dash > 0) {
        dsc.dash_width = dash;
        dsc.dash_gap = dash;
        dsc.round_start = 0;
        dsc.round_end = 0;
    }
    dsc.p1 = a;
    dsc.p2 = b;
    lv_draw_line(layer, &dsc);
}

template <size_t N>
void draw_curve(lv_layer_t* layer, const std::array<Cubic, N>& curve, const lv_area_t& c,
                lv_color_t color) {
    const float sx = static_cast<float>(lv_area_get_width(&c)) / kBoxW;
    const float sy = static_cast<float>(lv_area_get_height(&c)) / kBoxH;
    auto map = [&](P p) {
        return lv_point_precise_t{static_cast<lv_value_precise_t>(c.x1 + p.x * sx),
                                  static_cast<lv_value_precise_t>(c.y1 + p.y * sy)};
    };
    for (const Cubic& seg : curve) {
        lv_point_precise_t prev = map(seg.a);
        for (int i = 1; i <= kStepsPerSegment; ++i) {
            const lv_point_precise_t next =
                map(bezier(seg, static_cast<float>(i) / kStepsPerSegment));
            stroke(layer, prev, next, color, 2, 0);
            prev = next;
        }
    }
}

void on_draw(lv_event_t* e) {
    auto* obj = lv_event_get_current_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_content_coords(obj, &c);
    if (lv_area_get_width(&c) <= 0 || lv_area_get_height(&c) <= 0) {
        return;
    }
    const auto target_y = static_cast<lv_value_precise_t>(
        c.y1 + kTargetY * static_cast<float>(lv_area_get_height(&c)) / kBoxH);
    stroke(layer, {static_cast<lv_value_precise_t>(c.x1), target_y},
           {static_cast<lv_value_precise_t>(c.x2), target_y}, theme_manager_get_color("border"), 1,
           4);

    const bool mpc = lv_obj_get_user_data(obj) != nullptr;
    if (mpc) {
        draw_curve(layer, kMpc, c, theme_manager_get_color("success"));
    } else {
        draw_curve(layer, kPid, c, theme_manager_get_color("warning"));
    }
}

void* sketch_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    return obj;
}

void sketch_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);
    for (int i = 0; attrs[i] != nullptr; i += 2) {
        if (std::strcmp(attrs[i], "method") == 0) {
            // A non-null user pointer marks MPC; no storage to free with the widget.
            lv_obj_set_user_data(obj, std::strcmp(attrs[i + 1], "mpc") == 0 ? obj : nullptr);
        }
    }
}

} // namespace

void register_heater_response_sketch_widget() {
    lv_xml_register_widget("heater_response_sketch", sketch_xml_create, sketch_xml_apply);
    spdlog::trace("[HeaterResponseSketch] Widget registered with XML system");
}

} // namespace helix::ui
