// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_drag_path.cpp
 * @brief Drives a real drag through GridEditMode's instance methods.
 *
 * The four public statics (screen_to_grid_cell, round_to_grid_cell,
 * compute_resize_result, clamp_span) are well covered elsewhere, but nothing
 * exercises the geometry as the live drag path actually calls it —
 * handle_drag_move() reads current_metrics() and CellMetrics::gutter directly,
 * and no existing test drives that call. This test seeds a real pointer
 * device, drags a widget through GridEditMode's own event handlers, and
 * asserts the snap target the drag actually lands on, so that corrupting the
 * gutter inside handle_drag_move() is caught (it previously was not: the same
 * mutation passed all 67 pre-existing [grid_edit] tests).
 */

#include "ui_breakpoint.h"

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "theme_manager.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// State consumed by the synthetic indev's read callback. File-scope, not a
/// stack local: LVGL retains the indev (and therefore this callback) for as
/// long as the indev exists, and ScopedTestIndev below is what bounds that
/// lifetime to the test.
struct DragIndevState {
    lv_point_t point{0, 0};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
};

DragIndevState g_drag_indev_state;

void drag_indev_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_drag_indev_state.point;
    data->state = g_drag_indev_state.state;
}

/// Owns a hand-rolled pointer indev for the duration of one test.
///
/// lv_indev_active() (which handle_drag_start()/handle_drag_move() call
/// through lv_indev_get_point()) is non-null ONLY while lv_indev_read() is on
/// the stack (lib/lvgl/src/indev/lv_indev.c:229-296) — there is no public
/// setter — so every call into GridEditMode's handlers has to happen from
/// inside send() below, not from a bare event dispatch.
///
/// Unlike the file-static virtual_indev in ui_test_utils.cpp (deliberately
/// left for the whole binary's lifetime, cleaned up by lv_deinit()), this one
/// is deleted at scope exit: lv_indev_create() also arms a periodic read
/// timer, and an un-deleted indev would keep polling g_drag_indev_state on
/// every later test's process_lvgl()/lv_timer_handler() call for the rest of
/// the binary's run.
class ScopedTestIndev {
  public:
    ScopedTestIndev() {
        indev_ = lv_indev_create();
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev_, drag_indev_read_cb);
    }
    ~ScopedTestIndev() {
        g_drag_indev_state.state = LV_INDEV_STATE_RELEASED;
        lv_indev_delete(indev_);
    }
    ScopedTestIndev(const ScopedTestIndev&) = delete;
    ScopedTestIndev& operator=(const ScopedTestIndev&) = delete;

    /// Set the point/state and drive one read cycle. Dispatches PRESSED and/or
    /// PRESSING (or RELEASED/CLICKED) synchronously, bubbling from whatever the
    /// point hits up to the container — see the LV_OBJ_FLAG_EVENT_BUBBLE note
    /// on dots_overlay_ below for why that reaches our forwarding callback.
    void send(int x, int y, lv_indev_state_t state) {
        g_drag_indev_state.point = {x, y};
        g_drag_indev_state.state = state;
        lv_indev_read(indev_);
    }

  private:
    lv_indev_t* indev_ = nullptr;
};

/// Forwards LV_EVENT_PRESSING to the GridEditMode instance under test — the
/// same wiring HomePanel::on_home_grid_pressing uses in production
/// (src/ui/ui_panel_home.cpp), minus the safe-event-cb exception handling this
/// test doesn't need.
void forward_pressing(lv_event_t* e) {
    auto* em = static_cast<GridEditMode*>(lv_event_get_user_data(e));
    em->handle_pressing(e);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: real drag lands on the gutter-aware snap target",
                 "[grid_edit][grid_edit_drag]") {
    // theme_manager_get_spacing() reads "ui_xml" as a path relative to the
    // process's cwd. Run this test from anywhere but the repo root and the
    // token silently resolves to 0 — gutters vanish and the whole test
    // becomes vacuous (it would still pass, having proven nothing).
    const int gutter = theme_manager_get_spacing("space_xs");
    REQUIRE(gutter > 0);

    // current_metrics() derives cols/rows from the live ui_breakpoint subject,
    // not from the container's own grid descriptor — pin the breakpoint this
    // fixture's fixed 800x480 display resolves to (see
    // test_panel_widget_manager_cell_px.cpp for the same assumption) so the
    // hand computation below and the container's own descriptor agree with
    // what GridEditMode will actually read.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Medium);
    const int ncols = GridLayout::get_cols(UiBreakpoint::Medium);
    const int nrows = GridLayout::get_rows(UiBreakpoint::Medium);
    REQUIRE(ncols > 0);
    REQUIRE(nrows > 0);

    // Container sized so every track is an exact 30px cell (no LVGL remainder
    // distribution to muddy the arithmetic): content = cols*cell + (cols-1)*gutter.
    constexpr int CELL_PX = 30;
    const int content_w = ncols * CELL_PX + (ncols - 1) * gutter;
    const int content_h = nrows * CELL_PX + (nrows - 1) * gutter;

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, content_w, content_h);

    // Real grid descriptor + pad_column/pad_row = gutter, matching what
    // PanelWidgetManager installs on the live home-panel container
    // (src/ui/panel_widget_manager.cpp) — this is what current_metrics()
    // needs a REAL grid for: not its own cols/rows count (it does not read
    // the descriptor for that), but so LVGL's own grid engine positions our
    // child widget for real, which is what the press points below are read
    // from.
    auto col_dsc = GridLayout::make_col_dsc(UiBreakpoint::Medium);
    auto row_dsc = GridLayout::make_row_dsc(UiBreakpoint::Medium);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    // Dragged widget: 2x2 cells at the origin. Big enough (2*30+gutter ~= 65px
    // per side) that its center sits comfortably outside the resize-edge grab
    // band (edge_hit_band() in grid_edit_mode.cpp, which derives it from the
    // cell size and floors it at 14px) — this test wants a plain move, not a
    // resize.
    constexpr int COLSPAN = 2;
    constexpr int ROWSPAN = 2;
    lv_obj_t* widget = lv_obj_create(container);
    lv_obj_set_name(widget, "temperature");
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, COLSPAN, LV_GRID_ALIGN_STRETCH, 0,
                         ROWSPAN);
    lv_obj_update_layout(container);

    // Config: put the widget on a SECOND page. PanelWidgetConfig::load()
    // appends registry-default widgets onto page 0 only (parse_widget_array's
    // append_registry_defaults, gated on `pages_.empty()` in
    // src/system/panel_widget_config.cpp) — a second page is the same trick
    // test_panel_widget_manager_cell_px.cpp uses to keep the page's widget
    // list exactly what this test wrote.
    const std::string panel_id = "test_grid_edit_drag_path";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "temperature"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", COLSPAN},
                             {"rowspan", ROWSPAN}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);
    constexpr int PAGE_INDEX = 1; // "spy" page above

    // Sanity check on the config wiring itself, not the drag: "temperature" is
    // a real registered widget ID (panel_widget_registry.cpp) — an ID
    // parse_widget_array() doesn't recognize is silently dropped
    // (find_widget_def() == nullptr), which would make the drag below fail
    // with "widget not in config" rather than a snap-target mismatch.
    const auto& spy_entries = config.page_entries(static_cast<size_t>(PAGE_INDEX));
    REQUIRE(spy_entries.size() == 1);
    REQUIRE(spy_entries[0].id == "temperature");

    GridEditMode em;
    em.enter(container, &config, PAGE_INDEX);
    em.select_widget(widget);
    REQUIRE(em.selected_widget() == widget);

    lv_obj_add_event_cb(container, forward_pressing, LV_EVENT_PRESSING, &em);

    // --- Hand-computed expected snap target -----------------------------
    //
    // cell = (content - (n-1)*gutter) / n; pitch = cell + gutter. This is the
    // same formula grid_cell_metrics()/round_to_grid_cell() implement, but
    // re-derived here rather than called — calling the production helper to
    // build the expectation could not tell a correct helper from a broken
    // one.
    //
    // Column target = 3, landing point picked so the CORRECT pitch rounds it
    // down to 3 with real margin from the 3/4 cell boundary, while the
    // GUTTER-BLIND pitch (m.gutter treated as 0, content/cols instead of
    // (content-(n-1)*gutter)/n + gutter) rounds the SAME pixel up to 4 —
    // exactly the mutation Step 6 introduces. Both 3 and 4 are <= the max
    // valid target_col (ncols - colspan), so neither result gets clamped back
    // to the other — the mutation is genuinely observable, not masked.
    const float pitch_correct_col =
        static_cast<float>(content_w + gutter) / static_cast<float>(ncols);
    const float pitch_buggy_col = static_cast<float>(content_w) / static_cast<float>(ncols);
    constexpr int EXPECTED_COL = 3;
    const int target_px_x = static_cast<int>(
        std::lround((EXPECTED_COL + 0.5f) * (pitch_correct_col + pitch_buggy_col) / 2.0f));

    // Row target = 1, landing exactly on the correct track origin. The row
    // axis isn't the boundary-straddling case above (that needs only one
    // axis to prove the point) but it still exercises real gutter-aware
    // pixel math, and a bug that only broke rows would still fail it.
    constexpr int EXPECTED_ROW = 1;
    const int target_px_y = EXPECTED_ROW * (CELL_PX + gutter);

    lv_area_t content_area;
    lv_obj_get_content_coords(container, &content_area);
    const int target_x = content_area.x1 + target_px_x;
    const int target_y = content_area.y1 + target_px_y;

    // --- Drive the drag ---------------------------------------------------
    ScopedTestIndev indev;

    lv_area_t sel_area;
    lv_obj_get_coords(widget, &sel_area);
    const lv_point_t center{(sel_area.x1 + sel_area.x2) / 2, (sel_area.y1 + sel_area.y2) / 2};

    // Frame 1: press at the widget's center. handle_pressing() selects the
    // drag-pending path (selected_ is already set) and records press_origin_.
    indev.send(center.x, center.y, LV_INDEV_STATE_PRESSED);
    REQUIRE_FALSE(em.is_catalog_open());

    // Frame 2: move to the widget's exact top-left corner (still >12px from
    // press_origin_, so this crosses DRAG_THRESHOLD_PX and starts the real
    // drag) — chosen so drag_offset_ becomes (0,0), which makes frame 3's
    // point equal the widget's new top-left directly, with no extra offset
    // arithmetic to carry through the hand computation above.
    // detect_resize_edge() in handle_drag_start() is checked against
    // press_origin_ (the center, from frame 1), not this point, so landing
    // exactly on the corner here does not trigger resize mode.
    indev.send(sel_area.x1, sel_area.y1, LV_INDEV_STATE_PRESSED);

    // Frame 3: move to the computed target. dragging_ is now true, so
    // handle_pressing() calls handle_drag_move() directly.
    indev.send(target_x, target_y, LV_INDEV_STATE_PRESSED);

    const int snap_col = GridEditModeTestAccess::snap_col(em);
    const int snap_row = GridEditModeTestAccess::snap_row(em);

    // A drag that never reached handle_drag_move() also reports -1 (both
    // snap_preview_col_/row_ start there) — assert >= 0 explicitly so that
    // failure mode cannot be mistaken for a passing target of -1.
    REQUIRE(snap_col >= 0);
    REQUIRE(snap_row >= 0);
    CHECK(snap_col == EXPECTED_COL);
    CHECK(snap_row == EXPECTED_ROW);

    em.exit();
    mgr.clear_panel_config(panel_id);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: snap target reflects the container's actual row count, "
                 "not the breakpoint's",
                 "[grid_edit][grid_edit_drag]") {
    // PanelWidgetManager sizes the row axis from rows actually IN USE
    // (max_row_used, floored by a cached count), not from the breakpoint
    // table (panel_widget_manager.cpp:585-608). A page holding one widget at
    // row 0 gets a container built with a single row track even though the
    // Medium breakpoint's table says 4. current_metrics() must read that
    // single-row descriptor back off the container rather than asking
    // GridLayout for the breakpoint's row count, or the snap target it
    // computes describes a lattice the live grid does not have.
    const int gutter = theme_manager_get_spacing("space_xs");
    REQUIRE(gutter > 0);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Medium);
    const int ncols = GridLayout::get_cols(UiBreakpoint::Medium);
    const int breakpoint_rows = GridLayout::get_rows(UiBreakpoint::Medium);
    REQUIRE(ncols > 0);
    // The whole point of this test: the container we are about to build has
    // fewer rows than the breakpoint table, so a fix that still reads
    // GridLayout::get_rows() would not diverge from this test's expectation.
    REQUIRE(breakpoint_rows > 1);

    // Widget spans 2 columns so its width (2*cell+gutter) clears
    // 2*EDGE_HIT_INWARD (36px) — otherwise the center-press in frame 1 would
    // land inside the resize-edge zone on every side at once and
    // handle_drag_start() would enter resize mode instead of a plain drag.
    // Height uses the same generous cell size for the same reason: the
    // container's single row is this height, so it must clear the edge zone
    // on its own with no gutter to help.
    constexpr int CELL_PX = 80;
    constexpr int COLSPAN = 2;
    constexpr int ROWSPAN = 1;
    const int content_w = ncols * CELL_PX + (ncols - 1) * gutter;
    // Exactly one row's worth of content — no interior gutter since there is
    // only one track.
    const int content_h = CELL_PX;

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, content_w, content_h);

    // Column descriptor matches the breakpoint (unaffected by this bug — only
    // the row axis diverges, per PanelWidgetManager's std::max(max_row_used,
    // cached_rows) — cols always come from GridLayout::get_cols()). Row
    // descriptor is deliberately a single track, standing in for a page whose
    // widgets only occupy row 0.
    auto col_dsc = GridLayout::make_col_dsc(UiBreakpoint::Medium);
    std::vector<int32_t> row_dsc = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    lv_obj_t* widget = lv_obj_create(container);
    lv_obj_set_name(widget, "temperature");
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, COLSPAN, LV_GRID_ALIGN_STRETCH, 0,
                         ROWSPAN);
    lv_obj_update_layout(container);

    const std::string panel_id = "test_grid_edit_drag_path_row_divergence";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "temperature"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", COLSPAN},
                             {"rowspan", ROWSPAN}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);
    constexpr int PAGE_INDEX = 1;

    const auto& spy_entries = config.page_entries(static_cast<size_t>(PAGE_INDEX));
    REQUIRE(spy_entries.size() == 1);
    REQUIRE(spy_entries[0].id == "temperature");

    GridEditMode em;
    em.enter(container, &config, PAGE_INDEX);
    em.select_widget(widget);
    REQUIRE(em.selected_widget() == widget);

    lv_obj_add_event_cb(container, forward_pressing, LV_EVENT_PRESSING, &em);

    lv_area_t content_area;
    lv_obj_get_content_coords(container, &content_area);

    ScopedTestIndev indev;

    lv_area_t sel_area;
    lv_obj_get_coords(widget, &sel_area);
    const lv_point_t center{(sel_area.x1 + sel_area.x2) / 2, (sel_area.y1 + sel_area.y2) / 2};

    // Frame 1: press at the widget's center.
    indev.send(center.x, center.y, LV_INDEV_STATE_PRESSED);
    REQUIRE_FALSE(em.is_catalog_open());

    // Frame 2: move to the widget's top-left corner — crosses DRAG_THRESHOLD_PX
    // and starts the real drag, drag_offset_ becomes (0,0).
    indev.send(sel_area.x1, sel_area.y1, LV_INDEV_STATE_PRESSED);

    // Frame 3: drag far below the container's single row — hundreds of px past
    // its bottom edge. round_to_grid_cell() clamps to [0, ncells] before the
    // final std::min(target_row, nrows - rowspan) clamp, so this lands
    // squarely at whatever the LAST valid row index is. With the container's
    // real row count (1), that final clamp is min(_, 1 - 1) == 0: the snap
    // target can only ever be row 0. Reading the breakpoint's row count (4)
    // instead would clamp to min(_, 4 - 1) == 3 — a row this grid does not
    // have.
    const int target_x = sel_area.x1;
    const int target_y = content_area.y1 + content_h + 500;
    indev.send(target_x, target_y, LV_INDEV_STATE_PRESSED);

    const int snap_row = GridEditModeTestAccess::snap_row(em);

    // A drag that never reached handle_drag_move() also reports -1.
    REQUIRE(snap_row >= 0);
    CHECK(snap_row == 0);

    em.exit();
    mgr.clear_panel_config(panel_id);
}

// ===========================================================================
// handle_drag_start's ownership guard is anchored at the press origin (#1169)
// ===========================================================================
//
// A unit test of press_owns_widget() proves the predicate but cannot prove
// which point handle_drag_start() feeds it — verified empirically: reverting
// the call site to the live pointer left the whole [1169] pure-function set
// green. Only a gesture driven through the real indev reaches that call site,
// so these two tests live here, next to the harness that can do it.

namespace {

/// Shared setup for the two guard tests below: a container with a real grid
/// descriptor and one 2x2 "temperature" widget on a private page.
///
/// Mirrors the first test's setup because that is this file's convention
/// (each test owns its geometry — the second test deliberately builds a
/// single-row container), but the parts that actually matter here are pulled
/// out as named fields so both guard tests share one copy rather than two.
struct GuardFixture {
    lv_obj_t* container = nullptr;
    lv_obj_t* widget = nullptr;
    PanelWidgetConfig* config = nullptr;
    std::string panel_id;
    int page_index = 1;
    // lv_obj_set_grid_dsc_array() stores the RAW pointers — LVGL does not copy
    // the arrays — so they must outlive the container exactly like
    // PanelWidgetManager's grid_descriptors_ member does in production. As
    // locals in make_guard_fixture() they died at return while the container
    // kept using them (nightly ASAN: heap-use-after-free in
    // grid_count_tracks via GridEditMode::current_metrics).
    std::vector<int32_t> col_dsc;
    std::vector<int32_t> row_dsc;
};

GuardFixture make_guard_fixture(lv_obj_t* parent, const std::string& panel_id) {
    const int gutter = theme_manager_get_spacing("space_xs");
    REQUIRE(gutter > 0); // see the cwd note on the first test

    const int ncols = GridLayout::get_cols(UiBreakpoint::Medium);
    const int nrows = GridLayout::get_rows(UiBreakpoint::Medium);
    REQUIRE(ncols >= 2);
    REQUIRE(nrows >= 2);

    // 80px cells: the 2x2 widget is then ~160px per side, so its vertical
    // centre sits ~80px from the top and bottom edges — far outside the grab
    // band, which keeps the right-edge press below unambiguous rather than a
    // corner that detect_resize_edge() could resolve to Top or Bottom.
    constexpr int CELL_PX = 80;
    const int content_w = ncols * CELL_PX + (ncols - 1) * gutter;
    const int content_h = nrows * CELL_PX + (nrows - 1) * gutter;

    GuardFixture f;
    f.panel_id = panel_id;

    f.container = lv_obj_create(parent);
    lv_obj_remove_flag(f.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(f.container, 0, 0);
    lv_obj_set_style_border_width(f.container, 0, 0);
    lv_obj_set_size(f.container, content_w, content_h);

    f.col_dsc = GridLayout::make_col_dsc(UiBreakpoint::Medium);
    f.row_dsc = GridLayout::make_row_dsc(UiBreakpoint::Medium);
    lv_obj_set_grid_dsc_array(f.container, f.col_dsc.data(), f.row_dsc.data());
    lv_obj_set_style_pad_column(f.container, gutter, 0);
    lv_obj_set_style_pad_row(f.container, gutter, 0);

    constexpr int COLSPAN = 2;
    constexpr int ROWSPAN = 2;
    f.widget = lv_obj_create(f.container);
    lv_obj_set_name(f.widget, "temperature");
    lv_obj_remove_flag(f.widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(f.widget, LV_GRID_ALIGN_STRETCH, 0, COLSPAN, LV_GRID_ALIGN_STRETCH, 0,
                         ROWSPAN);
    lv_obj_update_layout(f.container);

    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "temperature"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", COLSPAN},
                             {"rowspan", ROWSPAN}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    f.config = &mgr.get_widget_config(panel_id);

    const auto& entries = f.config->page_entries(static_cast<size_t>(f.page_index));
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].id == "temperature");
    return f;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a grow-resize whose pointer leaves the widget still resizes",
                 "[grid_edit][grid_edit_drag][resize][1169]") {
    // Growing a widget drags AWAY from it: the finger starts on the edge and
    // travels outward. By the time DRAG_THRESHOLD_PX is crossed the live
    // pointer is legitimately off-widget, so a guard that tests the live
    // pointer drops exactly the gestures that should have become resizes —
    // shrinking (which drags inward) worked, growing never did.
    GuardFixture f = make_guard_fixture(test_screen(), "test_grid_edit_guard_grow");

    GridEditMode em;
    em.enter(f.container, f.config, f.page_index);
    em.select_widget(f.widget);
    REQUIRE(em.selected_widget() == f.widget);

    lv_obj_add_event_cb(f.container, forward_pressing, LV_EVENT_PRESSING, &em);

    ScopedTestIndev indev;
    lv_area_t sel_area;
    lv_obj_get_coords(f.widget, &sel_area);

    // Press 5px inside the right edge, vertically centred.
    const lv_point_t origin{sel_area.x2 - 5, (sel_area.y1 + sel_area.y2) / 2};

    // Preconditions, asserted so a geometry or registry change fails loudly
    // instead of making the assertion below vacuous.
    //  - the origin must classify as Right, not a corner;
    //  - "temperature" must still be scalable, or handle_drag_start() takes
    //    the move path and resizing_ stays false for an unrelated reason.
    REQUIRE(em.detect_resize_edge(origin.x, origin.y, sel_area) == GridEditMode::ResizeEdge::Right);

    // Frame 1: finger lands. handle_pressing() records press_origin_ here.
    indev.send(origin.x, origin.y, LV_INDEV_STATE_PRESSED);

    // Frame 2: drag outward to grow — 40px past the right edge, well beyond
    // the grab band, and >12px of travel so DRAG_THRESHOLD_PX is crossed and
    // handle_drag_start() runs on this frame.
    const int far_x = sel_area.x2 + 40;
    REQUIRE(far_x - origin.x > 12);
    indev.send(far_x, origin.y, LV_INDEV_STATE_PRESSED);

    // resizing_ + resize_edge_ are the direct witness: together they prove the
    // gesture was admitted by the guard AND classified as the right-hand edge.
    // resize_preview_ alone would only prove the branch ran; dragging_ tells a
    // gesture dropped at the guard apart from one that fell through to a move.
    CHECK(GridEditModeTestAccess::resizing(em));
    CHECK(GridEditModeTestAccess::resize_edge(em) == GridEditMode::ResizeEdge::Right);
    CHECK_FALSE(GridEditModeTestAccess::dragging(em));
    CHECK(GridEditModeTestAccess::resize_preview(em) != nullptr);

    indev.send(far_x, origin.y, LV_INDEV_STATE_RELEASED);
    em.exit();
    PanelWidgetManager::instance().clear_panel_config(f.panel_id);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a press in the widget interior drags rather than resizes",
                 "[grid_edit][grid_edit_drag][resize][1169]") {
    // The counterpart to the test above: the guard is anchored at the press
    // origin, not disabled. An origin nowhere near an edge must still produce a
    // MOVE even though the pointer ends up in the same off-widget place, so a
    // change that made every gesture a resize — or one that let the grab band
    // grow until it swallowed the widget interior (the clamp in
    // edge_hit_band_for_cell) — goes red here.
    GuardFixture f = make_guard_fixture(test_screen(), "test_grid_edit_guard_interior");

    GridEditMode em;
    em.enter(f.container, f.config, f.page_index);
    em.select_widget(f.widget);
    REQUIRE(em.selected_widget() == f.widget);

    lv_obj_add_event_cb(f.container, forward_pressing, LV_EVENT_PRESSING, &em);

    ScopedTestIndev indev;
    lv_area_t sel_area;
    lv_obj_get_coords(f.widget, &sel_area);

    const lv_point_t centre{(sel_area.x1 + sel_area.x2) / 2, (sel_area.y1 + sel_area.y2) / 2};
    REQUIRE(em.detect_resize_edge(centre.x, centre.y, sel_area) == GridEditMode::ResizeEdge::None);

    indev.send(centre.x, centre.y, LV_INDEV_STATE_PRESSED);

    // Same destination as the grow test — only the origin differs.
    const int far_x = sel_area.x2 + 40;
    REQUIRE(far_x - centre.x > 12);
    indev.send(far_x, centre.y, LV_INDEV_STATE_PRESSED);

    CHECK_FALSE(GridEditModeTestAccess::resizing(em));
    CHECK(GridEditModeTestAccess::resize_edge(em) == GridEditMode::ResizeEdge::None);
    CHECK(GridEditModeTestAccess::dragging(em)); // admitted, as a move

    indev.send(far_x, centre.y, LV_INDEV_STATE_RELEASED);
    em.exit();
    PanelWidgetManager::instance().clear_panel_config(f.panel_id);
}
