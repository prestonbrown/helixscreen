// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_breakpoint.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/scoped_whole_cell_def.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// The grid the placement and snapping tests below are written against. Grid
// size is a property of the container being subdivided rather than of the
// breakpoint, so these state it outright instead of installing a panel
// geometry and inferring it. 6x4 is the shape their coordinates assume.
constexpr helix::GridDimensions kGrid6x4{6, 4};

using namespace helix;

TEST_CASE("PanelWidgetEntry::disable_and_unplace surrenders the cell with the flag",
          "[grid_edit][panel_widget_config]") {
    // save() persists col/row for every entry regardless of `enabled`, so
    // switching a widget off without clearing its coordinates leaves a claim on
    // a cell nothing draws in, which the anchored pass could hand to another
    // tile (#1414).
    PanelWidgetEntry e;
    e.id = "print_status";
    e.enabled = true;
    e.col = 4;
    e.row = 1;
    e.colspan = 3;
    e.rowspan = 3;
    REQUIRE(e.has_grid_position());

    e.disable_and_unplace();

    CHECK_FALSE(e.enabled);
    CHECK(e.col == -1);
    CHECK(e.row == -1);
    // has_grid_position() is what the anchored pass gates on, so this is the
    // property that actually matters: the entry must no longer claim a cell.
    CHECK_FALSE(e.has_grid_position());
    // Spans are the widget's authored size, not a claim on the grid - an entry
    // re-enabled later should come back at its own size, so they survive.
    CHECK(e.colspan == 3);
    CHECK(e.rowspan == 3);
}

TEST_CASE("GridEditMode: enter, exit and selection are tracked and guarded",
          "[grid_edit][edit_mode][selection][catalog]") {
    GridEditMode em;
    REQUIRE_FALSE(em.is_active());
    REQUIRE_FALSE(em.is_catalog_open());

    // lv_obj_t is opaque, so any address stands in for a widget here.
    int dummy = 0;
    auto* fake = reinterpret_cast<lv_obj_t*>(&dummy);

    // Inactive: exit and select do nothing.
    em.exit();
    REQUIRE_FALSE(em.is_active());
    em.select_widget(fake);
    REQUIRE(em.selected_widget() == nullptr);

    // A null container and config are enough for the state machine.
    em.enter(nullptr, nullptr);
    REQUIRE(em.is_active());
    em.enter(nullptr, nullptr); // a second enter is ignored
    REQUIRE(em.is_active());
    REQUIRE_FALSE(em.is_catalog_open());

    REQUIRE(em.selected_widget() == nullptr);
    em.select_widget(fake);
    REQUIRE(em.selected_widget() == fake);
    em.select_widget(fake); // the same widget again changes nothing
    REQUIRE(em.selected_widget() == fake);
    em.select_widget(nullptr);
    REQUIRE(em.selected_widget() == nullptr);

    // Exit clears the selection and leaves the catalog flag alone.
    em.select_widget(fake);
    em.exit();
    REQUIRE_FALSE(em.is_active());
    REQUIRE(em.selected_widget() == nullptr);
    REQUIRE_FALSE(em.is_catalog_open());
}

TEST_CASE("GridEditMode: screen_to_grid_cell maps a point to its cell, clamping to the grid",
          "[grid_edit][drag][grid_metrics]") {
    struct Row {
        const char* what;
        int x, y;
        int ox, oy, w, h, cols, rows, gutter;
        int col, row;
    };
    const Row rows[] = {
        // 6x4 cells of 100px in a 600x400 container at (100, 0).
        {"inside the first cell", 150, 50, 100, 0, 600, 400, 6, 4, 0, 0, 0},
        {"inside the last cell", 690, 390, 100, 0, 600, 400, 6, 4, 0, 5, 3},
        {"widget centre (3,2)", 450, 300, 100, 50, 600, 400, 6, 4, 0, 3, 2},
        {"widget centre (5,1)", 650, 200, 100, 50, 600, 400, 6, 4, 0, 5, 1},
        {"before the origin clamps to (0,0)", 50, 10, 100, 20, 600, 400, 6, 4, 0, 0, 0},
        {"past the extent clamps to the last cell", 800, 500, 100, 20, 600, 400, 6, 4, 0, 5, 3},
        // A drag targets by the dragged widget's centre: a 2x2 at (200,100) and a
        // 1x1 at the same corner.
        {"2x2 widget centre", 300, 200, 0, 0, 600, 400, 6, 4, 0, 3, 2},
        {"1x1 widget centre", 250, 150, 0, 0, 600, 400, 6, 4, 0, 2, 1},
        // Cell edges: a left edge belongs to its own cell.
        {"exactly on a left edge", 100, 50, 0, 0, 600, 400, 6, 4, 0, 1, 0},
        {"one pixel before it", 99, 50, 0, 0, 600, 400, 6, 4, 0, 0, 0},
        {"the container's last pixel", 599, 50, 0, 0, 600, 400, 6, 4, 0, 5, 0},
        // 6 columns in 480px with 4px gutters: track 76.67px, pitch 80.67px, so
        // track 3 starts at 242 where the gutter-blind pitch of 80 would put it
        // at 240.
        {"in track 2's gutter", 241, 10, 0, 0, 480, 272, 6, 4, 4, 2, 0},
        {"past it into track 3", 243, 10, 0, 0, 480, 272, 6, 4, 4, 3, 0},
        {"no gutter: pitch 80", 240, 10, 0, 0, 480, 272, 6, 4, 0, 3, 0},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const auto [col, r] = GridEditMode::screen_to_grid_cell(
            row.x, row.y, row.ox, row.oy, row.w, row.h, row.cols, row.rows, row.gutter);
        CHECK(col == row.col);
        CHECK(r == row.row);
    }

    // The centre of every cell of a 4x3 grid maps to that cell.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            CAPTURE(c, r);
            const auto cell = GridEditMode::screen_to_grid_cell(c * 100 + 50, r * 100 + 50, 0, 0,
                                                                400, 300, 4, 3, 0);
            CHECK(cell.first == c);
            CHECK(cell.second == r);
        }
    }
}

TEST_CASE("clamp_span: a span is held to the widget's registry range per axis",
          "[grid_edit][resize][sizing]") {
    // "humidity" held to min 2x2, max 4x4 tracks makes the ceiling reachable; a
    // widget whose maximum is the whole grid would never hit it.
    ScopedWholeCellDef whole_cell("humidity");
    const auto* humidity = find_widget_def("humidity");
    const auto* printer_image = find_widget_def("printer_image");
    const auto* preheat = find_widget_def("preheat");
    const auto* tips = find_widget_def("tips");
    REQUIRE(humidity != nullptr);
    REQUIRE(printer_image != nullptr);
    REQUIRE(preheat != nullptr);
    REQUIRE(tips != nullptr);
    // The premises each row leans on.
    REQUIRE(humidity->effective_max_colspan() == 4);
    REQUIRE(humidity->effective_max_rowspan() == 4);
    REQUIRE(printer_image->is_scalable());
    // preheat scales its width but not its height.
    REQUIRE(preheat->effective_min_rowspan() == preheat->effective_max_rowspan());
    REQUIRE(preheat->effective_max_colspan() > preheat->effective_min_colspan());
    // tips is a band widget: its width is capped only by the widest grid built.
    REQUIRE(tips->effective_max_colspan() == GridLayout::kMaxTracks);

    struct Row {
        const char* what;
        const char* id;
        int in_c, in_r;
        int out_c, out_r;
    };
    const Row rows[] = {
        {"humidity under the minimum", "humidity", 0, 0, 2, 2},
        {"humidity over the maximum", "humidity", 9, 9, 4, 4},
        {"humidity inside the range", "humidity", 3, 3, 3, 3},
        {"humidity at the maximum", "humidity", 4, 4, 4, 4},
        {"printer_image over the maximum", "printer_image", 9, 7,
         printer_image->effective_max_colspan(), printer_image->effective_max_rowspan()},
        {"printer_image under the minimum", "printer_image", 0, 0,
         printer_image->effective_min_colspan(), printer_image->effective_min_rowspan()},
        {"printer_image inside the range", "printer_image", 2, 2, 2, 2},
        {"preheat's fixed row holds while the width scales", "preheat",
         preheat->effective_max_colspan(), 8, preheat->effective_max_colspan(), preheat->rowspan},
        {"tips under both minimums", "tips", 1, 1, 4, 2},
        {"tips width passes through, height clamps", "tips", 12, 6, 12, 4},
        {"tips past the widest grid", "tips", GridLayout::kMaxTracks + 8, 6, GridLayout::kMaxTracks,
         4},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const auto [c, r] = GridEditMode::clamp_span(row.id, row.in_c, row.in_r);
        CHECK(c == row.out_c);
        CHECK(r == row.out_r);
    }
    // preheat's row stays fixed from either side.
    CHECK(GridEditMode::clamp_span("preheat", 6, 6).second == preheat->rowspan);
    CHECK(GridEditMode::clamp_span("preheat", 6, 1).second == preheat->rowspan);

    // An id with no registry entry still gets at least one track.
    const auto [uc, ur] = GridEditMode::clamp_span("nonexistent_widget_xyz", 0, 0);
    CHECK(uc >= 1);
    CHECK(ur >= 1);
}

TEST_CASE("PanelWidgetDef: effective min/max fall back to the default span",
          "[grid_edit][sizing]") {
    struct Row {
        const char* what;
        int colspan, rowspan, min_c, min_r, max_c, max_r;
        int eff_min_c, eff_min_r, eff_max_c, eff_max_r;
        bool scalable;
    };
    const Row rows[] = {
        {"explicit min and max", 2, 2, 1, 1, 4, 3, 1, 1, 4, 3, true},
        {"zero min/max are the default span", 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, false},
        {"scalable on one axis only", 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, true},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        PanelWidgetDef def{};
        def.colspan = row.colspan;
        def.rowspan = row.rowspan;
        def.min_colspan = row.min_c;
        def.min_rowspan = row.min_r;
        def.max_colspan = row.max_c;
        def.max_rowspan = row.max_r;
        CHECK(def.effective_min_colspan() == row.eff_min_c);
        CHECK(def.effective_min_rowspan() == row.eff_min_r);
        CHECK(def.effective_max_colspan() == row.eff_max_c);
        CHECK(def.effective_max_rowspan() == row.eff_max_r);
        CHECK(def.is_scalable() == row.scalable);
    }
}

TEST_CASE("All registered widgets have valid sizing constraints", "[grid_edit][sizing]") {
    const auto& defs = get_all_widget_defs();
    REQUIRE(defs.size() > 0);

    for (const auto& def : defs) {
        INFO("Widget: " << def.id);
        // Default span must be within min/max range
        CHECK(def.effective_min_colspan() <= def.colspan);
        CHECK(def.colspan <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.rowspan);
        CHECK(def.rowspan <= def.effective_max_rowspan());
        // Min must not exceed max
        CHECK(def.effective_min_colspan() <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.effective_max_rowspan());
    }
}

TEST_CASE_METHOD(XMLTestFixture, "build_default_grid only sets positions for anchor widgets",
                 "[grid]") {
    // build_default_grid() picks its anchor table from this subject, not from
    // anything passed in, so the expectations below are only meaningful at a
    // known breakpoint. Pin it rather than read it: an earlier test in the same
    // binary may have left it elsewhere, and which tests those are depends on
    // how the suite happens to be sharded. Same reasoning as GridFullFixture in
    // test_panel_widget_grid_full.cpp.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(bp_subj->type == LV_SUBJECT_TYPE_INT);
    const int prev_bp = lv_subject_get_int(bp_subj);
    lv_subject_set_int(bp_subj, to_int(UiBreakpoint::Medium));

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() > 3); // At least the 3 anchors + some auto-place widgets

    // Find anchor entries and verify their fixed positions
    const PanelWidgetEntry* printer_image = nullptr;
    const PanelWidgetEntry* print_status = nullptr;
    const PanelWidgetEntry* tips = nullptr;
    const PanelWidgetEntry* temperature = nullptr;
    const PanelWidgetEntry* bed_temperature = nullptr;

    for (const auto& e : entries) {
        if (e.id == "printer_image")
            printer_image = &e;
        if (e.id == "print_status")
            print_status = &e;
        if (e.id == "tips")
            tips = &e;
        if (e.id == "temperature")
            temperature = &e;
        if (e.id == "bed_temperature")
            bed_temperature = &e;
    }

    REQUIRE(printer_image != nullptr);
    CHECK(printer_image->col == 0);
    CHECK(printer_image->row == 0);
    CHECK(printer_image->colspan >= 2); // depends on breakpoint (2-3)
    CHECK(printer_image->rowspan >= 2); // depends on breakpoint (2-3)
    CHECK(printer_image->has_grid_position());

    REQUIRE(print_status != nullptr);
    CHECK(print_status->col >= 0);
    CHECK(print_status->row >= 0);
    CHECK(print_status->colspan >= 2);
    CHECK(print_status->rowspan >= 2);
    CHECK(print_status->has_grid_position());

    // tips is switched off on the 480-class tiers this runs at (the breakpoint
    // subject is zero-initialised to Micro here), and a disabled widget is
    // unplaced.
    REQUIRE(tips != nullptr);
    CHECK_FALSE(tips->enabled);
    CHECK_FALSE(tips->has_grid_position());

    REQUIRE(temperature != nullptr);
    CHECK(temperature->has_grid_position());

    REQUIRE(bed_temperature != nullptr);
    CHECK(bed_temperature->has_grid_position());

    // Positions are all-or-nothing. Which widgets the shipped table anchors is
    // the table's business and changes per tier, but no entry may come back
    // half-placed: a col with no row (or either one set on a widget that
    // reports no grid position) would be placed by one code path and
    // auto-placed by another.
    for (const auto& e : entries) {
        INFO("Widget '" << e.id << "' col=" << e.col << " row=" << e.row);
        if (e.has_grid_position()) {
            CHECK(e.col >= 0);
            CHECK(e.row >= 0);
            CHECK(e.colspan >= 1);
            CHECK(e.rowspan >= 1);
        } else {
            CHECK(e.col == -1);
            CHECK(e.row == -1);
        }
    }

    // Hand the subject back as it was found: pinning it and walking away would
    // just move the ordering hazard onto whoever runs next.
    lv_subject_set_int(bp_subj, prev_bp);
}

TEST_CASE("GridLayout: can_place needs the whole span in bounds and unoccupied", "[grid][drag]") {
    struct Row {
        const char* what;
        std::vector<GridPlacement> placed;
        int col, row, colspan, rowspan;
        bool fits;
    };
    const std::vector<GridPlacement> anchors = {
        {"printer_image", 0, 0, 2, 2}, {"tips", 2, 0, 4, 1}, {"widget_a", 2, 1, 1, 1}};
    const std::vector<GridPlacement> visible = {
        {"printer_image", 0, 0, 2, 2}, {"temperature", 4, 0, 1, 1}, {"fan", 5, 0, 1, 1}};
    // A hardware-gated widget is never placed on screen, so it must not be in
    // the collision grid either; this scene shows what a placed one would block.
    auto with_hidden = visible;
    with_hidden.push_back({"humidity", 3, 2, 1, 1});
    const Row rows[] = {
        {"last column", {}, 5, 0, 1, 1, true},
        {"one past the last column", {}, 6, 0, 1, 1, false},
        {"two wide from the last column", {}, 5, 0, 2, 1, false},
        {"two wide ending on the edge", {}, 4, 0, 2, 1, true},
        {"last row", {}, 0, 3, 1, 1, true},
        {"one past the last row", {}, 0, 4, 1, 1, false},
        {"two tall from the last row", {}, 0, 3, 1, 2, false},
        {"two tall ending on the edge", {}, 0, 2, 1, 2, true},
        {"2x2 in the corner", {}, 4, 2, 2, 2, true},
        {"2x2 past the columns", {}, 5, 2, 2, 2, false},
        {"2x2 past the rows", {}, 4, 3, 2, 2, false},
        {"wider than the grid", {}, 0, 0, 8, 1, false},
        {"negative column", {}, -1, 0, 1, 1, false},
        {"negative row", {}, 0, -1, 1, 1, false},
        {"zero colspan", {}, 0, 0, 0, 1, false},
        {"zero rowspan", {}, 0, 0, 1, 0, false},
        {"an empty cell among anchors", anchors, 3, 1, 1, 1, true},
        {"an occupied cell", anchors, 0, 0, 1, 1, false},
        {"free cells only", visible, 3, 2, 1, 1, true},
        {"a 2x2 over free cells", visible, 2, 2, 2, 2, true},
        {"a 2x2 over a placed hidden widget", with_hidden, 2, 2, 2, 2, false},
        {"a 1x1 beside it", with_hidden, 2, 2, 1, 1, true},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        GridLayout grid(UiBreakpoint::Medium, kGrid6x4);
        REQUIRE(grid.cols() == 6);
        REQUIRE(grid.rows() == 4);
        for (const auto& p : row.placed) {
            REQUIRE(grid.place(p));
        }
        CHECK(grid.can_place(row.col, row.row, row.colspan, row.rowspan) == row.fits);
    }
}

TEST_CASE("detect_resize_edge: the edge whose band holds the point, nearest first",
          "[grid_edit][resize][1126]") {
    using Edge = GridEditMode::ResizeEdge;
    GridEditMode em;
    // With no container the band is the 18px fallback the literals below assume.
    REQUIRE(GridEditModeTestAccess::edge_hit_band(em) == 18);

    // 200x200 at 100..300: each band runs 18px inside and 18px outside its edge.
    const lv_area_t sq = {100, 100, 300, 300};
    // 30 wide x 90 tall: two 18px inward bands would overlap and leave no
    // interior to drag by, so the inward band is a third of the width (10px).
    const lv_area_t narrow = {100, 100, 130, 190};
    // 30 wide x 200 tall: the axes clamp independently, 10px across, 18px down.
    const lv_area_t tall = {100, 100, 130, 300};
    struct Row {
        const char* what;
        int x, y;
        lv_area_t area;
        Edge edge;
    };
    const Row rows[] = {
        {"centre", 200, 200, sq, Edge::None},
        {"inside the right edge", 295, 200, sq, Edge::Right},
        {"right band inner limit", 282, 200, sq, Edge::Right},
        {"one inside it", 281, 200, sq, Edge::None},
        {"right band outer limit", 318, 200, sq, Edge::Right},
        {"one past it", 319, 200, sq, Edge::None},
        {"inside the left edge", 105, 200, sq, Edge::Left},
        {"left band inner limit", 118, 200, sq, Edge::Left},
        {"one inside it", 119, 200, sq, Edge::None},
        {"left band outer limit", 82, 200, sq, Edge::Left},
        {"one past it", 81, 200, sq, Edge::None},
        {"inside the bottom edge", 200, 295, sq, Edge::Bottom},
        {"bottom band inner limit", 200, 282, sq, Edge::Bottom},
        {"one inside it", 200, 281, sq, Edge::None},
        {"bottom band outer limit", 200, 318, sq, Edge::Bottom},
        {"one past it", 200, 319, sq, Edge::None},
        {"inside the top edge", 200, 105, sq, Edge::Top},
        {"top band inner limit", 200, 118, sq, Edge::Top},
        {"one inside it", 200, 119, sq, Edge::None},
        {"top band outer limit", 200, 82, sq, Edge::Top},
        {"one past it", 200, 81, sq, Edge::None},
        {"bottom-right, nearer the right", 296, 292, sq, Edge::Right},
        {"bottom-right, nearer the bottom", 292, 296, sq, Edge::Bottom},
        {"top-left, nearer the top", 108, 102, sq, Edge::Top},
        {"top-left, nearer the left", 102, 108, sq, Edge::Left},
        {"top-right, nearer the right", 298, 105, sq, Edge::Right},
        {"bottom-left, nearer the bottom", 105, 298, sq, Edge::Bottom},
        {"well outside, top-left", 50, 50, sq, Edge::None},
        {"well outside, bottom-right", 350, 350, sq, Edge::None},
        {"right band, above the widget", 295, 50, sq, Edge::None},
        {"right band, below the widget", 295, 350, sq, Edge::None},
        {"narrow: centre is interior", 115, 145, narrow, Edge::None},
        {"narrow: left edge", 101, 145, narrow, Edge::Left},
        {"narrow: right edge", 129, 145, narrow, Edge::Right},
        {"tall: 12px below the top", 115, 112, tall, Edge::Top},
        {"tall: 40px below the top", 115, 140, tall, Edge::None},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what, row.x, row.y);
        CHECK(em.detect_resize_edge(row.x, row.y, row.area) == row.edge);
    }
    // Equidistant from two edges: one of them wins, never neither.
    CHECK(em.detect_resize_edge(295, 295, sq) != Edge::None);
}

TEST_CASE("press_owns_widget: the press origin decides, with the grab band of drift",
          "[grid_edit][drag][resize][1169]") {
    GridEditMode em;
    const int band = GridEditModeTestAccess::edge_hit_band(em); // no container: fallback
    auto owns = [&](int x, int y, const lv_area_t& area) {
        return GridEditModeTestAccess::press_owns_widget(em, lv_point_t{x, y}, area);
    };

    // A press that lands slightly off a widget still owns it, up to the band.
    const lv_area_t box = {100, 50, 200, 150};
    CHECK(owns(150, 100, box));
    CHECK(owns(200, 100, box));
    CHECK(owns(200 + band, 100, box));
    CHECK_FALSE(owns(201 + band, 100, box));
    CHECK(owns(150, 50 - band, box));
    CHECK_FALSE(owns(150, 49 - band, box));
    CHECK(owns(100 - band, 100, box));
    CHECK_FALSE(owns(99 - band, 100, box));
    CHECK(owns(150, 150 + band, box));
    CHECK_FALSE(owns(150, 151 + band, box));

    // A resize that grows a widget drags away from it: by the drag threshold the
    // live pointer is past the band, but the origin on the edge still owns it and
    // still classifies as that edge.
    const lv_area_t sq = {100, 100, 300, 300};
    const lv_point_t origin{295, 200};
    const lv_point_t live{334, 200};
    REQUIRE(live.x > sq.x2 + band);
    CHECK(owns(origin.x, origin.y, sq));
    CHECK(em.detect_resize_edge(origin.x, origin.y, sq) == GridEditMode::ResizeEdge::Right);
    CHECK_FALSE(owns(live.x, live.y, sq));

    // The guard is anchored, not disabled: an origin nowhere near is turned away.
    CHECK_FALSE(owns(300 + band + 40, 200, sq));
}

TEST_CASE("edge_hit_band_for_cell: a sixth of the cell, clamped to [14, 32]",
          "[grid_edit][resize][1169]") {
    struct Row {
        float cell;
        int band;
    };
    const Row rows[] = {
        {120.0f, 20}, {150.0f, 25},               // proportional
        {60.0f, 14},  {30.0f, 14},  {84.0f, 14},  // floor, and exactly on it
        {300.0f, 32}, {192.0f, 32}, {186.0f, 31}, // ceiling, on it, just under
        {0.0f, 18},   {-5.0f, 18},                // no grid yet: the fallback, not 14
    };
    for (const auto& row : rows) {
        CAPTURE(row.cell);
        CHECK(GridEditModeTestAccess::edge_hit_band_for_cell(row.cell) == row.band);
    }
}

TEST_CASE("round_to_grid_cell: snaps a pixel to the nearest stepped track boundary",
          "[grid_edit][resize][grid_metrics][half_cell]") {
    struct Row {
        const char* what;
        int px, origin, size, ncells, gutter, step;
        int boundary;
    };
    const Row rows[] = {
        // 6 cells in 600px from x=0: pitch 100.
        {"boundary 0", 0, 0, 600, 6, 0, 1, 0},
        {"boundary 1", 100, 0, 600, 6, 0, 1, 1},
        {"boundary 3", 300, 0, 600, 6, 0, 1, 3},
        {"boundary 6", 600, 0, 600, 6, 0, 1, 6},
        {"below a midpoint", 49, 0, 600, 6, 0, 1, 0},
        {"on a midpoint rounds up", 50, 0, 600, 6, 0, 1, 1},
        {"past a midpoint", 51, 0, 600, 6, 0, 1, 1},
        {"below 250", 249, 0, 600, 6, 0, 1, 2},
        {"past 250", 251, 0, 600, 6, 0, 1, 3},
        {"origin 100: boundary 0", 100, 100, 600, 6, 0, 1, 0},
        {"origin 100: boundary 1", 200, 100, 600, 6, 0, 1, 1},
        {"origin 100: boundary 6", 700, 100, 600, 6, 0, 1, 6},
        {"origin 100: on the midpoint", 150, 100, 600, 6, 0, 1, 1},
        {"origin 100: below it", 149, 100, 600, 6, 0, 1, 0},
        {"before the origin clamps to 0", -50, 0, 600, 6, 0, 1, 0},
        {"past the end clamps to ncells", 800, 0, 600, 6, 0, 1, 6},
        // 6 in 480px with 4px gutters: pitch 80.67, boundary 3 at 242, midpoint 201.7.
        {"gutter: past the midpoint", 202, 0, 480, 6, 4, 1, 3},
        {"gutter: before it", 201, 0, 480, 6, 4, 1, 2},
        {"gutter: clamps low", -500, 0, 480, 6, 4, 1, 0},
        {"gutter: clamps high", 9999, 0, 480, 6, 4, 1, 6},
        // 12 tracks in 600px: pitch 50; step 2 lands on even boundaries only.
        {"step 2: on boundary 2", 100, 0, 600, 12, 0, 2, 2},
        {"step 2: 2.8 tracks", 140, 0, 600, 12, 0, 2, 2},
        {"step 2: 3.2 tracks", 160, 0, 600, 12, 0, 2, 4},
        {"step 2: the end", 600, 0, 600, 12, 0, 2, 12},
        {"step 1 reaches an odd boundary", 150, 0, 600, 12, 0, 1, 3},
        // 13 tracks in 650px: pitch 50 exactly, which halving the count would not keep.
        {"odd count: 6 tracks", 300, 0, 650, 13, 0, 2, 6},
        {"odd count: 7 tracks ties up", 350, 0, 650, 13, 0, 2, 8},
        {"odd count: 8 tracks", 400, 0, 650, 13, 0, 2, 8},
        {"odd count: the last even boundary, not 13", 650, 0, 650, 13, 0, 2, 12},
        // (404 + 2) / 14 = 29.0: the gutter does not shift a stepped boundary.
        {"stepped gutter: 4 tracks", 116, 0, 404, 14, 2, 2, 4},
        {"stepped gutter: 6 tracks", 174, 0, 404, 14, 2, 2, 6},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        CHECK(GridEditMode::round_to_grid_cell(row.px, row.origin, row.size, row.ncells, row.gutter,
                                               row.step) == row.boundary);
    }
}

TEST_CASE("compute_resize_result: the dragged edge moves, the opposite edge stays",
          "[grid_edit][resize][half_cell]") {
    using Edge = GridEditMode::ResizeEdge;
    struct Row {
        const char* what;
        Edge edge;
        int col, row, colspan, rowspan, new_edge, ncells, step;
        int out_col, out_row, out_colspan, out_rowspan;
    };
    const Row rows[] = {
        {"right edge grows", Edge::Right, 1, 0, 2, 2, 4, 6, 1, 1, 0, 3, 2},
        {"right edge shrinks", Edge::Right, 1, 0, 3, 2, 3, 6, 1, 1, 0, 2, 2},
        {"left edge grows: origin shifts left", Edge::Left, 2, 0, 2, 2, 1, 6, 1, 1, 0, 3, 2},
        {"left edge shrinks: origin shifts right", Edge::Left, 1, 0, 3, 2, 2, 6, 1, 2, 0, 2, 2},
        {"top edge grows: origin shifts up", Edge::Top, 0, 2, 2, 2, 1, 4, 1, 0, 1, 2, 3},
        {"bottom edge grows", Edge::Bottom, 0, 0, 2, 2, 3, 4, 1, 0, 0, 2, 3},
        {"left edge past the right keeps one track", Edge::Left, 2, 0, 2, 2, 5, 6, 1, 3, 0, 1, 2},
        {"right edge past the grid clamps", Edge::Right, 4, 0, 2, 2, 7, 6, 1, 4, 0, 2, 2},
        {"top edge past the grid clamps", Edge::Top, 0, 1, 2, 2, -1, 4, 1, 0, 0, 2, 3},
        // Step 2: a span rounds down to whole cells, never straddling one.
        {"stepped: track 7 rounds to an even span", Edge::Right, 2, 0, 2, 2, 7, 12, 2, 2, 0, 4, 2},
        {"stepped: the floor is one whole cell", Edge::Right, 2, 0, 4, 2, 2, 12, 2, 2, 0, 2, 2},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const auto r =
            GridEditMode::compute_resize_result(row.edge, row.col, row.row, row.colspan,
                                                row.rowspan, row.new_edge, row.ncells, row.step);
        CHECK(r.col == row.out_col);
        CHECK(r.row == row.out_row);
        CHECK(r.colspan == row.out_colspan);
        CHECK(r.rowspan == row.out_rowspan);
    }
}

TEST_CASE("snap_step_for: whole-cell widgets step by a full cell",
          "[grid_edit][resize][half_cell]") {
    // control_buttons is authored around a fixed number of cells on both axes,
    // so it is the whole-cell fixed point. Picking a widget that later gains a
    // half flag would turn this into a test of nothing, so assert the premise.
    const auto* whole = helix::find_widget_def("control_buttons");
    REQUIRE(whole != nullptr);
    REQUIRE_FALSE(whole->supports_half_col);
    REQUIRE_FALSE(whole->supports_half_row);
    auto [wc, wr] = GridEditMode::snap_step_for("control_buttons");
    CHECK(wc == helix::GridLayout::TRACKS_PER_CELL);
    CHECK(wr == helix::GridLayout::TRACKS_PER_CELL);

    // preheat is half on the column only: its row span is authored fixed.
    const auto* mixed = helix::find_widget_def("preheat");
    REQUIRE(mixed != nullptr);
    REQUIRE(mixed->supports_half_col);
    REQUIRE_FALSE(mixed->supports_half_row);
    auto [hc, hr] = GridEditMode::snap_step_for("preheat");
    CHECK(hc == 1);
    CHECK(hr == helix::GridLayout::TRACKS_PER_CELL);

    auto [cc, cr] = GridEditMode::snap_step_for("clock");
    CHECK(cc == 1);
    CHECK(cr == 1);

    // An id with no registry entry gets the conservative whole-cell answer.
    auto [uc, ur] = GridEditMode::snap_step_for("not_a_widget");
    CHECK(uc == helix::GridLayout::TRACKS_PER_CELL);
    CHECK(ur == helix::GridLayout::TRACKS_PER_CELL);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "dots overlay: minor dots appear only for a half-capable selection",
                 "[grid_edit][half_cell][dots]") {
    // The lattice is (cols/step + 1) x (rows/step + 1) intersections. With
    // TRACKS_PER_CELL == 2 on a 12x8 track grid a whole-cell selection draws
    // 7x5 = 35 dots and a half-col selection draws 13x5 = 65.
    const int cols = 12;
    const int rows = 8;
    const int cell = helix::GridLayout::TRACKS_PER_CELL;

    CHECK(GridEditMode::dot_count(cols, rows, cell, cell) == (cols / cell + 1) * (rows / cell + 1));
    CHECK(GridEditMode::dot_count(cols, rows, 1, cell) == (cols + 1) * (rows / cell + 1));
    CHECK(GridEditMode::dot_count(cols, rows, 1, 1) == (cols + 1) * (rows + 1));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "dots overlay: the shield is kept and its lattice rebuilt for the selected "
                 "widget's snap step",
                 "[grid_edit][half_cell][dots]") {
    // dot_count() above proves the counting formula; this proves GridEditMode
    // calls it on every selection change. "temperature" has no
    // half-cell support (snap_step_for returns {cell, cell}) - it is a centred
    // icon over a readout, so an intermediate size buys only whitespace;
    // "shutdown" supports half columns (snap_step_for returns {1, cell}) - see
    // the registry entries this reads from (panel_widget_registry.cpp) and the
    // equivalent snap_step_for() assertions above.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Medium);
    // The container below is 640x400 with no padding or border, so that is its
    // content box and the grid has to be derived from it — the same measurement
    // PanelWidgetManager makes before installing a descriptor.
    const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, 640, 400);
    const int ncols = dims.cols;
    const int nrows = dims.rows;
    REQUIRE(ncols > 0);
    REQUIRE(nrows > 0);
    const int cell = GridLayout::TRACKS_PER_CELL;

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, 640, 400);

    auto col_dsc = GridLayout::make_col_dsc(ncols);
    auto row_dsc = GridLayout::make_row_dsc(nrows);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());

    lv_obj_t* temperature_widget = lv_obj_create(container);
    lv_obj_set_name(temperature_widget, "temperature");
    lv_obj_remove_flag(temperature_widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(temperature_widget, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0,
                         2);

    lv_obj_t* shutdown_widget = lv_obj_create(container);
    lv_obj_set_name(shutdown_widget, "shutdown");
    lv_obj_remove_flag(shutdown_widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(shutdown_widget, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_update_layout(container);

    // PanelWidgetConfig::load() appends registry defaults onto the FIRST page
    // parsed (parse_widget_array's append_registry_defaults, gated on
    // pages_.empty() in src/system/panel_widget_config.cpp), so the test's
    // two widgets live on a second page, the same trick test_grid_edit_drag_path
    // uses. main_page_index points AT that second page so the lattice does not
    // also get a delete_page_btn_.
    const std::string panel_id = "test_grid_edit_mode_dots";
    constexpr int kPageIndex = 1;
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", kPageIndex},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "test"},
                          {"widgets",
                           {{{"id", "temperature"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", 2},
                             {"rowspan", 2}},
                            {{"id", "shutdown"},
                             {"enabled", true},
                             {"col", 2},
                             {"row", 0},
                             {"colspan", 1},
                             {"rowspan", 1}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);
    REQUIRE(config.page_entries(static_cast<size_t>(kPageIndex)).size() == 2);

    GridEditMode em;
    em.enter(container, &config, kPageIndex);
    lv_obj_t* dots_at_enter = GridEditModeTestAccess::shield(em);
    REQUIRE(dots_at_enter != nullptr);

    em.select_widget(temperature_widget);
    lv_obj_t* dots_for_temperature = GridEditModeTestAccess::shield(em);
    REQUIRE(dots_for_temperature != nullptr);
    // The same shield object: the indev glues a live gesture to its press
    // target, so the shield must never be destroyed mid-session. What changes
    // with the selection is the lattice it draws, the legal drop targets.
    CHECK(dots_for_temperature == dots_at_enter);
    // Both these tiles snap by a single track now, so the lattice is at its
    // finest pitch. The two counts must differ, or this case would pass with a
    // lattice that ignored the selection entirely.
    REQUIRE(GridEditMode::dot_count(ncols, nrows, 1, 1) !=
            GridEditMode::dot_count(ncols, nrows, cell, cell));
    CHECK(GridEditModeTestAccess::drawn_dot_count(em) ==
          GridEditMode::dot_count(ncols, nrows, 1, 1));

    em.select_widget(shutdown_widget);
    lv_obj_t* dots_for_shutdown = GridEditModeTestAccess::shield(em);
    REQUIRE(dots_for_shutdown != nullptr);
    CHECK(dots_for_shutdown == dots_for_temperature);
    CHECK(GridEditModeTestAccess::drawn_dot_count(em) ==
          GridEditMode::dot_count(ncols, nrows, 1, 1));

    em.exit();
    mgr.clear_panel_config(panel_id);
}

TEST_CASE("find_catalog_placement: shrinks toward the minimum, rowspan first, when the default "
          "fits nowhere",
          "[grid_edit][shrink_to_fit]") {
    // Step 1 throughout: the fixtures are single-track fillers, which only a
    // half-cell widget's step can reach. The whole-cell step has its own
    // coverage in test_widget_catalog_fit.cpp.
    struct Row {
        const char* what;
        int full_rows;     // rows filled completely from the top
        bool column5_free; // leave (5,2) and (5,3) open in an otherwise full grid
        int min_colspan, min_rowspan;
        std::optional<CatalogPlacement> expect;
    };
    const Row rows[] = {
        {"empty grid: the default fits", 0, false, 1, 1, CatalogPlacement{0, 0, 2, 2}},
        {"one free row: 2x1 at minimum 2x1", 3, false, 2, 1, CatalogPlacement{0, 3, 2, 1}},
        {"one free row: the rowspan shrinks before the colspan", 3, false, 1, 1,
         CatalogPlacement{0, 3, 2, 1}},
        {"a 1x2 slot: the colspan shrinks when the rowspan cannot", 0, true, 1, 1,
         CatalogPlacement{5, 2, 1, 2}},
        {"a full grid", 4, false, 1, 1, std::nullopt},
        {"a widget that cannot shrink", 3, false, 2, 2, std::nullopt},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        GridLayout grid(UiBreakpoint::Medium, kGrid6x4);
        int n = 0;
        for (int r = 0; r < grid.rows(); ++r) {
            for (int c = 0; c < grid.cols(); ++c) {
                const bool filled = r < row.full_rows || (row.column5_free && !(c == 5 && r >= 2));
                if (filled) {
                    REQUIRE(grid.place({"filler_" + std::to_string(n++), c, r, 1, 1}));
                }
            }
        }
        PanelWidgetDef def{};
        def.colspan = 2;
        def.rowspan = 2;
        def.min_colspan = row.min_colspan;
        def.min_rowspan = row.min_rowspan;
        def.max_colspan = 2;
        def.max_rowspan = 2;

        const auto p = find_catalog_placement(grid, def, -1, -1, 1, 1);
        REQUIRE(p.has_value() == row.expect.has_value());
        if (p) {
            CHECK(p->col == row.expect->col);
            CHECK(p->row == row.expect->row);
            CHECK(p->colspan == row.expect->colspan);
            CHECK(p->rowspan == row.expect->rowspan);
        }
    }
}
