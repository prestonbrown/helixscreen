// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail_tray.h"

#include "filament_tube_stroker.h"
#include "sort_small.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstdio>

namespace helix::ui::ams_tray {

TrayState s_tray;

namespace {

constexpr int kMaxLids = kAmsDetailMaxSlots;

tray::ShellView shell_view() {
    return {s_tray.box, s_tray.shell, s_tray.spool_size, s_tray.lane_x, s_tray.lane_count};
}

lv_color_t tray_token(const char* name, bool dark) {
    char key[48];
    snprintf(key, sizeof(key), "%s_%s", name, dark ? "dark" : "light");
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("ams_unit_detail");
    const char* hex = scope ? lv_xml_get_const(scope, key) : nullptr;
    return hex ? theme_manager_parse_hex_color(hex) : theme_manager_get_color("card_bg");
}

TrayColors load_tray_colors(bool dark) {
    return {tray_token("tray_front", dark),     tray_token("tray_wall", dark),
            tray_token("tray_floor", dark),     tray_token("tray_back", dark),
            tray_token("tray_side", dark),      tray_token("tray_edge", dark),
            tray_token("tray_shell", dark),     tray_token("tray_glass", dark),
            tray_token("tray_glass_edge", dark)};
}

// Read on every draw, so a theme or dark-mode switch repaints the tray in the
// new colors.
void load_tray_look() {
    const bool dark = theme_manager_is_dark_mode();
    s_tray.color = load_tray_colors(dark);
    s_tray.opa = dark ? kDarkOpa : kLightOpa;
    if (s_tray.shell && s_tray.shell->lid_color) // solid boxes: their colours mark the edges
        s_tray.opa.edge = LV_OPA_TRANSP;
}

/// The lid spans: one over the row, or one per lane.
int lid_spans(float* x0, float* x1) {
    if (s_tray.lid == tray::LidMode::Unit) {
        x0[0] = s_tray.box.fl;
        x1[0] = s_tray.box.fr;
        return 1;
    }
    if (s_tray.lid != tray::LidMode::PerLane)
        return 0;
    for (int i = 0; i < s_tray.lane_count; ++i) {
        x0[i] = s_tray.lane_x[i] - s_tray.lane_half;
        x1[i] = s_tray.lane_x[i] + s_tray.lane_half;
    }
    return s_tray.lane_count;
}

lv_point_t container_origin(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(lv_obj_get_parent(obj), &a);
    return {a.x1, a.y1};
}

} // namespace

void tray_back_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || !s_tray.valid)
        return;
    load_tray_look();
    const lv_point_t o = container_origin(lv_event_get_target_obj(e));
    const tray::TrayFaces f = tray::tray_faces(s_tray.box);
    const TrayColors& c = s_tray.color;
    const bool simple = helix::ui::reduced_effects();

    if (!simple) {
        tray::fill_convex(layer, o, f.back_wall, 4, c.back, LV_OPA_COVER);
        tray::fill_convex(layer, o, f.floor, 4, c.floor, LV_OPA_COVER);
        if (s_tray.shell)
            tray::fill_cheek(shell_view(), layer, o, s_tray.box.fl, s_tray.shell->body);
        else
            tray::fill_convex(layer, o, f.left_wall, 4, c.wall, LV_OPA_COVER);
    }
    tray::edge(layer, o, f.back_wall[0], f.back_wall[1], c.edge, s_tray.opa.edge);
    if (s_tray.shell)
        tray::stroke_cheek(shell_view(), layer, o, s_tray.box.fl, c.edge, s_tray.opa.edge);

    if (simple || s_tray.lid == tray::LidMode::None)
        return;
    // The tinted interior shell, then the back wall again: it stands in front
    // of the lid's lower interior.
    float x0[kMaxLids], x1[kMaxLids];
    const int lids = lid_spans(x0, x1);
    tray::Lid lid;
    for (int i = 0; i < lids; ++i) {
        tray::build_lid(shell_view(), s_tray.box, s_tray.lid_h, x0[i], x1[i], lid);
        tray::fill_convex(layer, o, lid.hull, lid.hull_n, c.shell, s_tray.opa.shell);
    }
    tray::fill_convex(layer, o, f.back_wall, 4, c.back, LV_OPA_COVER);
    tray::edge(layer, o, f.back_wall[0], f.back_wall[1], c.edge, s_tray.opa.edge);
    if (s_tray.shell) {
        for (int i = 0; i < lids; ++i) {
            tray::build_lid(shell_view(), s_tray.box, s_tray.lid_h, x0[i], x1[i], lid);
            // A box per lane: both its sides behind the spools, box by box, so each box's left
            // side covers the one before it and each spool stands in front of its own.
            if (!s_tray.shell->lid_color) {
                tray::stroke(layer, o, lid.left, tray::kCapPoints, false, c.glass_edge,
                             s_tray.opa.glass_edge);
                continue;
            }
            tray::fill_box_side(shell_view(), layer, o, lid.left, x0[i], c.glass_edge,
                                s_tray.opa.glass_edge);
            tray::fill_box_side(shell_view(), layer, o, lid.right, x1[i], c.glass_edge,
                                s_tray.opa.glass_edge);
        }
    }
}

/// The sheen: one diffuse band, a horizontal gradient per row.
namespace {

void draw_sheen(lv_layer_t* layer, lv_point_t o, const tray::Lid& lid) {
    tray::SheenSpan span{};
    tray::SheenRow rows[tray::kSheenMaxRows];
    const int n = tray::sheen_rows(lid.box, s_tray.lid_h, s_tray.opa.sheen, span, rows);
    const float width = span.x1 - span.x0;
    if (n == 0 || width < 1)
        return;
    auto frac = [&](float x) { return (uint8_t)std::lround(255.0f * (x - span.x0) / width); };
    for (int i = 0; i < n; ++i) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_opa = LV_OPA_COVER;
        const lv_color_t white = lv_color_white();
        const lv_color_t colors[4] = {white, white, white, white};
        const lv_opa_t opas[4] = {0, rows[i].opa, rows[i].opa, 0};
        const uint8_t fracs[4] = {0, frac(span.full0), frac(span.full1), 255};
        lv_grad_init_stops(&rect.bg_grad, colors, opas, fracs, 4);
        lv_grad_horizontal_init(&rect.bg_grad);
        const int32_t y = o.y + (int32_t)std::floor(rows[i].y);
        const lv_area_t area = {o.x + (int32_t)span.x0, y, o.x + (int32_t)span.x1, y};
        lv_draw_rect(layer, &rect, &area);
    }
}

/// Front wall, right side, edges and the glass lid, in front of the spools.
} // namespace

void tray_front_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || !s_tray.valid)
        return;
    load_tray_look();
    const lv_point_t o = container_origin(lv_event_get_target_obj(e));
    const tray::TrayFaces f = tray::tray_faces(s_tray.box);
    const TrayColors& c = s_tray.color;
    const bool simple = helix::ui::reduced_effects();

    // A unit with a box per lane stands them in a dock: its front is drawn after them.
    const bool dock = s_tray.shell && s_tray.shell->lid_color;
    if (s_tray.shell && !simple && !dock) {
        tray::draw_shell_front(shell_view(), layer, o);
    } else if (!s_tray.shell && !simple) {
        lv_draw_rect_dsc_t front;
        lv_draw_rect_dsc_init(&front);
        front.bg_color = c.front;
        front.bg_opa = s_tray.opa.front;
        const lv_area_t area = {o.x + (int32_t)f.front[0].x, o.y + (int32_t)f.front[0].y,
                                o.x + (int32_t)f.front[2].x, o.y + (int32_t)f.front[2].y};
        lv_draw_rect(layer, &front, &area);
        tray::fill_convex(layer, o, f.right_side, 4, c.side, LV_OPA_COVER);
    }
    const tray::PointF fl_t = f.front[0], fr_t = f.front[1], fr_b = f.front[2], fl_b = f.front[3];
    const tray::PointF bl_t = f.back_wall[0], br_t = f.back_wall[1], br_b = f.back_wall[2],
                       bl_b = f.back_wall[3];
    const tray::PointF edges[][2] = {{fl_t, fr_t}, {fl_b, fr_b}, {fl_t, fl_b},
                                     {fr_t, fr_b}, {fr_t, br_t}, {fr_b, br_b},
                                     {br_t, br_b}, {fl_t, bl_t}, {fl_b, bl_b}};
    if (!s_tray.shell || simple) {
        for (const auto& ab : edges)
            tray::edge(layer, o, ab[0], ab[1], c.edge, s_tray.opa.edge);
    } else {
        tray::stroke_cheek(shell_view(), layer, o, s_tray.box.fr, c.edge, s_tray.opa.edge);
    }

    float x0[kMaxLids], x1[kMaxLids];
    const int lids = lid_spans(x0, x1);
    tray::Lid lid;
    const TrayOpacities& a = s_tray.opa;
    for (int i = 0; i < lids; ++i) {
        tray::build_lid(shell_view(), s_tray.box, s_tray.lid_h, x0[i], x1[i], lid);
        if (!simple) {
            tray::fill_convex(layer, o, lid.hull, lid.hull_n, c.glass, a.glass);
            if (dock) {
                // Its right side again, in front of its own spool's glow, stopping where the next
                // box's left side, drawn behind, takes over.
                const lv_area_t clip = layer->_clip_area;
                if (i + 1 < lids)
                    layer->_clip_area.x2 = std::min(clip.x2, o.x + (int32_t)x0[i + 1] - 1);
                tray::fill_box_side(shell_view(), layer, o, lid.right, x1[i], c.glass_edge,
                                    a.glass_edge);
                layer->_clip_area = clip;
            }
            if (!s_tray.shell)
                tray::fill_convex(layer, o, lid.right, tray::kCapPoints, c.glass, a.cap);
            else if (!dock) // a box per lane has its sides drawn behind the spools
                tray::fill_shell_cap(shell_view(), layer, o, lid.right, x1[i], c.glass, a.cap);
            draw_sheen(layer, o, lid);
        }
        if (!s_tray.shell)
            tray::stroke(layer, o, lid.hull, lid.hull_n, true, c.glass_edge, a.glass_edge);
        else
            tray::stroke_lid_top(layer, o, lid, c.glass_edge, a.glass_edge);
        if (!dock)
            tray::stroke(layer, o, lid.right, tray::kCapPoints, false, c.glass_edge,
                         (lv_opa_t)(a.glass_edge * 7 / 10));
        // A unit's left end stands behind its spools and is drawn with the back faces.
        if (!s_tray.shell || simple) {
            tray::stroke(layer, o, lid.left, tray::kCapPoints, false, c.glass_edge,
                         (lv_opa_t)(a.glass_edge * 3 / 10));
        }
    }
    if (dock && !simple)
        tray::draw_shell_front(shell_view(), layer, o);
}

} // namespace helix::ui::ams_tray
