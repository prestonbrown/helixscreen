// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_path_sketch.h"

#include "ui_belt_path_names.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace helix::ui {

namespace {

void draw_arrow(lv_layer_t* layer, lv_point_precise_t from, lv_point_precise_t to, lv_color_t color,
                int32_t head) {
    // Glow as stacked strokes: two wider, fainter passes under the line.
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.round_start = 1;
    line.round_end = 1;
    line.p1 = from;
    line.p2 = to;
    const struct {
        int32_t width;
        lv_opa_t opa;
    } passes[] = {{10, 30}, {6, 64}, {3, LV_OPA_COVER}};
    for (const auto& pass : passes) {
        line.width = pass.width;
        line.opa = pass.opa;
        lv_draw_line(layer, &line);
    }

    // Filled heads at both ends, pointing outward along the diagonal.
    const float dx = static_cast<float>(to.x - from.x);
    const float dy = static_cast<float>(to.y - from.y);
    const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    const float ux = dx / len;
    const float uy = dy / len;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;
    for (int end = 0; end < 2; ++end) {
        const float sign = end == 0 ? 1.0f : -1.0f;
        const lv_point_precise_t tip = end == 0 ? to : from;
        const float bx = static_cast<float>(tip.x) - sign * ux * head;
        const float by = static_cast<float>(tip.y) - sign * uy * head;
        tri.p[0] = tip;
        tri.p[1] = {static_cast<lv_value_precise_t>(bx - uy * head * 0.6f),
                    static_cast<lv_value_precise_t>(by + ux * head * 0.6f)};
        tri.p[2] = {static_cast<lv_value_precise_t>(bx + uy * head * 0.6f),
                    static_cast<lv_value_precise_t>(by - ux * head * 0.6f)};
        lv_draw_triangle(layer, &tri);
    }
}

void draw_label(lv_obj_t* obj, lv_layer_t* layer, const char* text, lv_color_t color,
                const lv_font_t* font, lv_area_t area, lv_text_align_t align) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    // Owned by the sketch, so LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS can report the label.
    dsc.base.obj = obj;
    dsc.text = text;
    // The draw task runs after this returns; the text may be a caller's temporary.
    dsc.text_local = 1;
    dsc.color = color;
    dsc.font = font;
    dsc.align = align;
    lv_draw_label(layer, &dsc, &area);
}

/// @p text cut to one line of @p max_w, ending in "..." when it is wider. A
/// drawn label has no long mode, so a long name would otherwise wrap across
/// the diagonals.
std::string fit_width(const char* text, const lv_font_t* font, int32_t max_w) {
    const auto width = [font](const std::string& t) {
        lv_point_t size;
        lv_text_get_size(&size, t.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return size.x;
    };
    std::string out = text;
    if (width(out) <= max_w) {
        return out;
    }
    while (!out.empty()) {
        // Drop one UTF-8 code point: its lead byte and any continuation bytes.
        size_t cut = out.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.erase(cut);
        if (width(out + "...") <= max_w) {
            return out + "...";
        }
    }
    return "...";
}

void on_draw(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) {
        return;
    }

    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    const int32_t w = lv_area_get_width(&box);
    const int32_t h = lv_area_get_height(&box);
    const int32_t side = std::min(w, h);
    if (side < 40) {
        return;
    }

    const lv_font_t* font = theme_manager_get_font("font_small");
    const int32_t font_h = theme_manager_get_font_height(font);
    const int32_t pad = theme_manager_get_spacing("space_sm");
    const lv_color_t a_color = theme_manager_get_color("belt_path_a");
    const lv_color_t b_color = theme_manager_get_color("belt_path_b");
    const lv_color_t muted = theme_manager_get_color("text_muted");

    // Square frame centred in the content box; the labels sit inside it.
    lv_area_t frame;
    frame.x1 = box.x1 + (w - side) / 2;
    frame.y1 = box.y1 + (h - side) / 2;
    frame.x2 = frame.x1 + side - 1;
    frame.y2 = frame.y1 + side - 1;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_TRANSP;
    rect.border_color = theme_manager_get_color("border");
    rect.border_width = 2;
    rect.border_opa = LV_OPA_COVER;
    rect.radius = theme_manager_get_spacing("space_sm");
    lv_draw_rect(layer, &rect, &frame);

    const lv_area_t top_row = {frame.x1 + pad, frame.y1 + pad, frame.x2 - pad,
                               frame.y1 + pad + font_h};
    const lv_area_t bottom_row = {frame.x1 + pad, frame.y2 - pad - font_h, frame.x2 - pad,
                                  frame.y2 - pad};
    // BACK takes the top row's left end, so B's name has the rest of it.
    const char* back = lv_tr("BACK");
    lv_point_t back_size;
    lv_text_get_size(&back_size, back, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    draw_label(obj, layer, back, muted, font, top_row, LV_TEXT_ALIGN_LEFT);
    // Each name sits at the +X end of its own arrow: B's 1,1 diagonal points
    // up-right, A's 1,-1 diagonal down-right. Each stays on one line.
    const int32_t row_w = lv_area_get_width(&top_row);
    const std::string b_name =
        fit_width(belt_path_name(calibration::BeltPath::PATH_B).title.c_str(), font,
                  row_w - back_size.x - pad);
    const std::string a_name =
        fit_width(belt_path_name(calibration::BeltPath::PATH_A).title.c_str(), font, row_w);
    draw_label(obj, layer, b_name.c_str(), b_color, font, top_row, LV_TEXT_ALIGN_RIGHT);
    draw_label(obj, layer, a_name.c_str(), a_color, font, bottom_row, LV_TEXT_ALIGN_RIGHT);

    // Diagonals through the centre, kept clear of the label rows.
    const float cx = (frame.x1 + frame.x2) / 2.0f;
    const float cy = (frame.y1 + frame.y2) / 2.0f;
    const float reach = std::max(10.0f, side / 2.0f - pad * 2.0f - font_h);
    const auto pt = [](float x, float y) {
        return lv_point_precise_t{static_cast<lv_value_precise_t>(x),
                                  static_cast<lv_value_precise_t>(y)};
    };
    const int32_t head = std::max<int32_t>(8, side / 18);
    // +Y is toward the back, which is up on screen.
    draw_arrow(layer, pt(cx - reach, cy + reach), pt(cx + reach, cy - reach), b_color, head);
    draw_arrow(layer, pt(cx - reach, cy - reach), pt(cx + reach, cy + reach), a_color, head);

    // Toolhead in the middle, drawn last so the arrows pass under it.
    const int32_t th = std::max<int32_t>(14, side / 7);
    lv_area_t head_area = {static_cast<int32_t>(cx) - th / 2, static_cast<int32_t>(cy) - th / 2,
                           static_cast<int32_t>(cx) + th / 2, static_cast<int32_t>(cy) + th / 2};
    rect.bg_color = theme_manager_get_color("card_bg");
    rect.bg_opa = LV_OPA_COVER;
    rect.border_color = muted;
    rect.border_width = 2;
    rect.radius = th / 5;
    lv_draw_rect(layer, &rect, &head_area);
}

void* sketch_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    return obj;
}

} // namespace

void register_belt_path_sketch_widget() {
    lv_xml_register_widget("belt_path_sketch", sketch_xml_create, lv_xml_obj_apply);
    spdlog::trace("[BeltPathSketch] Widget registered with XML system");
}

} // namespace helix::ui
