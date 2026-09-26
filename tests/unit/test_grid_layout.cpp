// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_layout.cpp
 * @brief Unit tests for GridLayout — grid dimensions, descriptor generation,
 *        widget placement, collision detection, and breakpoint adaptation.
 */

#include "grid_layout.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"

#include <map>
#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix;

// The grid these tests are written against, stated directly. Placement,
// collision, growth and descriptor generation do not depend on how a track
// count was derived, and get_dimensions() takes the container's content box
// rather than reading any global, so there is nothing to install first.
// 6x4 is the shape the pre-square-cell grid had at MICRO, which is what the
// hardcoded coordinates below were authored against.
//
// These fixtures are built from single-track widgets, so they pass a search and
// growth step of 1 (kStep). A span of one track only exists for a widget that
// declares half-cell support, whose step IS 1 — the whole-cell default would
// make half the origins in a 6x4 grid unreachable and turn "scans left to
// right" into "scans every other column". The whole-cell step is covered on its
// own terms in test_grid_half_cell_placement.cpp.
constexpr int kStep = 1;
constexpr GridDimensions kGrid6x4{6, 4};
constexpr GridDimensions kGrid8x6{8, 6};

// =============================================================================
// Descriptor array generation
// =============================================================================

TEST_CASE("GridLayout make_col_dsc: correct length and values", "[grid_layout][descriptor]") {
    SECTION("6 cols") {
        auto dsc = GridLayout::make_col_dsc(6);
        REQUIRE(dsc.size() == 7); // 6 FR values + terminator
        for (int i = 0; i < 6; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[6] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("8 cols") {
        auto dsc = GridLayout::make_col_dsc(8);
        REQUIRE(dsc.size() == 9); // 8 FR values + terminator
        for (int i = 0; i < 8; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[8] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("a non-positive count yields a bare terminator") {
        auto dsc = GridLayout::make_col_dsc(0);
        REQUIRE(dsc.size() == 1);
        CHECK(dsc[0] == LV_GRID_TEMPLATE_LAST);
    }
}

TEST_CASE("GridLayout make_row_dsc: correct length and values", "[grid_layout][descriptor]") {
    SECTION("4 rows") {
        auto dsc = GridLayout::make_row_dsc(4);
        REQUIRE(dsc.size() == 5); // 4 FR values + terminator
        for (int i = 0; i < 4; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[4] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("6 rows") {
        auto dsc = GridLayout::make_row_dsc(6);
        REQUIRE(dsc.size() == 7); // 6 FR values + terminator
        for (int i = 0; i < 6; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[6] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("a non-positive count yields a bare terminator") {
        auto dsc = GridLayout::make_row_dsc(-3);
        REQUIRE(dsc.size() == 1);
        CHECK(dsc[0] == LV_GRID_TEMPLATE_LAST);
    }
}

// =============================================================================
// Widget placement — successful
// =============================================================================

TEST_CASE("GridLayout place: single widget at origin", "[grid_layout][placement]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.place({"widget_a", 0, 0, 2, 1}));
    REQUIRE(grid.placements().size() == 1);
    CHECK(grid.placements()[0].widget_id == "widget_a");
}

TEST_CASE("GridLayout place: multiple non-overlapping widgets", "[grid_layout][placement]") {
    GridLayout grid(UiBreakpoint::Tiny, kGrid6x4);
    REQUIRE(grid.place({"w1", 0, 0, 2, 2}));
    REQUIRE(grid.place({"w2", 2, 0, 2, 2}));
    REQUIRE(grid.place({"w3", 4, 0, 2, 2}));
    REQUIRE(grid.place({"w4", 0, 2, 3, 2}));
    CHECK(grid.placements().size() == 4);
}

TEST_CASE("GridLayout place: widget filling entire grid", "[grid_layout][placement]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.place({"full", 0, 0, 6, 4}));
    CHECK(grid.placements().size() == 1);
}

// =============================================================================
// Collision detection
// =============================================================================

TEST_CASE("GridLayout place: rejects overlapping placements", "[grid_layout][collision]") {
    GridLayout grid(UiBreakpoint::Tiny, kGrid6x4);
    REQUIRE(grid.place({"w1", 1, 1, 2, 2})); // occupies (1,1)-(2,2)

    // Exact overlap
    CHECK_FALSE(grid.place({"w2", 1, 1, 2, 2}));

    // Partial overlap — top-left corner overlaps
    CHECK_FALSE(grid.place({"w3", 2, 2, 2, 2}));

    // Partial overlap — single cell
    CHECK_FALSE(grid.place({"w4", 2, 1, 1, 1}));

    // Adjacent — no overlap, should succeed
    CHECK(grid.place({"w5", 3, 1, 2, 2}));
}

TEST_CASE("GridLayout can_place: returns false for occupied cells", "[grid_layout][collision]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    grid.place({"w1", 0, 0, 2, 2});

    CHECK_FALSE(grid.can_place(0, 0, 1, 1));
    CHECK_FALSE(grid.can_place(1, 1, 1, 1));
    CHECK(grid.can_place(2, 0, 1, 1));
    CHECK(grid.can_place(0, 2, 1, 1));
}

// =============================================================================
// Out-of-bounds rejection
// =============================================================================

TEST_CASE("GridLayout place: rejects out-of-bounds placements", "[grid_layout][bounds]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);

    // Exceeds columns
    CHECK_FALSE(grid.place({"oob1", 5, 0, 2, 1})); // col 5 + span 2 = 7 > 6

    // Exceeds rows
    CHECK_FALSE(grid.place({"oob2", 0, 3, 1, 2})); // row 3 + span 2 = 5 > 4

    // Negative position
    CHECK_FALSE(grid.place({"oob3", -1, 0, 1, 1}));

    // Zero span
    CHECK_FALSE(grid.place({"oob4", 0, 0, 0, 1}));
    CHECK_FALSE(grid.place({"oob5", 0, 0, 1, 0}));

    // Exactly at boundary — should succeed
    CHECK(grid.place({"edge", 5, 3, 1, 1}));
}

// =============================================================================
// find_available()
// =============================================================================

TEST_CASE("GridLayout find_available: finds first open position", "[grid_layout][find]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    grid.place({"w1", 0, 0, 2, 1});

    auto pos = grid.find_available(2, 1, kStep, kStep);
    REQUIRE(pos.has_value());
    // First available 2x1 slot: (2,0) — same row, after w1
    CHECK(pos->first == 2);
    CHECK(pos->second == 0);
}

TEST_CASE("GridLayout find_available: scans top-to-bottom, left-to-right", "[grid_layout][find]") {
    GridLayout grid(UiBreakpoint::Tiny, kGrid6x4);

    // Fill top row completely
    grid.place({"r0a", 0, 0, 3, 1});
    grid.place({"r0b", 3, 0, 3, 1});

    // Next available 1x1 should be at row 1
    auto pos = grid.find_available(1, 1, kStep, kStep);
    REQUIRE(pos.has_value());
    CHECK(pos->first == 0);
    CHECK(pos->second == 1);
}

TEST_CASE("GridLayout find_available: returns nullopt when no space", "[grid_layout][find]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);

    // Fill the entire grid with 1x1 widgets
    int id = 0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
            REQUIRE(grid.place({"fill_" + std::to_string(id++), c, r, 1, 1}));
        }
    }

    CHECK_FALSE(grid.find_available(1, 1, kStep, kStep).has_value());
}

TEST_CASE("GridLayout find_available: large widget in fragmented grid", "[grid_layout][find]") {
    GridLayout grid(UiBreakpoint::Tiny, kGrid6x4);

    // Place checkerboard-style: occupy (0,0), (2,0), (4,0) with 1x1 widgets
    grid.place({"c1", 0, 0, 1, 1});
    grid.place({"c2", 2, 0, 1, 1});
    grid.place({"c3", 4, 0, 1, 1});

    // A 2x1 widget can fit at (0,1) on the second row
    auto pos = grid.find_available(2, 1, kStep, kStep);
    REQUIRE(pos.has_value());
    // Actually it should find something on row 0 at position (0,0) is occupied,
    // (1,0) is free — so (1,0) with span 2 needs (1,0) and (2,0). But (2,0) is occupied.
    // Next candidate: (3,0) with span 2 needs (3,0) and (4,0). (4,0) is occupied.
    // Then (5,0) needs (5,0)+(6,0)=out of bounds for 6 col grid? 5+2=7>6, no.
    // Row 1: (0,1) is free and (1,1) is free — so (0,1) works.
    CHECK(pos->first == 0);
    CHECK(pos->second == 1);
}

// =============================================================================
// remove()
// =============================================================================

TEST_CASE("GridLayout remove: removes existing widget", "[grid_layout][remove]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    grid.place({"w1", 0, 0, 2, 2});
    grid.place({"w2", 2, 0, 2, 2});

    REQUIRE(grid.remove("w1"));
    CHECK(grid.placements().size() == 1);
    CHECK(grid.placements()[0].widget_id == "w2");

    // Space freed: can place at (0,0) again
    CHECK(grid.can_place(0, 0, 2, 2));
}

TEST_CASE("GridLayout remove: returns false for nonexistent widget", "[grid_layout][remove]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    CHECK_FALSE(grid.remove("nonexistent"));
}

// =============================================================================
// clear()
// =============================================================================

TEST_CASE("GridLayout clear: removes all placements", "[grid_layout][clear]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    grid.place({"w1", 0, 0, 1, 1});
    grid.place({"w2", 1, 0, 1, 1});
    REQUIRE(grid.placements().size() == 2);

    grid.clear();
    CHECK(grid.placements().empty());
    CHECK(grid.can_place(0, 0, 6, 4)); // full grid available
}

// =============================================================================
// filter_for_grid()
// =============================================================================

TEST_CASE("GridLayout filter_for_grid: separates fitting vs non-fitting", "[grid_layout][filter]") {
    std::vector<GridPlacement> all = {
        {"fits_1", 0, 0, 2, 2},   // fits in 6x4
        {"fits_2", 2, 0, 2, 1},   // fits in 6x4
        {"too_wide", 0, 0, 7, 1}, // needs 7 cols, grid has 6
        {"too_tall", 0, 0, 1, 5}, // needs 5 rows, grid has 4
    };

    auto [fits, no_fit] = GridLayout::filter_for_grid(kGrid6x4, all);

    REQUIRE(fits.size() == 2);
    REQUIRE(no_fit.size() == 2);

    CHECK(fits[0].widget_id == "fits_1");
    CHECK(fits[1].widget_id == "fits_2");
    CHECK(no_fit[0].widget_id == "too_wide");
    CHECK(no_fit[1].widget_id == "too_tall");
}

TEST_CASE("GridLayout filter_for_grid: all fit in an 8x6 grid", "[grid_layout][filter]") {
    std::vector<GridPlacement> all = {
        {"w1", 0, 0, 4, 3},
        {"w2", 4, 0, 4, 2},
    };

    auto [fits, no_fit] = GridLayout::filter_for_grid(kGrid8x6, all);
    CHECK(fits.size() == 2);
    CHECK(no_fit.empty());
}

// =============================================================================
// Breakpoint transition scenarios
// =============================================================================

TEST_CASE("GridLayout breakpoint transition: LARGE placement does not fit MICRO",
          "[grid_layout][transition]") {
    // A widget placed at col 7 in an 8-col (LARGE) grid should not fit in a
    // 6-col (MICRO) grid.
    std::vector<GridPlacement> placements = {
        {"corner", 7, 4, 1, 1}, // col 7 + span 1 = 8; row 4 + span 1 = 5
    };

    // 6x4 — corner overruns both axes.
    auto [fits, no_fit] = GridLayout::filter_for_grid(kGrid6x4, placements);
    CHECK(fits.empty());
    CHECK(no_fit.size() == 1);

    // 8x6 — the same placement now fits.
    auto [fits2, no_fit2] = GridLayout::filter_for_grid(kGrid8x6, placements);
    CHECK(fits2.size() == 1);
    CHECK(no_fit2.empty());
}

TEST_CASE("GridLayout breakpoint transition: LARGE placement partially fits in SMALL",
          "[grid_layout][transition]") {
    std::vector<GridPlacement> placements = {
        {"top_left", 0, 0, 2, 2},   // fits everywhere
        {"wide_right", 6, 0, 2, 1}, // needs col 6+2=8, only fits an 8-col-or-wider grid
        {"bottom_row", 0, 4, 3, 1}, // needs row 4+1=5, only fits a 5-row-or-taller grid
    };

    // 6x4: only top_left fits.
    auto [small_fits, small_no] = GridLayout::filter_for_grid(kGrid6x4, placements);
    CHECK(small_fits.size() == 1);
    CHECK(small_fits[0].widget_id == "top_left");
    CHECK(small_no.size() == 2);

    // 8x6: all fit.
    auto [large_fits, large_no] = GridLayout::filter_for_grid(kGrid8x6, placements);
    CHECK(large_fits.size() == 3);
    CHECK(large_no.empty());
}

// =============================================================================
// Instance breakpoint accessor
// =============================================================================

TEST_CASE("GridLayout instance: breakpoint and dimensions match", "[grid_layout][instance]") {
    UiBreakpoint bps[] = {UiBreakpoint::Micro,  UiBreakpoint::Tiny,  UiBreakpoint::Small,
                          UiBreakpoint::Medium, UiBreakpoint::Large, UiBreakpoint::XLarge};
    for (auto bp : bps) {
        GridLayout grid(bp, kGrid8x6);
        CHECK(grid.breakpoint() == bp);
        CHECK(grid.cols() == kGrid8x6.cols);
        CHECK(grid.rows() == kGrid8x6.rows);
    }
}

TEST_CASE("GridLayout dimensions: out-of-range breakpoints clamp to the array bounds",
          "[grid_layout][dimensions]") {
    // clamp_bp() is file-local in grid_layout.cpp; exercised here through
    // get_dimensions() with indices outside [Micro, XLarge].
    constexpr int kW = 710;
    constexpr int kH = 466;
    auto below = GridLayout::get_dimensions(static_cast<UiBreakpoint>(-1), kW, kH);
    auto micro = GridLayout::get_dimensions(UiBreakpoint::Micro, kW, kH);
    CHECK(below.cols == micro.cols);
    CHECK(below.rows == micro.rows);

    // The clamp lands on the LAST tier in GRID_CELL, which is XXLarge now that
    // the table carries a rung for it. It used to land on XLarge because the
    // table was one short — the bug that made a 1080p panel draw a 720p-sized
    // grid — so pinning XLarge here would re-encode it.
    auto above = GridLayout::get_dimensions(
        static_cast<UiBreakpoint>(GridLayout::NUM_BREAKPOINTS + 3), kW, kH);
    auto top = GridLayout::get_dimensions(UiBreakpoint::XXLarge, kW, kH);
    CHECK(above.cols == top.cols);
    CHECK(above.rows == top.rows);
}

// =============================================================================
// PanelWidgetDef scalability constraints
// =============================================================================

TEST_CASE("PanelWidgetDef: default (non-scalable) widget", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 1;
    def.rowspan = 1;
    // min/max all 0 = use colspan/rowspan

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 1);
    CHECK(def.effective_max_rowspan() == 1);
    CHECK_FALSE(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: scalable widget with explicit min/max", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 2;
    def.rowspan = 2;
    def.min_colspan = 1;
    def.min_rowspan = 1;
    def.max_colspan = 4;
    def.max_rowspan = 3;

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 4);
    CHECK(def.effective_max_rowspan() == 3);
    CHECK(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: horizontally scalable only", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 2;
    def.rowspan = 1;
    def.min_colspan = 2;
    def.max_colspan = 6;
    // min/max rowspan = 0, so effective = rowspan = 1

    CHECK(def.effective_min_colspan() == 2);
    CHECK(def.effective_max_colspan() == 6);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_rowspan() == 1);
    CHECK(def.is_scalable()); // max_col > min_col
}

TEST_CASE("PanelWidgetDef: registry entries have valid scalability constraints",
          "[widget_def][scalability]") {
    // Force registration so defs have their final state
    init_widget_registrations();

    for (const auto& def : get_all_widget_defs()) {
        INFO("Widget: " << def.id);
        // Min must not exceed max
        CHECK(def.effective_min_colspan() <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.effective_max_rowspan());
        // Default must be within min..max range
        CHECK(def.colspan >= def.effective_min_colspan());
        CHECK(def.colspan <= def.effective_max_colspan());
        CHECK(def.rowspan >= def.effective_min_rowspan());
        CHECK(def.rowspan <= def.effective_max_rowspan());
    }
}

TEST_CASE("PanelWidgetDef: half-cell capability is classified per widget",
          "[widget_def][half_cell][1126]") {
    // Every registry id appears below, so a new widget cannot be added without
    // deciding this. The rule (PanelWidgetDef::supports_half_col): an axis gets
    // half-cell resolution when half a cell of extra room shows MORE. That
    // covers continuous content - a chart, an aspect-fit frame, wrapping text,
    // a scrolling strip, stacked readout rows - and it covers a centred glyph,
    // which scales to whatever box it is given rather than sitting fixed in the
    // middle of it (prestonbrown/helixscreen#1559).
    //
    // What stays whole is a widget whose layout is authored around a fixed
    // number of cells, where a half cell buys nothing but a finer drag snap
    // that costs real precision at a 34px track.
    const std::map<std::string, std::pair<bool, bool>> expected = {
        // id                      half_col  half_row
        {"printer_image", {true, true}},   // aspect-fit render
        {"print_status", {true, true}},    // filename/times/progress reflow
        {"camera", {true, true}},          // aspect-fit frame
        {"temp_graph", {true, true}},      // chart
        {"tips", {true, true}},            // wrapping body text
        {"job_queue", {true, true}},       // list rows
        {"print_stats", {true, true}},     // stat rows
        {"ams", {true, true}},             // lane slots side by side
        {"active_spool", {true, true}},    // measured compact/wide switch
        {"nozzle_temps", {true, true}},    // decide_nozzle_layout() is measured
        {"temp_stack", {true, true}},      // 2-3 stacked readout rows
        {"fan_stack", {true, true}},       // 2-3 stacked readout rows
        {"tool_switcher", {true, true}},   // horizontal chip strip
        {"clog_detection", {true, true}},  // carousel arc scales with the box
        {"preheat", {true, false}},        // flex row; row span is fixed
        {"fan", {true, true}},             // user fan name, long_mode=dots
        {"thermistor", {true, true}},      // user sensor name, long_mode=dots
        {"bypass", {true, true}},          // material name, long_mode=dots
        {"favorite_macro", {true, false}}, // user macro name, long_mode=dots
        {"shutdown", {true, true}},        // fixed 1x1: placement only
        {"lock", {true, true}},            // fixed 1x1: placement only
        {"firmware_restart", {true, true}},
        {"led_controls", {true, true}},
        {"clock", {true, true}}, // digits and date reflow on both axes
        // Centred fixed glyph + short label: an intermediate size is whitespace.
        {"network", {true, true}},
        {"led", {true, true}},
        {"filament", {true, true}},
        {"humidity", {false, false}},
        {"width_sensor", {false, false}},
        {"notifications", {true, true}},
        {"temperature", {true, true}},
        {"bed_temperature", {true, true}},
        {"chamber_temperature", {true, true}},
        // Fixed 1x1/2x1 action buttons that never gained placement freedom.
        {"power_device", {true, true}},
        {"macros", {true, true}},
        {"motion", {true, true}},
        {"gcode_console", {true, true}},
        {"control_buttons", {false, false}},
    };

    helix::init_widget_registrations();
    for (const auto& def : helix::get_all_widget_defs()) {
        INFO("widget " << def.id);
        auto it = expected.find(def.id);
        REQUIRE(it != expected.end()); // new widget: classify it above
        CHECK(def.supports_half_col == it->second.first);
        CHECK(def.supports_half_row == it->second.second);
    }
}

TEST_CASE("PanelWidgetDef: a sub-cell floor belongs only to a widget that can decline one",
          "[widget_def][half_cell][1126][1559]") {
    // A one-track column is 31-40px, which clips a widget whose layout is
    // authored around whole cells. Only a widget that measures itself may sit
    // below a cell, because only it can refuse a box it cannot draw:
    // PanelWidget::fits_at is consulted by the resize clamp and the load path
    // alike, and grow_span_to_fit lifts the span back above the floor when the
    // answer is no.
    //
    // The ROW floor stays a whole cell for everyone. Height is what carries the
    // glyph, its reading and its label stacked, and no measurement recovers a
    // 31px stack.
    constexpr int cell = GridLayout::TRACKS_PER_CELL;
    int sub_cell = 0;
    // The factories are filled in by registration, not by the static table.
    helix::init_widget_registrations();
    for (const auto& def : helix::get_all_widget_defs()) {
        INFO("widget " << def.id);
        CHECK(def.effective_min_rowspan() >= cell);

        if (def.effective_min_colspan() >= cell) {
            continue;
        }
        ++sub_cell;
        // Below a cell, so it must both step by a half cell and be able to
        // refuse one.
        CHECK(def.supports_half_col);
        REQUIRE(def.factory != nullptr);
        auto instance = def.factory(def.id);
        REQUIRE(instance != nullptr);
        // fits_at must be a real predicate, not a constant: a widget allowed
        // below a whole cell has to accept a roomy box and refuse a box nothing
        // can be drawn in. The sizes it refuses in between are geometry- and
        // tier-dependent, which is what test_widget_content_fits measures at
        // the smallest span each widget is actually offered; pinning a specific
        // one here would pin this tier's arithmetic, not the rule.
        INFO("widget " << def.id << " sits below a whole cell, so fits_at must discriminate");
        CHECK(instance->fits_at(480, 480));
        CHECK_FALSE(instance->fits_at(1, 1));
    }

    // A registry where nothing sits below a cell would pass the loop above
    // having checked none of this.
    INFO("widgets authored below a whole cell");
    CHECK(sub_cell > 0);
}

TEST_CASE("PanelWidgetDef: half-cell defaults to off", "[widget_def][half_cell]") {
    helix::PanelWidgetDef def{};
    CHECK_FALSE(def.supports_half_col);
    CHECK_FALSE(def.supports_half_row);
}

// =============================================================================
// Descriptor generation with dynamic sizing
// =============================================================================

TEST_CASE("GridLayout make_col_dsc: descriptor length matches the computed column count",
          "[grid_layout][descriptor][dynamic]") {
    // A wide, short content box, so the column count is well clear of both
    // clamps and the descriptor has to follow it rather than a fixed table.
    const int expected = GridLayout::get_cols(UiBreakpoint::Tiny, 1832, 428);
    auto dsc = GridLayout::make_col_dsc(expected);
    const auto expected_cols = static_cast<size_t>(expected);
    REQUIRE(dsc.size() == expected_cols + 1);
    for (size_t i = 0; i < expected_cols; ++i) {
        CHECK(dsc[i] == LV_GRID_FR(1));
    }
    CHECK(dsc[expected_cols] == LV_GRID_TEMPLATE_LAST);
}

TEST_CASE("GridLayout make_row_dsc: descriptor length matches the computed row count",
          "[grid_layout][descriptor][dynamic]") {
    // A narrow, tall content box, for the same reason.
    const int expected = GridLayout::get_rows(UiBreakpoint::Large, 428, 1768);
    auto dsc = GridLayout::make_row_dsc(expected);
    const auto expected_rows = static_cast<size_t>(expected);
    REQUIRE(dsc.size() == expected_rows + 1);
    for (size_t i = 0; i < expected_rows; ++i) {
        CHECK(dsc[i] == LV_GRID_FR(1));
    }
    CHECK(dsc[expected_rows] == LV_GRID_TEMPLATE_LAST);
}

// =============================================================================
// Minimum-first placement and growth (#1216)
// =============================================================================
//
// Auto-placement used to ask for the largest span that fit and step down until
// something took. On a 3-column portrait grid that let `tips` (authored 4x2)
// claim a reduced 3x2 — 6 of 18 cells — and the three widgets behind it were
// then disabled with "grid full". The policy is now: every auto-placed widget
// gets its DECLARED MINIMUM first, so widget count is maximised, and only then
// does anything grow back toward its authored default.

TEST_CASE("GridLayout find_available_bottom_min: grants the minimum, not the largest fit",
          "[grid_layout][find][minfirst][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.cols() == 6);

    // tips: authored 4x2, minimum 2x1. A 6-column grid could hold the full 4x2,
    // and the old greedy finder handed it over. Minimum-first must not.
    auto fit = grid.find_available_bottom_min(2, 1, kStep, kStep);
    CHECK(fit.failure == GridLayout::PlacementFailure::None);
    CHECK(fit.colspan == 2);
    CHECK(fit.rowspan == 1);
    CHECK(fit.col == 4); // bottom-right packed
    CHECK(fit.row == 3);
}

TEST_CASE("GridLayout find_available_bottom_min: reports TooLargeForGrid, not GridFull",
          "[grid_layout][find][minfirst][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.cols() == 6);

    // A 7-column minimum can never exist in a 6-column grid, however it is
    // packed. The grid is empty, so "grid full" would name the wrong condition.
    auto fit = grid.find_available_bottom_min(7, 1, kStep, kStep);
    CHECK(fit.failure == GridLayout::PlacementFailure::TooLargeForGrid);
    CHECK_FALSE(fit.placed());
}

TEST_CASE("GridLayout find_available_bottom_min: reports GridFull when space runs out",
          "[grid_layout][find][minfirst][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    for (int r = 0; r < 4; ++r) {
        REQUIRE(grid.place({"filler" + std::to_string(r), 0, r, 6, 1}));
    }

    auto fit = grid.find_available_bottom_min(1, 1, kStep, kStep);
    CHECK(fit.failure == GridLayout::PlacementFailure::GridFull);
    CHECK_FALSE(fit.placed());
}

TEST_CASE("GridLayout failure_text names the condition that actually failed",
          "[grid_layout][minfirst][1216]") {
    // The toast said "grid full" for both causes. They must read differently.
    std::string full = GridLayout::failure_text(GridLayout::PlacementFailure::GridFull);
    std::string too_large = GridLayout::failure_text(GridLayout::PlacementFailure::TooLargeForGrid);
    CHECK(full == "grid full");
    CHECK(too_large != full);
    CHECK_FALSE(too_large.empty());
}

// --- growth ------------------------------------------------------------------

TEST_CASE("GridLayout grow_once: extends right before any other direction",
          "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.place({"w", 2, 1, 1, 1}));

    CHECK(grid.grow_once("w", 2, 2, kStep, kStep));
    const auto* p = grid.find_placement("w");
    REQUIRE(p);
    CHECK(p->col == 2); // origin unchanged — right is tried first
    CHECK(p->row == 1);
    CHECK(p->colspan == 2);
    CHECK(p->rowspan == 1);
}

TEST_CASE("GridLayout grow_once: falls back to left and up at the edge",
          "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    // Bottom-right corner: exactly where minimum-first bottom-packing puts the
    // first auto-placed widget, so left/up is the common growth path.
    REQUIRE(grid.place({"w", 5, 3, 1, 1}));

    REQUIRE(grid.grow_once("w", 2, 2, kStep, kStep));
    const auto* p = grid.find_placement("w");
    REQUIRE(p);
    CHECK(p->col == 4); // grew left
    CHECK(p->colspan == 2);
    CHECK(p->row == 3);
    CHECK(p->rowspan == 1);

    REQUIRE(grid.grow_once("w", 2, 2, kStep, kStep));
    p = grid.find_placement("w");
    REQUIRE(p);
    CHECK(p->row == 2); // then up
    CHECK(p->rowspan == 2);
    CHECK(p->col == 4);
    CHECK(p->colspan == 2);
}

TEST_CASE("GridLayout grow_once: never overruns an occupied neighbour",
          "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.place({"w", 0, 0, 1, 1}));
    REQUIRE(grid.place({"right", 1, 0, 1, 1}));
    REQUIRE(grid.place({"below", 0, 1, 1, 1}));

    // Right and down are taken; left and up are off-grid.
    CHECK_FALSE(grid.grow_once("w", 4, 4));
    const auto* p = grid.find_placement("w");
    REQUIRE(p);
    CHECK(p->colspan == 1);
    CHECK(p->rowspan == 1);
    // …and the neighbours are untouched.
    CHECK(grid.find_placement("right")->col == 1);
    CHECK(grid.find_placement("below")->row == 1);
}

TEST_CASE("GridLayout grow_once: stops at the target span", "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    REQUIRE(grid.place({"w", 0, 0, 2, 2}));

    // Already at the target: no growth even though the grid is mostly free.
    CHECK_FALSE(grid.grow_once("w", 2, 2, kStep, kStep));
    const auto* p = grid.find_placement("w");
    CHECK(p->colspan == 2);
    CHECK(p->rowspan == 2);
}

TEST_CASE("GridLayout grow_once: ignores an unknown widget", "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    CHECK_FALSE(grid.grow_once("nobody", 4, 4, kStep, kStep));
}

TEST_CASE("GridLayout grow_to_targets: expands into the free region", "[grid_layout][grow][1216]") {
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    // Two widgets parked at their minimum in the bottom-right corner.
    REQUIRE(grid.place({"a", 5, 3, 1, 1}));
    REQUIRE(grid.place({"b", 3, 3, 2, 1}));

    int steps = grid.grow_to_targets({{"a", 2, 2, kStep, kStep}, {"b", 2, 2, kStep, kStep}});
    CHECK(steps > 0);

    const auto* a = grid.find_placement("a");
    const auto* b = grid.find_placement("b");
    REQUIRE(a);
    REQUIRE(b);
    INFO("a=" << a->col << "," << a->row << " " << a->colspan << "x" << a->rowspan
              << "  b=" << b->col << "," << b->row << " " << b->colspan << "x" << b->rowspan);
    CHECK(a->colspan * a->rowspan > 1); // both grew
    CHECK(b->colspan * b->rowspan > 2);
    CHECK(a->colspan <= 2); // …and neither passed its target
    CHECK(a->rowspan <= 2);
    CHECK(b->colspan <= 2);
    CHECK(b->rowspan <= 2);
}

TEST_CASE("GridLayout grow_to_targets: round-robin, not first-come", "[grid_layout][grow][1216]") {
    // One row of slack shared by two widgets that both want it. Round-robin
    // hands one step to each in turn, so the first target cannot absorb the
    // whole strip while the second stays at its minimum.
    GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
    // Fill rows 0-1 so only rows 2-3 are in play, then park two 1x1s on row 3.
    REQUIRE(grid.place({"pad0", 0, 0, 6, 1}));
    REQUIRE(grid.place({"pad1", 0, 1, 6, 1}));
    REQUIRE(grid.place({"a", 0, 3, 1, 1}));
    REQUIRE(grid.place({"b", 1, 3, 1, 1}));

    grid.grow_to_targets({{"a", 1, 2, kStep, kStep}, {"b", 1, 2, kStep, kStep}});

    const auto* a = grid.find_placement("a");
    const auto* b = grid.find_placement("b");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->rowspan == 2);
    CHECK(b->rowspan == 2);
}

TEST_CASE("GridLayout grow_to_targets: same input, same result",
          "[grid_layout][grow][determinism][1216]") {
    auto run = [] {
        GridLayout grid(UiBreakpoint::Micro, kGrid6x4);
        REQUIRE(grid.place({"a", 5, 3, 1, 1}));
        REQUIRE(grid.place({"b", 3, 3, 2, 1}));
        REQUIRE(grid.place({"c", 1, 3, 2, 1}));
        grid.grow_to_targets(
            {{"a", 2, 2, kStep, kStep}, {"b", 2, 2, kStep, kStep}, {"c", 4, 2, kStep, kStep}});
        std::vector<std::tuple<std::string, int, int, int, int>> out;
        for (const auto& p : grid.placements()) {
            out.emplace_back(p.widget_id, p.col, p.row, p.colspan, p.rowspan);
        }
        return out;
    };

    auto first = run();
    for (int i = 0; i < 5; ++i) {
        CHECK(run() == first);
    }
}
