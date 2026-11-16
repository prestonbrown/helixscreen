// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_creality_k1.cpp
/// @brief Creality K1 toolhead renderer — metallic body with large fan, sloped top, V-cut bottom

#include "nozzle_renderer_creality_k1.h"

#include "nozzle_renderer_common.h"
#include "theme_manager.h"

void draw_nozzle_creality_k1(lv_layer_t* layer, int32_t cx, int32_t cy,
                             std::optional<lv_color_t> filament, int32_t scale_unit, lv_opa_t opa) {
    // Creality K1 toolhead: compact body (~1:1.2 W:H), large circular fan,
    // sloped upper section, V-cut bottom with small nozzle tip.
    // cy is the CENTER of the entire print head assembly.

    // Base colors - metallic silver-gray
    lv_color_t metal_base = helix::nr_dim(theme_manager_get_color("filament_metal"), opa);

    // Lighting: light from top-left
    lv_color_t front_light = nr_lighten(metal_base, 40);
    lv_color_t front_mid = metal_base;
    lv_color_t front_dark = nr_darken(metal_base, 25);
    lv_color_t side_color = nr_darken(metal_base, 40);
    lv_color_t top_color = nr_lighten(metal_base, 60);
    lv_color_t outline_color = nr_darken(metal_base, 50);

    // Dimensions scaled by scale_unit — K1 is compact (~1:1.2 W:H ratio)
    int32_t body_half_width = (scale_unit * 16) / 10;
    int32_t body_height = (scale_unit * 48) / 10;
    int32_t body_depth = (scale_unit * 6) / 10;

    // Shift extruder left so filament line bisects the top edge of top surface
    cx = cx - body_depth / 2;

    // Nozzle tip dimensions
    int32_t tip_top_width = (scale_unit * 8) / 10;
    int32_t tip_bottom_width = (scale_unit * 3) / 10;
    int32_t tip_height = (scale_unit * 5) / 10;

    // Fan duct — large, the dominant feature of K1
    int32_t fan_radius = (scale_unit * 11) / 10;

    // Section heights
    int32_t slope_height = (body_height * 35) / 100;                       // Sloped upper 35%
    int32_t vcut_height = (body_height * 15) / 100;                        // V-cut bottom 15%
    int32_t fan_section_height = body_height - slope_height - vcut_height; // Middle 50%

    // Slope narrows at top by this amount per side
    int32_t slope_inset = body_half_width / 4;

    // Y positions
    int32_t body_top = cy - body_height / 2;
    int32_t body_bottom = cy + body_height / 2;
    int32_t slope_bottom = body_top + slope_height;
    int32_t vcut_top = body_bottom - vcut_height;
    int32_t tip_top = body_bottom;

    // ========================================
    // STEP 0: Sloped upper section (~35% of body)
    // Narrower at top, widens to full body width at slope_bottom
    // ========================================
    {
        int32_t top_half_width = body_half_width - slope_inset;
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.opa = LV_OPA_COVER;

        // === TAPERED ISOMETRIC TOP (top surface of the sloped section) ===
        for (int32_t d = 0; d <= body_depth; d++) {
            float iso_factor = (float)d / (float)body_depth;
            int32_t y_offset = (int32_t)(iso_factor * body_depth / 2);
            int32_t y_row = body_top - y_offset;
            int32_t x_left = cx - top_half_width + d;
            int32_t x_right = cx + top_half_width + d;

            lv_color_t row_color = nr_blend(top_color, nr_darken(top_color, 20), iso_factor);
            fill.color = row_color;
            lv_area_t row = {x_left, y_row, x_right, y_row};
            lv_draw_fill(layer, &fill, &row);
        }

        // === TAPERED FRONT FACE — per-scanline fill for slope ===
        for (int32_t dy = 0; dy <= slope_height; dy++) {
            float factor = (float)dy / (float)slope_height;
            int32_t half_w = top_half_width + (int32_t)(slope_inset * factor);
            int32_t y_row = body_top + dy;

            // Gradient from light at top to mid-tone at breakover
            lv_color_t base_color = nr_blend(front_light, front_mid, factor * 0.7f);

            // Horizontal shading for rounded bevel effect
            helix::nr_draw_bevel_row(layer, cx, half_w, y_row, base_color, 15);
        }

        // === TAPERED RIGHT SIDE ===
        for (int32_t dy = 0; dy <= slope_height; dy++) {
            float factor = (float)dy / (float)slope_height;
            int32_t half_w = top_half_width + (int32_t)(slope_inset * factor);
            int32_t y_front = body_top + dy;
            int32_t x_base = cx + half_w;

            for (int32_t d = 0; d <= body_depth; d++) {
                float iso_factor = (float)d / (float)body_depth;
                int32_t y_offset = (int32_t)(iso_factor * body_depth / 2);
                lv_color_t side_col = nr_blend(side_color, nr_darken(side_color, 30), iso_factor);
                fill.color = side_col;
                lv_area_t pixel = {x_base + d, y_front - y_offset, x_base + d, y_front - y_offset};
                lv_draw_fill(layer, &fill, &pixel);
            }
        }

        // === LEFT EDGE HIGHLIGHT (diagonal from narrow top to full width) ===
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_lighten(front_light, 30);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - top_half_width;
        line_dsc.p1.y = body_top;
        line_dsc.p2.x = cx - body_half_width;
        line_dsc.p2.y = slope_bottom;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 1: Breakover line — subtle horizontal separator
    // ========================================
    {
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_darken(front_mid, 15);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = slope_bottom;
        line_dsc.p2.x = cx + body_half_width;
        line_dsc.p2.y = slope_bottom;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 2: Fan section — vertical main body (~50%)
    // ========================================
    {
        // Front face with vertical gradient
        nr_draw_gradient_rect(layer, cx - body_half_width, slope_bottom, cx + body_half_width,
                              vcut_top, front_light, front_dark);

        // Right side face (isometric depth)
        nr_draw_iso_side(layer, cx + body_half_width, slope_bottom, vcut_top, body_depth,
                         side_color, nr_darken(side_color, 20));

        // Left edge highlight
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_lighten(front_light, 30);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = slope_bottom;
        line_dsc.p2.x = cx - body_half_width;
        line_dsc.p2.y = vcut_top;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 3: Large circular fan duct (K1's dominant feature)
    // ========================================
    {
        // Fan center positioned in the middle of the fan section
        int32_t fan_cy = slope_bottom + fan_section_height / 2;
        int32_t fan_cx = cx;
        helix::nr_draw_fan_ring(layer, fan_cx, fan_cy, fan_radius, front_mid, front_light,
                                metal_base, opa, true);
    }

    // ========================================
    // STEP 4: Bottom V-cut section (~15% of body)
    // Two chamfered corners creating V-shape, flat bottom between them
    // ========================================
    {
        int32_t cut_inset = body_half_width / 3; // How far the V cuts in from each side
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.opa = LV_OPA_COVER;

        for (int32_t dy = 0; dy <= vcut_height; dy++) {
            float factor = (float)dy / (float)vcut_height;
            // Left edge moves inward, right edge moves inward
            int32_t left_x = cx - body_half_width + (int32_t)(cut_inset * factor);
            int32_t right_x = cx + body_half_width - (int32_t)(cut_inset * factor);
            int32_t y_row = vcut_top + dy;

            // Gradient continues from fan section
            lv_color_t base_color = nr_blend(front_dark, nr_darken(front_dark, 15), factor);
            fill.color = base_color;
            lv_area_t row = {left_x, y_row, right_x, y_row};
            lv_draw_fill(layer, &fill, &row);
        }

        // Right side iso depth for V-cut section
        for (int32_t dy = 0; dy <= vcut_height; dy++) {
            float factor = (float)dy / (float)vcut_height;
            int32_t right_x = cx + body_half_width - (int32_t)(cut_inset * factor);
            int32_t y_front = vcut_top + dy;

            for (int32_t d = 0; d <= body_depth; d++) {
                float iso_factor = (float)d / (float)body_depth;
                int32_t y_offset = (int32_t)(iso_factor * body_depth / 2);
                lv_color_t side_col = nr_blend(side_color, nr_darken(side_color, 30), iso_factor);
                fill.color = side_col;
                lv_area_t pixel = {right_x + d, y_front - y_offset, right_x + d,
                                   y_front - y_offset};
                lv_draw_fill(layer, &fill, &pixel);
            }
        }

        // Bottom outline
        int32_t bottom_left = cx - body_half_width + cut_inset;
        int32_t bottom_right = cx + body_half_width - cut_inset;
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = outline_color;
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = bottom_left;
        line_dsc.p1.y = body_bottom;
        line_dsc.p2.x = bottom_right;
        line_dsc.p2.y = body_bottom;
        lv_draw_line(layer, &line_dsc);

        // Left chamfer edge
        line_dsc.color = nr_lighten(front_light, 20);
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = vcut_top;
        line_dsc.p2.x = bottom_left;
        line_dsc.p2.y = body_bottom;
        lv_draw_line(layer, &line_dsc);

        // Right chamfer edge
        line_dsc.color = outline_color;
        line_dsc.p1.x = cx + body_half_width;
        line_dsc.p1.y = vcut_top;
        line_dsc.p2.x = bottom_right;
        line_dsc.p2.y = body_bottom;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 5: Nozzle tip (small brass/copper tip below V-cut)
    // ========================================
    {
        helix::nr_draw_tinted_tip(layer, cx, tip_top, tip_top_width, tip_bottom_width, tip_height,
                                  nr_lighten(metal_base, 30), nr_darken(metal_base, 20), filament,
                                  opa);
    }
}
