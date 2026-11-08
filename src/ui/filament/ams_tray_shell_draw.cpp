// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_tray_shell_draw.h"

#include "callout_layout.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::tray {

namespace {

float cross(PointF o, PointF a, PointF b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

} // namespace

void stroke(lv_layer_t* layer, lv_point_t origin, const PointF* pts, int n, bool closed,
            lv_color_t color, lv_opa_t opa) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.opa = opa;
    line.width = 1;
    line.round_start = 1;
    line.round_end = 1;
    const int segments = closed ? n : n - 1;
    for (int i = 0; i < segments; ++i) {
        line.p1 = to_screen(origin, pts[i]);
        line.p2 = to_screen(origin, pts[(i + 1) % n]);
        lv_draw_line(layer, &line);
    }
}

void build_lid(const ShellView& view, const TrayBox& unit_box, float lid_h, float x0, float x1,
               Lid& lid) {
    lid.box = unit_box;
    lid.box.fl = x0;
    lid.box.fr = x1;
    if (view.shell) {
        shell_cap_polyline(unit_box, *view.shell, view.spool_size, x0, lid.left, kCapPoints);
        shell_cap_polyline(unit_box, *view.shell, view.spool_size, x1, lid.right, kCapPoints);
    } else {
        cap_polyline(unit_box, lid_h, x0, lid.left, kCapPoints);
        cap_polyline(unit_box, lid_h, x1, lid.right, kCapPoints);
    }
    // Monotone-chain hull; the right cap is the left one shifted along x.
    PointF pts[2 * kCapPoints];
    for (int i = 0; i < kCapPoints; ++i) {
        pts[i] = lid.left[i];
        pts[kCapPoints + i] = lid.right[i];
    }
    helix::sort_small(pts, pts + 2 * kCapPoints,
                      [](PointF a, PointF b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    int k = 0;
    for (int i = 0; i < 2 * kCapPoints; ++i) {
        while (k >= 2 && cross(lid.hull[k - 2], lid.hull[k - 1], pts[i]) <= 0)
            --k;
        lid.hull[k++] = pts[i];
    }
    for (int i = 2 * kCapPoints - 2, lower = k + 1; i >= 0; --i) {
        while (k >= lower && cross(lid.hull[k - 2], lid.hull[k - 1], pts[i]) <= 0)
            --k;
        lid.hull[k++] = pts[i];
    }
    lid.hull_n = k - 1;
}

lv_point_precise_t to_screen(lv_point_t origin, PointF p) {
    return {static_cast<lv_value_precise_t>(origin.x + p.x),
            static_cast<lv_value_precise_t>(origin.y + p.y)};
}

/// A convex polygon as a triangle fan.
void fill_convex(lv_layer_t* layer, lv_point_t origin, const PointF* pts, int n, lv_color_t color,
                 lv_opa_t opa) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;
    for (int i = 1; i + 1 < n; ++i) {
        tri.p[0] = to_screen(origin, pts[0]);
        tri.p[1] = to_screen(origin, pts[i]);
        tri.p[2] = to_screen(origin, pts[i + 1]);
        lv_draw_triangle(layer, &tri);
    }
}

void edge(lv_layer_t* layer, lv_point_t origin, PointF a, PointF b, lv_color_t color,
          lv_opa_t opa) {
    const PointF pts[2] = {a, b};
    stroke(layer, origin, pts, 2, false, color, opa);
}

/// Inside faces and the lid's interior, behind the spools.
void fill_box(lv_layer_t* layer, lv_point_t o, float x0, float y0, float x1, float y1, uint32_t rgb,
              int32_t radius) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(rgb);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    const lv_area_t area = {o.x + (int32_t)x0, o.y + (int32_t)y0, o.x + (int32_t)x1,
                            o.y + (int32_t)y1};
    lv_draw_rect(layer, &dsc, &area);
}

/// The solid side of the unit at @p x, floor to the cheek's top edge.
void fill_cheek(const ShellView& view, lv_layer_t* layer, lv_point_t o, float x, uint32_t rgb) {
    const UnitShell& shell = *view.shell;
    for (int i = 0; i + 1 < shell.cheek_n; ++i) {
        const PointF top0 = cheek_point(view.box, shell, view.spool_size, x, i);
        const PointF top1 = cheek_point(view.box, shell, view.spool_size, x, i + 1);
        const float rise = view.box.rise / view.box.depth;
        const float z0 = shell.cheek[i].depth_frac * view.box.depth;
        const float z1 = shell.cheek[i + 1].depth_frac * view.box.depth;
        const PointF quad[4] = {
            top0, top1, {top1.x, view.box.fb - rise * z1}, {top0.x, view.box.fb - rise * z0}};
        fill_convex(layer, o, quad, 4, lv_color_hex(rgb), LV_OPA_COVER);
    }
}

/// The glass at a lid's end, reaching down to the body's contour.
void fill_shell_cap(const ShellView& view, lv_layer_t* layer, lv_point_t o, const PointF* cap,
                    float x, lv_color_t color, lv_opa_t opa) {
    for (int i = 0; i + 1 < kCapPoints; ++i) {
        const PointF low0 = cheek_under(view.box, *view.shell, view.spool_size, x, cap[i]);
        const PointF low1 = cheek_under(view.box, *view.shell, view.spool_size, x, cap[i + 1]);
        const PointF strip[4] = {cap[i], cap[i + 1], low1, low0};
        fill_convex(layer, o, strip, 4, color, opa);
    }
}

/// The body's top edge along one end of the unit.
void stroke_cheek(const ShellView& view, lv_layer_t* layer, lv_point_t o, float x, lv_color_t color,
                  lv_opa_t opa) {
    const UnitShell& shell = *view.shell;
    for (int i = 0; i + 1 < shell.cheek_n; ++i) {
        edge(layer, o, cheek_point(view.box, shell, view.spool_size, x, i),
             cheek_point(view.box, shell, view.spool_size, x, i + 1), color, opa);
    }
}

/// The lid's top edges across the row: where its flat top starts and ends.
void stroke_lid_top(lv_layer_t* layer, lv_point_t o, const Lid& lid, lv_color_t color,
                    lv_opa_t opa) {
    float crest = lid.right[0].y;
    for (int i = 1; i < kCapPoints; ++i)
        crest = std::min(crest, lid.right[i].y);
    int first = -1, last = -1;
    for (int i = 0; i < kCapPoints; ++i) {
        if (lid.right[i].y <= crest + 1.0f) {
            first = first < 0 ? i : first;
            last = i;
        }
    }
    edge(layer, o, lid.left[first], lid.right[first], color, opa);
    if (last != first)
        edge(layer, o, lid.left[last], lid.right[last], color, opa);
}

/// A unit's own front wall, rail and side, in the shell's hardware colours.
void draw_shell_front(const ShellView& view, lv_layer_t* layer, lv_point_t o) {
    const UnitShell& shell = *view.shell;
    const TrayBox& b = view.box;
    const float scale = view.spool_size / kShellReferenceSpool;
    const float rail = shell.rail_px * scale;
    fill_cheek(view, layer, o, b.fr, shell.body);
    fill_box(layer, o, b.fl, b.ft, b.fr, b.fb, shell.body, 3);
    fill_box(layer, o, b.fl + 3, b.ft + rail + 2, b.fr - 3, b.fb - 3, shell.panel, 3);
    fill_box(layer, o, b.fl + 4, b.ft, b.fr - 4, b.ft + rail, shell.rail, 2);
    const float tab = 7.0f * scale;
    for (int i = 0; i < view.lane_count; ++i) {
        const float x = view.lane_x[i];
        fill_box(layer, o, x - tab, b.ft - 3.0f * scale, x + tab, b.ft, shell.accent);
        fill_box(layer, o, x - 1, b.ft + rail * 0.35f, x + 1, b.ft + rail * 0.85f, 0xf7f7f2);
    }
    if (shell.corner) {
        const float block = 8.0f * scale;
        const float back_x = b.fr + kDepthSkew * b.depth;
        fill_box(layer, o, back_x - block, b.ft - b.rise, back_x, b.fb - b.rise, shell.corner);
        fill_box(layer, o, b.fl, b.ft - 1.0f, b.fl + block, b.fb, shell.corner);
        fill_box(layer, o, b.fr - block, b.ft - 1.0f, b.fr, b.fb, shell.corner);
    }
    if (shell.has_latch) {
        // Below the rail, so it never lands on a lane's tick.
        const float mid = (b.fl + b.fr) / 2.0f;
        fill_box(layer, o, mid - 15.0f * scale, b.ft + rail - 3.0f * scale, mid + 15.0f * scale,
                 b.ft + rail + 5.0f * scale, 0xd8dade, 4);
    }
}

} // namespace helix::ui::tray
