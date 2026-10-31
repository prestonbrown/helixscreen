// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "grid_edit_cross_page.h"
#include "grid_edit_drop.h"
#include "grid_edit_page_set.h"
#include "grid_layout.h"
#include "lvgl/lvgl.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace helix {

class PanelWidgetConfig;
struct PanelWidgetDef;
struct GridLatticeSpec;
struct GridEditModeTestAccess; // test-only friend (tests/test_helpers/)

/// A grid spot the catalog placement search found for a widget: the cell it
/// lands on and the span it lands with, the span possibly shrunk from the
/// def's default down to its minimum.
struct CatalogPlacement {
    int col = -1;
    int row = -1;
    int colspan = 0;
    int rowspan = 0;
};

/// Where a widget added from the catalog lands on @p occupancy: the origin
/// cell the catalog was opened from when it sits on a snap step, then the
/// first free position, then the same search at progressively smaller spans
/// down to the def's minimum, one snap step at a time. Returns nothing when
/// even the minimum fits nowhere.
///
/// Pure over its inputs, so the catalog's fit predicate and the placement
/// itself ask the same question and a test can drive a hand-built grid.
std::optional<CatalogPlacement> find_catalog_placement(const GridLayout& occupancy,
                                                       const PanelWidgetDef& def, int origin_col,
                                                       int origin_row, int col_step, int row_step);

/// Manages in-panel grid editing for the home dashboard.
/// Handles enter/exit transitions, grid intersection dot overlay,
/// widget selection with corner brackets, and (X) removal.
class GridEditMode {
  public:
    using RebuildCallback = std::function<void()>;
    using DeletePageCallback = std::function<void()>;
    /// Told that owns_gesture() or is_catalog_open() may have changed; HomePanel
    /// sets the carousel swipe policy from them.
    using GestureOwnershipCallback = std::function<void()>;
    /// Asks the owner to show page @p page and re-scope the session to it: the
    /// page next to the scoped one when a dragged widget's majority crosses a
    /// page border or the pointer dwells in an edge zone, a live drag's origin
    /// page when the drag ends off it having committed nothing, or the
    /// session's own page when the widget catalog closes, since the carousel
    /// may have paged while the catalog held the session. The owner ignores a
    /// page it has no container for.
    using ShowPageCallback = std::function<void(int page)>;

    /// Asked whether the owner shows a page past the last one, for a drag to
    /// flip onto and a drop there to create.
    using NextPageSlotCallback = std::function<bool()>;

    /// Asked at every drag move for the page frame: fill @p frame with the
    /// screen area the scoped page occupies with the carousel at rest and
    /// return true, or return false when there is none to report. The
    /// cross-page rules measure against it, so a page sliding in mid-flip
    /// moves nothing they compare.
    using PageFrameCallback = std::function<bool(lv_area_t& frame)>;

    /// Fired on the tick after a commit changed the page set itself - a drop
    /// created a page, or a page emptied and was pruned - with the change,
    /// numbered as before it, and never inside the input dispatch that committed
    /// it. The owner rebuilds the carousel where helix::page_set_landing() puts
    /// it, which deletes the scoped container, so it calls forget_scope() first,
    /// then shows the focus page; the grid skips its own deferred rebuild.
    using PagesChangedCallback = std::function<void(const helix::PageSetChange& change)>;

    /// Asked, on the tick after a move or resize committed on the scoped page,
    /// to re-seat that page's tiles at their config cells in place
    /// (PanelWidgetManager::relayout_tiles): the first argument names the
    /// widgets the commit placed, the second (empty for none) the one told its
    /// new span. Returns false when the page needs
    /// the full rebuild instead, which then runs.
    using RelayoutCallback = std::function<bool(const std::vector<std::string>& changed_ids,
                                                const std::string& resized_id)>;

    GridEditMode() = default;
    ~GridEditMode();

    GridEditMode(const GridEditMode&) = delete;
    GridEditMode& operator=(const GridEditMode&) = delete;
    GridEditMode(GridEditMode&&) = delete;
    GridEditMode& operator=(GridEditMode&&) = delete;

    /// Start an edit session. Returns whether edit mode is active afterwards:
    /// false when PanelWidgetManager::refuse_layout_edit() turns it away.
    bool enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index = 0);
    void exit();

    /// Re-scope a live edit session to @p container as page @p page_index.
    ///
    /// The session decides what travels:
    /// - The scope already held (same container and page) is a no-op.
    /// - While the widget catalog is open the scope is kept: the catalog places
    ///   onto the page it was opened from.
    /// - A live drag is carried: the dragged widget, the event shield and its
    ///   lattice, and the selection chrome move to the new container, the
    ///   widget keeping its screen position under the pointer, and the snap
    ///   target and its preview are recomputed on the landing page for the
    ///   widget's last position, so a release before the next move drops there.
    ///   Drag state is kept; the drop commits against the landing page.
    /// - Anything else is dropped, including a press armed but not yet
    ///   dragging, whose widget is still laid out in its own page's grid: the
    ///   selection and chrome go, the gesture state clears, and only the
    ///   shield and lattice move.
    ///
    /// Never saves config, rebuilds widgets or touches the home_edit_mode
    /// subject: it runs per page change, unlike exit()+enter().
    void switch_page(lv_obj_t* container, int page_index);

    /// Forget the scoped container and every object of the session in it,
    /// deleting nothing, ahead of a rebuild of the owner's pages that deletes
    /// them. The session stays live, scoped to no container, until the owner's
    /// next switch_page().
    void forget_scope();

    bool is_active() const {
        return active_;
    }

    /// True while an edit-mode gesture (armed press, drag, or resize) owns
    /// the pointer. The carousel swipe policy keys off this, so pages stay
    /// reachable whenever no widget owns the gesture
    /// (prestonbrown/helixscreen#1638).
    bool owns_gesture() const {
        return press_armed_ || dragging_ || resizing_;
    }

    /// True while the widget catalog overlay is open. The session then keeps
    /// its page and takes no grid input, and survives the panel deactivation
    /// the catalog's push causes.
    bool is_catalog_open() const {
        return catalog_open_;
    }

    /// True while a drag carries the selected widget, from the drag's start to
    /// its end. A press armed on the widget and a resize are not drags.
    bool is_dragging() const {
        return dragging_;
    }

    /// True while the session is scoped to @p container. False for nullptr, and
    /// between forget_scope() and the next switch_page().
    bool is_scoped_to(const lv_obj_t* container) const {
        return container != nullptr && container == container_;
    }

    void set_rebuild_callback(RebuildCallback cb) {
        rebuild_cb_ = std::move(cb);
    }

    void set_relayout_callback(RelayoutCallback cb) {
        relayout_cb_ = std::move(cb);
    }

    void set_delete_page_callback(DeletePageCallback cb) {
        delete_page_cb_ = std::move(cb);
    }

    void set_gesture_ownership_callback(GestureOwnershipCallback cb) {
        gesture_ownership_cb_ = std::move(cb);
    }

    void set_show_page_callback(ShowPageCallback cb) {
        show_page_cb_ = std::move(cb);
    }

    /// Install the next-page source (see NextPageSlotCallback). Without
    /// one, a page past the last is available while the config is below its
    /// page cap.
    void set_next_page_slot_callback(NextPageSlotCallback cb) {
        next_page_slot_cb_ = std::move(cb);
    }

    /// Install the page frame source (see PageFrameCallback). Without one, the
    /// cross-page rules measure the container's live content area.
    void set_page_frame_callback(PageFrameCallback cb) {
        page_frame_cb_ = std::move(cb);
    }

    void set_pages_changed_callback(PagesChangedCallback cb) {
        pages_changed_cb_ = std::move(cb);
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

    /// A hold (LONG_PRESSED) in a live session grabs: the selected widget, or
    /// the widget under the pointer once it selects it. A hold on empty grid
    /// with nothing selected opens the widget catalog instead. An inert gesture
    /// (see gesture_inert_) does neither, and nor does a hold while the catalog
    /// is open (see grid_input_acts).
    void handle_long_press(lv_event_t* e);

    /// End a gesture that owns the pointer without committing it: carry a drag
    /// scoped past the last page back to its origin page, clear the gesture
    /// state, tear down both previews and any snap animation, drop the
    /// selection, and schedule the deferred rebuild that restores widgets,
    /// chrome and lattice from config and then recreates the shield the rebuild
    /// deletes. The single cancel path, behind a press LVGL took away
    /// (handle_press_cancelled), begin_press and the owner's exit. A drop that
    /// commits nothing ends differently: it keeps its widget selected and
    /// rebuilds nothing, since its release left every object where it belongs.
    void end_gesture_uncommitted();

    /// PRESS_LOST or INDEV_RESET reaching the grid handlers: LVGL took a press
    /// away without a RELEASED. A gesture that owns the pointer ends
    /// uncommitted; with none, nothing happens.
    void handle_press_cancelled(lv_event_t* e);

    /// Start a new gesture. Call on every PRESSED the grid receives while edit
    /// mode is active. A gesture still owning the pointer here breaks an
    /// invariant: its press target is the shield, whose RELEASED, PRESS_LOST
    /// and INDEV_RESET each reach the grid handlers and end it. What remains is
    /// the shield deleted under a live gesture: a rebuild moves the shield off
    /// its page before deleting it, so the INDEV_RESET that delete sends
    /// reaches no grid handler. The gesture then ends uncommitted here, with a
    /// warning. The gesture
    /// state clears for the new press, which is what resets the grab latch
    /// after a swipe, whose RELEASED the grid handlers never act on. A press
    /// that lands while a resize snap animation runs finishes the snap and is
    /// inert (see gesture_inert_).
    void begin_press();
    /// PRESSING while edit mode is active. Moves the drag or resize the
    /// gesture holds. Otherwise the gesture's first pressing cycle decides
    /// its grab latch from the selection standing at the press: a press on
    /// the selected widget, its grab band included, arms, and travel past
    /// kDragThresholdPx starts a drag or a resize. Any other press selects
    /// the widget under the pointer on that first cycle, and no later cycle
    /// of the gesture selects again.
    void handle_pressing(lv_event_t* e);
    void handle_released(lv_event_t* e);

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

    /// Round a pixel position to the nearest grid cell boundary, snapping to
    /// multiples of @p step tracks.
    /// Returns a cell boundary index (0 to ncells inclusive, floored to a step
    /// multiple).
    static int round_to_grid_cell(int px, int content_origin, int content_size, int ncells,
                                  int gutter, int step);

    /// Compute new widget position/span for a resize operation.
    /// @param edge Which edge is being dragged
    /// @param orig_col/row/colspan/rowspan Original widget placement
    /// @param new_edge_cell The grid cell boundary the edge was dragged to
    /// @param ncells Number of cells along the resize axis
    /// @param step Track step the resulting span must land on (see snap_step_for)
    static ResizeResult compute_resize_result(ResizeEdge edge, int orig_col, int orig_row,
                                              int orig_colspan, int orig_rowspan, int new_edge_cell,
                                              int ncells, int step);

    /// Track step (in tracks) a widget snaps to on each axis, based on whether
    /// its registry entry allows occupying half a cell. Widgets with no
    /// registry entry get the conservative whole-cell answer.
    /// @return {col_step, row_step}
    static std::pair<int, int> snap_step_for(const std::string& widget_id);

    /// Number of lattice intersections drawn for a selection with these snap
    /// steps. Public so the object cost can be pinned without a live grid.
    static int dot_count(int ncols, int nrows, int col_step, int row_step);

    /// Detect which resize edge the pointer is near, or None if not near any edge.
    ResizeEdge detect_resize_edge(int px, int py, const lv_area_t& widget_area) const;

  private:
    friend struct helix::GridEditModeTestAccess;

    /// Whether input on the grid acts: a press, hold or click the grid handlers
    /// pass on, and a tap on the selection's configure or remove button or the
    /// delete-page button. True while the session is live and the widget
    /// catalog is closed. The catalog places onto the page and cell it was
    /// opened from; navigation's dismiss backdrop normally takes every press
    /// beside it, and when that backdrop could not be created the grid still
    /// takes no action.
    bool grid_input_acts() const {
        return active_ && !catalog_open_;
    }

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

    /// Content area of the scoped page at rest, in screen coordinates: the page
    /// frame inset by the container's own padding, the insets
    /// lv_obj_get_content_coords() applies. The container's live content area
    /// when no frame callback reports a frame.
    lv_area_t settled_content_area() const;

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

    /// Create the persistent event shield in the scoped container, or move the
    /// existing one there, then redraw its lattice. The object must survive
    /// selection changes and page flips mid-session: the indev glues a gesture
    /// to its press target, and destroying that target mid-gesture ends the
    /// press with no event reaching the grid handlers. Its input events bubble
    /// to the handlers on carousel_host; its only callbacks draw the lattice
    /// and free what that draw reads.
    void ensure_shield();
    /// Redraw the shield for the current selection: the lattice it draws, the
    /// boundaries the selected widget can snap to, and its one child, the
    /// delete-page button. Mid-gesture safe: the button is never the press
    /// target and the shield itself is kept.
    void rebuild_lattice();
    /// Dots in the lattice the shield draws, 0 with none.
    int drawn_dot_count() const;
    std::string selected_widget_id() const;
    void create_selection_chrome(lv_obj_t* widget);
    void destroy_selection_chrome();
    void remove_selected_widget();
    void configure_selected_widget();

    /// Run rebuild_cb_ on the next lv_timer_handler tick (helix::ui::run_next_tick),
    /// so its deletes run outside input dispatch. A request made while a
    /// rebuild is pending joins it: one rebuild restores every page.
    /// @param post_rebuild  Optional work to run after the rebuild completes
    void schedule_deferred_rebuild(std::function<void()> post_rebuild = nullptr);

    /// Set the session's page to the one @p change ends on
    /// (helix::page_set_focus()), and tell the owner through pages_changed_cb_,
    /// on the next tick, that the page set changed.
    void notify_pages_changed(const helix::PageSetChange& change);

    /// Find the config entry index for a given container child widget.
    /// Returns -1 if not found.
    int find_config_index_for_widget(lv_obj_t* widget) const;

    /// Sync config grid positions from actual widget screen coordinates.
    /// Called on enter() to ensure config matches the visual layout.
    void sync_config_from_screen();

    /// Bind edit mode to whatever container_/page_index_ name: disarm the
    /// container's widget clicks, then move the session's objects in (a
    /// carried widget below the event shield, the shield and its lattice, the
    /// selection chrome above it) and, with @p sync_config, sync config
    /// positions. The disarm runs first because it strips CLICKABLE from every
    /// descendant, which would include the shield and the chrome buttons once
    /// they are children. The tail enter() and switch_page() share, so a
    /// long-pressed page and a swiped-to page get identical chrome.
    /// @p sync_config only at enter(): the sync's drift path writes the config
    /// to disk, which must not happen per page change.
    void attach_to_current_page(bool sync_config);

    /// Tell the owner that owns_gesture() or is_catalog_open() may have
    /// changed, through gesture_ownership_cb_. Call it after every change that
    /// arms or ends a gesture (arming a press, starting a drag or resize, a
    /// release, an abort, a page switch that drops the gesture) and after the
    /// widget catalog opens or closes.
    void notify_gesture_ownership();

    /// Which placed widgets a placement check treats as holding their cells.
    enum class Occupants {
        /// Only entries with a laid-out object in the scoped container, so a
        /// drop or a catalog placement lands where the user sees room.
        /// Population creates an object for every entry it places, a
        /// hardware-gated widget included (dimmed), so this leaves out only an
        /// entry whose widget failed to configure or to create.
        OnScreen,
        /// Every placed entry, one with no object included, so a resize never
        /// grows over a cell another entry holds in config.
        AllPlaced,
    };
    /// The scoped page's placed widgets, @p exclude_id left out, on the grid its
    /// container is laid out on: the occupancy every drag, drop, resize and
    /// catalog placement checks a rectangle against.
    GridLayout page_occupancy(const std::string& exclude_id, Occupants occupants) const;

    // Drag helpers
    void handle_drag_start(lv_event_t* e);
    void handle_drag_move(lv_event_t* e);
    /// Put the dragged widget, and its selection outline, at screen point
    /// @p widget_pos on the top layer.
    void place_dragged_widget(lv_point_t widget_pos);
    /// Move the selected widget and its selection outline from the page to the
    /// top layer at the screen position they hold, for a drag.
    void lift_dragged_widget();
    /// Put a lifted widget back into the scoped page, in its grid cell below the
    /// shield, as it was before the lift; with no page to return to, delete it.
    /// Nothing when no widget is lifted. Every end of a drag runs this.
    void settle_dragged_widget();
    /// Resolve the release with helix::resolve_drop(), commit it, prune the
    /// page it emptied, and save once.
    void handle_drag_end(lv_event_t* e);
    /// The live drag's release, gathered for helix::resolve_drop(). Requires a
    /// drag.
    helix::DropInput drop_input() const;
    /// Place the dragged entry, id @p widget_id, as @p drop resolved it, adding
    /// the page past the last one first for a CreatePage. Returns the page the
    /// entry landed on, or -1 when nothing landed. Saves nothing.
    int commit_drop(const std::string& widget_id, const helix::DropResolution& drop);
    /// Select @p widget again once its gesture ended without a rebuild and the
    /// layout put it back in its cell, if it is still a child of the scoped
    /// container.
    void reselect_in_place(lv_obj_t* widget);
    /// Compute the drag's snap target for the dragged widget's top-left at
    /// screen point @p widget_pos, against the scoped page at rest, and draw
    /// its preview. A page still sliding in is measured where it settles, which
    /// is where the pointer will find it.
    void update_drag_snap_target(lv_point_t widget_pos);
    void update_snap_preview(int col, int row, int colspan, int rowspan, bool valid);
    void destroy_snap_preview();

    /// End the drag or resize the gesture holds: drop the dragged widget's
    /// FLOATING flag, destroy both previews, then clear_gesture_state().
    void tear_down_gesture();

    /// Reset every per-gesture value: pending, dragging and resizing, the grab
    /// latch and the inert mark, the cross-page state and both flip timers, the
    /// drag origin and the grab offset. Touches no
    /// LVGL object beyond cancelling the flip timers, so it is safe inside
    /// input dispatch; callers that retire objects do that around it.
    void clear_gesture_state();

    /// Null every pointer to an object living in the scoped container (the
    /// selection, its chrome, the shield and its delete-page button, the snap
    /// preview and its cell) without deleting anything. For the rebuilds that
    /// destroy the container's children: a pointer kept past lv_obj_clean
    /// dangles.
    void forget_container_children();

    /// Schedule the deferred rebuild, then recreate the shield and lattice and
    /// select the rebuilt direct child of the container named @p widget_id
    /// (nothing when it is empty or absent). Skipped when the session ended
    /// before the rebuild ran.
    void rebuild_then_select(std::string widget_id);

    /// After a commit that placed @p changed_ids on the scoped page and changed
    /// nothing else: drop the selection now, then on the next tick ask
    /// relayout_cb_ to re-seat the page in place (giving @p widget_id its new
    /// span when @p resized) and select @p widget_id again. Falls back to
    /// rebuild_then_select() when the relayout is refused or there is no
    /// callback.
    void relayout_then_select(std::string widget_id, std::vector<std::string> changed_ids,
                              bool resized);

    // Resize helpers
    bool is_selected_widget_resizable() const;
    void handle_resize_move(lv_event_t* e);
    void handle_resize_end(lv_event_t* e);
    void update_resize_preview_px(int x, int y, int w, int h, bool valid);
    void commit_resize_with_snap(const ResizeResult& result);

    /// Stop the resize snap animation if one is in flight, and lay the resized
    /// widget out at the cell its resize committed, in place.
    ///
    /// Its completion callback holds a raw `this` and dereferences config_, so
    /// both exit() (which nulls config_) and the destructor must run this. The
    /// animation's deleted_cb frees the heap context and clears
    /// snap_anim_outline_, so this is also the leak-free cancel path. The
    /// in-place layout is a grid cell write that creates and deletes nothing,
    /// so a stop no rebuild follows (switch_page) still shows the committed
    /// span, from inside input dispatch or under a live gesture alike.
    /// @return Whether a widget was laid out at its committed cell, which leaves
    ///         its content sized for the old span until something rebuilds it.
    bool cancel_snap_animation();

    /// Finish a resize snap animation in flight: stop it, retire its preview,
    /// and schedule the relayout its completion would have run, re-selecting
    /// the resized widget. Nothing when no snap is in flight.
    void finish_resize_snap();

    // Widget catalog placement
    /// find_catalog_placement() over the scoped page: its live occupancy, the
    /// origin cell the catalog remembered and the def's snap steps.
    std::optional<CatalogPlacement> find_catalog_placement(const PanelWidgetDef& def) const;

    void place_widget_from_catalog(const std::string& widget_id);

    bool active_ = false;
    lv_obj_t* container_ = nullptr;
    helix::ui::WidgetRef shield_;
    helix::ui::WidgetRef selected_;
    helix::ui::WidgetRef selection_overlay_;
    helix::ui::WidgetRef remove_btn_; // Trash button (container child, not overlay child)
    helix::ui::WidgetRef
        configure_btn_; // Configure button (upper-left, shown if widget supports it)
    PanelWidgetConfig* config_ = nullptr;
    int page_index_ = 0;
    RebuildCallback rebuild_cb_;
    RelayoutCallback relayout_cb_;
    DeletePageCallback delete_page_cb_;
    GestureOwnershipCallback gesture_ownership_cb_;
    ShowPageCallback show_page_cb_;
    NextPageSlotCallback next_page_slot_cb_;
    PagesChangedCallback pages_changed_cb_;
    PageFrameCallback page_frame_cb_;

    /// Edge push, crossing and dwell state of the live drag. The rules are
    /// helix::cross_page_step(); this class applies what each step returns.
    helix::CrossPageState cross_page_;
    /// lv_tick_get() at the previous drag step, so the edge push advances by
    /// the time between reads rather than per read.
    uint32_t drag_step_tick_ = 0;
    /// One-shot flip a majority crossing requested, due
    /// kCrossPageCrossingDelayMs after the crossing, and its direction.
    /// Dwell changes never cancel it.
    lv_timer_t* crossing_flip_timer_ = nullptr;
    int crossing_flip_dir_ = 0;
    /// One-shot flip counting kCrossPageDwellMs while the pointer stays in an
    /// edge zone, toward cross_page_.dwell_dir.
    lv_timer_t* dwell_flip_timer_ = nullptr;

    /// Screen position of the dragged widget's top-left at the last drag step,
    /// after the edge push. A flip that carries the drag computes the landing
    /// page's snap target from it, and the release measures the widget against
    /// the page frame's right border from it.
    lv_point_t drag_widget_pos_ = {0, 0};

    /// A page past the last one is available (see NextPageSlotCallback).
    bool has_next_page_slot() const;
    /// The session is scoped past the config's last page: the owner's
    /// next-page slot.
    bool on_next_page_slot() const;
    /// Ask the owner to carry a live drag back to its origin page, so a drag
    /// ending past the last page having created nothing resolves on a page
    /// that exists.
    void return_drag_to_origin();

    /// Remove @p page if it just emptied (and others remain). Returns true when a
    /// page was removed. Saves nothing and leaves the session's page alone: the
    /// commit that emptied the page saves once and reports the change through
    /// notify_pages_changed().
    bool prune_empty_page(int page);
    /// Schedule the crossing flip toward @p dir, replacing one still pending.
    void start_crossing_flip(int dir);
    /// Restart the dwell toward @p dir, or cancel it when @p dir is 0.
    void restart_dwell_flip(int dir);
    /// Cancel both flip timers. Cancel-safe from the destructor and from
    /// inside lv_timer_handler.
    void stop_page_flip_timers();
    /// Ask the owner to show the page one toward @p dir while a drag is live,
    /// logging which trigger asked.
    void request_page_flip(const char* trigger, int dir);
    static void crossing_flip_cb(lv_timer_t* timer);
    static void dwell_flip_cb(lv_timer_t* timer);
    helix::ui::WidgetRef delete_page_btn_;
    /// What the shield draws as the lattice, owned by the shield.
    GridLatticeSpec* lattice_spec_ = nullptr;
    /// What the drawn lattice was built for: the shield and page holding it,
    /// the grid, the selection's snap steps, the content size and whether the
    /// delete-page button shows. rebuild_lattice() keeps a lattice whose key
    /// still matches.
    using LatticeKey = std::tuple<lv_obj_t*, lv_obj_t*, int, int, int, int, int, int, bool>;
    LatticeKey lattice_key_{};

    // Drag threshold: track press origin, only start real drag after movement
    static constexpr int kDragThresholdPx = 12;
    /// A press on the selected widget is armed, watching for the drag threshold.
    bool press_armed_ = false;
    /// Where the grab's press armed: the arming cycle's point, or the point
    /// of the hold that grabbed. Drag start classifies against it.
    lv_point_t press_origin_ = {0, 0};

    /// Grab latch for the current gesture. A press-and-move grabs only a widget
    /// selected before the press: the latch is decided on the gesture's first
    /// PRESSING from the selection standing then, so a press that selects on
    /// that cycle and keeps moving stays a swipe. A hold grabs whatever it
    /// selects (handle_long_press sets the latch). clear_gesture_state()
    /// resets it.
    bool gesture_can_arm_ = false;
    /// The latch above has been decided for the current gesture, and a
    /// gesture that cannot arm has made its one selection.
    bool gesture_press_seen_ = false;
    /// The current gesture takes no further grid action: its pressing cycles
    /// neither arm nor select, and its holds neither grab nor open the
    /// catalog. Set for the hold that entered edit mode, which has made its
    /// one selection, and for a press that finished a resize snap, whose
    /// relayout can fall back to a rebuild that replaces every object under
    /// the press on the next tick.
    /// clear_gesture_state() resets it.
    bool gesture_inert_ = false;

    // Drag state
    bool dragging_ = false;
    int drag_orig_page_ = -1; // Page the drag started on (cross-page drops move the entry)
    int drag_orig_col_ = -1;
    int drag_orig_row_ = -1;
    int drag_orig_colspan_ = 1;
    int drag_orig_rowspan_ = 1;
    lv_point_t drag_offset_ = {0, 0};
    /// The selected widget is on the top layer for a drag
    /// (lift_dragged_widget()), and the local width and height it had before
    /// the lift pinned its size, restored when it settles.
    bool lifted_ = false;
    bool lifted_had_w_ = false;
    bool lifted_had_h_ = false;
    lv_style_value_t lifted_w_{};
    lv_style_value_t lifted_h_{};
    int snap_preview_col_ = -1;
    int snap_preview_row_ = -1;
    /// The cell, span and validity snap_preview_ is drawn for: {col, row,
    /// colspan, rowspan, valid}. Meaningful only while snap_preview_ exists.
    std::tuple<int, int, int, int, bool> snap_preview_rect_{};

    // Resize state
    bool resizing_ = false;
    ResizeEdge resize_edge_ = ResizeEdge::None;
    /// An edit preview's four edge bars (top, bottom, left, right) on the top
    /// layer (ensure_outline() in grid_edit_mode.cpp says why). All null, or
    /// all live.
    using ResizeOutline = std::array<helix::ui::WeakWidget, 4>;
    /// The pixel-tracking resize outline.
    ResizeOutline resize_outline_{};
    /// The box resize_outline_ is drawn around, in screen coordinates.
    lv_area_t resize_outline_box_{};
    /// The grid-snapped landing preview of a drag or resize.
    ResizeOutline snap_preview_{};

    // Outline the resize snap animation is driving, all null when none is in
    // flight. Bar 0 is the animation's `var`, which is what lets LVGL
    // auto-cancel on its deletion and what cancel_snap_animation() cancels by;
    // deleting any other bar cancels it too.
    ResizeOutline snap_anim_outline_{};
    /// The page the animating resize ran on, where a stopped snap lays the
    /// widget out (cancel_snap_animation). Checked live before use.
    helix::ui::WidgetRef snap_anim_page_;
    /// The widget that snap animation resized, which the rebuild after it
    /// selects again. Empty when no snap is in flight.
    std::string snap_anim_widget_id_;
    /// The cell that resize committed, where a stopped snap lays the widget
    /// out (cancel_snap_animation).
    ResizeResult snap_anim_cell_{};

    // Widget catalog placement: grid cell where the long-press originated
    int catalog_origin_col_ = -1;
    int catalog_origin_row_ = -1;

    // Set while the widget catalog overlay is open to prevent
    // on_deactivate → exit() from killing edit mode state.
    bool catalog_open_ = false;

    /// A deferred rebuild is scheduled and has not run yet.
    bool rebuild_pending_ = false;
    /// A stopped resize snap left a widget at its new cell with content sized
    /// for the old span (see cancel_snap_animation()); exit() rebuilds it.
    bool rebuild_on_exit_ = false;
    /// relayout_then_select() has scheduled its tick and it has not run yet;
    /// exit() rebuilds in its place.
    bool relayout_pending_ = false;
    /// Work to run after the pending rebuild, in request order.
    std::vector<std::function<void()>> rebuild_posts_;

    // Guards the work schedule_deferred_rebuild() and notify_pages_changed() run
    // on the next tick.
    // Destructor expires all tokens so callbacks become no-ops if GridEditMode
    // (and its owning HomePanel) is destroyed during switch_printer teardown.
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix
