// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// Grid dimensions for a specific breakpoint
struct GridDimensions {
    int cols;
    int rows;
};

/// Pixel geometry of one grid track pair, gutters accounted for.
struct CellMetrics {
    float cell_w; ///< Width of a single column track, gutters excluded
    float cell_h; ///< Height of a single row track, gutters excluded
    int gutter;   ///< Inter-track gap in px, same on both axes
    int cols;
    int rows;
};

/// Track geometry for a cols x rows grid inside a content rectangle.
///
/// LVGL distributes LV_GRID_FR(1) tracks as (content - (n-1)*gutter) / n. The
/// content rectangle from lv_obj_get_content_coords() excludes padding and
/// border but NOT these inter-track gaps, so dividing content by the track
/// count overstates every track and the error compounds across the axis.
///
/// Degenerate inputs (zero or negative counts, gutters wider than the content)
/// yield zero-size tracks rather than negative or infinite ones.
CellMetrics grid_cell_metrics(int content_w, int content_h, int cols, int rows, int gutter);

/// Offset of track `index` from the start of the content area, in px.
inline float grid_track_origin(float cell, int gutter, int index) {
    return static_cast<float>(index) * (cell + static_cast<float>(gutter));
}

/// Pixel extent of `span` consecutive tracks, including the gutters between
/// them but not the ones on either outside edge.
inline float grid_track_extent(float cell, int gutter, int span) {
    if (span <= 0) {
        return 0.0f;
    }
    return static_cast<float>(span) * cell + static_cast<float>(span - 1) * gutter;
}

/// Smallest span at or above (@p colspan, @p rowspan) whose pixel extent the
/// widget can actually draw, capped by @p max_colspan / @p max_rowspan.
///
/// Both the resize clamp and the load path ask this, so the rule lives once:
/// two hand-written copies would agree by convention until they silently did
/// not. `fits` must be MONOTONIC (see PanelWidget::fits_at), which is what
/// makes the first accepting size the nearest one and bounds the walk.
///
/// @param fits  answers whether the widget draws inside a w x h pixel box
std::pair<int, int> grow_span_to_fit(const std::function<bool(int, int)>& fits, int colspan,
                                     int rowspan, int max_colspan, int max_rowspan, int step_c,
                                     int step_r, const CellMetrics& metrics);

/// A rectangle of grid tracks.
struct TrackRect {
    int col = 0;
    int row = 0;
    int colspan = 1;
    int rowspan = 1;
};

/// Whether a tile at @p cell covers the whole @p cols x @p rows grid of page @p page_index
/// and that page is not the main page: the one rule for "this tile is a full page"
/// (prestonbrown/helixscreen#1931). A grid with no tracks has no full page.
inline bool cell_fills_extra_page(int page_index, int main_page_index, const TrackRect& cell,
                                  int cols, int rows) {
    return page_index != main_page_index && cols > 0 && rows > 0 && cell.col == 0 &&
           cell.row == 0 && cell.colspan >= cols && cell.rowspan >= rows;
}

/// Where a widget saved at @p saved can sit once grown to @p grown_colspan x
/// @p grown_rowspan (from grow_span_to_fit), on a @p cols x @p rows grid.
///
/// Growth may only take tracks @p available accepts: the grown rectangle
/// always covers the saved one, extending right/down first and shifting its
/// origin left/up by the widget's snap steps when that side is free. When no
/// such rectangle is available the saved one comes back unchanged, so a
/// widget that cannot draw its saved span keeps it rather than taking a
/// neighbour's place.
///
/// @param available  answers whether a candidate rectangle may be taken
TrackRect seat_grown_span(const TrackRect& saved, int grown_colspan, int grown_rowspan, int cols,
                          int rows, int step_c, int step_r,
                          const std::function<bool(const TrackRect&)>& available);

/// Number of tracks in an LVGL grid descriptor array.
///
/// The array is terminated by LV_GRID_TEMPLATE_LAST. LVGL's own counter is
/// file-local, so callers reading a descriptor back off a container need this.
/// Returns 0 for a null array, for an empty one, and for an array with no
/// terminator within kGridTrackScanMax — a missing terminator means the
/// pointer is stale or not a descriptor, and walking further reads off the end.
int grid_count_tracks(const int32_t* dsc);

/// A widget placement on the grid
struct GridPlacement {
    std::string widget_id;
    int col;
    int row;
    int colspan;
    int rowspan;
};

/// Manages grid layout for the home panel dashboard.
/// Handles grid descriptor generation, widget placement, collision detection,
/// and breakpoint adaptation.
class GridLayout {
  public:
    /// Number of defined breakpoints. One per UiBreakpoint tier, XXLarge
    /// included — a short table here does not fail to compile, it silently
    /// clamps the top tier onto the one below and the grid stops growing.
    static constexpr int kNumBreakpoints = 7;

    /// Number of grid tracks that make up one authored cell.
    ///
    /// Authored spans — in the registry, in default_layout.json and in a saved
    /// layout — are expressed in cells. The grid lays out tracks. Every site
    /// that converts between the two reads this, so a widget that is not
    /// allowed to occupy half a cell can never be placed straddling one.
    static constexpr int TRACKS_PER_CELL = 2;

    /// Target track edge in px, per breakpoint tier, indexed by UiBreakpoint.
    ///
    /// A track is half a cell, so a widget's authored colspan and rowspan are
    /// the same physical unit and it is authored once for every panel and
    /// orientation. Dividing each screen axis by the same number is what makes
    /// the cell square: a rotated panel transposes its grid exactly.
    ///
    /// The XXLarge rung carries its own entry, equal to XLarge's 72px edge, so
    /// 1920x1080 yields a 12x7-cell (24x14-track) grid.
    static constexpr int GRID_CELL[kNumBreakpoints] = {34, 40, 40, 60, 60, 72, 72};
    static_assert(std::size(GRID_CELL) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
                  "GRID_CELL must carry one track edge per UiBreakpoint tier");

    /// Degenerate-display guard, in tracks. Reached only by a content box that
    /// is empty or has not been laid out yet; the narrowest shipping content
    /// box is 264px against a 68px cell, which gives 8 tracks.
    ///
    /// A whole number of cells, so clamping cannot produce an odd track count.
    static constexpr int kMinTracks = 4;

    /// Ceiling on track count. High enough that no plausible content box
    /// reaches it — a lower cap would stretch the track and break the
    /// square-cell invariant instead of merely capping memory. The cost is
    /// descriptor entries, not objects: a 64x64 grid is 130 int32 values
    /// (cols+1 plus rows+1) in the LVGL grid descriptor.
    ///
    /// A whole number of cells, so clamping cannot produce an odd track count.
    static constexpr int kMaxTracks = 64;

    /// Track counts for a content rectangle at a given breakpoint.
    ///
    /// `content_w` and `content_h` are the container's CONTENT box — what the
    /// tracks are actually laid out inside — not the panel resolution. Panel
    /// chrome takes a different bite out of each axis and out of each
    /// orientation, so dividing the panel extent sizes every track against a
    /// rectangle the grid never occupies.
    ///
    /// Each axis is quantised to the NEAREST whole cell rather than the
    /// largest that fits. Flooring throws away up to a full cell and spreads it
    /// across the survivors, which on some panels stretches a track by a
    /// quarter of its target; rounding keeps the delivered track within half a
    /// cell of GRID_CELL on both axes, which is what keeps the cell square.
    /// The result is a whole number of cells, so the track count is always even.
    ///
    /// A zero or negative extent yields the kMinTracks floor, deliberately and
    /// identically on both axes, rather than a plausible-looking grid derived
    /// from nothing.
    static GridDimensions get_dimensions(UiBreakpoint bp, int content_w, int content_h);

    /// Column track count for a content rectangle. See get_dimensions().
    static int get_cols(UiBreakpoint bp, int content_w, int content_h);

    /// Row track count for a content rectangle. See get_dimensions().
    static int get_rows(UiBreakpoint bp, int content_w, int content_h);

    /// Inter-track gap the home grid is built with, in px: the per-breakpoint
    /// `home_grid_gap` token (ui_xml/globals.xml), which matches space_xs up to
    /// medium and widens from large up. It is grid spacing, so a user's own
    /// layout gets it too.
    ///
    /// Single source of truth for the spacing token: PanelWidgetManager sets the
    /// container's pad_column/pad_row from this, and every consumer that
    /// converts pixels to cells reads the same value. Returns 0 when the theme
    /// is not initialized, which is also the correct answer for a grid that has
    /// not been styled yet.
    static int gutter_px();

    /// Generate an LVGL column descriptor array of `ncols` equal tracks.
    /// Returns vector of int32_t values terminated by LV_GRID_TEMPLATE_LAST.
    static std::vector<int32_t> make_col_dsc(int ncols);

    /// Generate an LVGL row descriptor array of `nrows` equal tracks.
    /// Returns vector of int32_t values terminated by LV_GRID_TEMPLATE_LAST.
    static std::vector<int32_t> make_row_dsc(int nrows);

    /// Construct a GridLayout for placement on a grid of the given size.
    ///
    /// The size is supplied rather than derived: it is a property of the
    /// container being subdivided, and a caller that has already built a
    /// descriptor must place widgets against the same track counts that
    /// descriptor was built with. Callers holding a container get `dims` from
    /// get_dimensions(); GridEditMode reads them off the live descriptor.
    GridLayout(UiBreakpoint bp, GridDimensions dims);

    /// Get the breakpoint this layout was constructed for
    UiBreakpoint breakpoint() const {
        return breakpoint_;
    }

    /// Get grid dimensions
    GridDimensions dimensions() const;
    int cols() const;
    int rows() const;

    /// Try to place a widget. Returns true if placed successfully.
    /// Fails if placement overlaps existing widgets or is out of bounds.
    bool place(const GridPlacement& placement);

    /// Remove a widget by ID. Returns true if found and removed.
    bool remove(const std::string& widget_id);

    /// Check if a placement would be valid (no collision, in bounds)
    bool can_place(int col, int row, int colspan, int rowspan) const;

    /// Find first available position for a widget of given size.
    /// Scans top-to-bottom, left-to-right (row-major order).
    ///
    /// `col_step` / `row_step` are the track boundaries the origin may land on
    /// — TRACKS_PER_CELL for a widget that must occupy whole cells, 1 for an
    /// axis it declares half-cell support on. They default to a whole cell:
    /// half-cell placement is opt-in everywhere else, and a search that walked
    /// every track seated whole-cell widgets straddling two of them whenever a
    /// half-cell neighbour had left an odd-aligned gap (#1126).
    std::optional<std::pair<int, int>> find_available(int colspan, int rowspan,
                                                      int col_step = TRACKS_PER_CELL,
                                                      int row_step = TRACKS_PER_CELL) const;

    /// Find first available position scanning bottom-to-top, right-to-left.
    /// Used by auto-placement to pack widgets toward the bottom of the grid.
    /// `col_step` / `row_step` as for find_available().
    std::optional<std::pair<int, int>> find_available_bottom(int colspan, int rowspan,
                                                             int col_step = TRACKS_PER_CELL,
                                                             int row_step = TRACKS_PER_CELL) const;

    /// Why a flexible placement attempt failed.
    enum class PlacementFailure {
        None,            ///< Placed successfully
        GridFull,        ///< Fits the grid, but no free run of cells is left
        TooLargeForGrid, ///< Exceeds the grid even at the declared minimum span
    };

    /// Outcome of find_available_bottom_min(): position plus the span that was
    /// actually granted, or the reason nothing could be granted.
    struct SpanPlacement {
        int col = -1;
        int row = -1;
        int colspan = 0;
        int rowspan = 0;
        PlacementFailure failure = PlacementFailure::TooLargeForGrid;

        bool placed() const {
            return failure == PlacementFailure::None;
        }
    };

    /// Bottom-packed placement at a widget's DECLARED MINIMUM span.
    ///
    /// A widget authored for the 6-column landscape grid (tips: colspan 4)
    /// cannot exist in a 2-column portrait grid, but it advertises a minimum it
    /// *can* live at. Auto-placement asks for the minimum so widget count is
    /// maximised, then grows survivors back with grow_to_targets(). Handing out
    /// the largest span that fit instead let one widget eat the cells its
    /// neighbours needed and disabled them with "grid full" (#1216).
    ///
    /// `failure` distinguishes "no space left" from "larger than the whole
    /// grid" so the caller can say which condition actually failed.
    SpanPlacement find_available_bottom_min(int min_colspan, int min_rowspan,
                                            int col_step = TRACKS_PER_CELL,
                                            int row_step = TRACKS_PER_CELL) const;

    /// Short, user-facing phrase naming a placement failure. Kept next to the
    /// enum so the toast and the log cannot drift apart.
    static const char* failure_text(PlacementFailure reason);

    /// A placed widget's growth goal — the span its definition authors, which
    /// is where grow_to_targets() tries to get it back to.
    ///
    /// The steps are the same per-axis track boundaries find_available() takes:
    /// growth moves an origin as well as a span, so a widget that may not
    /// straddle a cell must grow a whole cell at a time or not at all (#1126).
    struct GrowthTarget {
        std::string widget_id;
        int colspan;
        int rowspan;
        int col_step = TRACKS_PER_CELL;
        int row_step = TRACKS_PER_CELL;
    };

    /// Expand one already-placed widget by a single growth step toward
    /// `target_colspan` x `target_rowspan`. Returns true when the placement
    /// changed.
    ///
    /// Directions are tried in a fixed order — RIGHT, DOWN, LEFT, UP. Right and
    /// down come first because they keep the widget's (col,row) origin fixed,
    /// so growth does not reshuffle the grid; left and up are the fallback for
    /// a widget already against the right or bottom edge, which is where
    /// bottom-packed minimum placement puts most of them. The whole new strip
    /// must be free and in bounds, so a widget never overruns a neighbour.
    ///
    /// The target is a ceiling, never a floor: a widget already at or past its
    /// target does not move.
    ///
    /// One step is `col_step` tracks horizontally and `row_step` vertically —
    /// a whole cell unless the widget declared half-cell support on that axis.
    bool grow_once(const std::string& widget_id, int target_colspan, int target_rowspan,
                   int col_step = TRACKS_PER_CELL, int row_step = TRACKS_PER_CELL);

    /// Round-robin expansion toward every target: each pass offers every widget
    /// in `targets` ONE growth step, and passes repeat until one changes
    /// nothing. Round-robin rather than one-widget-at-a-time so a single large
    /// widget cannot absorb all the slack while the widget behind it stays at
    /// its minimum. Returns the number of growth steps applied.
    ///
    /// Deterministic: the direction order is fixed and `targets` is walked in
    /// the caller's order, so the same grid and the same list always produce the
    /// same layout.
    int grow_to_targets(const std::vector<GrowthTarget>& targets);

    /// Current placement for a widget id, or nullptr when it is not placed.
    const GridPlacement* find_placement(const std::string& widget_id) const;

    /// Get all current placements
    const std::vector<GridPlacement>& placements() const {
        return placements_;
    }

    /// Check which placements from a list fit within a grid of the given size.
    /// Returns two vectors: (fits, does_not_fit)
    static std::pair<std::vector<GridPlacement>, std::vector<GridPlacement>>
    filter_for_grid(GridDimensions dims, const std::vector<GridPlacement>& placements);

    /// Clear all placements
    void clear();

    /// Check if a cell is occupied by any existing placement
    bool is_occupied(int col, int row) const;

  private:
    /// True when every cell of the rectangle is in bounds and unoccupied.
    /// Growth checks the strip it is about to swallow, which never overlaps the
    /// growing widget's own cells, so no self-exclusion is needed.
    bool strip_is_free(int col, int row, int colspan, int rowspan) const;

    GridPlacement* find_placement_mut(const std::string& widget_id);

    UiBreakpoint breakpoint_;
    GridDimensions dims_;
    std::vector<GridPlacement> placements_;
};

} // namespace helix
