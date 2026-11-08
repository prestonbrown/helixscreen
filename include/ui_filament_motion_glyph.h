// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_motion.h"
#include "lvgl/lvgl.h"

/**
 * @file ui_filament_motion_glyph.h
 * @brief <filament_motion_glyph>: a toolhead fed by a filament column on which
 *        chevrons stream while a manual Extrude or Retract runs.
 *
 *   <filament_motion_glyph bind_motion="filament_manual_motion"
 *                          style_line_color="#info" style_border_color="#text_muted"
 *                          style_bg_color="#text_muted"/>
 *
 * - `bind_motion`: int subject holding a helix::FilamentMotion. Idle shows the
 *   column with no chevrons; Extrude streams them down into the toolhead and
 *   Retract back up. With animations off they stand still, pointing the way
 *   the filament moves.
 * - `style_line_color`: the chevrons. `style_border_color`: the tube walls.
 *   `style_bg_color`: the filament in the tube and the toolhead (the widget
 *   paints no background of its own).
 *
 * The toolhead is the user's configured style, scaled to fit the lower part of
 * the box; the column fills the rest above it.
 */
namespace helix::ui {

void register_filament_motion_glyph_widget();

/// Set the motion directly, for a glyph with no subject bound.
void filament_motion_glyph_set_motion(lv_obj_t* obj, helix::FilamentMotion motion);

/// The motion @p obj (a <filament_motion_glyph>) shows.
helix::FilamentMotion filament_motion_glyph_motion(lv_obj_t* obj);

/// Whether the glyph's chevrons are animating (an lv_anim is running on it).
bool filament_motion_glyph_animating(lv_obj_t* obj);

} // namespace helix::ui
