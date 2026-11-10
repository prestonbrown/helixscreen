// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_tray_shell_draw.h
 * @brief Drawing primitives for the live spool tray, and a specific unit's shell on it.
 */

#include "ams_tray_projection.h"
#include "lvgl/lvgl.h"

namespace helix::ui::tray {

inline constexpr int kCapPoints = 17;

/// One lid: its two caps and their convex hull.
struct Lid {
    TrayBox box; // fl/fr are the lid's ends
    PointF left[kCapPoints], right[kCapPoints];
    PointF hull[2 * kCapPoints];
    int hull_n = 0;
};

/// What a unit's shell needs from the laid-out tray.
struct ShellView {
    const TrayBox& box;
    const UnitShell* shell;
    float spool_size;
    const float* lane_x;
    int lane_count;
};

lv_point_precise_t to_screen(lv_point_t origin, PointF p);

/// A convex polygon as a triangle fan.
void fill_convex(lv_layer_t* layer, lv_point_t origin, const PointF* pts, int n, lv_color_t color,
                 lv_opa_t opa);

void edge(lv_layer_t* layer, lv_point_t origin, PointF a, PointF b, lv_color_t color, lv_opa_t opa);

/// Inside faces and the lid's interior, behind the spools.
void fill_box(lv_layer_t* layer, lv_point_t o, float x0, float y0, float x1, float y1, uint32_t rgb,
              int32_t radius = 0);

/// The solid side of the unit at @p x, floor to the cheek's top edge.
void fill_cheek(const ShellView& view, lv_layer_t* layer, lv_point_t o, float x, uint32_t rgb);

/// The glass at a lid's end, reaching down to the body's contour.
void fill_shell_cap(const ShellView& view, lv_layer_t* layer, lv_point_t o, const PointF* cap,
                    float x, lv_color_t color, lv_opa_t opa);

/// One side of a unit's box at @p x, a box per lane: its base's rising contour, its solid top
/// above it, and the glass's edge up the front of that top to the crest.
void fill_box_side(const ShellView& view, lv_layer_t* layer, lv_point_t o, const PointF* cap,
                   float x, lv_color_t edge_color, lv_opa_t edge_opa);

/// The body's top edge along one end of the unit.
void stroke_cheek(const ShellView& view, lv_layer_t* layer, lv_point_t o, float x, lv_color_t color,
                  lv_opa_t opa);

/// The lid's top edges across the row: where its flat top starts and ends.
void stroke_lid_top(lv_layer_t* layer, lv_point_t o, const Lid& lid, lv_color_t color,
                    lv_opa_t opa);

/// A unit's own front wall, rail and side, in the shell's hardware colours.
void draw_shell_front(const ShellView& view, lv_layer_t* layer, lv_point_t o);

void stroke(lv_layer_t* layer, lv_point_t origin, const PointF* pts, int n, bool closed,
            lv_color_t color, lv_opa_t opa);

void build_lid(const ShellView& view, const TrayBox& unit_box, float lid_h, float x0, float x1,
               Lid& lid);

} // namespace helix::ui::tray
