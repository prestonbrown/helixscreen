// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file height_layer_dwell.h
 * @brief When a layer number derived from commanded Z may be shown.
 *
 * Pure logic: no LVGL, no clock of its own. Commanded Z equals the layer
 * height only while the nozzle prints. A Z-hop lifts it several layers for the
 * length of a travel move (a 0.4mm hop over 0.13mm layers is three layers), so a
 * layer read straight off Z flips up and back on every travel. A new candidate
 * is therefore shown only once Z has held it for a dwell: longer than any travel
 * move, shorter than any printed layer.
 */

namespace helix {

/// How long commanded Z must hold a new layer before it is shown, in seconds of
/// the caller's clock.
inline constexpr double kHeightLayerDwellSeconds = 1.5;

/// A derived layer waiting out the dwell.
struct HeightLayerPending {
    int layer = 0;        ///< 0 when nothing is pending
    double since_s = 0.0; ///< clock reading when this candidate was first derived
};

struct HeightLayerDecision {
    int show = 0;               ///< the layer to display
    HeightLayerPending pending; ///< state to pass to the next call
};

/**
 * @brief Debounce one Z-derived layer reading.
 *
 * @param first     no Z-derived layer has been shown this print; it applies at once
 * @param shown     the layer currently displayed
 * @param candidate the layer commanded Z derives now
 * @param pending   the state the previous call returned
 * @param now_s     the caller's clock, monotonic within a print
 * @param dwell_s   how long a candidate must hold before it is shown
 *
 * A candidate equal to the shown layer cancels anything pending, so a hop that
 * lands back where it took off leaves no trace. A candidate that changes before
 * the dwell ends restarts it, so the layer reached after a hop's descent waits
 * the full dwell from the moment Z settled on it.
 */
[[nodiscard]] inline HeightLayerDecision
decide_height_layer(bool first, int shown, int candidate, HeightLayerPending pending, double now_s,
                    double dwell_s = kHeightLayerDwellSeconds) {
    if (first || candidate == shown) {
        return {candidate, {}};
    }
    if (candidate != pending.layer) {
        return {shown, {candidate, now_s}};
    }
    if (now_s - pending.since_s >= dwell_s) {
        return {candidate, {}};
    }
    return {shown, pending};
}

} // namespace helix
