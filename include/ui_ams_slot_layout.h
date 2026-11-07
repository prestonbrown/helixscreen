// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

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
 * @param max_slot_width   Caps the slot width for up to four slots (0 = no cap);
 *                         a capped row is centered in the width it leaves
 * @return Computed layout with slot_width and overlap
 */
inline AmsSlotLayout calculate_ams_slot_layout(int32_t available_width, int slot_count,
                                               int32_t max_slot_width = 0) {
    AmsSlotLayout layout;

    if (slot_count <= 0 || available_width <= 0)
        return layout;

    if (slot_count > 4) {
        constexpr float kOverlapRatio = 0.5f;
        layout.slot_width = static_cast<int32_t>(
            available_width / (slot_count * (1.0f - kOverlapRatio) + kOverlapRatio));
        layout.overlap = static_cast<int32_t>(layout.slot_width * kOverlapRatio);
    } else {
        layout.slot_width = available_width / slot_count;
        layout.overlap = 0;
        if (max_slot_width > 0 && layout.slot_width > max_slot_width)
            layout.slot_width = max_slot_width;
    }

    // Center the row: the integer rounding remainder, or the room a capped row leaves
    int32_t rendered = slot_count * layout.slot_width - (slot_count - 1) * layout.overlap;
    layout.centering_offset = (available_width - rendered) / 2;

    return layout;
}

/// Pixels trimmed off each side of a slot's tap area.
struct AmsSlotHitBand {
    int32_t left_inset = 0;
    int32_t right_inset = 0;
};

/**
 * @brief The part of an overlapping slot that a tap belongs to.
 *
 * Adjacent slots share @p layout.overlap pixels. Each shared strip is split at
 * its middle, so every slot claims one pitch centered on its own spool and a tap
 * never lands on a neighbour. The end slots keep their outer edges.
 */
inline AmsSlotHitBand ams_slot_hit_band(const AmsSlotLayout& layout, int slot_index,
                                        int slot_count) {
    AmsSlotHitBand band;
    if (layout.overlap <= 0 || slot_index < 0 || slot_index >= slot_count)
        return band;
    if (slot_index > 0)
        band.left_inset = layout.overlap - layout.overlap / 2;
    if (slot_index < slot_count - 1)
        band.right_inset = layout.overlap / 2;
    return band;
}

namespace helix::ui {

/// The stagger row a slot's label stands in: @p rows rows, row 0 nearest the spools.
struct AmsLabelRow {
    int rows = 1;
    int row = 0;
};

/**
 * @brief Which row a slot's label stands in, so neighbouring labels never share one.
 *
 * Up to four slots all stand in one row. Below the medium breakpoint five or
 * more alternate between two rows: odd lanes nearest the spools, even lanes
 * above. From medium up seven or more take three rows and five or six two,
 * running high, mid, low, mid, high.
 */
inline AmsLabelRow ams_stagger_row(int slot_index, int total_count, UiBreakpoint bp) {
    AmsLabelRow r;
    if (total_count < 5)
        return r;
    if (bp < UiBreakpoint::Medium) {
        r.rows = 2;
        r.row = slot_index % 2;
        return r;
    }
    r.rows = total_count >= 7 ? 3 : 2;
    const int period = (r.rows - 1) * 2;
    const int pos = slot_index % period;
    r.row = pos < r.rows ? r.rows - 1 - pos : pos - r.rows + 1;
    return r;
}

/**
 * @brief Height of one row of staggered slot labels, for a label font of @p line_height.
 *
 * From the medium breakpoint up a row is 1.5 lines. Below it a row is one line,
 * and the height the label band gives up goes to the unit and the filament path.
 */
inline int32_t ams_stagger_row_height(int32_t line_height, UiBreakpoint bp) {
    if (bp >= UiBreakpoint::Medium)
        return line_height * 3 / 2;
    return line_height;
}

/**
 * @brief Widest a staggered label may be, or 0 for no limit.
 *
 * Below the medium breakpoint a label shares its row with the lanes two either
 * side, so it keeps to two slot spacings less @p gap and ellipsizes past that.
 */
inline int32_t ams_stagger_label_width(int32_t slot_spacing, UiBreakpoint bp, int32_t gap) {
    if (bp >= UiBreakpoint::Medium || slot_spacing <= 0)
        return 0;
    return 2 * slot_spacing - gap;
}

/**
 * @brief Whether the operation sidebar lays its step progress out in a row.
 *
 * A portrait panel below the medium breakpoint gives the sidebar about 120 px
 * under the diagram; a four-step vertical list (about 90 px) does not fit there
 * under the strip and status, a row does. Everywhere else the vertical list fits.
 */
inline bool ams_stepper_horizontal(bool portrait, UiBreakpoint bp) {
    return portrait && bp < UiBreakpoint::Medium;
}

} // namespace helix::ui
