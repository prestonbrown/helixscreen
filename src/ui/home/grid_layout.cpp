// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_layout.h"

#include "display_metrics.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {
constexpr int kGridTrackScanMax = 256;

// The track clamps are stated in tracks because that is what the grid
// descriptor holds, but the quantiser works in whole cells, so it clamps the
// cell count. Both bounds are whole cells, which is what lets the clamp run in
// cell space without ever producing an odd track count.
static_assert(GridLayout::kMinTracks % GridLayout::TRACKS_PER_CELL == 0,
              "MIN_TRACKS must be a whole number of cells");
static_assert(GridLayout::kMaxTracks % GridLayout::TRACKS_PER_CELL == 0,
              "MAX_TRACKS must be a whole number of cells");
constexpr int kMinCells = GridLayout::kMinTracks / GridLayout::TRACKS_PER_CELL;
constexpr int kMaxCells = GridLayout::kMaxTracks / GridLayout::TRACKS_PER_CELL;
} // namespace

static int clamp_bp(UiBreakpoint bp) {
    int32_t v = to_int(bp);
    if (v < 0)
        return 0;
    if (v >= GridLayout::kNumBreakpoints)
        return GridLayout::kNumBreakpoints - 1;
    return v;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

GridDimensions GridLayout::get_dimensions(UiBreakpoint bp, int content_w, int content_h) {
    // A cell is TRACKS_PER_CELL tracks, and only a whole cell can be occupied,
    // so the quantiser counts cells and multiplies back up. Nearest rather than
    // largest-that-fits: the remainder is spread across the tracks that survive,
    // so discarding a nearly-complete cell inflates every track on that axis,
    // while rounding it up shrinks each one by a much smaller share.
    // The authored track edge is scaled by the high-DPI UI scale factor before
    // quantising, so a phone-class panel gets a physically correct cell rather
    // than the ~12mm one its raw pixel count would pick. active_scale() is 1.0
    // on every shipping printer (they sit inside the DPI deadband), which makes
    // this multiply an exact no-op there.
    const int cell = helix::DisplayMetrics::scaled_px(
        GridLayout::TRACKS_PER_CELL * GRID_CELL[static_cast<size_t>(clamp_bp(bp))],
        helix::DisplayMetrics::active_scale());
    auto tracks = [cell](int content) {
        const int cells = (std::max(0, content) + cell / 2) / cell;
        return GridLayout::TRACKS_PER_CELL * std::clamp(cells, kMinCells, kMaxCells);
    };
    return {tracks(content_w), tracks(content_h)};
}

int GridLayout::get_cols(UiBreakpoint bp, int content_w, int content_h) {
    return get_dimensions(bp, content_w, content_h).cols;
}

int GridLayout::get_rows(UiBreakpoint bp, int content_w, int content_h) {
    return get_dimensions(bp, content_w, content_h).rows;
}

std::vector<int32_t> GridLayout::make_col_dsc(int ncols) {
    // A negative count would wrap in the reserve() cast; a descriptor of no
    // tracks is still a valid, terminated descriptor.
    const int count = std::max(0, ncols);
    std::vector<int32_t> dsc;
    dsc.reserve(static_cast<size_t>(count) + 1);
    for (int i = 0; i < count; ++i) {
        dsc.push_back(LV_GRID_FR(1));
    }
    dsc.push_back(LV_GRID_TEMPLATE_LAST);
    return dsc;
}

std::vector<int32_t> GridLayout::make_row_dsc(int nrows) {
    // A negative count would wrap in the reserve() cast; a descriptor of no
    // tracks is still a valid, terminated descriptor.
    const int count = std::max(0, nrows);
    std::vector<int32_t> dsc;
    dsc.reserve(static_cast<size_t>(count) + 1);
    for (int i = 0; i < count; ++i) {
        dsc.push_back(LV_GRID_FR(1));
    }
    dsc.push_back(LV_GRID_TEMPLATE_LAST);
    return dsc;
}

// ---------------------------------------------------------------------------
// Instance methods
// ---------------------------------------------------------------------------

GridLayout::GridLayout(UiBreakpoint bp, GridDimensions dims) : breakpoint_(bp), dims_(dims) {}

GridDimensions GridLayout::dimensions() const {
    return dims_;
}

int GridLayout::cols() const {
    return dims_.cols;
}

int GridLayout::rows() const {
    return dims_.rows;
}

bool GridLayout::is_occupied(int col, int row) const {
    for (const auto& p : placements_) {
        if (col >= p.col && col < p.col + p.colspan && row >= p.row && row < p.row + p.rowspan) {
            return true;
        }
    }
    return false;
}

bool GridLayout::can_place(int col, int row, int colspan, int rowspan) const {
    int ncols = cols();
    int nrows = rows();

    // Bounds check
    if (col < 0 || row < 0 || colspan <= 0 || rowspan <= 0)
        return false;
    if (col + colspan > ncols || row + rowspan > nrows)
        return false;

    // Collision check
    for (int c = col; c < col + colspan; ++c) {
        for (int r = row; r < row + rowspan; ++r) {
            if (is_occupied(c, r))
                return false;
        }
    }
    return true;
}

bool GridLayout::place(const GridPlacement& placement) {
    if (!can_place(placement.col, placement.row, placement.colspan, placement.rowspan)) {
        spdlog::debug("GridLayout: cannot place '{}' at ({},{}) span {}x{} in {}x{} grid",
                      placement.widget_id, placement.col, placement.row, placement.colspan,
                      placement.rowspan, cols(), rows());
        return false;
    }
    placements_.push_back(placement);
    return true;
}

bool GridLayout::remove(const std::string& widget_id) {
    auto it = std::find_if(placements_.begin(), placements_.end(),
                           [&](const GridPlacement& p) { return p.widget_id == widget_id; });
    if (it == placements_.end())
        return false;
    placements_.erase(it);
    return true;
}

namespace {
/// Largest multiple of `step` that is <= `value`. The scans below start from the
/// last origin that fits and walk back, so they have to start on a legal
/// boundary too — `ncols - colspan` is only even when both operands are.
///
/// A negative `value` means the span does not fit the axis at all; it is
/// returned as-is so the caller's `>= 0` guard still ends the scan before it
/// starts (C++ truncates -1/2 toward zero, which would otherwise read as 0).
int floor_to_step(int value, int step) {
    if (value < 0) {
        return value;
    }
    return step > 1 ? (value / step) * step : value;
}
} // namespace

std::optional<std::pair<int, int>> GridLayout::find_available(int colspan, int rowspan,
                                                              int col_step, int row_step) const {
    int ncols = cols();
    int nrows = rows();
    col_step = std::max(1, col_step);
    row_step = std::max(1, row_step);

    // Scan top-to-bottom, left-to-right, visiting only origins the widget is
    // allowed to occupy (#1126).
    for (int r = 0; r <= nrows - rowspan; r += row_step) {
        for (int c = 0; c <= ncols - colspan; c += col_step) {
            if (can_place(c, r, colspan, rowspan)) {
                return std::make_pair(c, r);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>>
GridLayout::find_available_bottom(int colspan, int rowspan, int col_step, int row_step) const {
    int ncols = cols();
    int nrows = rows();
    col_step = std::max(1, col_step);
    row_step = std::max(1, row_step);

    // Scan bottom-to-top, right-to-left, from the last legal origin.
    for (int r = floor_to_step(nrows - rowspan, row_step); r >= 0; r -= row_step) {
        for (int c = floor_to_step(ncols - colspan, col_step); c >= 0; c -= col_step) {
            if (can_place(c, r, colspan, rowspan)) {
                return std::make_pair(c, r);
            }
        }
    }
    return std::nullopt;
}

GridLayout::SpanPlacement GridLayout::find_available_bottom_min(int min_colspan, int min_rowspan,
                                                                int col_step, int row_step) const {
    SpanPlacement out;

    // A span of 0 means "unset" in PanelWidgetDef; treat it as 1 cell.
    const int want_cols = std::max(1, min_colspan);
    const int want_rows = std::max(1, min_rowspan);

    // Larger than the whole grid even at the declared minimum: no arrangement of
    // free cells could ever hold it, so this is not a "grid full" condition.
    if (want_cols > cols() || want_rows > rows()) {
        out.failure = PlacementFailure::TooLargeForGrid;
        return out;
    }

    if (auto pos = find_available_bottom(want_cols, want_rows, col_step, row_step)) {
        return {pos->first, pos->second, want_cols, want_rows, PlacementFailure::None};
    }

    // It fits the grid's dimensions; there is simply no free run of cells left.
    out.failure = PlacementFailure::GridFull;
    return out;
}

const GridPlacement* GridLayout::find_placement(const std::string& widget_id) const {
    auto it = std::find_if(placements_.begin(), placements_.end(),
                           [&](const GridPlacement& p) { return p.widget_id == widget_id; });
    return it == placements_.end() ? nullptr : &*it;
}

GridPlacement* GridLayout::find_placement_mut(const std::string& widget_id) {
    auto it = std::find_if(placements_.begin(), placements_.end(),
                           [&](const GridPlacement& p) { return p.widget_id == widget_id; });
    return it == placements_.end() ? nullptr : &*it;
}

bool GridLayout::strip_is_free(int col, int row, int colspan, int rowspan) const {
    if (col < 0 || row < 0 || colspan <= 0 || rowspan <= 0)
        return false;
    if (col + colspan > cols() || row + rowspan > rows())
        return false;
    for (int c = col; c < col + colspan; ++c) {
        for (int r = row; r < row + rowspan; ++r) {
            if (is_occupied(c, r))
                return false;
        }
    }
    return true;
}

bool GridLayout::grow_once(const std::string& widget_id, int target_colspan, int target_rowspan,
                           int col_step, int row_step) {
    GridPlacement* p = find_placement_mut(widget_id);
    if (!p)
        return false;

    col_step = std::max(1, col_step);
    row_step = std::max(1, row_step);

    // The grid is a hard ceiling on top of the widget's own target.
    const int target_cols = std::min(std::max(1, target_colspan), cols());
    const int target_rows = std::min(std::max(1, target_rowspan), rows());

    // RIGHT then DOWN keep the origin fixed; LEFT then UP are the fallback for a
    // widget already against an edge. See grow_once() in grid_layout.h. The
    // whole step has to be free, not just its first track: growing by one track
    // into a two-track gap would leave a whole-cell widget straddling (#1126).
    if (p->colspan + col_step <= target_cols &&
        strip_is_free(p->col + p->colspan, p->row, col_step, p->rowspan)) {
        p->colspan += col_step;
        return true;
    }
    if (p->rowspan + row_step <= target_rows &&
        strip_is_free(p->col, p->row + p->rowspan, p->colspan, row_step)) {
        p->rowspan += row_step;
        return true;
    }
    if (p->colspan + col_step <= target_cols &&
        strip_is_free(p->col - col_step, p->row, col_step, p->rowspan)) {
        p->col -= col_step;
        p->colspan += col_step;
        return true;
    }
    if (p->rowspan + row_step <= target_rows &&
        strip_is_free(p->col, p->row - row_step, p->colspan, row_step)) {
        p->row -= row_step;
        p->rowspan += row_step;
        return true;
    }
    return false;
}

int GridLayout::grow_to_targets(const std::vector<GrowthTarget>& targets) {
    int steps = 0;
    // Terminates: every step consumes at least one previously-free cell, and the
    // grid holds finitely many.
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& t : targets) {
            if (grow_once(t.widget_id, t.colspan, t.rowspan, t.col_step, t.row_step)) {
                progress = true;
                ++steps;
            }
        }
    }
    return steps;
}

const char* GridLayout::failure_text(PlacementFailure reason) {
    switch (reason) {
    case PlacementFailure::GridFull:
        return "grid full";
    case PlacementFailure::TooLargeForGrid:
        return "too big for this screen";
    case PlacementFailure::None:
        break;
    }
    return "placed";
}

std::pair<std::vector<GridPlacement>, std::vector<GridPlacement>>
GridLayout::filter_for_grid(GridDimensions dims, const std::vector<GridPlacement>& placements) {
    std::vector<GridPlacement> fits;
    std::vector<GridPlacement> does_not_fit;

    for (const auto& p : placements) {
        if (p.col >= 0 && p.row >= 0 && p.colspan > 0 && p.rowspan > 0 &&
            p.col + p.colspan <= dims.cols && p.row + p.rowspan <= dims.rows) {
            fits.push_back(p);
        } else {
            does_not_fit.push_back(p);
        }
    }
    return {std::move(fits), std::move(does_not_fit)};
}

void GridLayout::clear() {
    placements_.clear();
}

int GridLayout::gutter_px() {
    return theme_manager_get_spacing("home_grid_gap");
}

int grid_count_tracks(const int32_t* dsc) {
    if (dsc == nullptr) {
        return 0;
    }
    for (int i = 0; i < kGridTrackScanMax; ++i) {
        if (dsc[i] == LV_GRID_TEMPLATE_LAST) {
            return i;
        }
    }
    return 0;
}

CellMetrics grid_cell_metrics(int content_w, int content_h, int cols, int rows, int gutter) {
    CellMetrics m{};
    m.cols = cols;
    m.rows = rows;
    m.gutter = std::max(0, gutter);

    if (cols > 0 && content_w > 0) {
        float usable = static_cast<float>(content_w) - static_cast<float>(cols - 1) * m.gutter;
        m.cell_w = (usable > 0.0f) ? usable / static_cast<float>(cols) : 0.0f;
    }
    if (rows > 0 && content_h > 0) {
        float usable = static_cast<float>(content_h) - static_cast<float>(rows - 1) * m.gutter;
        m.cell_h = (usable > 0.0f) ? usable / static_cast<float>(rows) : 0.0f;
    }
    return m;
}

std::pair<int, int> grow_span_to_fit(const std::function<bool(int, int)>& fits, int colspan,
                                     int rowspan, int max_colspan, int max_rowspan, int step_c,
                                     int step_r, const CellMetrics& metrics) {
    const int c0 = std::max(colspan, 1);
    const int r0 = std::max(rowspan, 1);
    const int sc = std::max(step_c, 1);
    const int sr = std::max(step_r, 1);

    const auto w_of = [&](int c) {
        return static_cast<int>(grid_track_extent(metrics.cell_w, metrics.gutter, c));
    };
    const auto h_of = [&](int r) {
        return static_cast<int>(grid_track_extent(metrics.cell_h, metrics.gutter, r));
    };

    // One axis at a time, width first: a box that is merely too narrow must not
    // come back taller as well, or a widget resized against one edge silently
    // claims cells on the other. Stepping by the widget's own snap step keeps
    // the answer on a span the placement engine can seat.
    for (int c = c0; c <= max_colspan; c += sc) {
        if (fits(w_of(c), h_of(r0))) {
            return {c, r0};
        }
    }
    for (int r = r0; r <= max_rowspan; r += sr) {
        if (fits(w_of(c0), h_of(r))) {
            return {c0, r};
        }
    }

    // Neither axis alone is enough, so grow both until one accepts. The maxima
    // bound the walk; a widget that never accepts comes to rest at its ceiling
    // rather than looping.
    int c = c0;
    int r = r0;
    while (c + sc <= max_colspan || r + sr <= max_rowspan) {
        if (c + sc <= max_colspan) {
            c += sc;
        }
        if (r + sr <= max_rowspan) {
            r += sr;
        }
        if (fits(w_of(c), h_of(r))) {
            return {c, r};
        }
    }
    return {c, r};
}

TrackRect seat_grown_span(const TrackRect& saved, int grown_colspan, int grown_rowspan, int cols,
                          int rows, int step_c, int step_r,
                          const std::function<bool(const TrackRect&)>& available) {
    const int gc = std::max(grown_colspan, saved.colspan);
    const int gr = std::max(grown_rowspan, saved.rowspan);
    if (gc == saved.colspan && gr == saved.rowspan) {
        return saved;
    }
    const int sc = std::max(step_c, 1);
    const int sr = std::max(step_r, 1);

    // Every candidate covers the saved rectangle, so the widget only ever adds
    // tracks around where the user put it. Smallest shift first: right and down
    // before the origin moves at all.
    for (int dr = 0; dr <= gr - saved.rowspan; dr += sr) {
        for (int dc = 0; dc <= gc - saved.colspan; dc += sc) {
            const TrackRect cand{saved.col - dc, saved.row - dr, gc, gr};
            if (cand.col < 0 || cand.row < 0 || cand.col + gc > cols || cand.row + gr > rows) {
                continue;
            }
            if (available(cand)) {
                return cand;
            }
        }
    }
    return saved;
}

} // namespace helix
