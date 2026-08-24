// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/**
 * @brief Floor share of the stacked portrait column reserved for the canvas.
 *
 * The canvas and the profiles list are the two elastic blocks in the portrait
 * column. Without a floor, a long profile list plus a five-row info card can
 * squeeze the plot until it is unreadable — and an unreadable plot is the one
 * thing this panel exists to show. A cramped profiles list is merely a scroll.
 *
 * This is a share of the WHOLE column (`column_h` below), not of whatever the
 * info card and profiles list happen to leave over (`avail_h`). A share of the
 * leftover can never exceed the leftover — min(0.35*avail_h, avail_h) is just
 * avail_h scaled down — so a floor computed from `avail_h` is algebraically
 * incapable of ever binding: it can only ask for less than what was already
 * available, never push back against the other blocks. Deriving it from the
 * column instead is what lets the floor demand MORE than the current leftover,
 * which is the only way a floor can mean anything.
 */
inline constexpr int32_t BED_MESH_PORTRAIT_CANVAS_MIN_PCT = 35;

/**
 * @brief Height for the portrait bed mesh canvas, or 0 for "cannot decide".
 *
 * Square is the target and the ceiling: a bed is a square-ish object and a
 * taller-than-wide plot buys no legibility while starving the profiles list.
 * When the column is too short for square, the canvas flattens rather than
 * pushing everything else off screen — a mesh squashed horizontally still
 * reads, one squashed vertically does not.
 *
 * Ceiling vs. floor: the ceiling always wins when the two conflict. The floor
 * is capped at `square` before it is ever compared against the fitted value,
 * so on a column tall enough that 35% of it exceeds `band_w`, the floor is
 * silently clamped down to `band_w` rather than inflating the canvas past its
 * own width. This is intentional and asymmetric — a canvas too SHORT is a
 * scroll away from readable (the floor exists to prevent exactly that), but a
 * canvas too TALL for its width is unreadable outright, so the harder
 * constraint has to be the one that cannot be overridden.
 *
 * @param band_w   Measured width of the canvas band.
 * @param avail_h  Height the stacked column has LEFT for the canvas, i.e.
 *                 column_h minus the info card, profiles list, and gaps.
 * @param column_h Height of the WHOLE stacked column, before anything else in
 *                 it is subtracted. This is what the floor is a share of — see
 *                 BED_MESH_PORTRAIT_CANVAS_MIN_PCT for why `avail_h` cannot be used
 *                 here. Passing avail_h in this slot recreates the dead floor;
 *                 the two parameters are not interchangeable.
 * @return Height in px, or 0 when any input is non-positive (the caller has
 *         not been laid out yet and should leave the XML default alone).
 */
constexpr int32_t bed_mesh_portrait_canvas_height(int32_t band_w, int32_t avail_h,
                                                  int32_t column_h) {
    if (band_w <= 0 || avail_h <= 0 || column_h <= 0) {
        return 0;
    }
    const int32_t square = band_w;
    // Square unless the column cannot afford it, then flatten.
    const int32_t fitted = (square <= avail_h) ? square : avail_h;
    // The floor is a share of the WHOLE column, so it can demand more than the
    // canvas's current leftover (`avail_h`) and push back against the info
    // card / profiles list — that is what makes it a floor rather than a
    // no-op. It is still capped at `square`: see the ceiling-vs-floor note
    // above for why the cap must never be lifted.
    const int32_t floor_h = column_h * BED_MESH_PORTRAIT_CANVAS_MIN_PCT / 100;
    const int32_t effective_floor = (floor_h < square) ? floor_h : square;
    return (fitted < effective_floor) ? effective_floor : fitted;
}

} // namespace helix
