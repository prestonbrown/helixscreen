// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "layout_port.h"

#include "ui_breakpoint.h"

#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "layout_manager.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace helix {

namespace {

// ---------------------------------------------------------------------------
// Frozen pre-v22 grid constants.
//
// These are dead values in the live grid — it derives both axes from one
// square-cell target — and exist only to reconstruct what the saved
// coordinates were counted against. Nothing here may be "kept in sync" with
// GridLayout: the whole point is that it describes a grid that no longer runs.
// ---------------------------------------------------------------------------

/// main GridLayout::GRID_DIMS, {cols, rows} per breakpoint. Six tiers, because
/// XXLarge did not exist yet and clamped onto XLarge.
constexpr int kLegacyBreakpoints = 6;
constexpr std::array<int, kLegacyBreakpoints> kLegacyCols = {6, 6, 6, 6, 8, 8};

constexpr int kLegacyTargetCellW = 160; // main TARGET_CELL_W_PX
constexpr int kLegacyMinDynamicCols = 4;
constexpr int kLegacyMaxDynamicCols = 16;
constexpr int kLegacyMinPortraitCols = 2;

/// Track step each axis of this widget may occupy, from the same registry
/// answer edit mode enforces during a drag.
std::pair<int, int> steps_for(const std::string& widget_id) {
    auto [c, r] = GridEditMode::snap_step_for(widget_id);
    return {std::max(1, c), std::max(1, r)};
}

/// Map one axis's distinct boundary coordinates onto tracks, once each.
///
/// Rebuilding every widget from a shared boundary map is what keeps neighbours
/// flush. Mapping a widget's own two edges instead lets the boundary it shares
/// with the next widget round one way here and the other way there, which opens
/// a sliver between two widgets the user had touching.
std::map<int, int> map_boundaries(const std::vector<LegacyPlacement>& saved, bool column_axis,
                                  int old_n, int new_n) {
    const int cell = GridLayout::TRACKS_PER_CELL;

    // Every boundary any positioned widget uses, plus both grid edges.
    std::set<int> bounds = {0, old_n};
    for (const auto& p : saved) {
        if (p.col < 0 || p.row < 0)
            continue;
        bounds.insert(column_axis ? p.col : p.row);
        bounds.insert(column_axis ? p.col + p.colspan : p.row + p.rowspan);
    }

    // A boundary may land on a half-cell track only where EVERY widget touching
    // it supports half cells on this axis. One whole-cell neighbour forces the
    // shared edge onto a cell, which is what stops the port handing a widget a
    // position edit mode would immediately refuse to give back.
    auto half_ok = [&](int b) {
        bool touched = false;
        for (const auto& p : saved) {
            if (p.col < 0 || p.row < 0)
                continue;
            const int lo = column_axis ? p.col : p.row;
            const int hi = lo + (column_axis ? p.colspan : p.rowspan);
            if (lo != b && hi != b)
                continue;
            touched = true;
            const auto [cs, rs] = steps_for(p.id);
            if ((column_axis ? cs : rs) != 1)
                return false;
        }
        return touched;
    };

    std::map<int, int> out;
    for (int b : bounds) {
        if (b <= 0) {
            out[b] = 0;
            continue;
        }
        if (b >= old_n) {
            out[b] = new_n;
            continue;
        }
        const double t = static_cast<double>(b) / old_n * new_n;
        out[b] = half_ok(b) ? static_cast<int>(t + 0.5) : static_cast<int>(t / cell + 0.5) * cell;
    }

    // A strictly increasing source must stay strictly increasing. Two
    // boundaries collapsing onto one track would hand a widget a zero span,
    // which reads downstream as "occupies nothing" rather than as a failure.
    //
    // Capped at the grid edge: more boundaries than tracks — an old grid finer
    // than the new one — would otherwise walk the tail past new_n and hand out
    // coordinates off the grid. Pinning them to the edge instead makes those
    // widgets collide and fall to auto-placement, which is the honest outcome.
    int prev = -1;
    for (auto& [b, t] : out) {
        const int step = half_ok(b) ? 1 : cell;
        if (t <= prev) {
            t = std::min(prev + step, new_n);
        }
        prev = t;
    }
    return out;
}

} // namespace

int legacy_grid_cols(int panel_w, int panel_h) {
    if (panel_w <= 0 || panel_h <= 0) {
        return 0;
    }
    // Explicit template argument: to_int() yields int32_t, which is long on the
    // xtensa toolchain, so deduction against the int literals fails there while
    // compiling clean on desktop.
    const int bp = std::clamp<int32_t>(to_int(breakpoint_for(std::min(panel_w, panel_h))), 0,
                                       kLegacyBreakpoints - 1);
    const LayoutType type = detect_layout_type(panel_w, panel_h);

    if (type == LayoutType::ULTRAWIDE) {
        return std::clamp(panel_w / kLegacyTargetCellW, kLegacyMinDynamicCols,
                          kLegacyMaxDynamicCols);
    }
    if (is_portrait_layout(type)) {
        return std::clamp(panel_w / kLegacyTargetCellW, kLegacyMinPortraitCols,
                          kLegacyMaxDynamicCols);
    }
    return kLegacyCols[static_cast<size_t>(bp)];
}

int legacy_grid_rows(const std::vector<LegacyPlacement>& saved, int cached_rows) {
    int used = 0;
    for (const auto& p : saved) {
        if (p.col < 0 || p.row < 0) {
            continue;
        }
        used = std::max(used, p.row + p.rowspan);
    }
    // The cache was a floor for widgets whose hardware gate had not yet fired,
    // never a cap: a layout taller than the cache is the layout that is real.
    return std::max({used, cached_rows, 1});
}

std::vector<PortedPlacement> port_legacy_layout(const std::vector<LegacyPlacement>& saved,
                                                int old_cols, int old_rows, int new_cols,
                                                int new_rows) {
    std::vector<PortedPlacement> out;
    out.reserve(saved.size());
    for (const auto& p : saved) {
        out.push_back({p.id, -1, -1, p.colspan, p.rowspan, false});
    }
    if (old_cols <= 0 || old_rows <= 0 || new_cols <= 0 || new_rows <= 0) {
        return out;
    }

    const auto xmap = map_boundaries(saved, true, old_cols, new_cols);
    const auto ymap = map_boundaries(saved, false, old_rows, new_rows);

    // Occupancy in tracks, so a collision is detected against what the grid will
    // actually hold rather than against the caller's intent.
    std::vector<bool> occupied(static_cast<size_t>(new_cols) * static_cast<size_t>(new_rows),
                               false);
    auto free_at = [&](int c, int r, int cs, int rs) {
        if (c < 0 || r < 0 || cs < 1 || rs < 1 || c + cs > new_cols || r + rs > new_rows) {
            return false;
        }
        for (int y = r; y < r + rs; ++y) {
            for (int x = c; x < c + cs; ++x) {
                if (occupied[static_cast<size_t>(y) * static_cast<size_t>(new_cols) +
                             static_cast<size_t>(x)]) {
                    return false;
                }
            }
        }
        return true;
    };
    auto occupy = [&](int c, int r, int cs, int rs) {
        for (int y = r; y < r + rs; ++y) {
            for (int x = c; x < c + cs; ++x) {
                occupied[static_cast<size_t>(y) * static_cast<size_t>(new_cols) +
                         static_cast<size_t>(x)] = true;
            }
        }
    };

    for (size_t i = 0; i < saved.size(); ++i) {
        const auto& in = saved[i];
        if (in.col < 0 || in.row < 0) {
            continue; // never had a position; auto-placement already owns it
        }
        const auto cx0 = xmap.find(in.col);
        const auto cx1 = xmap.find(in.col + in.colspan);
        const auto cy0 = ymap.find(in.row);
        const auto cy1 = ymap.find(in.row + in.rowspan);
        if (cx0 == xmap.end() || cx1 == xmap.end() || cy0 == ymap.end() || cy1 == ymap.end()) {
            continue;
        }

        const auto [col_step, row_step] = steps_for(in.id);
        auto ceil_to = [](int v, int step) { return ((v + step - 1) / step) * step; };
        auto floor_to = [](int v, int step) { return (v / step) * step; };

        int col = floor_to(cx0->second, col_step);
        int row = floor_to(cy0->second, row_step);
        int colspan = std::max(col_step, ceil_to(cx1->second - cx0->second, col_step));
        int rowspan = std::max(row_step, ceil_to(cy1->second - cy0->second, row_step));

        // A span wider than the grid cannot be seated at all; clamp it to the
        // grid rather than dropping the widget, then push the origin back far
        // enough to hold it. Same order as clamp_to_grid() in the manager.
        colspan = std::min(colspan, floor_to(new_cols, col_step));
        rowspan = std::min(rowspan, floor_to(new_rows, row_step));
        if (col + colspan > new_cols) {
            col = std::max(0, floor_to(new_cols - colspan, col_step));
        }
        if (row + rowspan > new_rows) {
            row = std::max(0, floor_to(new_rows - rowspan, row_step));
        }

        if (!free_at(col, row, colspan, rowspan)) {
            continue; // per-widget fallback: the manager's auto-place pass takes it
        }
        occupy(col, row, colspan, rowspan);
        out[i] = {in.id, col, row, colspan, rowspan, true};
    }
    return out;
}

} // namespace helix
