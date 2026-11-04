// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_motion.h" // For JogDirection and JogMode enums

#include <lvgl.h>

namespace helix {
/// Which zones a jog pad offers. The defaults are the full pad: a centre home
/// button and eight directional wedges.
struct JogPadZoneOptions {
    /// false leaves the centre an inert hub with no home button.
    bool home_zone = true;
    /// true splits the rings into four 90 degree wedges, so every jog moves one
    /// axis and no diagonal can be pressed.
    bool cardinal_only = false;
};

/// Which zone one point lands in: off the pad, the home button, or a directional
/// wedge in the inner or outer ring. The single source for every zone decision.
struct JogPadZone {
    bool on_pad;
    bool home;
    JogDirection direction;
    bool inner;
};

/// Classify a point at (@p dx, @p dy) pixels from the centre of a pad of radius
/// @p radius. A point on an inert hub is not on the pad.
JogPadZone jog_pad_zone_for_offset(int dx, int dy, int radius, const JogPadZoneOptions& options);
} // namespace helix

// Jog pad event callbacks
// The jog callback returns whether a hold-to-repeat under way may keep
// repeating this zone (false = the jog was refused or fully clamped).
// is_repeat is false for the tap click and for the first jog of a hold
// (a fresh press), true for every later hold tick.
typedef bool (*jog_pad_jog_cb_t)(helix::JogDirection direction, float distance_mm, bool is_repeat,
                                 void* user_data);
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
 * @param cb Callback function (direction, distance_mm, is_repeat, user_data);
 *           returns false to stop a hold-to-repeat in progress
 * @param user_data Optional user data passed to callback
 */
void ui_jog_pad_set_jog_callback(lv_obj_t* obj, jog_pad_jog_cb_t cb, void* user_data);

/**
 * Cancel any hold-to-repeat under way on the pad.
 *
 * The owner calls this when a jog error arrives asynchronously or the panel
 * deactivates: the repeat's next tick would re-send a jog the owner already
 * refused on the printer's behalf.
 *
 * @param obj Jog pad object
 */
// NAMESPACE_OK: joins this header's global ui_jog_pad_* free-function API
void ui_jog_pad_stop_repeat(lv_obj_t* obj);

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
 * Choose which zones the pad offers. A pad that never calls this keeps the full
 * set.
 *
 * @param obj Jog pad object
 * @param options Zones to offer
 */
// NAMESPACE_OK: joins this header's global ui_jog_pad_* free-function API
void ui_jog_pad_set_zone_options(lv_obj_t* obj, const helix::JogPadZoneOptions& options);

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
