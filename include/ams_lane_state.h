// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

namespace helix::ui {

/**
 * @brief What a lane fundamentally is, for every surface that draws one.
 *
 * Three states, not five. "Loaded" and "error" are NOT here: a blocked lane
 * still has filament and an active lane is still Present, so they are
 * decorations laid over a base state, driven by their own subjects
 * (slot_active_loaded, slot.error) and applied by the chrome.
 */
enum class LaneState {
    /// Has filament. Draw it at lane_fill_level().
    Present,
    /// Ejected but still carries an identity (#1071 keeps the override).
    /// Draw it at its last known fill with the WHOLE cell dimmed.
    Ghosted,
    /// No filament and no identity. The spool rendering shows a placeholder
    /// and "Empty"; the bar rendering draws nothing and leaves the gap.
    Empty,
};

/// Fill level for a lane that has filament but no weight data. One named
/// constant instead of the bare 1.0f that used to sit in display_fill_level()
/// and three places in ui_spool_canvas.cpp.
inline constexpr float ASSUMED_FILL_LEVEL = 1.0f;

/**
 * @brief Does this lane still carry an identity after being ejected?
 *
 * Spoolman link, material, brand or spool name — deliberately NOT cleared on
 * eject (#1071), so a lane with one is "assigned, not present".
 */
[[nodiscard]] inline bool lane_has_identity(const SlotInfo& slot) {
    return slot.spoolman_id > 0 || !slot.material.empty() || !slot.brand.empty() ||
           !slot.spool_name.empty();
}

/**
 * @brief Classify a lane. Pure — testable with no display.
 *
 * UNKNOWN is treated exactly as EMPTY. It is not a steady state on any backend:
 * every SlotStatus::UNKNOWN assignment is skeleton construction before firmware
 * data arrives (ams_backend_qidi.cpp:72, ams_backend_snapmaker.cpp:263,
 * ams_backend_happy_hare.cpp:1325, ams_backend_ace.cpp:1317,
 * ams_backend_afc.cpp:4339), plus one QIDI fallback for an unrecognised value.
 * Treating it as EMPTY lets it inherit the identity split, so a lane whose
 * material is already known dims rather than blanking during startup.
 */
[[nodiscard]] constexpr LaneState classify_lane(SlotStatus status, bool has_identity) {
    const bool absent = (status == SlotStatus::EMPTY || status == SlotStatus::UNKNOWN);
    if (!absent) {
        return LaneState::Present;
    }
    return has_identity ? LaneState::Ghosted : LaneState::Empty;
}

/**
 * @brief How full to draw this lane, 0.0-1.0.
 *
 * A ghosted lane keeps its last known fill. That reverses a106413f6 (#1071),
 * where an emptied lane rendered a full-strength 75% bar and read as loaded.
 * It is safe here ONLY because Ghosted dims the entire cell — the dimming is
 * the disclaimer. Do not reuse this value without the ghost.
 */
[[nodiscard]] inline float lane_fill_level(const SlotInfo& slot) {
    if (classify_lane(slot.status, lane_has_identity(slot)) == LaneState::Empty) {
        return 0.0f;
    }
    if (slot.total_weight_g > 0.0f && slot.remaining_weight_g >= 0.0f) {
        return slot.remaining_weight_g / slot.total_weight_g;
    }
    return ASSUMED_FILL_LEVEL;
}

} // namespace helix::ui
