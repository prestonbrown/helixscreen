// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_catalog_fit.cpp
 * @brief The catalog says why a widget does not fit, instead of closing onto
 *        a toast.
 *
 * The placement search GridEditMode runs when a catalog row is tapped (origin
 * cell, first free position, then shrinking to the def's minimum) also answers
 * the question the catalog asks per row while it is open: does this widget
 * still fit the page the catalog was opened from? Both callers share
 * find_catalog_placement(), so a row can never claim room the placement would
 * refuse (or hide a widget the placement would shrink and place).
 *
 * The first block drives that search directly, against a hand-built
 * GridLayout. The second block opens the real catalog with a fit predicate
 * and asserts what a refused row renders: dimmed, unclickable, its name
 * carrying the minimum span the search shrinks to. A row the predicate
 * accepts keeps its click handler, and a catalog opened with no predicate
 * marks nothing; that is the shape every other caller of show() sees. The
 * last case drives the real GridEditMode entry instead, so the predicate
 * itself cannot be dropped from the wiring without going red.
 */

#include "ui_breakpoint.h"
#include "ui_nav_manager.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A def sized only by the fields the placement search reads. The registry's
/// own defs are pinned by other suites; what varies here is the span pair.
PanelWidgetDef def_with_spans(int colspan, int rowspan, int min_colspan, int min_rowspan) {
    PanelWidgetDef def{};
    def.id = "fit_test_widget";
    def.display_name = "Fit Test";
    def.colspan = colspan;
    def.rowspan = rowspan;
    def.min_colspan = min_colspan;
    def.min_rowspan = min_rowspan;
    return def;
}

/// Whole-cell widget on a whole-cell step, the common case.
constexpr int CELL = GridLayout::TRACKS_PER_CELL;

// Row-test IDs: no hardware gate, not multi-instance, default_enabled=false
// (so a seeded empty layout leaves them unplaced and offerable). "preheat"'s
// minimum is 2 cells by 1, the brief's own example string.
constexpr const char* kRefusedId = "preheat";
constexpr const char* kOfferedId = "macros";
// Multi-instance base: 24 instances of it tile the full-page test below, where
// 24 copies of a single-instance def would be dropped at parse as unknown ids.
constexpr const char* kTileId = "favorite_macro";

/// An empty one-page layout: nothing placed, so no row is dimmed as Placed.
void seed_empty_layout(const std::string& panel_id) {
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages", {{{"id", "main"}, {"widgets", nlohmann::json::array()}}}}});
}

void collect_label_texts(lv_obj_t* obj, std::vector<std::string>& out) {
    if (!obj) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* txt = lv_label_get_text(obj);
        if (txt) {
            out.emplace_back(txt);
        }
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        collect_label_texts(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

/// Whether any label under @p row contains @p needle. The row's children are
/// icon / text column / badge group, so the name label is found by content,
/// not by position.
bool label_contains(lv_obj_t* row, const std::string& needle) {
    std::vector<std::string> texts;
    collect_label_texts(row, texts);
    for (const auto& t : texts) {
        if (t.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// The search-result rows cover every def, named by def id, so a fit test can
/// address its row without drilling into a category. The catalog builds them on
/// the first query, which this types and clears.
lv_obj_t* result_row(const char* widget_id) {
    lv_obj_t* root = WidgetCatalogOverlay::active_root();
    REQUIRE(root != nullptr);
    lv_obj_t* results = lv_obj_find_by_name(root, "search_results");
    REQUIRE(results != nullptr);
    if (lv_obj_get_child_count(results) == 0) {
        lv_obj_t* input = lv_obj_find_by_name(root, "catalog_search_input");
        REQUIRE(input != nullptr);
        lv_textarea_set_text(input, "zzqq no such widget");
        lv_textarea_set_text(input, "");
    }
    lv_obj_t* row = lv_obj_find_by_name(results, widget_id);
    INFO("no search-result row for '" << widget_id << "'");
    REQUIRE(row != nullptr);
    return row;
}

/// The reason string the catalog renders for @p def, with the minimum spans
/// in the cell unit the size badge uses. Even minimums only: this mirrors
/// with_fit_hint()'s whole-cell rendering without forking its half-cell arm.
std::string expected_fit_reason(const PanelWidgetDef& def) {
    REQUIRE(def.effective_min_colspan() % GridLayout::TRACKS_PER_CELL == 0);
    REQUIRE(def.effective_min_rowspan() % GridLayout::TRACKS_PER_CELL == 0);
    return "Needs " + std::to_string(def.effective_min_colspan() / GridLayout::TRACKS_PER_CELL) +
           "x" + std::to_string(def.effective_min_rowspan() / GridLayout::TRACKS_PER_CELL) +
           " free: remove or shrink a widget first";
}

} // namespace

// ---------------------------------------------------------------------------
// The placement search, driven directly
// ---------------------------------------------------------------------------

TEST_CASE("find_catalog_placement places at the origin cell when it is free",
          "[widget_catalog][fit]") {
    GridLayout grid(UiBreakpoint::Medium, {8, 8});
    const auto def = def_with_spans(4, 4, 4, 4);

    const auto p = find_catalog_placement(grid, def, 2, 2, CELL, CELL);

    REQUIRE(p.has_value());
    CHECK(p->col == 2);
    CHECK(p->row == 2);
    CHECK(p->colspan == 4);
    CHECK(p->rowspan == 4);
}

TEST_CASE("find_catalog_placement falls back to the first free position when the origin is taken",
          "[widget_catalog][fit]") {
    GridLayout grid(UiBreakpoint::Medium, {8, 8});
    REQUIRE(grid.place({"blocker", 0, 0, 4, 8})); // columns 0-3, all rows
    const auto def = def_with_spans(4, 4, 4, 4);

    const auto p = find_catalog_placement(grid, def, 2, 2, CELL, CELL);

    REQUIRE(p.has_value());
    CHECK(p->col == 4);
    CHECK(p->row == 0);
    CHECK(p->colspan == 4);
    CHECK(p->rowspan == 4);
}

TEST_CASE("find_catalog_placement ignores an origin cell that is off the snap step",
          "[widget_catalog][fit]") {
    GridLayout grid(UiBreakpoint::Medium, {8, 8});
    const auto def = def_with_spans(2, 2, 2, 2);

    // (1,1) is not a whole-cell boundary; the search must not place there.
    const auto p = find_catalog_placement(grid, def, 1, 1, CELL, CELL);

    REQUIRE(p.has_value());
    CHECK(p->col == 0);
    CHECK(p->row == 0);
}

TEST_CASE("find_catalog_placement shrinks to a smaller span that fits", "[widget_catalog][fit]") {
    GridLayout grid(UiBreakpoint::Medium, {8, 8});
    // Columns 0-5, all rows: only a two-track-wide strip stays free.
    REQUIRE(grid.place({"blocker", 0, 0, 6, 8}));
    const auto def = def_with_spans(4, 4, 2, 2);

    const auto p = find_catalog_placement(grid, def, 2, 2, CELL, CELL);

    REQUIRE(p.has_value());
    CHECK(p->col == 6);
    CHECK(p->row == 0);
    CHECK(p->colspan == 2);
    CHECK(p->rowspan == 4);
}

TEST_CASE("find_catalog_placement returns nothing on a full grid", "[widget_catalog][fit]") {
    GridLayout grid(UiBreakpoint::Medium, {8, 8});
    REQUIRE(grid.place({"blocker_a", 0, 0, 4, 8}));
    REQUIRE(grid.place({"blocker_b", 4, 0, 4, 8}));
    const auto def = def_with_spans(2, 2, 2, 2);

    CHECK_FALSE(find_catalog_placement(grid, def, 2, 2, CELL, CELL).has_value());
}

// ---------------------------------------------------------------------------
// The catalog's rows, under a fit predicate
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Catalog: a widget the fit predicate refuses is dimmed, unclickable and says why",
                 "[widget_catalog][fit]") {
    const std::string panel_id = "test_catalog_fit_refused";
    seed_empty_layout(panel_id);
    PanelWidgetConfig config(panel_id, *Config::get_instance());
    config.load();

    WidgetCatalogOverlay::show(
        test_screen(), config, nullptr, nullptr,
        [](const PanelWidgetDef& def) { return std::string(def.id) != kRefusedId; });
    process_lvgl(10);

    lv_obj_t* refused = result_row(kRefusedId);
    CHECK_FALSE(lv_obj_is_clickable(refused));
    const auto* def = find_widget_def(kRefusedId);
    REQUIRE(def != nullptr);
    CHECK(label_contains(refused, expected_fit_reason(*def)));

    lv_obj_t* offered = result_row(kOfferedId);
    CHECK(lv_obj_is_clickable(offered));
    CHECK_FALSE(label_contains(offered, "Needs"));

    NavigationManager::instance().go_back();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Catalog: with no fit predicate nothing is marked as not fitting",
                 "[widget_catalog][fit]") {
    const std::string panel_id = "test_catalog_fit_no_predicate";
    seed_empty_layout(panel_id);
    PanelWidgetConfig config(panel_id, *Config::get_instance());
    config.load();

    WidgetCatalogOverlay::show(test_screen(), config, nullptr);
    process_lvgl(10);

    lv_obj_t* row = result_row(kRefusedId);
    CHECK(lv_obj_is_clickable(row));
    CHECK_FALSE(label_contains(row, "Needs"));

    NavigationManager::instance().go_back();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Catalog: a fit predicate from a closed catalog does not mark the next open",
                 "[widget_catalog][fit]") {
    const std::string panel_id = "test_catalog_fit_reopen_clean";
    seed_empty_layout(panel_id);
    PanelWidgetConfig config(panel_id, *Config::get_instance());
    config.load();

    WidgetCatalogOverlay::show(
        test_screen(), config, nullptr, nullptr,
        [](const PanelWidgetDef& def) { return std::string(def.id) != kRefusedId; });
    process_lvgl(10);
    NavigationManager::instance().go_back();
    process_lvgl(10);

    // Closing drops every piece of catalog state, so an open with no predicate
    // starts from a clean slate rather than the previous catalog's verdicts.
    WidgetCatalogOverlay::show(test_screen(), config, nullptr);
    process_lvgl(10);

    lv_obj_t* row = result_row(kRefusedId);
    CHECK(lv_obj_is_clickable(row));
    CHECK_FALSE(label_contains(row, "Needs"));

    NavigationManager::instance().go_back();
    process_lvgl(10);
}

// The row tests above pass their own predicate; this one drives the production
// wiring, so a refactor that drops the fits argument from open_widget_catalog's
// show() call reverts to offering every row and goes red here.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "GridEditMode: the catalog it opens on a full page refuses rows",
                 "[widget_catalog][fit][grid_edit]") {
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    lv_subject_set_int(bp_subj, to_int(UiBreakpoint::Medium));

    const std::string panel_id = "test_catalog_open_fit_refused";
    auto* cfg = Config::get_instance();
    // Twenty-four macro instances at 2x2 tracks tile the whole 12x8 Medium
    // page, leaving no spot even a 1x1-cell minimum fits in.
    nlohmann::json widgets = nlohmann::json::array();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
            widgets.push_back({{"id", std::string(kTileId) + ":" + std::to_string(r * 6 + c + 1)},
                               {"enabled", true},
                               {"col", c * 2},
                               {"row", r * 2},
                               {"colspan", 2},
                               {"rowspan", 2}});
        }
    }
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages", {{{"id", "main"}, {"widgets", std::move(widgets)}}}}});

    PanelWidgetConfig config(panel_id, *cfg);
    config.load();

    // The container mirrors what PanelWidgetManager builds: a Medium 12x8
    // track grid with one named tile per entry, laid out at the entry's cell.
    // page_occupancy() only counts entries whose tile is on screen, so a bare
    // container would read as an empty page and nothing would be refused.
    // 715x475 is the geometry test_grid_edit_snap_anim_lifetime.cpp uses: a
    // 12x8 Medium grid of exact 55px tracks.
    const int gutter = helix::GridLayout::gutter_px();
    REQUIRE(gutter > 0);
    const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, 715, 475);
    REQUIRE(dims.cols == 12);
    REQUIRE(dims.rows == 8);
    std::vector<int32_t> col_dsc = GridLayout::make_col_dsc(dims.cols);
    std::vector<int32_t> row_dsc = GridLayout::make_row_dsc(dims.rows);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_scrollable(container, false);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, 715, 475);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
            lv_obj_t* tile = lv_obj_create(container);
            lv_obj_set_name(tile,
                            (std::string(kTileId) + ":" + std::to_string(r * 6 + c + 1)).c_str());
            lv_obj_set_scrollable(tile, false);
            lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, c * 2, 2, LV_GRID_ALIGN_STRETCH,
                                 r * 2, 2);
        }
    }
    lv_obj_update_layout(container);

    // Premise checks: every entry still holds its cell after enter()'s sync,
    // and every tile is reachable by name the way page_occupancy reads them.
    int placed_entries = 0;
    for (const auto& e : config.page_entries(0)) {
        if (e.is_placed()) {
            ++placed_entries;
        }
    }
    CHECK(placed_entries == 24);

    GridEditMode em;
    em.enter(container, &config, /*page_index=*/0);
    em.open_widget_catalog(test_screen());
    process_lvgl(10);

    lv_obj_t* refused = result_row(kRefusedId);
    CHECK_FALSE(lv_obj_is_clickable(refused));
    const auto* def = find_widget_def(kRefusedId);
    REQUIRE(def != nullptr);
    CHECK(label_contains(refused, expected_fit_reason(*def)));

    NavigationManager::instance().go_back();
    process_lvgl(10);
    em.exit();
    process_lvgl(10);
}
