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

#include "ams_lane_state.h"

namespace helix::ui {

/// Slack the widget's root adds around the spool graphic so the lane badge,
/// aligned to its corner, is not clipped. A caller laying out a fixed-width
/// cell around the widget subtracts this to know what is left beside it.
inline constexpr int32_t AMS_LANE_SPOOL_BADGE_MARGIN_PX = 8;

/**
 * @brief Point an ams_lane_spool widget at a slot of a backend
 *
 * Rebinds all per-slot observers when either index changes. No-op on a
 * non-ams_lane_spool object. A widget created from XML binds backend 0.
 */
void ams_lane_spool_set_index(lv_obj_t* spool, int slot_index, int backend_index);

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

/**
 * @brief Resize the spool graphic (px; <= 0 is refused)
 *
 * Rebuilds the visual layers at the new size and repaints them with the
 * cached presentation, so the widget keeps rendering from its subjects. Also
 * rebuilds when /ams/spool_style has flipped since the last build. No-op when
 * size and style already match.
 */
void ams_lane_spool_set_size(lv_obj_t* spool, int32_t spool_size);

/**
 * @brief The spool family's material-label rule
 *
 * One implementation of what a lane's material label reads, shared by the
 * ams_slot label and the mini-status strip's spool cells so the two surfaces
 * cannot reach different conclusions about one lane:
 *
 *   Empty    -> lv_tr("Empty") (UI copy, not a material name)
 *   Ghosted / Present with no material -> "--"
 *   otherwise -> the material name (not translated)
 *
 * Callers own their own truncation, ellipsizing and ghost opacity.
 */
const char* lane_material_text(helix::ui::LaneState state, const char* material);

} // namespace helix::ui

#endif
