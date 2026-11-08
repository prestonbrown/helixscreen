// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_drying_types.h"
#include "lvgl/lvgl.h"

/**
 * @file ui_dryer_airflow.h
 * @brief <dryer_airflow>: warm air rising through a dryer, drawn as a stream of chevrons.
 *
 *   <dryer_airflow bind_phase="ams_env_overlay_drying_phase" width="64" height="64"/>
 *
 * - `bind_phase`: int subject holding a DryingPhase. Holding draws in the theme's
 *   `info` token; heating, and a phase the backend does not report, in `warning`.
 *
 * Colours are read from the theme at each draw, so a theme change re-tints live. With
 * animations on the stream rises on one shared tick that redraws only widgets on
 * screen; with them off it holds still. Geometry: dryer_airflow_geometry.h.
 */
namespace helix::ui {

void register_dryer_airflow_widget();

/// Set the phase a widget draws, for one with no subject to bind.
void dryer_airflow_set_phase(lv_obj_t* obj, DryingPhase phase);

/// The theme colour a phase draws in.
lv_color_t dryer_airflow_color(DryingPhase phase);

/// The shared stream position, 0..1.
float dryer_airflow_position();

/// One step of the shared tick: advance the stream and redraw every widget on screen.
/// Does nothing while animations are off.
void dryer_airflow_tick();

} // namespace helix::ui
