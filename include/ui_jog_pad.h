// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_motion.h" // For JogDirection and JogMode enums

#include <lvgl.h>

// Jog pad event callbacks
typedef void (*jog_pad_jog_cb_t)(helix::JogDirection direction, float distance_mm, void* user_data);
typedef void (*jog_pad_home_cb_t)(void* user_data);

/**
 * Create a circular jog pad widget (Bambu Lab style)
 *
 * Features:
 * - Two-zone design: inner ring (small jogs), outer ring (large jogs)
 * - 8 directional zones (N, S, E, W, NE, NW, SE, SW)
 * - Center home button
 * - Theme-aware colors (reads from component scope or uses fallbacks)
 * - Visual press feedback
 *
 * @param parent Parent LVGL object
 * @return Created jog pad object (use as clickable container)
 */
lv_obj_t* ui_jog_pad_create(lv_obj_t* parent);

/**
 * Set jog callback (called when directional zone is clicked)
 *
 * @param obj Jog pad object
 * @param cb Callback function (direction, distance_mm, user_data)
 * @param user_data Optional user data passed to callback
 */
void ui_jog_pad_set_jog_callback(lv_obj_t* obj, jog_pad_jog_cb_t cb, void* user_data);

/**
 * Set home callback (called when center button is clicked)
 *
 * @param obj Jog pad object
 * @param cb Callback function (user_data)
 * @param user_data Optional user data passed to callback
 */
void ui_jog_pad_set_home_callback(lv_obj_t* obj, jog_pad_home_cb_t cb, void* user_data);

/**
 * Set jog mode (Fine/Coarse/Turbo - affects inner/outer ring distances)
 *
 * Fine:   inner=0.1mm, outer=1mm
 * Coarse: inner=1mm,   outer=10mm
 * Turbo:  inner=10mm,  outer=50mm
 *
 * @param obj Jog pad object
 * @param mode Jog mode
 */
void ui_jog_pad_set_mode(lv_obj_t* obj, helix::JogMode mode);

/**
 * Get current jog mode
 *
 * @param obj Jog pad object
 * @return Current jog mode
 */
helix::JogMode ui_jog_pad_get_mode(lv_obj_t* obj);

/**
 * Refresh colors from theme (call when theme changes)
 *
 * @param obj Jog pad object
 */
void ui_jog_pad_refresh_colors(lv_obj_t* obj);

/**
 * Enable or disable the jog pad.
 *
 * When disabled the pad adds LV_STATE_DISABLED (so LVGL's input handling stops
 * routing presses/clicks to it) and draws a translucent scrim so it visibly
 * reads as unavailable. Used to gate jogging while the printer is not ready.
 *
 * @param obj Jog pad object
 * @param enabled true to enable, false to disable + dim
 */
void ui_jog_pad_set_enabled(lv_obj_t* obj, bool enabled);

/**
 * Set the homing state of the jog pad.
 *
 * When not homed, the center home button's ring + icon are drawn in the theme
 * "warning" color to signal that homing is required before jogging.
 *
 * @param obj Jog pad object
 * @param homed true if all axes are homed, false otherwise
 */
void ui_jog_pad_set_homed(lv_obj_t* obj, bool homed);

/**
 * Layout box for the home glyph inside its ring.
 *
 * lv_draw_label centres vertically by laying the font's full line height down
 * from the box's top edge, so a box shorter than the line height spills the
 * glyph past its bottom. The box is therefore exactly one line height tall,
 * centred on the ring's centre, which puts the glyph's visual centre on the
 * ring's centre at every pad size.
 *
 * @param center_x ring centre x (absolute screen coords, as the draw cb uses)
 * @param center_y ring centre y
 * @param home_radius radius of the home ring
 * @param font the mdi icon font the glyph draws with
 * @return the label box to pass to lv_draw_label
 */
namespace helix {
lv_area_t jog_pad_home_icon_area(lv_coord_t center_x, lv_coord_t center_y, lv_coord_t home_radius,
                                 const lv_font_t* font);
} // namespace helix
