// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include <algorithm>
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
    int label_rows = 0; ///< Rows of labels above a drawn unit; 0 keeps the slots' own labels
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

namespace helix::ui {

/// Pixels trimmed off each side of a slot's tap area. An inset of 0 leaves the slot's own edge.
struct AmsSlotHitBand {
    int32_t left_inset = 0;
    int32_t right_inset = 0;
    /// Pixels a trimmed edge leans right per pixel down, below the height its inset is measured
    /// at, for lanes drawn at a slant; above it the edge stands upright.
    float slant = 0;
    /// The slot-local heights at which the left and right insets are measured.
    int32_t left_y = 0;
    int32_t right_y = 0;
};

/// Whether a tap at (@p x, @p y) falls in @p band of a slot whose left, top and right edges
/// are @p x1, @p y1 and @p x2.
inline bool ams_slot_hit_band_holds(const AmsSlotHitBand& band, int32_t x1, int32_t y1, int32_t x2,
                                    int32_t x, int32_t y) {
    auto lean = [&](int32_t at) { return band.slant * (float)std::max<int32_t>(0, y - y1 - at); };
    if (band.left_inset > 0 && (float)x < (float)(x1 + band.left_inset) + lean(band.left_y))
        return false;
    if (band.right_inset > 0 && (float)x > (float)(x2 - band.right_inset) + lean(band.right_y))
        return false;
    return true;
}

/// A drawn lane in screen pixels: its spool's middle and its outlet below.
struct AmsLaneLine {
    int32_t x, y;
    int32_t out_x, out_y;
};

/**
 * @brief The band a slot owns over a drawing whose lanes lean.
 *
 * Each boundary stands midway between two neighbouring spools and, below their middles, leans
 * with the lanes toward their outlets, so a tap on a lane's spool, its outlet or anywhere
 * between opens that lane. @p lanes holds all @p n lanes;
 * the slot spans @p x1 to @p x2 from top @p y1.
 */
inline AmsSlotHitBand ams_slot_lane_hit_band(const AmsLaneLine* lanes, int i, int n, int32_t x1,
                                             int32_t y1, int32_t x2) {
    AmsSlotHitBand band;
    if (!lanes || i < 0 || i >= n)
        return band;
    const AmsLaneLine& lane = lanes[i];
    if (lane.out_y != lane.y)
        band.slant = (float)(lane.out_x - lane.x) / (float)(lane.out_y - lane.y);
    // Neighbours compute each shared boundary from the same integer midpoint, so they meet.
    if (i > 0) {
        const int32_t mx = (lanes[i - 1].x + lane.x) / 2, my = (lanes[i - 1].y + lane.y) / 2;
        band.left_inset = mx > x1 ? mx - x1 : 1;
        band.left_y = my - y1;
    }
    if (i < n - 1) {
        const int32_t mx = (lane.x + lanes[i + 1].x) / 2, my = (lane.y + lanes[i + 1].y) / 2;
        band.right_inset = x2 > mx ? x2 - mx : 1;
        band.right_y = my - y1;
    }
    return band;
}

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

/// The stagger row a slot's label stands in: @p rows rows, row 0 nearest the spools.
struct AmsLabelRow {
    int rows = 1;
    int row = 0;
};

/**
 * @brief A label row that alternates lane by lane over @p rows rows (1 or 2): even
 * lanes nearest the spools, odd lanes above.
 */
inline AmsLabelRow ams_alternate_row(int slot_index, int rows) {
    AmsLabelRow r;
    r.rows = rows > 1 ? 2 : 1;
    r.row = r.rows > 1 ? slot_index % 2 : 0;
    return r;
}

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
 * @brief Rows the labels over a drawn unit take: each stands centered over its own lane,
 * lanes @p pitch apart, so one row holds when the widest fits a pitch less @p gap; else
 * they alternate lane by lane between two (ams_alternate_row()).
 */
inline int ams_over_lane_label_rows(int32_t widest, int32_t pitch, int32_t gap) {
    return widest <= pitch - gap ? 1 : 2;
}

/**
 * @brief Widest a label over a drawn unit may be in @p rows rows. In one row it keeps a
 * pitch less @p gap. In two its row neighbours stand two pitches away, and the tick of the
 * lane between rises past it one pitch away: it stops @p gap short of that tick each side.
 */
inline int32_t ams_over_lane_label_width(int32_t pitch, int rows, int32_t gap) {
    return rows > 1 ? 2 * (pitch - gap) : pitch - gap;
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
