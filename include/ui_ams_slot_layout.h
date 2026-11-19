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

/// Most rows the labels over a drawn unit stagger into.
inline constexpr int kAmsOverLaneMaxRows = 3;

/**
 * @brief A label row that cycles lane by lane over @p rows rows (1 to kAmsOverLaneMaxRows):
 * the first lane nearest the spools, each next one a row higher.
 */
inline AmsLabelRow ams_alternate_row(int slot_index, int rows) {
    AmsLabelRow r;
    r.rows = std::clamp(rows, 1, kAmsOverLaneMaxRows);
    r.row = slot_index % r.rows;
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

/// Where a label over a drawn unit stands about its lane: how far it reaches left and right of
/// the lane's center (its tick).
struct AmsLabelSpan {
    int32_t left = 0;
    int32_t right = 0;
    int32_t room_left = 0; ///< how far it could reach left, its neighbours and the row allowing
    int32_t room_right = 0;
};

/**
 * @brief Spans for labels over lanes at @p x (ascending), in rows @p row (0 nearest the unit),
 * each asking @p need wide. Centered on its lane where the room allows; where a neighbour
 * crowds one side, it slides toward the other, its own tick still under it. A label stops
 * @p gap short of halfway to a neighbour in its own row, and @p gap short of the tick of any
 * neighbour in a higher row, which rises past it; a neighbour in a lower row ticks down from
 * under it and takes nothing. No label reaches past @p lo or @p hi.
 */
inline void ams_over_lane_label_spans(const float x[], const int row[], const int32_t need[], int n,
                                      int32_t gap, float lo, float hi, AmsLabelSpan out[]) {
    for (int k = 0; k < n; ++k) {
        float room[2] = {x[k] - lo, hi - x[k]}; // left, right
        for (int j = 0; j < n; ++j) {
            if (j == k)
                continue;
            const int side = x[j] > x[k] ? 1 : 0;
            const float d = side ? x[j] - x[k] : x[k] - x[j];
            if (row[j] == row[k])
                room[side] = std::min(room[side], d / 2 - (float)gap / 2);
            else if (row[j] > row[k])
                room[side] = std::min(room[side], d - (float)gap);
        }
        const int32_t want = need[k];
        const int32_t room_l = (int32_t)std::max(0.0f, std::min(room[0], (float)want));
        const int32_t room_r = (int32_t)std::max(0.0f, std::min(room[1], (float)want));
        // Centered where both sides allow; else as far as the tighter side lets it, the rest
        // on the other.
        int32_t left = std::min(room_l, want / 2);
        const int32_t right = std::min(room_r, want - left);
        left = std::min(room_l, want - right);
        out[k] = {left, right, (int32_t)std::max(0.0f, std::min(room[0], 1e6f)),
                  (int32_t)std::max(0.0f, std::min(room[1], 1e6f))};
    }
}

/// Whether labels over lanes at @p x, each asking @p need wide, all get their room in
/// @p rows rows cycling label by label (k % rows). No label reaches past @p lo or @p hi.
inline bool ams_over_lane_labels_fit(const float x[], const int32_t need[], int n, int32_t gap,
                                     float lo, float hi, int rows) {
    int row[64];
    AmsLabelSpan span[64];
    n = std::min(n, 64);
    for (int k = 0; k < n; ++k)
        row[k] = k % rows;
    ams_over_lane_label_spans(x, row, need, n, gap, lo, hi, span);
    for (int k = 0; k < n; ++k) {
        if (span[k].left + span[k].right < need[k])
            return false;
    }
    return true;
}

/**
 * @brief The rows (1 to kAmsOverLaneMaxRows) the labels over lanes at @p x take, rows cycling
 * label by label (k % rows): the fewest that give every label its whole text (@p whole), else
 * the fewest that give each at least a few readable characters (@p readable), else
 * @p max_rows (at most kAmsOverLaneMaxRows), where the labels shorten to their room. No label
 * reaches past @p lo or @p hi.
 */
inline int ams_over_lane_label_rows(const float x[], const int32_t whole[],
                                    const int32_t readable[], int n, int32_t gap, float lo,
                                    float hi, int max_rows = kAmsOverLaneMaxRows) {
    max_rows = std::clamp(max_rows, 1, kAmsOverLaneMaxRows);
    for (const int32_t* need : {whole, readable}) {
        for (int rows = 1; rows <= max_rows; ++rows) {
            if (ams_over_lane_labels_fit(x, need, n, gap, lo, hi, rows))
                return rows;
        }
    }
    return max_rows;
}

/// How the lane numbers under a drawn unit stand: in one row or alternating between two (odd
/// lanes a number lower), and whether an empty lane's number keeps its "+".
struct AmsLaneBadgeLayout {
    int rows = 1;
    bool plus = true;
};

/**
 * @brief Lane numbers whose left edges stand at @p left (ascending), @p number wide with the
 * number alone and @p with_plus wide carrying an empty lane's "+": one row while neighbours
 * stand @p gap apart; else alternating rows; and where even that crowds them, no "+" (the
 * number's own tap state says it).
 */
inline AmsLaneBadgeLayout ams_lane_badge_layout(const float left[], const int32_t number[],
                                                const int32_t with_plus[], int n, int32_t gap) {
    const auto fits = [&](int rows, const int32_t w[]) {
        for (int i = 0; i + rows < n; ++i)
            if (left[i + rows] - (left[i] + (float)w[i]) < (float)gap)
                return false;
        return true;
    };
    if (fits(1, with_plus))
        return {1, true};
    if (fits(2, with_plus))
        return {2, true};
    return {2, false};
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
