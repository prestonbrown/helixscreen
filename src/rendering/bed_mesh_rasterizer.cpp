// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

/**
 * @file bed_mesh_rasterizer.cpp
 * @brief Triangle rasterization implementation for bed mesh visualization
 *
 * Implements scanline-based triangle filling with:
 * - Solid color fills using batched rectangle draws
 * - Gradient fills with adaptive segment counts
 * - Per-vertex color interpolation
 */

#include "bed_mesh_rasterizer.h"

#include "bed_mesh_buffer.h"
#include "bed_mesh_gradient.h"

#include <algorithm>

namespace {

// ============================================================================
// Internal Helper Functions
// ============================================================================

/**
 * Sort three vertices by Y coordinate using bubble sort
 * Optimized for the common case of 3 elements
 *
 * @tparam T Type of x coordinate (allows int or struct)
 */
template <typename T> inline void sort_by_y(int& y1, T& x1, int& y2, T& x2, int& y3, T& x3) {
    if (y1 > y2) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }
    if (y2 > y3) {
        std::swap(y2, y3);
        std::swap(x2, x3);
    }
    if (y1 > y2) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }
}

/**
 * Compute scanline X coordinates for triangle edges at given Y
 * Uses linear interpolation along triangle edges
 *
 * @param y Current scanline Y coordinate
 * @param y1, x1 Top vertex (after Y-sorting)
 * @param y2, x2 Middle vertex
 * @param y3, x3 Bottom vertex
 * @param out_x_left Output: left edge X coordinate
 * @param out_x_right Output: right edge X coordinate
 */
inline void compute_scanline_x(int y, int y1, int x1, int y2, int x2, int y3, int x3,
                               int* out_x_left, int* out_x_right) {
    // Long edge: y1 -> y3
    double t_long = (y - y1) / static_cast<double>(y3 - y1);
    int x_long = x1 + static_cast<int>(t_long * (x3 - x1));

    // Short edge: split at y2
    int x_short;
    if (y < y2) {
        // Upper half: y1 -> y2
        if (y2 == y1) {
            x_short = x1;
        } else {
            double t = (y - y1) / static_cast<double>(y2 - y1);
            x_short = x1 + static_cast<int>(t * (x2 - x1));
        }
    } else {
        // Lower half: y2 -> y3
        if (y3 == y2) {
            x_short = x2;
        } else {
            double t = (y - y2) / static_cast<double>(y3 - y2);
            x_short = x2 + static_cast<int>(t * (x3 - x2));
        }
    }

    // Ensure correct ordering
    *out_x_left = std::min(x_long, x_short);
    *out_x_right = std::max(x_long, x_short);
}

/**
 * Interpolate position and color along a triangle edge
 * Handles divide-by-zero case when edge vertices have same Y coordinate
 */
inline void interpolate_edge(int y, int y0, int x0, const bed_mesh_rgb_t& c0, int y1, int x1,
                             const bed_mesh_rgb_t& c1, int* x_out, bed_mesh_rgb_t* c_out) {
    if (y1 == y0) {
        *x_out = x0;
        *c_out = c0;
    } else {
        double t = (y - y0) / static_cast<double>(y1 - y0);
        *x_out = x0 + static_cast<int>(t * (x1 - x0));
        *c_out = bed_mesh_gradient_lerp_color(c0, c1, t);
    }
}

} // anonymous namespace

namespace helix {
namespace mesh {

void fill_triangle_solid(lv_layer_t* layer, int x1, int y1, int x2, int y2, int x3, int y3,
                         lv_color_t color, lv_opa_t opacity) {
    // Sort vertices by Y coordinate
    sort_by_y(y1, x1, y2, x2, y3, x3);

    // Skip degenerate triangles
    if (y1 == y3)
        return;

    // Note: LVGL's layer system handles clipping automatically

    // Prepare draw descriptor for horizontal spans
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opacity;
    dsc.border_width = 0;

    // Scanline fill with batched rect draws (15-20% faster than pixel-by-pixel)
    for (int y = y1; y <= y3; y++) {
        // Compute left/right edges
        int x_left, x_right;
        compute_scanline_x(y, y1, x1, y2, x2, y3, x3, &x_left, &x_right);

        // Draw horizontal span as single rectangle
        if (x_left <= x_right) {
            lv_area_t rect_area;
            rect_area.x1 = x_left;
            rect_area.y1 = y;
            rect_area.x2 = x_right;
            rect_area.y2 = y;
            lv_draw_rect(layer, &dsc, &rect_area);
        }
    }
}

void fill_triangle_gradient(lv_layer_t* layer, int x1, int y1, lv_color_t c1, int x2, int y2,
                            lv_color_t c2, int x3, int y3, lv_color_t c3, lv_opa_t opacity) {
    // Sort vertices by Y coordinate, keeping colors aligned
    struct Vertex {
        int x, y;
        bed_mesh_rgb_t color;
    };
    Vertex v[3] = {{x1, y1, {c1.red, c1.green, c1.blue}},
                   {x2, y2, {c2.red, c2.green, c2.blue}},
                   {x3, y3, {c3.red, c3.green, c3.blue}}};

    if (v[0].y > v[1].y)
        std::swap(v[0], v[1]);
    if (v[1].y > v[2].y)
        std::swap(v[1], v[2]);
    if (v[0].y > v[1].y)
        std::swap(v[0], v[1]);

    // Skip degenerate triangles
    if (v[0].y == v[2].y)
        return;

    // Note: LVGL's layer system handles clipping automatically

    // Prepare draw descriptor for gradient segments
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = opacity;
    dsc.border_width = 0;

    // Scanline fill with color interpolation and batched rect draws
    for (int y = v[0].y; y <= v[2].y; y++) {
        // Interpolate along long edge (v0 -> v2)
        double t_long = (y - v[0].y) / static_cast<double>(v[2].y - v[0].y);
        int x_long = v[0].x + static_cast<int>(t_long * (v[2].x - v[0].x));
        bed_mesh_rgb_t c_long = bed_mesh_gradient_lerp_color(v[0].color, v[2].color, t_long);

        // Interpolate along short edge (upper half: v0->v1, lower half: v1->v2)
        int x_short;
        bed_mesh_rgb_t c_short;
        if (y < v[1].y) {
            interpolate_edge(y, v[0].y, v[0].x, v[0].color, v[1].y, v[1].x, v[1].color, &x_short,
                             &c_short);
        } else {
            interpolate_edge(y, v[1].y, v[1].x, v[1].color, v[2].y, v[2].x, v[2].color, &x_short,
                             &c_short);
        }

        // Ensure left/right ordering
        int x_left = std::min(x_long, x_short);
        int x_right = std::max(x_long, x_short);
        bed_mesh_rgb_t c_left = (x_long < x_short) ? c_long : c_short;
        bed_mesh_rgb_t c_right = (x_long < x_short) ? c_short : c_long;

        int line_width = x_right - x_left + 1;
        if (line_width <= 0)
            continue;

        // Performance: use solid color for thin lines
        if (line_width < GRADIENT_MIN_LINE_WIDTH) {
            bed_mesh_rgb_t avg = bed_mesh_gradient_lerp_color(c_left, c_right, 0.5);
            lv_color_t avg_color = lv_color_make(avg.r, avg.g, avg.b);
            dsc.bg_color = avg_color;

            lv_area_t rect_area;
            rect_area.x1 = x_left;
            rect_area.y1 = y;
            rect_area.x2 = x_right;
            rect_area.y2 = y;
            lv_draw_rect(layer, &dsc, &rect_area);
        } else {
            // ========== Adaptive Gradient Rasterization ==========
            // Use adaptive segment count based on line width for quality/performance balance
            int segment_count;
            if (line_width < GRADIENT_THIN_LINE_THRESHOLD) {
                segment_count = GRADIENT_THIN_SEGMENT_COUNT;
            } else if (line_width < GRADIENT_MEDIUM_LINE_THRESHOLD) {
                segment_count = GRADIENT_MEDIUM_SEGMENT_COUNT;
            } else {
                segment_count = GRADIENT_WIDE_SEGMENT_COUNT;
            }

            for (int segment_index = 0; segment_index < segment_count; segment_index++) {
                // Compute segment horizontal span
                int seg_x_start = x_left + (segment_index * line_width) / segment_count;
                int seg_x_end = x_left + ((segment_index + 1) * line_width) / segment_count - 1;
                if (seg_x_start > seg_x_end)
                    continue;

                // Sample color at segment center for better color distribution
                double interpolation_factor =
                    (segment_index + GRADIENT_SEGMENT_SAMPLE_POSITION) / segment_count;
                bed_mesh_rgb_t seg_color =
                    bed_mesh_gradient_lerp_color(c_left, c_right, interpolation_factor);
                lv_color_t color = lv_color_make(seg_color.r, seg_color.g, seg_color.b);
                dsc.bg_color = color;

                // Draw segment as horizontal rectangle
                lv_area_t rect_area;
                rect_area.x1 = seg_x_start;
                rect_area.y1 = y;
                rect_area.x2 = seg_x_end;
                rect_area.y2 = y;
                lv_draw_rect(layer, &dsc, &rect_area);
            }
        }
    }
}

// ============================================================================
// Buffer-targeted overloads (no LVGL calls — safe for background threads)
// ============================================================================

void fill_triangle_solid(PixelBuffer& buf, int x1, int y1, int x2, int y2, int x3, int y3,
                         lv_color_t color, lv_opa_t opacity) {
    // Convert lv_opa_t (0-255) to alpha byte
    buf.fill_triangle_solid(x1, y1, x2, y2, x3, y3, color.red, color.green, color.blue, opacity);
}

void fill_triangle_gradient(PixelBuffer& buf, int x1, int y1, lv_color_t c1, int x2, int y2,
                            lv_color_t c2, int x3, int y3, lv_color_t c3, lv_opa_t opacity) {
    buf.fill_triangle_gradient(x1, y1, c1.red, c1.green, c1.blue, x2, y2, c2.red, c2.green, c2.blue,
                               x3, y3, c3.red, c3.green, c3.blue, opacity);
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
