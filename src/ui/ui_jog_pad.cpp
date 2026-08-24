// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_jog_pad.h"

#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_widget_memory.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>

using namespace helix;

// Mode distances are defined in ui_panel_motion.h via get_jog_mode_distances()

// Zone boundary ratios (as fraction of total radius)
// Home button: 0% - 25%
// Inner zone (1mm): 25% - 60%
// Outer zone (10mm): 60% - 100%
static constexpr float HOME_ZONE_RATIO = 0.25f;
static constexpr float INNER_ZONE_BOUNDARY_RATIO = 0.60f; // Inner/outer zone boundary

// Responsive font selection using theme system
// Returns icon font for home button (radius-based for icon sizing)
static const lv_font_t* get_icon_font(lv_coord_t radius) {
    if (radius >= 120)
        return &mdi_icons_32;
    if (radius >= 80)
        return &mdi_icons_24;
    return &mdi_icons_24; // Minimum readable size
}

// Axis labels (X+, Y-, etc) - use small font from theme
static const lv_font_t* get_label_font([[maybe_unused]] lv_coord_t radius) {
    const lv_font_t* font = theme_manager_get_font("font_small");
    return font ? font : &noto_sans_14; // Fallback if theme not ready
}

// Distance labels (1mm, 10mm) - use extra small font from theme
static const lv_font_t* get_distance_font([[maybe_unused]] lv_coord_t radius) {
    const lv_font_t* font = theme_manager_get_font("font_xs");
    return font ? font : &noto_sans_10; // Fallback if theme not ready
}

// Widget state (stored in LVGL object user_data)
typedef struct {
    // Callbacks
    jog_pad_jog_cb_t jog_callback;
    jog_pad_home_cb_t home_callback;
    void* jog_user_data;
    void* home_user_data;

    // Current jog mode
    JogMode current_mode;

    // Cached zone geometry in pixels (recomputed from radius each time geometry is known).
    // Single source of truth for the zone-boundary formulas — see jog_pad_recompute_zones().
    int32_t radius_px;
    int32_t home_radius_px;
    int32_t inner_boundary_px;

    // Press state tracking for visual feedback
    bool is_pressed;
    JogDirection pressed_direction;
    bool pressed_is_inner;
    bool pressed_is_home;

    // Homing state: when false (not all axes homed) the center home button is
    // drawn in a warning color to signal that homing is required.
    bool is_homed;

    // Theme-aware colors (loaded from component scope or fallbacks)
    lv_color_t jog_color_outer_ring;
    lv_color_t jog_color_inner_circle;
    lv_color_t jog_color_grid_lines;
    lv_color_t jog_color_home_bg;
    lv_color_t jog_color_home_border;
    lv_color_t jog_color_home_text;
    lv_color_t jog_color_home_border_unhomed;
    lv_color_t jog_color_home_text_unhomed;
    lv_color_t jog_color_boundary_lines;
    lv_color_t jog_color_distance_labels;
    lv_color_t jog_color_axis_labels;
    lv_color_t jog_color_highlight;
} jog_pad_state_t;

// Helper: Get widget state from object
static jog_pad_state_t* get_state(lv_obj_t* obj) {
    return (jog_pad_state_t*)lv_obj_get_user_data(obj);
}

// Helper: Recompute cached zone-boundary pixel values from the total radius.
// This is the SINGLE place the zone-ratio formulas live — draw, press, and click
// all call this before reading the cached fields, so the math can never drift.
static void jog_pad_recompute_zones(jog_pad_state_t* st, int32_t radius) {
    st->radius_px = radius;
    st->home_radius_px = (int32_t)(radius * HOME_ZONE_RATIO);
    st->inner_boundary_px = (int32_t)(radius * INNER_ZONE_BOUNDARY_RATIO);
}

// Helper: Load colors from semantic theme tokens
static void load_colors(jog_pad_state_t* state, const char* /*component_scope_name*/) {
    // Use semantic theme tokens — these adapt to any theme automatically
    state->jog_color_outer_ring = theme_manager_get_color("secondary");
    state->jog_color_inner_circle = theme_manager_get_color("primary");
    state->jog_color_home_bg = theme_manager_get_color("elevated_bg");
    state->jog_color_home_border = theme_manager_get_color("secondary");
    state->jog_color_home_text = theme_manager_get_color("text");

    // Not-homed variants: tint the ring + icon amber to signal homing is required.
    state->jog_color_home_border_unhomed = theme_manager_get_color("warning");
    state->jog_color_home_text_unhomed = theme_manager_get_color("warning");

    // The two main quadrant dividers are drawn in the surrounding card background color
    // so they read as gaps cut through the pad rather than lines painted on top of it.
    state->jog_color_grid_lines = theme_manager_get_color("card_bg");

    // The 8 zone boundaries stay a subtle dark border line
    state->jog_color_boundary_lines = theme_manager_get_color("border");

    // Labels and highlight still need contrast against ring backgrounds
    lv_color_t ring_contrast = theme_manager_get_contrast_color(state->jog_color_outer_ring);
    state->jog_color_axis_labels = ring_contrast;
    state->jog_color_distance_labels = ring_contrast;
    state->jog_color_highlight = ring_contrast;

    spdlog::debug("[JogPad] Colors loaded from theme tokens ({} mode)",
                  theme_manager_is_dark_mode() ? "dark" : "light");
}

// Helper: Calculate angle from center point (0° = North, clockwise)
static float calculate_angle(lv_coord_t dx, lv_coord_t dy) {
    // atan2 gives us angle with 0° = East, counter-clockwise
    // We need 0° = North, clockwise
    float angle =
        atan2f(static_cast<float>(dx), static_cast<float>(-dy)) * 180.0f / static_cast<float>(M_PI);
    if (angle < 0)
        angle += 360.0f;
    return angle;
}

// Helper: Convert our angle system (0°=North, CW) to LVGL's (0°=East, CW)
static int convert_angle_to_lvgl(int our_angle) {
    // Our system: 0°=North (top), LVGL: 0°=East (right)
    // Rotate by -90° (subtract 90, or equivalently add 270)
    return (our_angle + 270) % 360;
}

// Helper: Determine jog direction from angle (8 wedges of 45° each)
static JogDirection angle_to_direction(float angle) {
    // Wedge boundaries (centered on cardinals):
    // N: 337.5-22.5°, NE: 22.5-67.5°, E: 67.5-112.5°, SE: 112.5-157.5°
    // S: 157.5-202.5°, SW: 202.5-247.5°, W: 247.5-292.5°, NW: 292.5-337.5°

    if (angle >= 337.5f || angle < 22.5f)
        return JogDirection::N;
    else if (angle >= 22.5f && angle < 67.5f)
        return JogDirection::NE;
    else if (angle >= 67.5f && angle < 112.5f)
        return JogDirection::E;
    else if (angle >= 112.5f && angle < 157.5f)
        return JogDirection::SE;
    else if (angle >= 157.5f && angle < 202.5f)
        return JogDirection::S;
    else if (angle >= 202.5f && angle < 247.5f)
        return JogDirection::SW;
    else if (angle >= 247.5f && angle < 292.5f)
        return JogDirection::W;
    else
        return JogDirection::NW;
}

// Custom draw event: Draw two-zone circular jog pad (Bambu Lab style)
//
// ================================================================================
// LVGL ARC DRAWING SYSTEM - THE TRUTH (after much painful debugging)
// ================================================================================
//
// CRITICAL: LVGL arcs are NOT strokes - they are RINGS with thickness measured INWARD
//
// Parameters:
//   - `radius` = OUTER EDGE of the ring (NOT centerline!)
//   - `width` = THICKNESS measured INWARD from outer edge
//   - Inner edge = radius - width
//
// WRONG MENTAL MODEL (what we initially thought):
//   - radius = centerline, width = stroke that extends ±width/2
//   - This is INCORRECT and will give wrong results
//
// CORRECT MENTAL MODEL:
//   - radius = outer boundary you want
//   - width = how thick inward from that boundary
//
// Example: Draw a ring from 25% to 50% of total radius:
//   Step 1: Identify boundaries
//     - Inner edge (start) = 25%
//     - Outer edge (end) = 50%
//   Step 2: Calculate parameters
//     - radius = 50% (the OUTER edge we want)
//     - width = 50% - 25% = 25% (thickness measured inward)
//   Step 3: Verify
//     - Inner edge = radius - width = 50% - 25% = 25% ✓
//     - Outer edge = radius = 50% ✓
//
// Example 2: Draw a ring from 50% to 100%:
//     - radius = 100% (outer edge at full radius)
//     - width = 100% - 50% = 50% (thickness inward)
//     - Inner edge = 100% - 50% = 50% ✓
//
// This matches how the background circles work:
//   - Full outer circle: radius = 100%, width = 100% (fills 0% to 100%)
//   - Inner overlay: radius = 50%, width = 50% (fills 0% to 50%)
//
// DO NOT confuse this with other drawing APIs that use centerline+stroke!
//
static void jog_pad_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    jog_pad_state_t* state = get_state(obj);
    if (!state)
        return;

    // Get container dimensions and center point
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    lv_coord_t width = lv_area_get_width(&obj_coords);
    lv_coord_t height = lv_area_get_height(&obj_coords);
    lv_coord_t center_x = obj_coords.x1 + width / 2;
    lv_coord_t center_y = obj_coords.y1 + height / 2;
    lv_coord_t radius = width / 2;

    // Zone boundary: Inner circle at INNER_ZONE_BOUNDARY_RATIO of total radius
    jog_pad_recompute_zones(state, radius);
    lv_coord_t inner_boundary = state->inner_boundary_px;

    // LAYERED APPROACH: Draw from back to front
    // Layer 1: Full light gray background circle (0% to 100% radius)
    // Layer 2: Dark gray inner circle overlay (0% to 60% radius)
    // Result: Outer ring (60-100%) shows as light gray, extends all the way to edge

    // Layer 1: Full background circle (light gray)
    lv_draw_arc_dsc_t bg_arc_dsc;
    lv_draw_arc_dsc_init(&bg_arc_dsc);
    bg_arc_dsc.color = state->jog_color_outer_ring;
    bg_arc_dsc.width = static_cast<uint16_t>(radius * 2); // Width = diameter, fills entire circle
    bg_arc_dsc.center.x = center_x;
    bg_arc_dsc.center.y = center_y;
    bg_arc_dsc.radius = static_cast<uint16_t>(radius); // Centerline at outer edge
    bg_arc_dsc.start_angle = 0;
    bg_arc_dsc.end_angle = 360;
    lv_draw_arc(layer, &bg_arc_dsc);

    // Layer 2: Inner circle overlay (dark gray)
    lv_draw_arc_dsc_t inner_arc_dsc;
    lv_draw_arc_dsc_init(&inner_arc_dsc);
    inner_arc_dsc.color = state->jog_color_inner_circle;
    inner_arc_dsc.width = static_cast<uint16_t>(inner_boundary * 2); // Width = inner diameter
    inner_arc_dsc.center.x = center_x;
    inner_arc_dsc.center.y = center_y;
    inner_arc_dsc.radius = static_cast<uint16_t>(inner_boundary);
    inner_arc_dsc.start_angle = 0;
    inner_arc_dsc.end_angle = 360;
    lv_draw_arc(layer, &inner_arc_dsc);

    // Draw 2 diagonal divider lines (NE-SW and NW-SE) like Bambu Lab
    // Creates 4 pie-shaped quadrants for diagonal movements
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = state->jog_color_grid_lines;
    line_dsc.width = 6;
    line_dsc.opa = LV_OPA_COVER;

    // NE-SW diagonal (45°-225°)
    line_dsc.p1.x = center_x + radius * 0.707f;
    line_dsc.p1.y = center_y - radius * 0.707f;
    line_dsc.p2.x = center_x - radius * 0.707f;
    line_dsc.p2.y = center_y + radius * 0.707f;
    lv_draw_line(layer, &line_dsc);

    // NW-SE diagonal (315°-135°)
    line_dsc.p1.x = center_x - radius * 0.707f;
    line_dsc.p1.y = center_y - radius * 0.707f;
    line_dsc.p2.x = center_x + radius * 0.707f;
    line_dsc.p2.y = center_y + radius * 0.707f;
    lv_draw_line(layer, &line_dsc);

    // Draw center home button area
    lv_coord_t home_radius = state->home_radius_px;

    // Draw filled background circle for home area
    lv_draw_arc_dsc_t home_bg_dsc;
    lv_draw_arc_dsc_init(&home_bg_dsc);
    home_bg_dsc.color = state->jog_color_home_bg;
    home_bg_dsc.width = static_cast<uint16_t>(home_radius * 2); // Fill entire circle
    home_bg_dsc.center.x = center_x;
    home_bg_dsc.center.y = center_y;
    home_bg_dsc.radius = static_cast<uint16_t>(home_radius);
    home_bg_dsc.start_angle = 0;
    home_bg_dsc.end_angle = 360;
    lv_draw_arc(layer, &home_bg_dsc);

    // Draw visual ring border around home area (warning-tinted when not homed)
    lv_draw_arc_dsc_t home_ring_dsc;
    lv_draw_arc_dsc_init(&home_ring_dsc);
    home_ring_dsc.color =
        state->is_homed ? state->jog_color_home_border : state->jog_color_home_border_unhomed;
    home_ring_dsc.width = 3; // Border thickness
    home_ring_dsc.center.x = center_x;
    home_ring_dsc.center.y = center_y;
    home_ring_dsc.radius = static_cast<uint16_t>(home_radius);
    home_ring_dsc.start_angle = 0;
    home_ring_dsc.end_angle = 360;
    lv_draw_arc(layer, &home_ring_dsc);

    // Draw center home icon (scaled to jog pad size)
    lv_draw_label_dsc_t home_label_dsc;
    lv_draw_label_dsc_init(&home_label_dsc);
    home_label_dsc.color =
        state->is_homed ? state->jog_color_home_text : state->jog_color_home_text_unhomed;
    home_label_dsc.text = ICON_HOME;
    home_label_dsc.font = get_icon_font(radius);
    home_label_dsc.align = LV_TEXT_ALIGN_CENTER;

    // Scale icon area proportionally to home button size
    lv_coord_t icon_half_w = (lv_coord_t)(home_radius * 0.6f);
    lv_coord_t icon_half_h = (lv_coord_t)(home_radius * 0.4f);
    lv_area_t home_label_area;
    home_label_area.x1 = center_x - icon_half_w;
    home_label_area.y1 = center_y - icon_half_h;
    home_label_area.x2 = center_x + icon_half_w;
    home_label_area.y2 = center_y + icon_half_h;
    lv_draw_label(layer, &home_label_dsc, &home_label_area);

    // Draw zone boundary lines to show the edges of each directional click zone
    // 8 zones means 8 boundaries - at 22.5° intervals starting from 22.5°
    for (float angle_deg : {22.5f, 67.5f, 112.5f, 157.5f, 202.5f, 247.5f, 292.5f, 337.5f}) {
        float angle_rad = angle_deg * static_cast<float>(M_PI) / 180.0f;

        lv_draw_line_dsc_t boundary_line_dsc;
        lv_draw_line_dsc_init(&boundary_line_dsc);
        boundary_line_dsc.color = state->jog_color_boundary_lines;
        boundary_line_dsc.width = 1;
        boundary_line_dsc.opa = LV_OPA_50;

        // Start point: just outside home button (27% of radius)
        boundary_line_dsc.p1.x = center_x + (radius * 0.27f) * cosf(angle_rad);
        boundary_line_dsc.p1.y = center_y + (radius * 0.27f) * sinf(angle_rad);

        // End point: at outer edge (98% to avoid clipping)
        boundary_line_dsc.p2.x = center_x + (radius * 0.98f) * cosf(angle_rad);
        boundary_line_dsc.p2.y = center_y + (radius * 0.98f) * sinf(angle_rad);

        lv_draw_line(layer, &boundary_line_dsc);
    }

    // Draw distance labels showing movement amounts for each ring (scaled fonts)
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = state->jog_color_distance_labels;
    label_dsc.font = get_distance_font(radius);
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t label_area;

    // Scale label box sizes proportionally (approx 0.25 * radius for width)
    lv_coord_t dist_label_w = (lv_coord_t)(radius * 0.30f);
    lv_coord_t dist_label_h = (lv_coord_t)(radius * 0.12f);
    lv_coord_t dist_offset_y = (lv_coord_t)(radius * 0.05f);

    // Distance labels from current mode (Fine: 0.1/1, Coarse: 1/10, Turbo: 10/50)
    const auto& mode_dist = get_jog_mode_distances(state->current_mode);
    const char* inner_label = mode_dist.inner_label;
    const char* outer_label = mode_dist.outer_label;

    label_dsc.text = inner_label;
    lv_coord_t inner_label_radius = (lv_coord_t)((home_radius + inner_boundary) * 0.5f);
    label_area.x1 = center_x + (lv_coord_t)(inner_label_radius * 0.707f);
    label_area.y1 = center_y - (lv_coord_t)(inner_label_radius * 0.707f) + dist_offset_y;
    label_area.x2 = label_area.x1 + dist_label_w;
    label_area.y2 = label_area.y1 + dist_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Outer ring distance label
    label_dsc.text = outer_label;
    lv_coord_t outer_label_radius = (lv_coord_t)((radius + inner_boundary) * 0.5f);
    label_area.x1 = center_x + (lv_coord_t)(outer_label_radius * 0.707f);
    label_area.y1 = center_y - (lv_coord_t)(outer_label_radius * 0.707f) + dist_offset_y;
    label_area.x2 = label_area.x1 + dist_label_w + 10; // Slightly wider for longer labels
    label_area.y2 = label_area.y1 + dist_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Draw axis labels (cardinal directions) with scaled font
    label_dsc.color = state->jog_color_axis_labels;
    label_dsc.font = get_label_font(radius);

    // Scale axis label positions and sizes proportionally
    lv_coord_t axis_label_w = (lv_coord_t)(radius * 0.18f);
    lv_coord_t axis_label_h = (lv_coord_t)(radius * 0.14f);
    lv_coord_t axis_offset = (lv_coord_t)(radius * 0.08f);

    // Y+ (North)
    label_dsc.text = "Y+";
    label_area.x1 = center_x - axis_label_w / 2;
    label_area.y1 = (int32_t)(center_y - radius * 0.80f) - axis_offset;
    label_area.x2 = label_area.x1 + axis_label_w;
    label_area.y2 = label_area.y1 + axis_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // X+ (East)
    label_dsc.text = "X+";
    label_area.x1 = (int32_t)(center_x + radius * 0.80f) - axis_label_w / 2;
    label_area.y1 = center_y - axis_offset;
    label_area.x2 = label_area.x1 + axis_label_w;
    label_area.y2 = label_area.y1 + axis_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Y- (South)
    label_dsc.text = "Y-";
    label_area.x1 = center_x - axis_label_w / 2;
    label_area.y1 = (int32_t)(center_y + radius * 0.80f) - axis_offset;
    label_area.x2 = label_area.x1 + axis_label_w;
    label_area.y2 = label_area.y1 + axis_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // X- (West)
    label_dsc.text = "X-";
    label_area.x1 = (int32_t)(center_x - radius * 0.80f) - axis_label_w / 2;
    label_area.y1 = center_y - axis_offset;
    label_area.x2 = label_area.x1 + axis_label_w;
    label_area.y2 = label_area.y1 + axis_label_h;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Draw press highlight overlay if a zone is pressed
    if (state->is_pressed) {
        if (state->pressed_is_home) {
            // Highlight home button with filled circle
            lv_draw_arc_dsc_t highlight_dsc;
            lv_draw_arc_dsc_init(&highlight_dsc);
            highlight_dsc.color = state->jog_color_highlight;
            highlight_dsc.opa = LV_OPA_60; // ~23% opacity
            lv_coord_t highlight_home_radius = state->home_radius_px;
            highlight_dsc.width = static_cast<uint16_t>(highlight_home_radius * 2);
            highlight_dsc.center.x = center_x;
            highlight_dsc.center.y = center_y;
            highlight_dsc.radius = static_cast<uint16_t>(highlight_home_radius);
            highlight_dsc.start_angle = 0;
            highlight_dsc.end_angle = 360;
            lv_draw_arc(layer, &highlight_dsc);
        } else {
            // Highlight directional wedge (45° segment)
            // Map direction enum to angle in our coordinate system (0°=North, CW)
            // Enum order: N=0, S=1, E=2, W=3, NE=4, NW=5, SE=6, SW=7
            // Angle mapping: N=0°, NE=45°, E=90°, SE=135°, S=180°, SW=225°, W=270°, NW=315°
            int direction_angles[] = {0, 180, 90, 270, 45, 315, 135, 225};
            int our_wedge_center = direction_angles[static_cast<int>(state->pressed_direction)];
            int our_wedge_start = our_wedge_center - 22; // 22.5° on each side
            int our_wedge_end = our_wedge_center + 23;   // Total 45° wedge

            // Convert to LVGL's coordinate system (0°=East, CW)
            int lvgl_start = convert_angle_to_lvgl(our_wedge_start);
            int lvgl_end = convert_angle_to_lvgl(our_wedge_end);

            lv_draw_arc_dsc_t highlight_dsc;
            lv_draw_arc_dsc_init(&highlight_dsc);
            highlight_dsc.color = state->jog_color_highlight;
            highlight_dsc.opa = LV_OPA_60;

            if (state->pressed_is_inner) {
                // Inner zone: Draw arc ring from 25% to 60%
                lv_coord_t highlight_inner_boundary = state->inner_boundary_px;
                lv_coord_t home_edge = state->home_radius_px;

                highlight_dsc.width =
                    static_cast<uint16_t>(highlight_inner_boundary - home_edge); // 35% thickness
                highlight_dsc.center.x = center_x;
                highlight_dsc.center.y = center_y;
                highlight_dsc.radius =
                    static_cast<uint16_t>(highlight_inner_boundary); // 60% outer edge
                highlight_dsc.start_angle = lvgl_start;
                highlight_dsc.end_angle = lvgl_end;
                lv_draw_arc(layer, &highlight_dsc);
            } else {
                // Outer zone: Draw arc ring from 60% to 100%
                lv_coord_t highlight_outer_inner_boundary = state->inner_boundary_px;

                highlight_dsc.width =
                    static_cast<uint16_t>(radius - highlight_outer_inner_boundary); // 40% thickness
                highlight_dsc.center.x = center_x;
                highlight_dsc.center.y = center_y;
                highlight_dsc.radius = static_cast<uint16_t>(radius); // 100% outer edge
                highlight_dsc.start_angle = lvgl_start;
                highlight_dsc.end_angle = lvgl_end;
                lv_draw_arc(layer, &highlight_dsc);
            }
        }
    }

    // Disabled state: overlay a translucent scrim so the pad visibly reads as
    // unavailable. Presses are already blocked upstream (LVGL's input handling
    // skips LV_STATE_DISABLED objects), so this is purely the visual cue.
    if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        lv_draw_arc_dsc_t scrim_dsc;
        lv_draw_arc_dsc_init(&scrim_dsc);
        scrim_dsc.color = theme_manager_get_color("elevated_bg");
        scrim_dsc.opa = LV_OPA_70;
        scrim_dsc.width = static_cast<uint16_t>(radius * 2); // Fill entire circle
        scrim_dsc.center.x = center_x;
        scrim_dsc.center.y = center_y;
        scrim_dsc.radius = static_cast<uint16_t>(radius);
        scrim_dsc.start_angle = 0;
        scrim_dsc.end_angle = 360;
        lv_draw_arc(layer, &scrim_dsc);
    }
}

// Press event: Track pressed zone for visual feedback
static void jog_pad_press_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    jog_pad_state_t* state = get_state(obj);
    if (!state)
        return;

    lv_point_t point;
    lv_indev_t* indev = lv_indev_active();
    lv_indev_get_point(indev, &point);

    // Get container dimensions and center
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    lv_coord_t width = lv_area_get_width(&obj_coords);
    lv_coord_t center_x = obj_coords.x1 + width / 2;
    lv_coord_t center_y = obj_coords.y1 + width / 2;
    lv_coord_t radius = width / 2;

    // Calculate distance and angle from center
    lv_coord_t dx = point.x - center_x;
    lv_coord_t dy = point.y - center_y;
    float distance = sqrtf((float)(dx * dx + dy * dy));

    // Check if press is within circular boundary
    if (distance > radius) {
        state->is_pressed = false;
        return;
    }

    state->is_pressed = true;

    jog_pad_recompute_zones(state, radius);

    // Home button: center at HOME_ZONE_RATIO
    if (distance < state->home_radius_px) {
        state->pressed_is_home = true;
        state->pressed_is_inner = false;
        lv_obj_invalidate(obj); // Trigger redraw
        return;
    }

    state->pressed_is_home = false;

    // Determine direction from angle
    float angle = calculate_angle(dx, dy);
    state->pressed_direction = angle_to_direction(angle);

    // Determine if inner or outer zone
    state->pressed_is_inner = (distance < state->inner_boundary_px);

    // Trigger redraw to show highlight
    lv_obj_invalidate(obj);
}

// Release event: Clear press state
static void jog_pad_release_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    jog_pad_state_t* state = get_state(obj);
    if (!state)
        return;

    if (state->is_pressed) {
        state->is_pressed = false;
        // Trigger redraw to remove highlight
        lv_obj_invalidate(obj);
    }
}

// Click event: Detect zone and trigger jog
static void jog_pad_click_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    jog_pad_state_t* state = get_state(obj);
    if (!state)
        return;

    lv_point_t point;
    lv_indev_t* indev = lv_indev_active();
    lv_indev_get_point(indev, &point);

    // Get container dimensions and center
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    lv_coord_t width = lv_area_get_width(&obj_coords);
    lv_coord_t center_x = obj_coords.x1 + width / 2;
    lv_coord_t center_y = obj_coords.y1 + width / 2;
    lv_coord_t radius = width / 2;

    // Calculate distance and angle from center
    lv_coord_t dx = point.x - center_x;
    lv_coord_t dy = point.y - center_y;
    float distance = sqrtf((float)(dx * dx + dy * dy));

    // Check if click is within circular boundary
    if (distance > radius)
        return;

    jog_pad_recompute_zones(state, radius);

    // Home button: center at HOME_ZONE_RATIO
    if (distance < state->home_radius_px) {
        if (state->home_callback) {
            state->home_callback(state->home_user_data);
        }
        spdlog::debug("[JogPad] Home button clicked");
        return;
    }

    // Determine direction from angle
    float angle = calculate_angle(dx, dy);
    JogDirection direction = angle_to_direction(angle);

    // Zone boundary: inner ring (25-60%), outer ring (60-100%)
    const auto& mode_dist = get_jog_mode_distances(state->current_mode);
    float jog_dist = (distance < state->inner_boundary_px) ? mode_dist.inner : mode_dist.outer;

    if (state->jog_callback) {
        state->jog_callback(direction, jog_dist, state->jog_user_data);
    }

    const char* dir_names[] = {"N(+Y)",    "S(-Y)",    "E(+X)",    "W(-X)",
                               "NE(+X+Y)", "NW(-X+Y)", "SE(+X-Y)", "SW(-X-Y)"};
    spdlog::debug("[JogPad] Jog: {} {:.1f}mm", dir_names[static_cast<int>(direction)], jog_dist);
}

// Cleanup callback: Free allocated state
static void jog_pad_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    // Transfer ownership to RAII wrapper - automatic cleanup
    lvgl_unique_ptr<jog_pad_state_t> state(get_state(obj));
    lv_obj_set_user_data(obj, nullptr);
    // state automatically freed via ~unique_ptr() when function exits
}

// Public API Implementation
lv_obj_t* ui_jog_pad_create(lv_obj_t* parent) {
    // Create base object
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj)
        return nullptr;

    // Allocate state using RAII helper
    auto state_ptr = lvgl_make_unique<jog_pad_state_t>();
    if (!state_ptr) {
        helix::ui::safe_delete(obj);
        return nullptr;
    }

    // Get raw pointer for initialization
    jog_pad_state_t* state = state_ptr.get();

    // Initialize state
    state->current_mode = JogMode::Coarse;
    state->is_homed = false; // Printer boots unhomed; observer updates this

    // Load colors from component scope (tries "motion_panel" first, falls back to defaults)
    load_colors(state, "motion_panel");

    // Transfer ownership to LVGL widget
    lv_obj_set_user_data(obj, state_ptr.release());

    // Configure object appearance
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 160, 0); // Circular appearance
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, jog_pad_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, jog_pad_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, jog_pad_release_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(obj, jog_pad_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, jog_pad_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[JogPad] Widget created");
    return obj;
}

void ui_jog_pad_set_jog_callback(lv_obj_t* obj, jog_pad_jog_cb_t cb, void* user_data) {
    jog_pad_state_t* state = get_state(obj);
    if (state) {
        state->jog_callback = cb;
        state->jog_user_data = user_data;
    }
}

void ui_jog_pad_set_home_callback(lv_obj_t* obj, jog_pad_home_cb_t cb, void* user_data) {
    jog_pad_state_t* state = get_state(obj);
    if (state) {
        state->home_callback = cb;
        state->home_user_data = user_data;
    }
}

void ui_jog_pad_set_mode(lv_obj_t* obj, JogMode mode) {
    jog_pad_state_t* state = get_state(obj);
    if (state && static_cast<int>(mode) >= 0 && static_cast<int>(mode) < JOG_MODE_COUNT) {
        state->current_mode = mode;
    }
}

JogMode ui_jog_pad_get_mode(lv_obj_t* obj) {
    jog_pad_state_t* state = get_state(obj);
    return state ? state->current_mode : JogMode::Coarse;
}

void ui_jog_pad_refresh_colors(lv_obj_t* obj) {
    jog_pad_state_t* state = get_state(obj);
    if (state) {
        load_colors(state, "motion_panel");
        lv_obj_invalidate(obj); // Trigger redraw
    }
}

void ui_jog_pad_set_homed(lv_obj_t* obj, bool homed) {
    jog_pad_state_t* state = get_state(obj);
    if (!state || state->is_homed == homed)
        return; // No change — avoid a redundant redraw
    state->is_homed = homed;
    lv_obj_invalidate(obj); // Redraw the center home button in its new color
}

void ui_jog_pad_set_enabled(lv_obj_t* obj, bool enabled) {
    if (!obj)
        return;
    bool currently_disabled = lv_obj_has_state(obj, LV_STATE_DISABLED);
    if (enabled == !currently_disabled)
        return; // No change — avoid a redundant redraw
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
    lv_obj_invalidate(obj); // Redraw with/without the disabled scrim
}
