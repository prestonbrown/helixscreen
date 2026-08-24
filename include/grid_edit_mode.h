// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "grid_layout.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace helix {

class PanelWidgetConfig;
struct GridEditModeTestAccess; // test-only friend (tests/test_helpers/)

/// Manages in-panel grid editing for the home dashboard.
/// Handles enter/exit transitions, grid intersection dot overlay,
/// widget selection with corner brackets, and (X) removal.
class GridEditMode {
  public:
    using SaveCallback = std::function<void()>;
    using RebuildCallback = std::function<void()>;
    using DeletePageCallback = std::function<void()>;

    GridEditMode() = default;
    ~GridEditMode();

    GridEditMode(const GridEditMode&) = delete;
    GridEditMode& operator=(const GridEditMode&) = delete;
    GridEditMode(GridEditMode&&) = delete;
    GridEditMode& operator=(GridEditMode&&) = delete;

    void enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index = 0);
    void exit();

    bool is_active() const {
        return active_;
    }

    /// True when the widget catalog overlay is open (suppresses deactivate exit)
    bool is_catalog_open() const {
        return catalog_open_;
    }

    void set_save_callback(SaveCallback cb) {
        save_cb_ = std::move(cb);
    }

    void set_rebuild_callback(RebuildCallback cb) {
        rebuild_cb_ = std::move(cb);
    }

    void set_delete_page_callback(DeletePageCallback cb) {
        delete_page_cb_ = std::move(cb);
    }

    /// Page index that edit mode is currently scoped to
    int page_index() const {
        return page_index_;
    }

    /// Currently selected widget (nullptr if none)
    lv_obj_t* selected_widget() const {
        return selected_;
    }

    /// Select a widget (shows corner brackets + X button), or nullptr to deselect
    void select_widget(lv_obj_t* widget);

    /// Handle a click event on the container — hit-tests children for selection
    void handle_click(lv_event_t* e);

    /// Handle drag lifecycle events (called from container event callbacks)
    void handle_long_press(lv_event_t* e);
    void handle_pressing(lv_event_t* e);
    void handle_released(lv_event_t* e);
    void handle_drag_start(lv_event_t* e);

    /// Open the widget catalog overlay for adding a new widget.
    /// @param screen  The parent screen to host the overlay
    void open_widget_catalog(lv_obj_t* screen);

    /// Map screen coordinates to grid cell (col, row). Clamps to valid range.
    static std::pair<int, int> screen_to_grid_cell(int screen_x, int screen_y, int container_x,
                                                   int container_y, int container_w,
                                                   int container_h, int ncols, int nrows,
                                                   int gutter);

    /// Clamp desired colspan/rowspan to the min/max allowed by the widget registry.
    /// Returns {clamped_colspan, clamped_rowspan}.
    static std::pair<int, int> clamp_span(const std::string& widget_id, int desired_colspan,
                                          int desired_rowspan);

    /// Which edge of a widget the pointer is near (for resize detection)
    enum class ResizeEdge { None, Top, Bottom, Left, Right };

    /// Result of computing a resize operation
    struct ResizeResult {
        int col;
        int row;
        int colspan;
        int rowspan;
    };

    /// Round a pixel position to the nearest grid cell boundary.
    /// Returns a cell boundary index (0 to ncells inclusive).
    static int round_to_grid_cell(int px, int content_origin, int content_size, int ncells,
                                  int gutter);

    /// Compute new widget position/span for a resize operation.
    /// @param edge Which edge is being dragged
    /// @param orig_col/row/colspan/rowspan Original widget placement
    /// @param new_edge_cell The grid cell boundary the edge was dragged to
    /// @param ncells Number of cells along the resize axis
    static ResizeResult compute_resize_result(ResizeEdge edge, int orig_col, int orig_row,
                                              int orig_colspan, int orig_rowspan, int new_edge_cell,
                                              int ncells);

    /// Detect which resize edge the pointer is near, or None if not near any edge.
    ResizeEdge detect_resize_edge(int px, int py, const lv_area_t& widget_area) const;

  private:
    friend struct helix::GridEditModeTestAccess;

    /// Track geometry of the live grid container.
    ///
    /// The nine drag, resize, preview and lattice paths all need the same four
    /// numbers. Deriving them in one place keeps the int-vs-float rounding and
    /// the gutter handling consistent between the cell a drop is computed
    /// against and the pixels the preview is drawn at.
    ///
    /// @param out_content  Optional; receives the container's content area.
    /// @return Zeroed metrics when there is no container or it has no extent.
    helix::CellMetrics current_metrics(lv_area_t* out_content = nullptr) const;

    /// Edge grab band in px for a grid cell of @p cell_px on its shorter axis.
    ///
    /// A fraction of the cell rather than a fixed pixel count: the same 18px is
    /// a large share of a cell on a 480x272 panel and a sliver of one on a
    /// 1024x600 panel, so a constant makes the edge either impossible to miss
    /// or impossible to hit depending on the screen. Clamped at both ends to
    /// stay finger-sized.
    ///
    /// @return The fallback band when @p cell_px is not positive.
    static int edge_hit_band_for_cell(float cell_px);

    /// Edge grab band in px, derived from the live grid's cell size. Falls back
    /// to a fixed band before a grid exists (cell_w/cell_h are 0 then).
    int edge_hit_band() const;

    /// Whether a press at @p origin counts as landing on @p area, allowing the
    /// edge grab band of slop outside the bounds.
    ///
    /// Anchored at the press origin rather than the live pointer because a
    /// resize that grows a widget drags *away* from it by design: by the time
    /// the drag threshold is met the pointer is legitimately off-widget, and
    /// testing it there rejects exactly the gestures that should have become
    /// resizes. Where the finger first landed is what decides ownership.
    bool press_owns_widget(lv_point_t origin, const lv_area_t& area) const;

    void create_dots_overlay();
    void destroy_dots_overlay();
    void create_selection_chrome(lv_obj_t* widget);
    void destroy_selection_chrome();
    void remove_selected_widget();
    void configure_selected_widget();

    /// Defer rebuild_cb_ + lv_indev_reset to the next lv_timer_handler tick
    /// via lv_async_call, so that lv_obj_clean runs outside indev_proc_release.
    /// @param post_rebuild  Optional work to run after the rebuild completes
    void schedule_deferred_rebuild(std::function<void()> post_rebuild = nullptr);

    /// Find the config entry index for a given container child widget.
    /// Returns -1 if not found.
    int find_config_index_for_widget(lv_obj_t* widget) const;

    /// Sync config grid positions from actual widget screen coordinates.
    /// Called on enter() to ensure config matches the visual layout.
    void sync_config_from_screen();

    // Drag helpers
    void handle_drag_move(lv_event_t* e);
    void handle_drag_end(lv_event_t* e);
    void update_snap_preview(int col, int row, int colspan, int rowspan, bool valid);
    void destroy_snap_preview();
    void cleanup_drag_state();

    // Resize helpers
    bool is_selected_widget_resizable() const;
    void handle_resize_move(lv_event_t* e);
    void handle_resize_end(lv_event_t* e);
    void update_resize_preview_px(int x, int y, int w, int h, bool valid);
    void commit_resize_with_snap(const ResizeResult& result);

    /// Stop the resize snap animation if one is in flight.
    ///
    /// Its completion callback holds a raw `this` and dereferences config_, so
    /// both exit() (which nulls config_) and the destructor must run this. The
    /// animation's deleted_cb frees the heap context and clears
    /// snap_anim_preview_, so this is also the leak-free cancel path.
    void cancel_snap_animation();

    // Widget catalog placement
    void place_widget_from_catalog(const std::string& widget_id);
    bool hit_test_any_widget(int screen_x, int screen_y) const;

    bool active_ = false;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* dots_overlay_ = nullptr;
    lv_obj_t* selected_ = nullptr;
    lv_obj_t* selection_overlay_ = nullptr;
    lv_obj_t* remove_btn_ = nullptr; // Trash button (container child, not overlay child)
    lv_obj_t* configure_btn_ =
        nullptr; // Configure button (upper-left, shown if widget supports it)
    PanelWidgetConfig* config_ = nullptr;
    int page_index_ = 0;
    SaveCallback save_cb_;
    RebuildCallback rebuild_cb_;
    DeletePageCallback delete_page_cb_;
    lv_obj_t* delete_page_btn_ = nullptr;

    // Drag threshold: track press origin, only start real drag after movement
    static constexpr int DRAG_THRESHOLD_PX = 12;
    bool drag_pending_ = false;        // Finger is down on a widget, watching for threshold
    lv_point_t press_origin_ = {0, 0}; // Screen point where the press started

    // Drag state
    bool dragging_ = false;
    int drag_cfg_idx_ = -1; // Config index of dragged widget (saved at drag start)
    int drag_orig_col_ = -1;
    int drag_orig_row_ = -1;
    int drag_orig_colspan_ = 1;
    int drag_orig_rowspan_ = 1;
    lv_point_t drag_offset_ = {0, 0};
    lv_obj_t* snap_preview_ = nullptr;
    int snap_preview_col_ = -1;
    int snap_preview_row_ = -1;

    // Resize state
    bool resizing_ = false;
    ResizeEdge resize_edge_ = ResizeEdge::None;
    lv_obj_t* resize_preview_ = nullptr; // Pixel-tracking preview overlay

    // Widget the resize snap animation is driving, or nullptr when none is in
    // flight. It is the animation's `var`, which is what lets LVGL auto-cancel
    // on widget deletion and what cancel_snap_animation() cancels by.
    lv_obj_t* snap_anim_preview_ = nullptr;

    // Widget catalog placement: grid cell where the long-press originated
    int catalog_origin_col_ = -1;
    int catalog_origin_row_ = -1;

    // Set while the widget catalog overlay is open to prevent
    // on_deactivate → exit() from killing edit mode state.
    bool catalog_open_ = false;

    // Guards deferred lv_async_call callbacks scheduled by schedule_deferred_rebuild().
    // Destructor expires all tokens so callbacks become no-ops if GridEditMode
    // (and its owning HomePanel) is destroyed during switch_printer teardown.
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix
