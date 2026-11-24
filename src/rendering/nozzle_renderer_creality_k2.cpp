// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_creality_k2.cpp
/// @brief Creality K2 toolhead renderer — dark tower with vertical vent slits and U-cutout

#include "nozzle_renderer_creality_k2.h"

#include "nozzle_renderer_common.h"
#include "render_theme.h"

void draw_nozzle_creality_k2(lv_layer_t* layer, int32_t cx, int32_t cy,
                             std::optional<lv_color_t> filament, int32_t scale_unit, lv_opa_t opa) {
    // Creality K2 toolhead: tall narrow dark tower (~1:2 W:H ratio)
    // Key features: sloped upper section, 13 vertical vent slits, U-cutout at bottom
    // cy is the CENTER of the entire print head assembly.

    // Base color — metallic gray (same as K1 / Bambu default)
    lv_color_t body_base = helix::nr_dim(helix::render_theme::color("filament_metal"), opa);

    // Lighting: light from top-left
    lv_color_t front_light = nr_lighten(body_base, 35);
    lv_color_t front_mid = body_base;
    lv_color_t front_dark = nr_darken(body_base, 15);
    lv_color_t side_color = nr_darken(body_base, 25);
    lv_color_t top_color = nr_lighten(body_base, 50);
    lv_color_t outline_color = nr_darken(body_base, 35);

    // Dimensions — K2 is tall and narrow (~1:2 W:H ratio)
    int32_t body_half_width = (scale_unit * 10) / 10;
    int32_t body_height = (scale_unit * 48) / 10;
    int32_t body_depth = (scale_unit * 5) / 10;

    // Shift extruder left so filament line bisects the top edge of top surface
    cx = cx - body_depth / 2;

    // Nozzle tip — barely protrudes below body
    int32_t tip_top_width = (scale_unit * 5) / 10;
    int32_t tip_bottom_width = (scale_unit * 2) / 10;
    int32_t tip_height = (scale_unit * 3) / 10;

    // Section heights
    int32_t slope_height = (body_height * 45) / 100;  // Sloped upper 45%
    int32_t vent_height = (body_height * 30) / 100;   // Vent section 30%
    (void)(body_height - slope_height - vent_height); // Bottom pillars ~25%

    // Slope narrows at top
    int32_t slope_inset = body_half_width / 4;

    // U-cutout dimensions
    int32_t cutout_half_width = (body_half_width * 60) / 100;
    int32_t pillar_width = body_half_width - cutout_half_width;

    // Y positions
    int32_t body_top = cy - body_height / 2;
    int32_t body_bottom = cy + body_height / 2;
    int32_t slope_bottom = body_top + slope_height;
    int32_t vent_bottom = slope_bottom + vent_height;
    int32_t tip_top = body_bottom;

    // ========================================
    // STEP 0: Sloped upper section (~45% of body)
    // Narrower at top, widens to full body width at slope_bottom
    // ========================================
    {
        int32_t top_half_width = body_half_width - slope_inset;
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.opa = LV_OPA_COVER;

        // === TAPERED ISOMETRIC TOP (top surface of sloped section) ===
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
            helix::nr_draw_bevel_row(layer, cx, half_w, y_row, base_color, 12);
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
                lv_color_t side_col = nr_blend(side_color, nr_darken(side_color, 20), iso_factor);
                fill.color = side_col;
                lv_area_t pixel = {x_base + d, y_front - y_offset, x_base + d, y_front - y_offset};
                lv_draw_fill(layer, &fill, &pixel);
            }
        }

        // === LEFT EDGE HIGHLIGHT (diagonal from narrow top to full width) ===
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_lighten(front_light, 25);
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
        line_dsc.color = nr_darken(front_mid, 12);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = slope_bottom;
        line_dsc.p2.x = cx + body_half_width;
        line_dsc.p2.y = slope_bottom;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 2: Vent section (~30% of body) — vertical front face with 13 vent slits
    // ========================================
    {
        // Front face with vertical gradient
        nr_draw_gradient_rect(layer, cx - body_half_width, slope_bottom, cx + body_half_width,
                              vent_bottom, front_light, front_dark);

        // Right side face (isometric depth)
        nr_draw_iso_side(layer, cx + body_half_width, slope_bottom, vent_bottom, body_depth,
                         side_color, nr_darken(side_color, 15));

        // Left edge highlight
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_lighten(front_light, 25);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = slope_bottom;
        line_dsc.p2.x = cx - body_half_width;
        line_dsc.p2.y = vent_bottom;
        lv_draw_line(layer, &line_dsc);

        // === 13 VERTICAL VENT SLITS ===
        // Evenly spaced across the front face width
        int32_t vent_margin = body_half_width / 6; // Small margin from edges
        int32_t vent_area_left = cx - body_half_width + vent_margin;
        int32_t vent_area_right = cx + body_half_width - vent_margin;
        int32_t vent_area_width = vent_area_right - vent_area_left;
        int32_t slit_top = slope_bottom + 2;   // Small gap below breakover
        int32_t slit_bottom = vent_bottom - 2; // Small gap above bottom section

        lv_color_t slit_color = nr_darken(body_base, 30);

        constexpr int32_t kSlitCount = 13;
        lv_draw_fill_dsc_t slit_fill;
        lv_draw_fill_dsc_init(&slit_fill);
        slit_fill.color = slit_color;
        slit_fill.opa = LV_OPA_COVER;

        for (int32_t i = 0; i < kSlitCount; i++) {
            int32_t slit_x = vent_area_left + (vent_area_width * i) / (kSlitCount - 1);
            // Each slit is a 1px wide vertical line
            lv_area_t slit = {slit_x, slit_top, slit_x, slit_bottom};
            lv_draw_fill(layer, &slit_fill, &slit);
        }
    }

    // ========================================
    // STEP 3: Bottom pillars + U-cutout (~25% of body)
    // Two narrow pillars flanking a tall dark U-shaped cutout
    // ========================================
    {
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.opa = LV_OPA_COVER;

        // Left pillar — front face
        nr_draw_gradient_rect(layer, cx - body_half_width, vent_bottom,
                              cx - body_half_width + pillar_width, body_bottom, front_mid,
                              front_dark);

        // Right pillar — front face
        nr_draw_gradient_rect(layer, cx + body_half_width - pillar_width, vent_bottom,
                              cx + body_half_width, body_bottom, front_mid, front_dark);

        // Right side face (isometric depth) — extends for full bottom section
        nr_draw_iso_side(layer, cx + body_half_width, vent_bottom, body_bottom, body_depth,
                         side_color, nr_darken(side_color, 15));

        // Left edge highlight for left pillar
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = nr_lighten(front_light, 25);
        line_dsc.width = 1;
        line_dsc.opa = LV_OPA_COVER;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = vent_bottom;
        line_dsc.p2.x = cx - body_half_width;
        line_dsc.p2.y = body_bottom;
        lv_draw_line(layer, &line_dsc);

        // U-cutout — dark recessed area between pillars
        int32_t cutout_left = cx - cutout_half_width;
        int32_t cutout_right = cx + cutout_half_width;
        int32_t cutout_top = vent_bottom + 1; // Start just below vent section
        int32_t cutout_radius = LV_MAX(cutout_half_width / 3, 2);

        // Dark cutout background with slightly rounded top corners
        lv_color_t cutout_color = helix::nr_dim(lv_color_hex(0x0A0A0A), opa);
        fill.color = cutout_color;
        fill.radius = cutout_radius;
        lv_area_t cutout_area = {cutout_left, cutout_top, cutout_right, body_bottom};
        lv_draw_fill(layer, &fill, &cutout_area);

        // Heat block hidden inside U-cutout — not visible on K2
        // (recessed behind the dark cutout opening)

        // Bottom outline
        line_dsc.color = outline_color;
        line_dsc.p1.x = cx - body_half_width;
        line_dsc.p1.y = body_bottom;
        line_dsc.p2.x = cx + body_half_width;
        line_dsc.p2.y = body_bottom;
        lv_draw_line(layer, &line_dsc);
    }

    // ========================================
    // STEP 4: Nozzle tip (barely protrudes below body)
    // ========================================
    {
        helix::nr_draw_tinted_tip(layer, cx, tip_top, tip_top_width, tip_bottom_width, tip_height,
                                  nr_lighten(body_base, 25), nr_darken(body_base, 10), filament,
                                  opa, 0);
    }
}
