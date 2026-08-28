// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_lane_bar.h
 * @brief Custom LVGL XML widget rendering one AMS lane as a compact vertical bar
 *
 * ams_lane_bar draws a single lane's LaneState (Present/Ghosted/Empty, see
 * ams_lane_state.h) as a bar_bg/bar_fill pair plus a status_line, driven
 * entirely by AmsState's per-slot subjects (lane_state, color, fill,
 * active_loaded) — no consumer pushes rendering into it imperatively.
 *
 * XML usage:
 * @code{.xml}
 * <ams_lane_bar slot_index="0"/>
 * @endcode
 *
 * Named children: bar_bg, bar_fill, status_line.
 */

/**
 * @brief Register the ams_lane_bar widget with LVGL's XML system.
 *
 * Safe to call from every consumer — mirrors ui_ams_slot_register() /
 * ui_spool_canvas_register(): there is no central widget registration point,
 * so each panel (and each test) that uses <ams_lane_bar> calls this itself.
 */
void ui_ams_lane_bar_register(void);

#ifdef __cplusplus
}
#endif
