// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/grid_edit_mode_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "grid_edit_mode.h"

namespace helix {

/// Friend access to GridEditMode internals for tests.
struct GridEditModeTestAccess {
    /// The snap target GridEditMode computed for the current drag.
    ///
    /// snap_preview_col_/row_ are the direct output of handle_drag_move()'s cell
    /// computation and the values handle_drag_end() commits to config, so they
    /// are what a drag test needs to see. Both reset to -1 on drag start and on
    /// exit.
    static int snap_col(const GridEditMode& em) {
        return em.snap_preview_col_;
    }
    static int snap_row(const GridEditMode& em) {
        return em.snap_preview_row_;
    }

    /// The event shield, which draws the lattice: a test can confirm the shield
    /// object survives a selection change or a page switch.
    static lv_obj_t* shield(const GridEditMode& em) {
        return em.shield_;
    }

    /// Dots in the lattice the shield draws for the current selection.
    static int drawn_dot_count(const GridEditMode& em) {
        return em.drawn_dot_count();
    }

    /// The pixel-tracking resize outline a live resize drag creates, by its
    /// first bar, the one the snap animation is keyed on. Reads as nullptr once
    /// commit_resize_with_snap() has handed it to the snap animation, so a
    /// lifetime test has to latch it before committing.
    static lv_obj_t* resize_preview(const GridEditMode& em) {
        return em.resize_outline_[0];
    }

    static std::array<lv_obj_t*, 4> raw_bars(const std::array<helix::ui::WeakWidget, 4>& bars) {
        return {bars[0].get(), bars[1].get(), bars[2].get(), bars[3].get()};
    }
    /// All four bars of the resize outline.
    static std::array<lv_obj_t*, 4> resize_outline(const GridEditMode& em) {
        return raw_bars(em.resize_outline_);
    }

    /// Create the resize outline the way handle_resize_move() does. A full drag
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
    /// the selected widget, as pure geometry. Which point the guard is fed is
    /// pinned by the [1169] cases, which drive a real indev.
    static bool press_owns_widget(const GridEditMode& em, lv_point_t origin,
                                  const lv_area_t& area) {
        return em.press_owns_widget(origin, area);
    }

    /// Track geometry of the live grid, the input the cross-page edge zone and
    /// push speed derive from.
    static helix::CellMetrics cell_metrics(const GridEditMode& em) {
        return em.current_metrics();
    }

    /// Edge grab band derived from the live grid (fallback with no container).
    static int edge_hit_band(const GridEditMode& em) {
        return em.edge_hit_band();
    }

    /// The pure cell-size -> band derivation, testable without a grid.
    static int edge_hit_band_for_cell(float cell_px) {
        return GridEditMode::edge_hit_band_for_cell(cell_px);
    }

    /// Pointer travel from the press origin that an armed press must exceed,
    /// in px, before it becomes a drag or a resize.
    static int drag_threshold_px() {
        return GridEditMode::kDragThresholdPx;
    }

    /// The container the session is scoped to, or nullptr outside a session.
    static lv_obj_t* container(const GridEditMode& em) {
        return em.container_;
    }

    /// The dragged widget's top-left in screen coordinates, as the drag's snap
    /// target and the drop's border test read it.
    static lv_point_t drag_widget_pos(const GridEditMode& em) {
        return em.drag_widget_pos_;
    }

    /// Which drag lifecycle handle_drag_start() committed the gesture to.
    ///
    /// resizing_ + resize_edge_ together are the direct witness that the resize
    /// branch was taken AND which edge it classified — resize_outline_ only
    /// proves the branch ran, and dragging_ separates "went down the move path"
    /// from "was dropped at the guard", which both leave resizing_ false.
    static bool resizing(const GridEditMode& em) {
        return em.resizing_;
    }
    static GridEditMode::ResizeEdge resize_edge(const GridEditMode& em) {
        return em.resize_edge_;
    }
    static bool dragging(const GridEditMode& em) {
        return em.is_dragging();
    }

    /// A press on the selected widget is armed and has not yet travelled past
    /// the drag threshold.
    static bool press_armed(const GridEditMode& em) {
        return em.press_armed_;
    }

    /// The selected widget's id as the edit session resolves it. The lattice
    /// and snap preview derive their granularity from it, so a drag test can
    /// assert it still resolves after a flip scopes the session to a page
    /// other than the one the widget's entry lives on.
    static std::string selected_widget_id(const GridEditMode& em) {
        return em.selected_widget_id();
    }

    /// The selection chrome: the outline overlay and its remove and configure
    /// buttons (the configure button exists only for a widget that offers
    /// configuration). All three are children of the scoped container.
    static lv_obj_t* selection_overlay(const GridEditMode& em) {
        return em.selection_overlay_;
    }
    static lv_obj_t* remove_button(const GridEditMode& em) {
        return em.remove_btn_;
    }
    static lv_obj_t* configure_button(const GridEditMode& em) {
        return em.configure_btn_;
    }

    /// The live drag's grid-snapped drop preview, or nullptr when none is drawn.
    static lv_obj_t* snap_preview(const GridEditMode& em) {
        return em.snap_preview_[0];
    }

    /// All four bars of the snap preview.
    static std::array<lv_obj_t*, 4> snap_preview_bars(const GridEditMode& em) {
        return raw_bars(em.snap_preview_);
    }

    /// Lift the selected widget for a drag, and settle it back, as a drag's
    /// start and end do.
    static void lift_dragged_widget(GridEditMode& em) {
        em.lift_dragged_widget();
    }
    static void settle_dragged_widget(GridEditMode& em) {
        em.settle_dragged_widget();
    }

    /// Move the dragged widget and its chrome the way a drag step does.
    static void place_dragged_widget(GridEditMode& em, lv_point_t widget_pos) {
        em.place_dragged_widget(widget_pos);
    }

    /// Settle a gesture that committed nothing, as a no-op drop does.
    static void reselect_in_place(GridEditMode& em, lv_obj_t* widget) {
        em.reselect_in_place(widget);
    }

    /// Draw the snap preview the way a drag or resize step does.
    static void update_snap_preview(GridEditMode& em, int col, int row, int colspan, int rowspan,
                                    bool valid) {
        em.update_snap_preview(col, row, colspan, rowspan, valid);
    }

    /// Whether a release now would create the page past the last one: the
    /// live drag resolved as its release resolves it.
    static bool drop_wants_new_page(const GridEditMode& em) {
        if (!em.dragging_ || !em.selected_) {
            return false;
        }
        const helix::DropResolution drop = helix::resolve_drop(
            em.drop_input(),
            em.page_occupancy(em.selected_widget_id(), GridEditMode::Occupants::OnScreen));
        return drop.outcome == helix::DropOutcome::CreatePage;
    }

    /// The delete-page button on the shield, or nullptr when none is drawn.
    static lv_obj_t* delete_page_button(const GridEditMode& em) {
        return em.delete_page_btn_;
    }

    /// Release the selection chrome, the snap preview, and redraw the lattice,
    /// each as edit mode's own paths do.
    static void destroy_selection_chrome(GridEditMode& em) {
        em.destroy_selection_chrome();
    }
    static void destroy_snap_preview(GridEditMode& em) {
        em.destroy_snap_preview();
    }
    static void rebuild_lattice(GridEditMode& em) {
        em.rebuild_lattice();
    }

    /// A committed resize is still easing into its cell: the rebuild that lays
    /// the resized widget out has not been scheduled yet.
    static bool snap_animating(const GridEditMode& em) {
        return em.snap_anim_outline_[0] != nullptr;
    }
};

} // namespace helix
