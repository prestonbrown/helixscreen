// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_renderer_common.h"

namespace helix {

namespace {

// Largest polygon across the traced toolheads
constexpr int kMaxPolygonPoints = 80;

int64_t cross_product_sign(const lv_point_t& a, const lv_point_t& b, const lv_point_t& c) {
    return (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x);
}

bool point_in_triangle(const lv_point_t& p, const lv_point_t& a, const lv_point_t& b,
                       const lv_point_t& c) {
    int64_t d1 = cross_product_sign(p, a, b);
    int64_t d2 = cross_product_sign(p, b, c);
    int64_t d3 = cross_product_sign(p, c, a);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
}

bool is_convex_vertex(const int* indices, int idx_cnt, int i, const lv_point_t* pts, bool ccw) {
    int prev_i = (i - 1 + idx_cnt) % idx_cnt;
    int next_i = (i + 1) % idx_cnt;
    int64_t cross = cross_product_sign(pts[indices[prev_i]], pts[indices[i]], pts[indices[next_i]]);
    return ccw ? (cross > 0) : (cross < 0);
}

bool is_ear(const int* indices, int idx_cnt, int i, const lv_point_t* pts, bool ccw) {
    if (!is_convex_vertex(indices, idx_cnt, i, pts, ccw))
        return false;
    int prev_i = (i - 1 + idx_cnt) % idx_cnt;
    int next_i = (i + 1) % idx_cnt;
    const lv_point_t& a = pts[indices[prev_i]];
    const lv_point_t& b = pts[indices[i]];
    const lv_point_t& c = pts[indices[next_i]];
    for (int j = 0; j < idx_cnt; j++) {
        if (j == prev_i || j == i || j == next_i)
            continue;
        if (point_in_triangle(pts[indices[j]], a, b, c))
            return false;
    }
    return true;
}

} // namespace

void nr_draw_polygon(lv_layer_t* layer, const lv_point_t* pts, int cnt, lv_color_t color) {
    if (cnt < 3)
        return;
    if (cnt > kMaxPolygonPoints)
        cnt = kMaxPolygonPoints;

    if (cnt == 3) {
        lv_draw_triangle_dsc_t tri_dsc;
        lv_draw_triangle_dsc_init(&tri_dsc);
        tri_dsc.color = color;
        tri_dsc.opa = LV_OPA_COVER;
        tri_dsc.p[0].x = pts[0].x;
        tri_dsc.p[0].y = pts[0].y;
        tri_dsc.p[1].x = pts[1].x;
        tri_dsc.p[1].y = pts[1].y;
        tri_dsc.p[2].x = pts[2].x;
        tri_dsc.p[2].y = pts[2].y;
        lv_draw_triangle(layer, &tri_dsc);
        return;
    }

    int64_t winding_sum = 0;
    for (int i = 0; i < cnt; i++) {
        int next = (i + 1) % cnt;
        winding_sum += (int64_t)(pts[next].x - pts[i].x) * (pts[next].y + pts[i].y);
    }
    bool ccw = (winding_sum < 0);

    int indices[kMaxPolygonPoints];
    for (int i = 0; i < cnt; i++)
        indices[i] = i;
    int idx_cnt = cnt;

    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.color = color;
    tri_dsc.opa = LV_OPA_COVER;

    int safety_counter = cnt * cnt;
    while (idx_cnt > 3 && safety_counter-- > 0) {
        bool ear_found = false;
        for (int i = 0; i < idx_cnt; i++) {
            if (is_ear(indices, idx_cnt, i, pts, ccw)) {
                int prev_i = (i - 1 + idx_cnt) % idx_cnt;
                int next_i = (i + 1) % idx_cnt;
                tri_dsc.p[0].x = pts[indices[prev_i]].x;
                tri_dsc.p[0].y = pts[indices[prev_i]].y;
                tri_dsc.p[1].x = pts[indices[i]].x;
                tri_dsc.p[1].y = pts[indices[i]].y;
                tri_dsc.p[2].x = pts[indices[next_i]].x;
                tri_dsc.p[2].y = pts[indices[next_i]].y;
                lv_draw_triangle(layer, &tri_dsc);
                for (int j = i; j < idx_cnt - 1; j++)
                    indices[j] = indices[j + 1];
                idx_cnt--;
                ear_found = true;
                break;
            }
        }
        if (!ear_found) {
            int64_t fcx = 0, fcy = 0;
            for (int j = 0; j < idx_cnt; j++) {
                fcx += pts[indices[j]].x;
                fcy += pts[indices[j]].y;
            }
            fcx /= idx_cnt;
            fcy /= idx_cnt;
            for (int j = 0; j < idx_cnt; j++) {
                int next_j = (j + 1) % idx_cnt;
                tri_dsc.p[0].x = (int32_t)fcx;
                tri_dsc.p[0].y = (int32_t)fcy;
                tri_dsc.p[1].x = pts[indices[j]].x;
                tri_dsc.p[1].y = pts[indices[j]].y;
                tri_dsc.p[2].x = pts[indices[next_j]].x;
                tri_dsc.p[2].y = pts[indices[next_j]].y;
                lv_draw_triangle(layer, &tri_dsc);
            }
            return;
        }
    }

    if (idx_cnt == 3) {
        tri_dsc.p[0].x = pts[indices[0]].x;
        tri_dsc.p[0].y = pts[indices[0]].y;
        tri_dsc.p[1].x = pts[indices[1]].x;
        tri_dsc.p[1].y = pts[indices[1]].y;
        tri_dsc.p[2].x = pts[indices[2]].x;
        tri_dsc.p[2].y = pts[indices[2]].y;
        lv_draw_triangle(layer, &tri_dsc);
    }
}

void nr_draw_polygons(lv_layer_t* layer, const NrPolygon* polys, size_t count,
                      const lv_color_t* palette, int32_t cx, int32_t cy, float scale,
                      lv_point_t design_center) {
    lv_point_t tmp[kMaxPolygonPoints];
    for (size_t p = 0; p < count; p++) {
        int cnt = LV_MIN(polys[p].cnt, kMaxPolygonPoints);
        for (int i = 0; i < cnt; i++) {
            tmp[i].x = cx + (int32_t)((polys[p].pts[i].x - design_center.x) * scale);
            tmp[i].y = cy + (int32_t)((polys[p].pts[i].y - design_center.y) * scale);
        }
        nr_draw_polygon(layer, tmp, cnt, palette[polys[p].color]);
    }
}

void nr_draw_tinted_tip(lv_layer_t* layer, int32_t cx, int32_t top_y, int32_t top_width,
                        int32_t bottom_width, int32_t height, lv_color_t left, lv_color_t right,
                        std::optional<lv_color_t> filament, lv_opa_t opa, int32_t glint_right) {
    if (filament) {
        lv_color_t tint = nr_dim(*filament, opa);
        left = nr_blend(left, tint, 0.4f);
        right = nr_blend(right, tint, 0.4f);
    }
    nr_draw_nozzle_tip(layer, cx, top_y, top_width, bottom_width, height, left, right);

    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = lv_color_hex(0xFFFFFF);
    fill_dsc.opa = LV_OPA_70;
    int32_t bottom = top_y + height;
    lv_area_t glint = {cx - 1, bottom - 1, cx + glint_right, bottom};
    lv_draw_fill(layer, &fill_dsc, &glint);
}

void nr_draw_filament_tip(lv_layer_t* layer, int32_t cx, int32_t top_y, int32_t top_width,
                          int32_t bottom_width, int32_t height, std::optional<lv_color_t> filament,
                          lv_opa_t opa) {
    lv_color_t tip_left, tip_right;
    if (filament) {
        lv_color_t tip = nr_dim(*filament, opa);
        tip_left = nr_lighten(tip, 30);
        tip_right = nr_darken(tip, 20);
    } else {
        lv_color_t metal = nr_dim(lv_color_hex(0x3A3A3A), opa);
        tip_left = nr_lighten(metal, 30);
        tip_right = nr_darken(metal, 10);
    }
    nr_draw_nozzle_tip(layer, cx, top_y, top_width, bottom_width, height, tip_left, tip_right);

    lv_draw_fill_dsc_t glint_dsc;
    lv_draw_fill_dsc_init(&glint_dsc);
    glint_dsc.color = nr_dim(lv_color_hex(0xFFFFFF), opa);
    glint_dsc.opa = LV_OPA_70;
    int32_t glint_y = top_y + height - 1;
    lv_area_t glint = {cx - 1, glint_y, cx + 1, glint_y + 1};
    lv_draw_fill(layer, &glint_dsc, &glint);
}

void nr_draw_fan_ring(lv_layer_t* layer, int32_t fan_cx, int32_t fan_cy, int32_t fan_radius,
                      lv_color_t front_mid, lv_color_t front_light, lv_color_t metal_base,
                      lv_opa_t opa, bool blade_hints) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = fan_cx;
    arc_dsc.center.y = fan_cy;
    arc_dsc.radius = fan_radius + 2;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;
    arc_dsc.width = 2;
    arc_dsc.color = nr_lighten(front_mid, 20);
    arc_dsc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &arc_dsc);

    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = nr_darken(metal_base, 80);
    fill_dsc.opa = opa;
    fill_dsc.radius = fan_radius;
    lv_area_t fan_area = {fan_cx - fan_radius, fan_cy - fan_radius, fan_cx + fan_radius,
                          fan_cy + fan_radius};
    lv_draw_fill(layer, &fill_dsc, &fan_area);

    int32_t hub_r = fan_radius / 3;
    fill_dsc.color = nr_darken(metal_base, 40);
    fill_dsc.radius = hub_r;
    lv_area_t hub_area = {fan_cx - hub_r, fan_cy - hub_r, fan_cx + hub_r, fan_cy + hub_r};
    lv_draw_fill(layer, &fill_dsc, &hub_area);

    if (blade_hints) {
        lv_draw_line_dsc_t blade_dsc;
        lv_draw_line_dsc_init(&blade_dsc);
        blade_dsc.color = nr_darken(metal_base, 55);
        blade_dsc.width = 1;
        blade_dsc.opa = LV_OPA_30;

        blade_dsc.p1.x = fan_cx - fan_radius + 2;
        blade_dsc.p1.y = fan_cy;
        blade_dsc.p2.x = fan_cx + fan_radius - 2;
        blade_dsc.p2.y = fan_cy;
        lv_draw_line(layer, &blade_dsc);

        blade_dsc.p1.x = fan_cx;
        blade_dsc.p1.y = fan_cy - fan_radius + 2;
        blade_dsc.p2.x = fan_cx;
        blade_dsc.p2.y = fan_cy + fan_radius - 2;
        lv_draw_line(layer, &blade_dsc);
    }

    arc_dsc.radius = fan_radius + 1;
    arc_dsc.start_angle = 200;
    arc_dsc.end_angle = 290;
    arc_dsc.width = 1;
    arc_dsc.color = nr_lighten(front_light, 50);
    lv_draw_arc(layer, &arc_dsc);
}

} // namespace helix
