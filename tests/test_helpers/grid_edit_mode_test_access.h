// tests/test_helpers/grid_edit_mode_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "grid_edit_mode.h"

namespace helix {

/// Reads the snap target GridEditMode computed for the current drag.
///
/// snap_preview_col_/row_ are the direct output of handle_drag_move()'s cell
/// computation and the values handle_drag_end() commits to config, so they are
/// what a drag test needs to see. Both reset to -1 on drag start and on exit.
struct GridEditModeTestAccess {
    static int snap_col(const GridEditMode& em) {
        return em.snap_preview_col_;
    }
    static int snap_row(const GridEditMode& em) {
        return em.snap_preview_row_;
    }

    /// The pixel-tracking resize overlay a live resize drag creates. Reads as
    /// nullptr once commit_resize_with_snap() has handed it to the snap
    /// animation, so a lifetime test has to latch it before committing.
    static lv_obj_t* resize_preview(const GridEditMode& em) {
        return em.resize_preview_;
    }

    /// Create resize_preview_ the way handle_resize_move() does. A full drag
    /// would reach the same call through the indev, but the snap animation's
    /// lifetime does not depend on how the preview came to exist.
    static void make_resize_preview(GridEditMode& em, int x, int y, int w, int h) {
        em.update_resize_preview_px(x, y, w, h, true);
    }

    /// Run the resize-commit path — the one that starts the snap animation.
    static void commit_resize(GridEditMode& em, const GridEditMode::ResizeResult& result) {
        em.commit_resize_with_snap(result);
    }

    /// The catalog's selection callback, called directly. Driving it through a
    /// real row click would also need the overlay, and what a placement test
    /// asserts on is the config this writes.
    static void place_from_catalog(GridEditMode& em, const std::string& widget_id) {
        em.place_widget_from_catalog(widget_id);
    }

    /// The guard handle_drag_start() uses to decide whether a gesture began on
    /// the selected widget. Driving it through a real drag would need an indev
    /// feeding synthetic points; what the rule asserts is pure geometry.
    static bool press_owns_widget(const GridEditMode& em, lv_point_t origin,
                                  const lv_area_t& area) {
        return em.press_owns_widget(origin, area);
    }

    /// Edge grab band derived from the live grid (fallback with no container).
    static int edge_hit_band(const GridEditMode& em) {
        return em.edge_hit_band();
    }

    /// The pure cell-size -> band derivation, testable without a grid.
    static int edge_hit_band_for_cell(float cell_px) {
        return GridEditMode::edge_hit_band_for_cell(cell_px);
    }

    /// Which drag lifecycle handle_drag_start() committed the gesture to.
    ///
    /// resizing_ + resize_edge_ together are the direct witness that the resize
    /// branch was taken AND which edge it classified — resize_preview_ only
    /// proves the branch ran, and dragging_ separates "went down the move path"
    /// from "was dropped at the guard", which both leave resizing_ false.
    static bool resizing(const GridEditMode& em) {
        return em.resizing_;
    }
    static GridEditMode::ResizeEdge resize_edge(const GridEditMode& em) {
        return em.resize_edge_;
    }
    static bool dragging(const GridEditMode& em) {
        return em.dragging_;
    }
};

} // namespace helix
