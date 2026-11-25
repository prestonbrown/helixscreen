// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace helix {

/// A saved home-screen placement in the pre-v22 unit: whole cells of the grid
/// that shipped before the square-cell rework (#1126).
struct LegacyPlacement {
    std::string id;
    int col = -1;
    int row = -1;
    int colspan = 1;
    int rowspan = 1;
    /// A disabled widget keeps its saved coordinates but held no space on the
    /// old grid, so it never takes a spot an enabled widget needs.
    bool enabled = true;
};

/// The same placement mapped onto the square-cell TRACK grid.
struct PortedPlacement {
    std::string id;
    int col = -1;
    int row = -1;
    int colspan = 1;
    int rowspan = 1;
    /// False when the port could not seat this widget on the new grid. col and
    /// row stay -1 so the caller's existing auto-placement pass takes it, which
    /// is the same path a never-positioned widget already travels. The port is
    /// per-widget: one widget failing costs only that widget its position.
    bool seated = false;
};

/// Column count the pre-v22 home grid would have had on this panel.
///
/// Frozen copy of main's `GridLayout::get_dimensions()` column axis, kept here
/// rather than in GridLayout because the live grid no longer has a fixed table
/// to consult — it derives both axes from one square-cell target. Deterministic
/// in the panel extent, which is what makes a deferred port possible at all:
/// nothing about the old grid needs to have been recorded.
int legacy_grid_cols(int panel_w, int panel_h);

/// Row count the pre-v22 home grid actually had, read back off the layout.
///
/// The old grid did not size its row axis from the breakpoint table; it took
/// `max(row + rowspan)` over the placed widgets and used the cached count only
/// as a floor (main panel_widget_manager.cpp:600). So the saved layout already
/// describes its own row count and no cache is needed to recover it.
/// `cached_rows` is that floor, 0 when unknown.
int legacy_grid_rows(const std::vector<LegacyPlacement>& saved, int cached_rows = 0);

/// Map cell-unit placements onto a `new_cols` x `new_rows` track grid.
///
/// Each axis's distinct boundary coordinates are mapped ONCE and every widget
/// is then rebuilt from the mapped boundaries, so two widgets that were flush
/// in the old grid stay flush. Mapping each widget's edges independently opens
/// sliver gaps between neighbours, because the shared boundary rounds twice and
/// need not round the same way.
///
/// A boundary lands on a half-cell track only where every widget touching it
/// declares half-cell support on that axis; otherwise it snaps to a whole cell,
/// which is the same rule `GridEditMode::snap_step_for()` enforces during a
/// drag. Entries without a grid position pass through unseated.
std::vector<PortedPlacement> port_legacy_layout(const std::vector<LegacyPlacement>& saved,
                                                int old_cols, int old_rows, int new_cols,
                                                int new_rows);

} // namespace helix
