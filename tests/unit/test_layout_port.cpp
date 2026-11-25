// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_layout_port.cpp
 * @brief Porting a pre-v22 saved home layout onto the square-cell track grid.
 *
 * The migration that preceded this unplaced every hand-arranged widget, on the
 * grounds that config load knows no screen size. It does not, but the first
 * grid build does, so the port is deferred rather than impossible. These tests
 * pin the three properties that make a deferred port worth having over a reset:
 * a proportional grid reproduces the layout exactly, widgets that were flush
 * stay flush, and a widget the port cannot seat costs only itself its position.
 */

#include "../test_helpers/scoped_whole_cell_def.h"
#include "grid_layout.h"
#include "layout_port.h"
#include "panel_widget_registry.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// By value: a reference return here trips -Wdangling-reference, which the
/// build takes as an error.
PortedPlacement by_id(const std::vector<PortedPlacement>& v, const std::string& id) {
    auto it =
        std::find_if(v.begin(), v.end(), [&](const PortedPlacement& p) { return p.id == id; });
    REQUIRE(it != v.end());
    return *it;
}

/// Do any two seated placements share a track?
bool any_overlap(const std::vector<PortedPlacement>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (!v[i].seated)
            continue;
        for (size_t j = i + 1; j < v.size(); ++j) {
            if (!v[j].seated)
                continue;
            const bool x = v[i].col < v[j].col + v[j].colspan && v[j].col < v[i].col + v[i].colspan;
            const bool y = v[i].row < v[j].row + v[j].rowspan && v[j].row < v[i].row + v[i].rowspan;
            if (x && y)
                return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// legacy_grid_cols — the frozen pre-v22 column table
// ---------------------------------------------------------------------------

TEST_CASE("legacy_grid_cols: landscape reads the frozen breakpoint table", "[layout_port][grid]") {
    // main GRID_DIMS: micro/tiny/small/medium = 6 cols, large/xlarge = 8.
    CHECK(legacy_grid_cols(480, 272) == 6);
    CHECK(legacy_grid_cols(480, 320) == 6);
    CHECK(legacy_grid_cols(800, 480) == 6);
    CHECK(legacy_grid_cols(1024, 600) == 8);
    CHECK(legacy_grid_cols(1280, 720) == 8);
    // XXLarge did not exist before v22; it clamped onto XLarge and got 8.
    CHECK(legacy_grid_cols(1920, 1080) == 8);
}

TEST_CASE("legacy_grid_cols: portrait divides width by the 160px target", "[layout_port][grid]") {
    // clamp(width / TARGET_CELL_W_PX, MIN_PORTRAIT_COLS=2, 16)
    CHECK(legacy_grid_cols(480, 800) == 3);
    CHECK(legacy_grid_cols(272, 480) == 2); // 272/160 = 1, floored up to the portrait minimum
    CHECK(legacy_grid_cols(320, 1480) == 2);
}

TEST_CASE("legacy_grid_cols: ultrawide divides width and floors at 4", "[layout_port][grid]") {
    CHECK(legacy_grid_cols(1480, 320) == 9);
    CHECK(legacy_grid_cols(1920, 440) == 12);
}

// ---------------------------------------------------------------------------
// legacy_grid_rows — recovered from the layout, not from a table
// ---------------------------------------------------------------------------

TEST_CASE("legacy_grid_rows: taken from the tallest placement", "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 2, 2},
        {"print_status", 0, 2, 2, 2}, // bottom edge at row 4
        {"temperature", 2, 1, 1, 1},
    };
    CHECK(legacy_grid_rows(saved) == 4);
}

TEST_CASE("legacy_grid_rows: the cached count is a floor, never a ceiling", "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {{"printer_image", 0, 0, 2, 2}};
    CHECK(legacy_grid_rows(saved, 0) == 2);
    CHECK(legacy_grid_rows(saved, 5) == 5); // hardware-gated widget had grown it
    CHECK(legacy_grid_rows(saved, 1) == 2); // stale cache does not shrink the grid
}

TEST_CASE("legacy_grid_rows: unpositioned entries do not count", "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {{"printer_image", 0, 0, 2, 2},
                                          {"temperature", -1, -1, 1, 1}};
    CHECK(legacy_grid_rows(saved) == 2);
}

TEST_CASE("legacy_grid_rows: a disabled widget's stale row does not count", "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 2, 2, true},
        {"print_status", 0, 2, 2, 2, true}, // bottom edge at row 4
        {"temp_stack", 2, 5, 1, 1, false},  // stale, below the real grid
    };
    CHECK(legacy_grid_rows(saved) == 4);
}

// ---------------------------------------------------------------------------
// port_legacy_layout
// ---------------------------------------------------------------------------

TEST_CASE("port_legacy_layout: a proportional grid reproduces the layout exactly",
          "[layout_port][grid]") {
    // 480x272 and 800x480 both went 6x4 cells -> 12x8 tracks: an exact doubling,
    // so every widget must land on precisely twice its old coordinates.
    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 2, 2},
        {"print_status", 0, 2, 2, 2},
        {"temperature", 2, 2, 1, 1},
        {"notifications", 5, 1, 1, 1},
    };
    auto out = port_legacy_layout(saved, 6, 4, 12, 8);

    for (const auto& in : saved) {
        const auto& p = by_id(out, in.id);
        INFO("widget " << in.id);
        REQUIRE(p.seated);
        CHECK(p.col == in.col * 2);
        CHECK(p.row == in.row * 2);
        CHECK(p.colspan == in.colspan * 2);
        CHECK(p.rowspan == in.rowspan * 2);
    }
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: widgets that were flush stay flush", "[layout_port][grid]") {
    // The regression this whole approach exists for. 8 old columns onto 18 new
    // tracks puts the shared boundary at 6.75 tracks. Rounding each widget's own
    // edges independently lands them on different tracks and opens a sliver
    // between two widgets the user had touching.
    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 3, 1}, // supports half cells
        {"temperature", 3, 0, 5, 1},   // whole-cell only
    };
    auto out = port_legacy_layout(saved, 8, 1, 18, 2);

    const auto& a = by_id(out, "printer_image");
    const auto& b = by_id(out, "temperature");
    REQUIRE(a.seated);
    REQUIRE(b.seated);
    CHECK(a.col + a.colspan == b.col); // flush, no sliver
    CHECK(a.col == 0);
    CHECK(b.col + b.colspan == 18); // and the pair still spans the whole grid
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: a whole-cell widget never straddles a cell", "[layout_port][grid]") {
    // control_buttons has neither supports_half_col nor supports_half_row, so
    // both its origin and its span must stay even however the proportions fall.
    // Assert that premise: a widget that later gains a half flag would turn
    // every check below into a test of nothing.
    const auto* whole = find_widget_def("control_buttons");
    REQUIRE(whole != nullptr);
    REQUIRE_FALSE(whole->supports_half_col);
    REQUIRE_FALSE(whole->supports_half_row);

    std::vector<LegacyPlacement> saved = {{"control_buttons", 1, 1, 1, 1}};
    auto out = port_legacy_layout(saved, 3, 3, 14, 14);

    const auto& p = by_id(out, "control_buttons");
    REQUIRE(p.seated);
    CHECK(p.col % GridLayout::TRACKS_PER_CELL == 0);
    CHECK(p.row % GridLayout::TRACKS_PER_CELL == 0);
    CHECK(p.colspan % GridLayout::TRACKS_PER_CELL == 0);
    CHECK(p.rowspan % GridLayout::TRACKS_PER_CELL == 0);
}

TEST_CASE("port_legacy_layout: a half-cell widget may land on an odd track",
          "[layout_port][grid]") {
    // printer_image supports half cells on both axes. Spanning 1 of 3 old
    // columns onto a 9-track grid is exactly 3 tracks, which a whole-cell
    // widget could not hold.
    std::vector<LegacyPlacement> saved = {{"printer_image", 1, 0, 1, 1}};
    auto out = port_legacy_layout(saved, 3, 1, 9, 2);

    const auto& p = by_id(out, "printer_image");
    REQUIRE(p.seated);
    CHECK(p.colspan == 3);
    CHECK(p.col == 3);
}

TEST_CASE("port_legacy_layout: a widget with no saved position passes through unseated",
          "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {{"printer_image", 0, 0, 2, 2},
                                          {"temperature", -1, -1, 1, 1}};
    auto out = port_legacy_layout(saved, 6, 4, 12, 8);

    CHECK(by_id(out, "printer_image").seated);
    const auto& t = by_id(out, "temperature");
    CHECK_FALSE(t.seated);
    CHECK(t.col == -1);
    CHECK(t.row == -1);
}

TEST_CASE("port_legacy_layout: a collision costs only the colliding widget",
          "[layout_port][grid]") {
    // Two widgets stacked in a 1x2 old grid, ported onto a grid one cell tall.
    // The second cannot be seated; the first must keep its position, and the
    // loser must come back unseated rather than overlapping or vanishing.
    // The loser must be a whole-cell widget: one that snaps by a single track
    // could seat in the remainder and there would be no collision to observe.
    ScopedWholeCellDef whole_cell("width_sensor");
    const auto* loser = find_widget_def("width_sensor");
    REQUIRE(loser != nullptr);
    REQUIRE_FALSE(loser->supports_half_col);
    REQUIRE_FALSE(loser->supports_half_row);

    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 1, 1},
        {"width_sensor", 0, 1, 1, 1},
    };
    auto out = port_legacy_layout(saved, 1, 2, 2, 2);

    REQUIRE(out.size() == 2);
    CHECK(by_id(out, "printer_image").seated);
    CHECK_FALSE(by_id(out, "width_sensor").seated);
    CHECK(by_id(out, "width_sensor").col == -1);
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: a disabled widget's stale spot never displaces an enabled one",
          "[layout_port][grid]") {
    // A Q2 home page: the 4-lane filament strip across the bottom row, and a
    // disabled widget still carrying coordinates inside that row. The live grid
    // ignores disabled widgets, so the strip was what the user saw; the port
    // must keep its 4x1 width rather than drop it to auto-placement.
    std::vector<LegacyPlacement> saved = {
        {"print_status", 0, 0, 2, 3, true},
        {"temp_stack", 2, 3, 1, 1, false},
        {"ams", 0, 3, 4, 1, true},
    };
    auto out = port_legacy_layout(saved, 6, 4, 12, 8);

    const auto ams = by_id(out, "ams");
    REQUIRE(ams.seated);
    CHECK(ams.col == 0);
    CHECK(ams.row == 6);
    CHECK(ams.colspan == 8);
    CHECK(ams.rowspan == 2);
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: a disabled widget's edges never move an enabled one's",
          "[layout_port][grid]") {
    // 6 columns onto 10 tracks is not a whole multiple: old columns 3 and 4
    // both round to track 6 for a whole-cell widget. Were the disabled widget's
    // edge at column 3 in the boundary map, the strip's edge at 4 would be
    // pushed on to track 8 to keep the edges increasing.
    ScopedWholeCellDef whole_cell("ams");
    const LegacyPlacement strip{"ams", 0, 0, 4, 1, true};
    const LegacyPlacement stale{"temp_stack", 3, 1, 1, 1, false};

    const auto alone = by_id(port_legacy_layout({strip}, 6, 4, 10, 8), "ams");
    const auto out = port_legacy_layout({strip, stale}, 6, 4, 10, 8);
    const auto ams = by_id(out, "ams");

    REQUIRE(alone.seated);
    REQUIRE(ams.seated);
    CHECK(ams.col == alone.col);
    CHECK(ams.colspan == alone.colspan);
    CHECK(ams.colspan == 6);
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: every seated widget lands inside the grid", "[layout_port][grid]") {
    std::vector<LegacyPlacement> saved = {
        {"printer_image", 0, 0, 3, 3},
        {"print_status", 3, 0, 5, 2},
        {"temperature", 3, 2, 2, 1},
        {"notifications", 5, 2, 3, 1},
    };
    // A deliberately awkward target: 8x3 cells onto a 9x7-track grid, so
    // neither axis divides evenly.
    auto out = port_legacy_layout(saved, 8, 3, 9, 7);

    for (const auto& p : out) {
        if (!p.seated)
            continue;
        INFO("widget " << p.id << " at " << p.col << "," << p.row << " " << p.colspan << "x"
                       << p.rowspan);
        CHECK(p.col >= 0);
        CHECK(p.row >= 0);
        CHECK(p.colspan >= 1);
        CHECK(p.rowspan >= 1);
        CHECK(p.col + p.colspan <= 9);
        CHECK(p.row + p.rowspan <= 7);
    }
    CHECK_FALSE(any_overlap(out));
}

TEST_CASE("port_legacy_layout: a degenerate source grid seats nothing", "[layout_port][grid]") {
    // old_cols/old_rows of zero would divide by zero. Reached only by a corrupt
    // or hand-edited config; everything must fall to auto-placement rather than
    // land on garbage coordinates.
    std::vector<LegacyPlacement> saved = {{"printer_image", 0, 0, 2, 2}};
    auto out = port_legacy_layout(saved, 0, 0, 12, 8);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].seated);

    auto out2 = port_legacy_layout(saved, 6, 4, 0, 0);
    REQUIRE(out2.size() == 1);
    CHECK_FALSE(out2[0].seated);
}
