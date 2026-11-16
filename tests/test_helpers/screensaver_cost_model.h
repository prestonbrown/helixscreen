// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver_frame.h"
#include "screensaver_gate.h"

#include <vector>

namespace helix::test {

/**
 * @brief What one 33 ms frame costs a two-core board, as a percent of a core
 *
 * Solved from three K1C runs of the load gate at fixed star counts, rendering inline. A
 * frame's cost is dominated by the NUMBER of areas it invalidates: an area costs about
 * 3600x what one of its pixels costs, because each is a separate draw task and flush.
 * Rough by construction: it catches a saver that grows an order of magnitude, and predicts
 * no percentage. It prices only the redraw of the areas a saver reports, not the saver's own
 * drawing into its canvas, which is small for the starfield and not for a saver that draws
 * most of its frame.
 */
inline constexpr double kSaverCostFixed = 10.1;
inline constexpr double kSaverCostPerArea = 0.244;
inline constexpr double kSaverCostPerPx = 6.7e-5;

/// Average areas and pixels a saver's frames asked to have redrawn.
struct SaverLoad {
    double areas = 0;
    double pixels = 0;
    double cost() const {
        return kSaverCostFixed + kSaverCostPerArea * areas + kSaverCostPerPx * pixels;
    }
    /// Adds one frame's dirty areas; divide by the frame count with average_over().
    void add_frame(const std::vector<helix::ui::DirtyRect>& dirty) {
        areas += static_cast<double>(dirty.size());
        for (const helix::ui::DirtyRect& r : dirty) {
            pixels += static_cast<double>(r.area());
        }
    }
    void average_over(int frames) {
        areas /= frames;
        pixels /= frames;
    }
};

/// A saver's budget on a fast `cores`-core board while the printer is idle, as a percent of a core.
inline double budget_pct(int cores) {
    return 100.0 * helix::ui::saver_budget_share(cores, false, false);
}

} // namespace helix::test
