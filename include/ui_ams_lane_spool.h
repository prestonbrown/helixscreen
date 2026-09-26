// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_lane_spool.h
 * @brief Custom LVGL XML widget: one lane's spool visual
 *
 * The spool rendering counterpart of ams_lane_bar: a round spool graphic
 * (pseudo-3D canvas or flat concentric rings, per /ams/spool_style) plus the
 * empty-lane placeholder and the error dot. All of it is driven by AmsState's
 * per-slot subjects:
 *
 * - ams_slot_N_lane_state  -> Present (full strength) / Ghosted (GHOST_OPA,
 *   "assigned, not present") / Empty (spool hidden, dashed placeholder)
 * - ams_slot_N_color       -> filament color
 * - ams_slot_N_fill        -> fill level (display_fill_pct encoding, -1 = no
 *   data, current render untouched)
 * - ams_slot_N_has_error / _error_severity -> error dot visibility and color
 *
 * XML usage:
 * @code{.xml}
 * <ams_lane_spool slot_index="0"/>
 * <ams_lane_spool slot_index="1" spool_size="48"/>
 * @endcode
 *
 * slot_index is normally parsed by xml_apply; a container that embeds the
 * widget before knowing its index (ams_slot_view) sets it afterwards via
 * helix::ui::ams_lane_spool_set_index().
 */

/**
 * @brief Register the ams_lane_spool widget with LVGL's XML system
 *
 * Must be called AFTER AmsState::init_subjects() and BEFORE any XML using
 * <ams_lane_spool> is registered or created.
 */
void ui_ams_lane_spool_register(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace helix::ui {

/**
 * @brief Point an ams_lane_spool widget at a different slot index
 *
 * Rebinds all per-slot observers. No-op on a non-ams_lane_spool object.
 */
void ams_lane_spool_set_index(lv_obj_t* spool, int slot_index);

/**
 * @brief Last fill level the widget applied (0.0-1.0), 1.0 when unbound
 *
 * Mirrors what is actually rendered: the subject value (or an imperative
 * ams_lane_spool_set_fill_level()), not a backend re-read.
 */
float ams_lane_spool_get_fill_level(lv_obj_t* spool);

/**
 * @brief Imperatively set the fill level (0.0-1.0)
 *
 * For XML consumers that pass a creation-time fill_level (the test panel).
 * A later per-slot fill subject value overrides it.
 */
void ams_lane_spool_set_fill_level(lv_obj_t* spool, float fill_level);

} // namespace helix::ui

#endif
