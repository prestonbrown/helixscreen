/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_panel_motion.h"
#include "ui_component_header_bar.h"
#include "ui_nav.h"
#include "ui_utils.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>
#include <cmath>

// Position subjects (reactive data binding)
static lv_subject_t pos_x_subject;
static lv_subject_t pos_y_subject;
static lv_subject_t pos_z_subject;

// Subject storage buffers
static char pos_x_buf[32];
static char pos_y_buf[32];
static char pos_z_buf[32];

// Current state
static jog_distance_t current_distance = JOG_DIST_1MM;
static float current_x = 0.0f;
static float current_y = 0.0f;
static float current_z = 0.0f;

// Press state tracking for visual feedback
static bool is_pressed = false;
static jog_direction_t pressed_direction = JOG_DIR_N;
static bool pressed_is_inner = false;
static bool pressed_is_home = false;

// Panel widgets (accessed by name)
static lv_obj_t* motion_panel = nullptr;
static lv_obj_t* parent_obj = nullptr;

// Distance button widgets
static lv_obj_t* dist_buttons[4] = {nullptr};

// Distance values in mm
static const float distance_values[] = {0.1f, 1.0f, 10.0f, 100.0f};

void ui_panel_motion_init_subjects() {
    // Initialize position subjects with default placeholder values
    snprintf(pos_x_buf, sizeof(pos_x_buf), "X:    --  mm");
    snprintf(pos_y_buf, sizeof(pos_y_buf), "Y:    --  mm");
    snprintf(pos_z_buf, sizeof(pos_z_buf), "Z:    --  mm");

    lv_subject_init_string(&pos_x_subject, pos_x_buf, nullptr, sizeof(pos_x_buf), pos_x_buf);
    lv_subject_init_string(&pos_y_subject, pos_y_buf, nullptr, sizeof(pos_y_buf), pos_y_buf);
    lv_subject_init_string(&pos_z_subject, pos_z_buf, nullptr, sizeof(pos_z_buf), pos_z_buf);

    // Register subjects with XML system (using NULL parent = global scope)
    lv_xml_register_subject(NULL, "motion_pos_x", &pos_x_subject);
    lv_xml_register_subject(NULL, "motion_pos_y", &pos_y_subject);
    lv_xml_register_subject(NULL, "motion_pos_z", &pos_z_subject);

    printf("[Motion] Subjects initialized: X/Y/Z position displays\n");
}

// Helper: Update distance button styling
static void update_distance_buttons() {
    for (int i = 0; i < 4; i++) {
        if (dist_buttons[i]) {
            if (i == current_distance) {
                // Active state - theme handles colors
                lv_obj_add_state(dist_buttons[i], LV_STATE_CHECKED);
            } else {
                // Inactive state - theme handles colors
                lv_obj_remove_state(dist_buttons[i], LV_STATE_CHECKED);
            }
        }
    }
}

// Event handler: Back button
static void back_button_cb(lv_event_t* e) {
    (void)e;  // Unused parameter

    // Use navigation history to go back to previous panel
    if (!ui_nav_go_back()) {
        // Fallback: If navigation history is empty, manually hide and show controls launcher
        if (motion_panel) {
            lv_obj_add_flag(motion_panel, LV_OBJ_FLAG_HIDDEN);
        }

        if (parent_obj) {
            lv_obj_t* controls_launcher = lv_obj_find_by_name(parent_obj, "controls_panel");
            if (controls_launcher) {
                lv_obj_clear_flag(controls_launcher, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// Event handler: Distance selector buttons
static void distance_button_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);

    // Find which button was clicked
    for (int i = 0; i < 4; i++) {
        if (btn == dist_buttons[i]) {
            current_distance = (jog_distance_t)i;
            printf("[Motion] Distance selected: %.1fmm\n", distance_values[i]);
            update_distance_buttons();
            return;
        }
    }
}

// Event handler: Z-axis buttons
static void z_button_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    const char* name = lv_obj_get_name(btn);

    printf("[Motion] Z button callback fired! Button name: '%s'\n", name ? name : "(null)");

    if (!name) {
        printf("[Motion]   ERROR: Button has no name!\n");
        return;
    }

    if (strcmp(name, "z_up_10") == 0) {
        ui_panel_motion_set_position(current_x, current_y, current_z + 10.0f);
        printf("[Motion] Z jog: +10mm (now %.1fmm)\n", current_z);
    } else if (strcmp(name, "z_up_1") == 0) {
        ui_panel_motion_set_position(current_x, current_y, current_z + 1.0f);
        printf("[Motion] Z jog: +1mm (now %.1fmm)\n", current_z);
    } else if (strcmp(name, "z_down_1") == 0) {
        ui_panel_motion_set_position(current_x, current_y, current_z - 1.0f);
        printf("[Motion] Z jog: -1mm (now %.1fmm)\n", current_z);
    } else if (strcmp(name, "z_down_10") == 0) {
        ui_panel_motion_set_position(current_x, current_y, current_z - 10.0f);
        printf("[Motion] Z jog: -10mm (now %.1fmm)\n", current_z);
    } else {
        printf("[Motion]   ERROR: Unknown button name: '%s'\n", name);
    }
}

// Event handler: Home buttons
static void home_button_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    const char* name = lv_obj_get_name(btn);

    if (!name) return;

    if (strcmp(name, "home_all") == 0) {
        ui_panel_motion_home('A');
    } else if (strcmp(name, "home_x") == 0) {
        ui_panel_motion_home('X');
    } else if (strcmp(name, "home_y") == 0) {
        ui_panel_motion_home('Y');
    } else if (strcmp(name, "home_z") == 0) {
        ui_panel_motion_home('Z');
    }
}

// Helper: Calculate angle from center point (0° = North, clockwise)
static float calculate_angle(lv_coord_t dx, lv_coord_t dy) {
    // atan2 gives us angle with 0° = East, counter-clockwise
    // We need 0° = North, clockwise
    float angle = atan2f((float)dx, (float)-dy) * 180.0f / M_PI;
    if (angle < 0) angle += 360.0f;
    return angle;
}

// Helper: Convert our angle system (0°=North, CW) to LVGL's (0°=East, CW)
static int convert_angle_to_lvgl(int our_angle) {
    // Our system: 0°=North (top), LVGL: 0°=East (right)
    // Rotate by -90° (subtract 90, or equivalently add 270)
    return (our_angle + 270) % 360;
}

// Helper: Determine jog direction from angle (8 wedges of 45° each)
static jog_direction_t angle_to_direction(float angle) {
    // Wedge boundaries (centered on cardinals):
    // N: 337.5-22.5°, NE: 22.5-67.5°, E: 67.5-112.5°, SE: 112.5-157.5°
    // S: 157.5-202.5°, SW: 202.5-247.5°, W: 247.5-292.5°, NW: 292.5-337.5°

    if (angle >= 337.5f || angle < 22.5f) return JOG_DIR_N;
    else if (angle >= 22.5f && angle < 67.5f) return JOG_DIR_NE;
    else if (angle >= 67.5f && angle < 112.5f) return JOG_DIR_E;
    else if (angle >= 112.5f && angle < 157.5f) return JOG_DIR_SE;
    else if (angle >= 157.5f && angle < 202.5f) return JOG_DIR_S;
    else if (angle >= 202.5f && angle < 247.5f) return JOG_DIR_SW;
    else if (angle >= 247.5f && angle < 292.5f) return JOG_DIR_W;
    else return JOG_DIR_NW;
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

    // Get container dimensions and center point
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    lv_coord_t width = lv_area_get_width(&obj_coords);
    lv_coord_t height = lv_area_get_height(&obj_coords);
    lv_coord_t center_x = obj_coords.x1 + width / 2;
    lv_coord_t center_y = obj_coords.y1 + height / 2;
    lv_coord_t radius = width / 2;

    // Zone boundary: Inner circle is 50% of total radius (30% smaller than before)
    lv_coord_t inner_boundary = (lv_coord_t)(radius * 0.50f);

    // LAYERED APPROACH: Draw from back to front
    // Layer 1: Full light gray background circle (0% to 100% radius)
    // Layer 2: Dark gray inner circle overlay (0% to 50% radius)
    // Result: Outer ring (50-100%) shows as light gray, extends all the way to edge

    // Layer 1: Full background circle (light gray)
    lv_draw_arc_dsc_t bg_arc_dsc;
    lv_draw_arc_dsc_init(&bg_arc_dsc);
    bg_arc_dsc.color = lv_color_hex(0x3a3a3a);  // Light gray (outer ring color)
    bg_arc_dsc.width = radius * 2;  // Width = diameter, fills entire circle
    bg_arc_dsc.center.x = center_x;
    bg_arc_dsc.center.y = center_y;
    bg_arc_dsc.radius = radius;  // Centerline at outer edge
    bg_arc_dsc.start_angle = 0;
    bg_arc_dsc.end_angle = 360;
    lv_draw_arc(layer, &bg_arc_dsc);

    // Layer 2: Inner circle overlay (dark gray)
    lv_draw_arc_dsc_t inner_arc_dsc;
    lv_draw_arc_dsc_init(&inner_arc_dsc);
    inner_arc_dsc.color = lv_color_hex(0x2a2a2a);  // Darker gray (inner circle color)
    inner_arc_dsc.width = inner_boundary * 2;  // Width = inner diameter
    inner_arc_dsc.center.x = center_x;
    inner_arc_dsc.center.y = center_y;
    inner_arc_dsc.radius = inner_boundary;  // Centerline at 70% boundary
    inner_arc_dsc.start_angle = 0;
    inner_arc_dsc.end_angle = 360;
    lv_draw_arc(layer, &inner_arc_dsc);

    // Draw 2 diagonal divider lines (NE-SW and NW-SE) like Bambu Lab
    // Creates 4 pie-shaped quadrants for diagonal movements
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x000000);
    line_dsc.width = 4;
    line_dsc.opa = LV_OPA_50;

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

    // Draw center home button area (doubled size for easier targeting)
    lv_coord_t home_radius = (lv_coord_t)(radius * 0.25f);  // ~25% of total radius (was 12%)

    // Draw filled background circle for home area
    lv_draw_arc_dsc_t home_bg_dsc;
    lv_draw_arc_dsc_init(&home_bg_dsc);
    home_bg_dsc.color = lv_color_hex(0x404040);  // Slightly lighter than inner circle
    home_bg_dsc.width = home_radius * 2;  // Fill entire circle
    home_bg_dsc.center.x = center_x;
    home_bg_dsc.center.y = center_y;
    home_bg_dsc.radius = home_radius;
    home_bg_dsc.start_angle = 0;
    home_bg_dsc.end_angle = 360;
    lv_draw_arc(layer, &home_bg_dsc);

    // Draw visual ring border around home area
    lv_draw_arc_dsc_t home_ring_dsc;
    lv_draw_arc_dsc_init(&home_ring_dsc);
    home_ring_dsc.color = lv_color_hex(0x606060);  // Lighter border color
    home_ring_dsc.width = 3;  // Border thickness
    home_ring_dsc.center.x = center_x;
    home_ring_dsc.center.y = center_y;
    home_ring_dsc.radius = home_radius;
    home_ring_dsc.start_angle = 0;
    home_ring_dsc.end_angle = 360;
    lv_draw_arc(layer, &home_ring_dsc);

    // Draw center home icon
    // Use simple text icon as lv_draw_image doesn't work reliably in draw callbacks
    lv_draw_label_dsc_t home_label_dsc;
    lv_draw_label_dsc_init(&home_label_dsc);
    home_label_dsc.color = lv_color_hex(0xffffff);
    home_label_dsc.text = LV_SYMBOL_HOME;
    home_label_dsc.font = &lv_font_montserrat_28;
    home_label_dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t home_label_area;
    home_label_area.x1 = center_x - 20;
    home_label_area.y1 = center_y - 14;
    home_label_area.x2 = center_x + 20;
    home_label_area.y2 = center_y + 14;
    lv_draw_label(layer, &home_label_dsc, &home_label_area);

    // Draw zone boundary lines to show the edges of each directional click zone
    // 8 zones means 8 boundaries - at 22.5° intervals starting from 22.5°
    // These mark the boundaries between: N/NE, NE/E, E/SE, SE/S, S/SW, SW/W, W/NW, NW/N
    for (float angle_deg : {22.5f, 67.5f, 112.5f, 157.5f, 202.5f, 247.5f, 292.5f, 337.5f}) {
        float angle_rad = angle_deg * M_PI / 180.0f;

        lv_draw_line_dsc_t boundary_line_dsc;
        lv_draw_line_dsc_init(&boundary_line_dsc);
        boundary_line_dsc.color = lv_color_hex(0x484848);  // More subtle gray
        boundary_line_dsc.width = 1;  // Very thin
        boundary_line_dsc.opa = LV_OPA_50;  // 50% opacity for extra subtlety

        // Start point: just outside home button (27% of radius)
        boundary_line_dsc.p1.x = center_x + (radius * 0.27f) * cosf(angle_rad);
        boundary_line_dsc.p1.y = center_y + (radius * 0.27f) * sinf(angle_rad);

        // End point: at outer edge (98% to avoid clipping)
        boundary_line_dsc.p2.x = center_x + (radius * 0.98f) * cosf(angle_rad);
        boundary_line_dsc.p2.y = center_y + (radius * 0.98f) * sinf(angle_rad);

        lv_draw_line(layer, &boundary_line_dsc);
    }

    // Draw distance labels showing movement amounts for each ring
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(0xcccccc);
    label_dsc.font = &lv_font_montserrat_14;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_area_t label_area;

    // "1mm" label in inner ring (offset down and right from diagonal)
    label_dsc.text = "1mm";
    lv_coord_t inner_label_radius = (lv_coord_t)((home_radius + inner_boundary) * 0.5f);
    label_area.x1 = center_x + (lv_coord_t)(inner_label_radius * 0.707f);
    label_area.y1 = center_y - (lv_coord_t)(inner_label_radius * 0.707f) + 8;
    label_area.x2 = label_area.x1 + 50;
    label_area.y2 = label_area.y1 + 18;
    lv_draw_label(layer, &label_dsc, &label_area);

    // "10mm" label in outer ring (offset down and right from diagonal)
    label_dsc.text = "10mm";
    lv_coord_t outer_label_radius = (lv_coord_t)((radius + inner_boundary) * 0.5f);
    label_area.x1 = center_x + (lv_coord_t)(outer_label_radius * 0.707f);
    label_area.y1 = center_y - (lv_coord_t)(outer_label_radius * 0.707f) + 8;
    label_area.x2 = label_area.x1 + 60;
    label_area.y2 = label_area.y1 + 18;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Draw axis labels (cardinal directions)
    label_dsc.color = lv_color_hex(0xffffff);
    label_dsc.font = &lv_font_montserrat_16;

    // Y+ (North)
    label_dsc.text = "Y+";
    label_area.x1 = center_x - 12;
    label_area.y1 = (int32_t)(center_y - radius * 0.80f) - 10;
    label_area.x2 = label_area.x1 + 25;
    label_area.y2 = label_area.y1 + 20;
    lv_draw_label(layer, &label_dsc, &label_area);

    // X+ (East)
    label_dsc.text = "X+";
    label_area.x1 = (int32_t)(center_x + radius * 0.80f) - 12;
    label_area.y1 = center_y - 10;
    label_area.x2 = label_area.x1 + 25;
    label_area.y2 = label_area.y1 + 20;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Y- (South)
    label_dsc.text = "Y-";
    label_area.x1 = center_x - 12;
    label_area.y1 = (int32_t)(center_y + radius * 0.80f) - 10;
    label_area.x2 = label_area.x1 + 25;
    label_area.y2 = label_area.y1 + 20;
    lv_draw_label(layer, &label_dsc, &label_area);

    // X- (West)
    label_dsc.text = "X-";
    label_area.x1 = (int32_t)(center_x - radius * 0.80f) - 12;
    label_area.y1 = center_y - 10;
    label_area.x2 = label_area.x1 + 25;
    label_area.y2 = label_area.y1 + 20;
    lv_draw_label(layer, &label_dsc, &label_area);

    // Draw press highlight overlay if a zone is pressed
    if (is_pressed) {
        if (pressed_is_home) {
            // Highlight home button with filled circle
            lv_draw_arc_dsc_t highlight_dsc;
            lv_draw_arc_dsc_init(&highlight_dsc);
            highlight_dsc.color = lv_color_hex(0xffffff);
            highlight_dsc.opa = LV_OPA_60;  // 60 = ~23% opacity
            lv_coord_t home_radius = (lv_coord_t)(radius * 0.25f);
            highlight_dsc.width = home_radius * 2;
            highlight_dsc.center.x = center_x;
            highlight_dsc.center.y = center_y;
            highlight_dsc.radius = home_radius;
            highlight_dsc.start_angle = 0;
            highlight_dsc.end_angle = 360;
            lv_draw_arc(layer, &highlight_dsc);
        } else {
            // Highlight directional wedge (45° segment)
            // Map direction enum to angle in our coordinate system (0°=North, CW)
            // Enum order: N=0, S=1, E=2, W=3, NE=4, NW=5, SE=6, SW=7
            // Angle mapping: N=0°, NE=45°, E=90°, SE=135°, S=180°, SW=225°, W=270°, NW=315°
            int direction_angles[] = {0, 180, 90, 270, 45, 315, 135, 225};  // Maps enum to angle
            int our_wedge_center = direction_angles[pressed_direction];
            int our_wedge_start = our_wedge_center - 22;  // 22.5° on each side
            int our_wedge_end = our_wedge_center + 23;    // Total 45° wedge

            // Convert to LVGL's coordinate system (0°=East, CW)
            int lvgl_start = convert_angle_to_lvgl(our_wedge_start);
            int lvgl_end = convert_angle_to_lvgl(our_wedge_end);

            lv_draw_arc_dsc_t highlight_dsc;
            lv_draw_arc_dsc_init(&highlight_dsc);
            highlight_dsc.color = lv_color_hex(0xffffff);
            highlight_dsc.opa = LV_OPA_60;

            if (pressed_is_inner) {
                // Inner zone: Draw arc ring from 25% to 50%
                // LVGL arc system: radius = outer edge, width = thickness inward
                // Inner edge = radius - width
                // So to fill 25% to 50%:
                //   - radius = 50% (outer edge)
                //   - width = 50% - 25% = 25% (thickness)
                //   - Inner edge = 50% - 25% = 25% ✓
                lv_coord_t inner_boundary = (lv_coord_t)(radius * 0.50f);
                lv_coord_t home_edge = (lv_coord_t)(radius * 0.25f);

                highlight_dsc.width = inner_boundary - home_edge;  // 25% thickness
                highlight_dsc.center.x = center_x;
                highlight_dsc.center.y = center_y;
                highlight_dsc.radius = inner_boundary;              // 50% outer edge
                highlight_dsc.start_angle = lvgl_start;
                highlight_dsc.end_angle = lvgl_end;
                lv_draw_arc(layer, &highlight_dsc);
            } else {
                // Outer zone: Draw arc ring from 50% to 100%
                //   - radius = 100% (outer edge)
                //   - width = 100% - 50% = 50% (thickness)
                //   - Inner edge = 100% - 50% = 50% ✓
                lv_coord_t inner_boundary = (lv_coord_t)(radius * 0.50f);

                highlight_dsc.width = radius - inner_boundary;     // 50% thickness
                highlight_dsc.center.x = center_x;
                highlight_dsc.center.y = center_y;
                highlight_dsc.radius = radius;                     // 100% outer edge
                highlight_dsc.start_angle = lvgl_start;
                highlight_dsc.end_angle = lvgl_end;
                lv_draw_arc(layer, &highlight_dsc);
            }
        }
    }
}

// Press event: Track pressed zone for visual feedback
static void jog_pad_press_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
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
        is_pressed = false;
        return;
    }

    is_pressed = true;

    // Home button: center 25% radius
    if (distance < radius * 0.25f) {
        pressed_is_home = true;
        pressed_is_inner = false;
        lv_obj_invalidate(obj);  // Trigger redraw
        return;
    }

    pressed_is_home = false;

    // Determine direction from angle
    float angle = calculate_angle(dx, dy);
    pressed_direction = angle_to_direction(angle);

    // Determine if inner or outer zone
    float inner_boundary = radius * 0.50f;
    pressed_is_inner = (distance < inner_boundary);

    // Trigger redraw to show highlight
    lv_obj_invalidate(obj);
}

// Release event: Clear press state
static void jog_pad_release_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);

    if (is_pressed) {
        is_pressed = false;
        // Trigger redraw to remove highlight
        lv_obj_invalidate(obj);
    }
}

// Click event: Detect zone and trigger jog
static void jog_pad_click_cb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
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
    if (distance > radius) return;

    // Home button: center 25% radius
    if (distance < radius * 0.25f) {
        ui_panel_motion_home('A');  // Home XY
        printf("[Motion] Jog pad: Home XY\n");
        return;
    }

    // Determine direction from angle
    float angle = calculate_angle(dx, dy);
    jog_direction_t direction = angle_to_direction(angle);

    // Zone boundary: inner ring (25-50%) = 1mm, outer ring (50-100%) = 10mm
    float inner_boundary = radius * 0.50f;
    float jog_dist;

    if (distance < inner_boundary) {
        // Inner zone - small movements (0.1mm or 1mm based on selector)
        jog_dist = (current_distance <= JOG_DIST_1MM) ?
            distance_values[current_distance] : distance_values[JOG_DIST_1MM];
    } else {
        // Outer zone - large movements (10mm or 100mm based on selector)
        jog_dist = (current_distance >= JOG_DIST_10MM) ?
            distance_values[current_distance] : distance_values[JOG_DIST_10MM];
    }

    ui_panel_motion_jog(direction, jog_dist);
}

// Resize callback for responsive padding
static void on_resize() {
    if (!motion_panel || !parent_obj) {
        return;
    }

    lv_obj_t* motion_content = lv_obj_find_by_name(motion_panel, "motion_content");
    if (motion_content) {
        lv_coord_t vertical_padding = ui_get_header_content_padding(lv_obj_get_height(parent_obj));
        // Set vertical padding (top/bottom) responsively, keep horizontal at medium (12px)
        lv_obj_set_style_pad_top(motion_content, vertical_padding, 0);
        lv_obj_set_style_pad_bottom(motion_content, vertical_padding, 0);
        lv_obj_set_style_pad_left(motion_content, UI_PADDING_MEDIUM, 0);
        lv_obj_set_style_pad_right(motion_content, UI_PADDING_MEDIUM, 0);
    }
}

void ui_panel_motion_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    motion_panel = panel;
    parent_obj = parent_screen;

    printf("[Motion] Setting up event handlers...\n");

    // Setup header for responsive height
    lv_obj_t* motion_header = lv_obj_find_by_name(panel, "motion_header");
    if (motion_header) {
        ui_component_header_bar_setup(motion_header, parent_screen);
    }

    // Set responsive padding for content area
    lv_obj_t* motion_content = lv_obj_find_by_name(panel, "motion_content");
    if (motion_content) {
        lv_coord_t vertical_padding = ui_get_header_content_padding(lv_obj_get_height(parent_screen));
        // Set vertical padding (top/bottom) responsively, keep horizontal at medium (12px)
        lv_obj_set_style_pad_top(motion_content, vertical_padding, 0);
        lv_obj_set_style_pad_bottom(motion_content, vertical_padding, 0);
        lv_obj_set_style_pad_left(motion_content, UI_PADDING_MEDIUM, 0);
        lv_obj_set_style_pad_right(motion_content, UI_PADDING_MEDIUM, 0);
        printf("[Motion]   ✓ Content padding: top/bottom=%dpx, left/right=%dpx (responsive)\n",
               vertical_padding, UI_PADDING_MEDIUM);
    }

    // Register resize callback
    ui_resize_handler_register(on_resize);

    // Back button
    lv_obj_t* back_btn = lv_obj_find_by_name(panel, "back_button");
    if (back_btn) {
        lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, nullptr);
    }

    // Distance selector buttons
    const char* dist_names[] = {"dist_0_1", "dist_1", "dist_10", "dist_100"};
    for (int i = 0; i < 4; i++) {
        dist_buttons[i] = lv_obj_find_by_name(panel, dist_names[i]);
        if (dist_buttons[i]) {
            lv_obj_add_event_cb(dist_buttons[i], distance_button_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
    update_distance_buttons();
    printf("[Motion]   ✓ Distance selector (4 buttons)\n");

    // Circular jog pad with custom drawing and click detection
    lv_obj_t* jog_pad = lv_obj_find_by_name(panel, "jog_pad_container");
    if (jog_pad) {
        // Set jog pad size as 80% of available vertical height (after header)
        lv_display_t* disp = lv_display_get_default();
        lv_coord_t screen_height = lv_display_get_vertical_resolution(disp);

        // Get header height (varies by screen size: 50-70px)
        lv_obj_t* header = lv_obj_find_by_name(panel, "motion_header");
        lv_coord_t header_height = header ? lv_obj_get_height(header) : 60;

        // Available height = screen height - header - padding (40px top+bottom)
        lv_coord_t available_height = screen_height - header_height - 40;

        // Jog pad = 80% of available height (leaves room for distance/home buttons)
        lv_coord_t jog_size = (lv_coord_t)(available_height * 0.80f);

        lv_obj_set_width(jog_pad, jog_size);
        lv_obj_set_height(jog_pad, jog_size);
        printf("[Motion]   Jog pad size: %dpx (available height: %dpx, screen: %dpx)\n",
               jog_size, available_height, screen_height);

        // Register custom draw event (draws circles, dividers, labels, home icon)
        lv_obj_add_event_cb(jog_pad, jog_pad_draw_cb, LV_EVENT_DRAW_POST, nullptr);

        // Register press/release events for visual feedback
        lv_obj_add_event_cb(jog_pad, jog_pad_press_cb, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(jog_pad, jog_pad_release_cb, LV_EVENT_RELEASED, nullptr);

        // Register click event (angle-based direction detection)
        lv_obj_add_event_cb(jog_pad, jog_pad_click_cb, LV_EVENT_CLICKED, nullptr);

        // Force redraw to show custom graphics
        lv_obj_invalidate(jog_pad);

        printf("[Motion]   ✓ Circular jog pad (custom draw + angle-based click)\n");
    } else {
        printf("[Motion]   ✗ jog_pad_container NOT FOUND!\n");
    }

    // Z-axis buttons
    const char* z_names[] = {"z_up_10", "z_up_1", "z_down_1", "z_down_10"};
    int z_found = 0;
    for (const char* name : z_names) {
        lv_obj_t* btn = lv_obj_find_by_name(panel, name);
        if (btn) {
            printf("[Motion]     Found '%s' at %p\n", name, (void*)btn);
            lv_obj_add_event_cb(btn, z_button_cb, LV_EVENT_CLICKED, nullptr);
            printf("[Motion]     Event handler attached successfully\n");
            z_found++;
        } else {
            printf("[Motion]   ✗ Z button '%s' NOT FOUND!\n", name);
        }
    }
    printf("[Motion]   ✓ Z-axis controls (%d/4 buttons found)\n", z_found);

    // Home buttons
    const char* home_names[] = {"home_all", "home_x", "home_y", "home_z"};
    for (const char* name : home_names) {
        lv_obj_t* btn = lv_obj_find_by_name(panel, name);
        if (btn) {
            lv_obj_add_event_cb(btn, home_button_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
    printf("[Motion]   ✓ Home buttons (4 buttons)\n");

    printf("[Motion] Setup complete!\n");
}

void ui_panel_motion_set_position(float x, float y, float z) {
    current_x = x;
    current_y = y;
    current_z = z;

    // Update subjects (will automatically update bound UI elements)
    snprintf(pos_x_buf, sizeof(pos_x_buf), "X: %6.1f mm", x);
    snprintf(pos_y_buf, sizeof(pos_y_buf), "Y: %6.1f mm", y);
    snprintf(pos_z_buf, sizeof(pos_z_buf), "Z: %6.1f mm", z);

    lv_subject_copy_string(&pos_x_subject, pos_x_buf);
    lv_subject_copy_string(&pos_y_subject, pos_y_buf);
    lv_subject_copy_string(&pos_z_subject, pos_z_buf);
}

jog_distance_t ui_panel_motion_get_distance() {
    return current_distance;
}

void ui_panel_motion_set_distance(jog_distance_t dist) {
    if (dist >= 0 && dist <= 3) {
        current_distance = dist;
        update_distance_buttons();
    }
}

void ui_panel_motion_jog(jog_direction_t direction, float distance_mm) {
    const char* dir_names[] = {"N(+Y)", "S(-Y)", "E(+X)", "W(-X)",
                               "NE(+X+Y)", "NW(-X+Y)", "SE(+X-Y)", "SW(-X-Y)"};

    printf("[Motion] Jog command: %s %.1fmm\n", dir_names[direction], distance_mm);

    // Mock position update (simulate jog movement)
    float dx = 0.0f, dy = 0.0f;

    switch (direction) {
        case JOG_DIR_N:  dy = distance_mm; break;
        case JOG_DIR_S:  dy = -distance_mm; break;
        case JOG_DIR_E:  dx = distance_mm; break;
        case JOG_DIR_W:  dx = -distance_mm; break;
        case JOG_DIR_NE: dx = distance_mm; dy = distance_mm; break;
        case JOG_DIR_NW: dx = -distance_mm; dy = distance_mm; break;
        case JOG_DIR_SE: dx = distance_mm; dy = -distance_mm; break;
        case JOG_DIR_SW: dx = -distance_mm; dy = -distance_mm; break;
    }

    ui_panel_motion_set_position(current_x + dx, current_y + dy, current_z);

    // TODO: Send actual G-code command via Moonraker API
    // Example: G0 X{new_x} Y{new_y} F{feedrate}
}

void ui_panel_motion_home(char axis) {
    printf("[Motion] Home command: %c axis\n", axis);

    // Mock position update (simulate homing)
    switch (axis) {
        case 'X': ui_panel_motion_set_position(0.0f, current_y, current_z); break;
        case 'Y': ui_panel_motion_set_position(current_x, 0.0f, current_z); break;
        case 'Z': ui_panel_motion_set_position(current_x, current_y, 0.0f); break;
        case 'A': ui_panel_motion_set_position(0.0f, 0.0f, 0.0f); break;  // All axes
    }

    // TODO: Send actual G-code command via Moonraker API
    // Example: G28 X (home X), G28 (home all)
}
