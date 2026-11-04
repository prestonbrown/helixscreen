// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_leader_line.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"

#include <vector>

namespace {

/// Runs before lv_line's own draw. A dashed line is drawn here, dash by dash,
/// and the default draw is skipped; an undashed one falls through to it.
void draw_dashed(lv_event_t* e) {
    auto* line = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const int32_t dash = lv_obj_get_style_line_dash_width(line, LV_PART_MAIN);
    const int32_t gap = lv_obj_get_style_line_dash_gap(line, LV_PART_MAIN);
    const lv_point_precise_t* src = lv_line_get_points(line);
    const uint32_t count = lv_line_get_point_count(line);
    if (dash <= 0 || gap <= 0 || !src || count < 2)
        return;

    lv_area_t area;
    lv_obj_get_coords(line, &area);
    const float x_ofs = static_cast<float>(area.x1 - lv_obj_get_scroll_x(line));
    const float y_ofs = static_cast<float>(area.y1 - lv_obj_get_scroll_y(line));
    std::vector<helix::ui::LeaderPoint> pts(count);
    for (uint32_t i = 0; i < count; ++i)
        pts[i] = {static_cast<float>(src[i].x) + x_ofs, static_cast<float>(src[i].y) + y_ofs};

    lv_layer_t* layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.base.layer = layer;
    lv_obj_init_draw_line_dsc(line, LV_PART_MAIN, &dsc);
    dsc.dash_width = 0;
    dsc.dash_gap = 0;
    for (const auto& d : helix::ui::leader_dashes(pts.data(), pts.size(), static_cast<float>(dash),
                                                  static_cast<float>(gap))) {
        dsc.p1.x = static_cast<lv_value_precise_t>(d.a.x);
        dsc.p1.y = static_cast<lv_value_precise_t>(d.a.y);
        dsc.p2.x = static_cast<lv_value_precise_t>(d.b.x);
        dsc.p2.y = static_cast<lv_value_precise_t>(d.b.y);
        lv_draw_line(layer, &dsc);
    }
    lv_event_stop_processing(e);
}

void* leader_line_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* line = lv_line_create(static_cast<lv_obj_t*>(lv_xml_state_get_parent(state)));
    lv_obj_add_event_cb(line, draw_dashed,
                        static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS),
                        nullptr);
    return line;
}

} // namespace

void helix::ui::register_leader_line_widget() {
    lv_xml_register_widget("leader_line", leader_line_create, lv_xml_obj_apply);
}
