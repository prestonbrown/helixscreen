// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

/**
 * @file ui_ams_slot_layout.h
 * @brief Shared slot grid sizing logic for AMS panels
 *
 * Extracted to avoid duplicating the slot width/overlap calculation
 * across AmsPanel, AmsOverviewPanel, and path canvas setup.
 */

/// Result of slot grid sizing calculation
struct AmsSlotLayout {
    int32_t slot_width = 0;       ///< Width of each slot widget (pixels)
    int32_t overlap = 0;          ///< Overlap between adjacent slots (pixels, 0 for ≤4 slots)
    int32_t centering_offset = 0; ///< Left padding to center slots within available width
};

/**
 * @brief Calculate slot widths and overlap for a given container width
 *
 * For ≤4 slots: evenly distributed, no overlap.
 * For 5+ slots: 50% overlap ratio so spools visually overlap.
 *
 * Formula (5+ slots):
 *   slot_width = available_width / (count * 0.5 + 0.5)
 *   overlap = slot_width * 0.5
 *
 * @param available_width  Content width of the slot container (pixels)
 * @param slot_count       Number of slots to fit
 * @return Computed layout with slot_width and overlap
 */
inline AmsSlotLayout calculate_ams_slot_layout(int32_t available_width, int slot_count) {
    AmsSlotLayout layout;

    if (slot_count <= 0 || available_width <= 0)
        return layout;

    if (slot_count > 4) {
        constexpr float OVERLAP_RATIO = 0.5f;
        layout.slot_width = static_cast<int32_t>(
            available_width / (slot_count * (1.0f - OVERLAP_RATIO) + OVERLAP_RATIO));
        layout.overlap = static_cast<int32_t>(layout.slot_width * OVERLAP_RATIO);
    } else {
        layout.slot_width = available_width / slot_count;
        layout.overlap = 0;
    }

    // Compute centering offset from integer rounding remainder
    int32_t rendered = slot_count * layout.slot_width - (slot_count - 1) * layout.overlap;
    layout.centering_offset = (available_width - rendered) / 2;

    return layout;
}
