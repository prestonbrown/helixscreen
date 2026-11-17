// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_half_cell_placement.cpp
 * @brief prestonbrown/helixscreen#1126 — auto-placement must honour the same
 *        half-cell rule that edit mode enforces.
 *
 * The grid lays out half-cell tracks, and `GridEditMode::snap_step_for()` keeps
 * a widget that declares neither half-cell flag on even track boundaries while
 * it is dragged or resized. Auto-placement had no such notion: `find_available`
 * and `find_available_bottom` walked `++c` / `--c`, so the first free run they
 * hit could start on an odd track and a whole-cell-only widget was seated
 * straddling two cells.
 *
 * It is reachable without touching a config file. Resize a widget that DOES
 * declare half-cell support to an odd span — an ordinary edit-mode drag — and
 * the free run it leaves behind is odd-aligned. The next widget added from the
 * catalog lands in it.
 *
 * The rule the two halves now share: a widget may only start on, and span, a
 * multiple of its own per-axis step.
 */

#include "../test_fixtures.h"
#include "../test_helpers/scoped_widget_factory.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix_test::ScopedWidgetFactory;

namespace {

constexpr int kCell = GridLayout::TRACKS_PER_CELL;

// =============================================================================
// GridLayout: the search itself
// =============================================================================

/// A grid whose only free run is odd-aligned: tracks 7 and 8 of a 12-track row,
/// with everything else taken. `whole` is what a whole-cell-only widget asks
/// for; the run is wide enough for it, but starts on the wrong boundary.
GridLayout odd_hole_grid() {
    GridLayout grid(UiBreakpoint::Medium, GridDimensions{12, 2});
    grid.place({"left", 0, 0, 7, 2});  // tracks 0-6
    grid.place({"right", 9, 0, 3, 2}); // tracks 9-11
    return grid;                       // free: tracks 7-8
}

} // namespace

TEST_CASE("GridLayout find_available_bottom: a whole-cell step skips an odd-aligned run",
          "[grid_layout][find][half_cell][1126]") {
    auto grid = odd_hole_grid();

    // Premise: the run IS wide enough — a step-agnostic search takes it, and
    // that is exactly the straddle this test exists to prevent.
    auto unstepped = grid.find_available_bottom(kCell, kCell, /*col_step=*/1, /*row_step=*/1);
    REQUIRE(unstepped.has_value());
    CHECK(unstepped->first == 7);

    // A widget that declares no half-cell support may not start there, and no
    // other run is left, so there is nowhere for it to go.
    auto stepped = grid.find_available_bottom(kCell, kCell, kCell, kCell);
    CHECK_FALSE(stepped.has_value());
}

TEST_CASE("GridLayout find_available: a whole-cell step returns an even origin",
          "[grid_layout][find][half_cell][1126]") {
    // Same odd hole, plus a legal even-aligned run further along, so the search
    // has a real choice to make rather than only a reject-or-nothing one.
    GridLayout grid(UiBreakpoint::Medium, GridDimensions{16, 2});
    grid.place({"left", 0, 0, 7, 2}); // tracks 0-6
    grid.place({"mid", 9, 0, 3, 2});  // tracks 9-11, leaving 7-8 free
                                      // tracks 12-15 free
    auto pos = grid.find_available(kCell, kCell, kCell, kCell);
    REQUIRE(pos.has_value());
    CHECK(pos->first % kCell == 0);
    CHECK(pos->first == 12); // the odd 7-8 run was passed over
}

TEST_CASE("GridLayout find_available_bottom: a half-cell widget still takes the odd run",
          "[grid_layout][find][half_cell][1126]") {
    // The flag has to buy something. A widget that declares half-cell support
    // on an axis gets a step of 1 there and may seat itself at track 7.
    auto grid = odd_hole_grid();
    auto pos = grid.find_available_bottom(kCell, kCell, /*col_step=*/1, /*row_step=*/kCell);
    REQUIRE(pos.has_value());
    CHECK(pos->first == 7);
}

TEST_CASE("GridLayout find_available_bottom_min: reports GridFull, not a straddle",
          "[grid_layout][find][half_cell][1126]") {
    // The manager's entry point. An odd-aligned run is not a placement for a
    // whole-cell widget, so the honest answer is that the grid is full — the
    // caller then disables the widget and says so, rather than seating it half
    // a cell off and leaving the user to notice.
    auto grid = odd_hole_grid();
    auto fit = grid.find_available_bottom_min(kCell, kCell, kCell, kCell);
    CHECK_FALSE(fit.placed());
    CHECK(fit.failure == GridLayout::PlacementFailure::GridFull);
}

TEST_CASE("GridLayout: the search step defaults to a whole cell",
          "[grid_layout][find][half_cell][1126]") {
    // Half-cell placement is opt-in everywhere else in the system, so a caller
    // that has not thought about the step must not get the permissive
    // behaviour by omission.
    auto grid = odd_hole_grid();
    CHECK_FALSE(grid.find_available(kCell, kCell).has_value());
    CHECK_FALSE(grid.find_available_bottom(kCell, kCell).has_value());
}

// =============================================================================
// snap_step_for(): the one place the rule is derived from a definition
// =============================================================================

TEST_CASE("snap_step_for: whole-cell widgets step by a cell, half-cell ones by a track",
          "[grid_edit][half_cell][1126]") {
    // `control_buttons` and `clock` are the fixed points these placement tests
    // are written against; if either changes its flags the scenarios below stop
    // describing what they claim to.
    const auto* controls = find_widget_def("control_buttons");
    const auto* clock = find_widget_def("clock");
    REQUIRE(controls != nullptr);
    REQUIRE(clock != nullptr);
    REQUIRE_FALSE(controls->supports_half_col);
    REQUIRE_FALSE(controls->supports_half_row);
    REQUIRE(clock->supports_half_col);

    CHECK(GridEditMode::snap_step_for("control_buttons") == std::make_pair(kCell, kCell));
    CHECK(GridEditMode::snap_step_for("clock").first == 1);

    // An id with no definition is not a licence to straddle.
    CHECK(GridEditMode::snap_step_for("no_such_widget") == std::make_pair(kCell, kCell));
}

// =============================================================================
// PanelWidgetManager: the path a user actually walks
// =============================================================================

namespace {

/// Stand-in for any registry widget, so these tests exercise placement without
/// dragging real widgets' subjects and hardware gates in behind them.
struct StubWidget : PanelWidget {
    explicit StubWidget(std::string widget_id) : id_(std::move(widget_id)) {}
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return id_.c_str();
    }
    std::string get_component_name() const override {
        return "test_half_cell_stub";
    }

  private:
    std::string id_;
};

/// A registry factory building a StubWidget for the id it is asked for.
WidgetFactory stub_factory() {
    return [](const std::string& id) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<StubWidget>(id);
    };
}

nlohmann::json entry(const char* id, int col, int row, int colspan, int rowspan) {
    return {{"id", id},   {"enabled", true},    {"col", col},
            {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
}

} // namespace

/// Fixture: a landscape panel big enough for the 12-track row the scenario needs.
class HalfCellPlacementFixture : public XMLTestFixture {
  public:
    HalfCellPlacementFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        lv_xml_register_component_from_data(
            "test_half_cell_stub",
            "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    }
};

TEST_CASE_METHOD(HalfCellPlacementFixture,
                 "Auto-place never straddles a cell after a half-cell resize",
                 "[panel_widget][manager][half_cell][regression][1126]") {
    ScopedWidgetFactory tips("tips", stub_factory());                // filler, anchored
    ScopedWidgetFactory clock("clock", stub_factory());              // declares half-cell support
    ScopedWidgetFactory controls("control_buttons", stub_factory()); // none: the widget at risk

    const auto* cb = find_widget_def("control_buttons");
    REQUIRE(cb != nullptr);
    REQUIRE_FALSE(cb->supports_half_col);
    REQUIRE_FALSE(cb->supports_half_row);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_update_layout(container);
    const GridDimensions grid = GridLayout::get_dimensions(
        as_breakpoint(lv_subject_get_int(theme_manager_get_breakpoint_subject())),
        lv_obj_get_content_width(container), lv_obj_get_content_height(container));

    // The scenario needs a row wide enough to hold filler + an odd-span widget
    // + a two-track hole between them, and at least one spare row band.
    REQUIRE(grid.cols >= 12);
    REQUIRE(grid.rows >= 4);
    REQUIRE(grid.cols % kCell == 0);

    const int hole = grid.cols - 7; // odd, since cols is even
    const int clock_col = grid.cols - 3;
    const int run_width = clock_col - hole;
    REQUIRE(hole % kCell != 0);
    // The run between the filler and `clock` is the only seat left, and it must
    // be wide enough for the def this case auto-places: a narrower run is
    // honestly skipped by the placement search, col comes back -1, and every
    // assertion below stops proving anything.
    REQUIRE(run_width >= cb->colspan);

    // Rows 0-1: filler on the left, a legitimately half-cell-resized `clock` on
    // the right (span 3 = 1.5 cells, which edit mode allows it), leaving an
    // odd-aligned four-track run between them. Rows 2+ are filled so the run is
    // the only one left.
    nlohmann::json widgets = nlohmann::json::array();
    widgets.push_back(entry("tips", 0, 0, hole, kCell));
    widgets.push_back(entry("clock", clock_col, 0, 3, kCell));
    widgets.push_back(entry("print_status", 0, kCell, grid.cols, grid.rows - kCell));
    widgets.push_back(entry("control_buttons", -1, -1, cb->colspan, cb->rowspan)); // auto-placed

    const std::string panel_id = "test_half_cell_autoplace";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 1},
                       {"pages", {{{"id", "main"}, {"widgets", std::move(widgets)}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    process_lvgl(10);

    auto built = mgr.populate_widgets(panel_id, container, /*page_index=*/0);

    const auto& entries = mgr.get_widget_config(panel_id).page_entries(0);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const PanelWidgetEntry& e) { return e.id == "control_buttons"; });
    REQUIRE(it != entries.end());

    INFO("control_buttons placed at col " << it->col << " row " << it->row << " (hole at " << hole
                                          << ", grid " << grid.cols << "x" << grid.rows << ")");
    // Either it was seated on a cell boundary, or it was honestly refused. What
    // it must never be is seated at `hole`.
    if (it->enabled && it->col >= 0) {
        CHECK(it->col % kCell == 0);
        CHECK(it->row % kCell == 0);
    }

    // The half-cell widget that created the situation keeps its odd span: the
    // fix constrains who may straddle, it does not withdraw the feature.
    auto clock_it = std::find_if(entries.begin(), entries.end(),
                                 [](const PanelWidgetEntry& e) { return e.id == "clock"; });
    REQUIRE(clock_it != entries.end());
    CHECK(clock_it->colspan == 3);

    mgr.clear_panel_config(panel_id);
}

TEST_CASE_METHOD(HalfCellPlacementFixture,
                 "A saved odd origin is corrected for a whole-cell widget",
                 "[panel_widget][manager][half_cell][regression][1126]") {
    // The other way in: a layout that already holds an odd origin — written by
    // a build where the widget declared half-cell support, or by hand. The load
    // path honoured it verbatim, so the straddle survived every restart.
    ScopedWidgetFactory controls("control_buttons", stub_factory());
    const auto* cb = find_widget_def("control_buttons");
    REQUIRE(cb != nullptr);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_update_layout(container);
    const GridDimensions grid = GridLayout::get_dimensions(
        as_breakpoint(lv_subject_get_int(theme_manager_get_breakpoint_subject())),
        lv_obj_get_content_width(container), lv_obj_get_content_height(container));
    REQUIRE(grid.cols % kCell == 0);

    // An odd origin whose extent still fits the def: the saved layout is
    // in-bounds, so the only thing wrong with it is the straddle.
    const int odd_origin = grid.cols - 5;
    REQUIRE(odd_origin % kCell != 0);
    REQUIRE(odd_origin + cb->colspan <= grid.cols);

    nlohmann::json widgets = nlohmann::json::array();
    widgets.push_back(entry("control_buttons", odd_origin, 0, cb->colspan, cb->rowspan));

    const std::string panel_id = "test_half_cell_saved_origin";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 1},
                       {"pages", {{{"id", "main"}, {"widgets", std::move(widgets)}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    process_lvgl(10);

    auto built = mgr.populate_widgets(panel_id, container, /*page_index=*/0);

    const auto& entries = mgr.get_widget_config(panel_id).page_entries(0);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const PanelWidgetEntry& e) { return e.id == "control_buttons"; });
    REQUIRE(it != entries.end());
    INFO("control_buttons loaded at col " << it->col);
    CHECK(it->enabled);
    CHECK(it->col % kCell == 0);

    mgr.clear_panel_config(panel_id);
}
