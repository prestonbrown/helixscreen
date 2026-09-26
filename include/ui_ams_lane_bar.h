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

namespace helix::ui {
/**
 * @brief Create one ams_lane_bar per slot for slots
 * [first_slot_index, first_slot_index + slot_count).
 *
 * For C++ consumers whose lane count and bar width are MEASURED (overview
 * mini bars, mini-status bar mode): the loop stays in C++ (declarative rule
 * 8's measured-layout exception), the per-lane rendering stays in the widget.
 */
void ams_lane_bar_create_range(lv_obj_t* parent, int first_slot_index, int slot_count,
                               int32_t bar_width, int32_t bar_height);

/**
 * @brief Re-apply create-time sizing to an existing bar.
 *
 * Pooled consumers (mini-status bar mode) keep their bars across rebuilds and
 * resize them as the measured layout moves. bar_width/bar_height have the
 * same meaning as the create attrs.
 */
void ams_lane_bar_resize(lv_obj_t* bar, int32_t bar_width, int32_t bar_height);
} // namespace helix::ui
#endif
