// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_layout.cpp
 * @brief Unit tests for decide_tool_switcher_layout() — the pure layout
 *        decision behind the home tool switcher.
 *
 * The content boxes are the ones the tile really measures at 800x480 and
 * 480x272 for each grid span, so the mode boundaries below are the live ones.
 * Mode selection depends only on the box, the touch floor and the gap; the
 * font sizes decide the detail block alone, so every table row is checked at
 * both breakpoints' fonts.
 *
 * Run with: ./build/bin/helix-tests "[tool_switcher]"
 */

#include "../test_helpers/sample_checks.h"
#include "src/ui/panel_widgets/tool_switcher_layout.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::decide_tool_switcher_layout;
using helix::ToolSwitcherLayout;
using helix::ToolSwitcherMode;

namespace {

constexpr int kMin = 48;
constexpr int kGap = 2;

struct Fonts {
    int body;
    int small;
};
constexpr Fonts kFonts800{18, 16};
constexpr Fonts kFonts480{12, 10};

struct Box {
    const char* name;
    int w;
    int h;
};

constexpr Box k800_1x1{"800x480 1x1", 95, 93};
constexpr Box k800_2x1{"800x480 2x1", 214, 93};
constexpr Box k800_1x2{"800x480 1x2", 95, 211};
constexpr Box k800_2x2{"800x480 2x2", 214, 211};
constexpr Box k480_1x1{"480x272 1x1", 58, 52};
constexpr Box k480_2x1{"480x272 2x1", 130, 52};
constexpr Box k480_1x2{"480x272 1x2", 58, 118};
constexpr Box k480_2x2{"480x272 2x2", 130, 118};

constexpr helix::ToolSwitcherActions kPark{true, false};
constexpr helix::ToolSwitcherActions kParkFeeder{true, true};
constexpr helix::ToolSwitcherActions kFeederOnly{false, true};

ToolSwitcherLayout layout_for(int heads, Box box, Fonts f = kFonts800, bool park = true) {
    return decide_tool_switcher_layout(heads, box.w, box.h, kMin, kGap, f.body, f.small,
                                       helix::ToolSwitcherActions{park, false});
}

ToolSwitcherLayout layout_with(int heads, int w, int h, helix::ToolSwitcherActions actions,
                               Fonts f = kFonts800) {
    return decide_tool_switcher_layout(heads, w, h, kMin, kGap, f.body, f.small, actions);
}

const char* mode_name(ToolSwitcherMode m) {
    switch (m) {
    case ToolSwitcherMode::RailWithActions:
        return "RailWithActions";
    case ToolSwitcherMode::Rail:
        return "Rail";
    case ToolSwitcherMode::Pair:
        return "Pair";
    case ToolSwitcherMode::Single:
        return "Single";
    }
    return "?";
}

} // namespace

TEST_CASE("Tool switcher layout: mode per measured box and head count", "[tool_switcher][1784]") {
    using M = ToolSwitcherMode;
    constexpr M RP = M::RailWithActions, R = M::Rail, P = M::Pair, S = M::Single;
    const Box boxes[8] = {k800_1x1, k800_2x1, k800_1x2, k800_2x2,
                          k480_1x1, k480_2x1, k480_1x2, k480_2x2};
    struct Row {
        int heads;
        M modes[8];
    };
    const Row table[] = {
        {2, {S, RP, RP, RP, S, R, R, RP}},
        {4, {S, R, R, RP, S, P, P, R}},
        {5, {S, P, P, RP, S, P, P, P}},
        {8, {S, P, P, RP, S, P, P, P}},
    };

    for (const Fonts f : {kFonts800, kFonts480}) {
        for (const Row& row : table) {
            for (int i = 0; i < 8; i++) {
                const ToolSwitcherLayout l = layout_for(row.heads, boxes[i], f);
                INFO(row.heads << " heads, " << boxes[i].name << ", body " << f.body << ": got "
                               << mode_name(l.mode) << ", want " << mode_name(row.modes[i]));
                CHECK(l.mode == row.modes[i]);
            }
        }
    }
}

TEST_CASE("Tool switcher layout: 2x1 at 800x480 holds four 48 px segments but not five",
          "[tool_switcher][1784]") {
    // Four segments need 4*48 + 3*2 = 198 of the 214 px; five (or four plus
    // Park) need 248.
    REQUIRE(4 * kMin + 3 * kGap <= k800_2x1.w);
    REQUIRE(5 * kMin + 4 * kGap > k800_2x1.w);

    const ToolSwitcherLayout four = layout_for(4, k800_2x1);
    CHECK(four.mode == ToolSwitcherMode::Rail);
    CHECK(four.cols == 4);
    CHECK(four.rows == 1);
    CHECK(four.cell_w == (214 - 3 * kGap) / 4);
    CHECK(four.cell_w >= kMin);

    const ToolSwitcherLayout five = layout_for(5, k800_2x1);
    CHECK(five.mode == ToolSwitcherMode::Pair);
}

TEST_CASE("Tool switcher layout: the rail spans exactly its columns, never the box's remainder",
          "[tool_switcher]") {
    // A 213 px box less three gaps does not divide by four, so the cells
    // leave a remainder that a full-width rail would round its corners into.
    constexpr int kOddW = 213;
    REQUIRE((kOddW - 3 * kGap) % 4 != 0);

    const ToolSwitcherLayout four = layout_with(4, kOddW, k800_2x1.h, kPark);
    REQUIRE(four.mode == ToolSwitcherMode::Rail);
    CHECK(four.rail_w == 4 * four.cell_w + 3 * kGap);
    CHECK(four.rail_w < kOddW);

    // Outside the rail modes the box is the rail.
    const ToolSwitcherLayout pair = layout_for(5, k800_2x1);
    REQUIRE(pair.mode == ToolSwitcherMode::Pair);
    CHECK(pair.rail_w == k800_2x1.w);
}

TEST_CASE("Tool switcher layout: with no Park to offer, no mode draws a Park cell",
          "[tool_switcher][1784]") {
    // 2x1 at 800x480: two heads would take three cells with Park; four fill
    // the rail either way.
    const ToolSwitcherLayout two = layout_for(2, k800_2x1, kFonts800, /*park=*/false);
    CHECK(two.mode == ToolSwitcherMode::Rail);
    CHECK(two.cols == 2);
    CHECK(two.cell_w == (214 - kGap) / 2);
    const ToolSwitcherLayout four = layout_for(4, k800_2x1, kFonts800, false);
    CHECK(four.mode == ToolSwitcherMode::Rail);
    CHECK(four.cols == 4);

    // 2x2 at 800x480: five heads and no Park cell.
    CHECK(layout_for(5, k800_2x2, kFonts800, false).mode == ToolSwitcherMode::Rail);

    // 2x1 at 800x480 with five heads is a Pair beside Park; with no Park the
    // face takes the whole box.
    REQUIRE(layout_for(5, k800_2x1).mode == ToolSwitcherMode::Pair);
    const ToolSwitcherLayout single = layout_for(5, k800_2x1, kFonts800, false);
    CHECK(single.mode == ToolSwitcherMode::Single);
    CHECK(single.cell_w == k800_2x1.w);
    CHECK(single.cell_h == k800_2x1.h);

    for (int heads = 1; heads <= 16; heads++) {
        for (const Box box :
             {k800_1x1, k800_2x1, k800_1x2, k800_2x2, k480_1x1, k480_2x1, k480_1x2, k480_2x2}) {
            const ToolSwitcherLayout l = layout_for(heads, box, kFonts800, false);
            INFO(heads << " heads, " << box.name << ": " << mode_name(l.mode));
            CHECK(l.mode != ToolSwitcherMode::RailWithActions);
            CHECK(l.mode != ToolSwitcherMode::Pair);
        }
    }
}

TEST_CASE("Tool switcher layout: rail grid wraps to the first row count that fits",
          "[tool_switcher][1784]") {
    // 5 heads + Park on 214x211: one row of six is 34 px wide, two rows of
    // three are 70x104.
    const ToolSwitcherLayout l = layout_for(5, k800_2x2);
    REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
    CHECK(l.cols == 3);
    CHECK(l.rows == 2);
    CHECK(l.cell_w == (214 - 2 * kGap) / 3);
}

TEST_CASE("Tool switcher layout: one head and sixteen heads on a 2x2 box",
          "[tool_switcher][1784]") {
    const ToolSwitcherLayout one = layout_for(1, k800_2x2);
    CHECK(one.mode == ToolSwitcherMode::RailWithActions);
    CHECK(one.cols == 2);
    CHECK(one.rows == 1);

    // Seventeen cells do not fit; sixteen do as a 4x4 grid.
    const ToolSwitcherLayout sixteen = layout_for(16, k800_2x2);
    CHECK(sixteen.mode == ToolSwitcherMode::Rail);
    CHECK(sixteen.cols == 4);
    CHECK(sixteen.rows == 4);
    CHECK(sixteen.cell_w >= kMin);
    CHECK(sixteen.cell_h >= kMin);
}

TEST_CASE("Tool switcher layout: degenerate inputs fall back to Single", "[tool_switcher][1784]") {
    SECTION("a 0x0 box") {
        const ToolSwitcherLayout l =
            decide_tool_switcher_layout(5, 0, 0, kMin, kGap, 18, 16, kPark);
        CHECK(l.mode == ToolSwitcherMode::Single);
        CHECK(l.cell_w == 0);
        CHECK(l.cell_h == 0);
        CHECK_FALSE(l.detail);
    }
    SECTION("no heads at all") {
        const ToolSwitcherLayout l = layout_for(0, k800_2x2);
        CHECK(l.mode == ToolSwitcherMode::Single);
        CHECK(l.cell_w == k800_2x2.w);
        CHECK(l.cell_h == k800_2x2.h);
    }
}

TEST_CASE("Tool switcher layout: Pair lies along the long axis with a 30% Park cell",
          "[tool_switcher][1784]") {
    SECTION("wide box: face and Park side by side") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x1);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK(l.pair_horizontal);
        CHECK(l.cols == 2);
        CHECK(l.rows == 1);
        // Park takes max(48, 30% of 214) = 64; the face the rest.
        CHECK(l.cell_w == 214 - kGap - 64);
        CHECK(l.cell_h == 93);
        CHECK(l.rail_h == 93);
        CHECK_FALSE(l.detail);
    }
    SECTION("tall box: face over Park") {
        const ToolSwitcherLayout l = layout_for(5, k800_1x2);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK_FALSE(l.pair_horizontal);
        CHECK(l.cols == 1);
        CHECK(l.rows == 2);
        CHECK(l.cell_w == 95);
        CHECK(l.cell_h == 211 - kGap - 63);
    }
    SECTION("a short long axis still gives Park the 48 px floor") {
        // 30% of 130 is 39, so Park takes 48 and the face 80.
        const ToolSwitcherLayout l = layout_for(5, k480_2x1, kFonts480);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK(l.cell_w == 130 - kGap - kMin);
    }
}

TEST_CASE("Tool switcher layout: Single fills the whole box", "[tool_switcher][1784]") {
    const ToolSwitcherLayout l = layout_for(5, k800_1x1);
    REQUIRE(l.mode == ToolSwitcherMode::Single);
    CHECK(l.cols == 1);
    CHECK(l.rows == 1);
    CHECK(l.cell_w == 95);
    CHECK(l.cell_h == 93);
    CHECK(l.rail_h == 93);
    CHECK_FALSE(l.detail);
}

TEST_CASE("Tool switcher layout: no mode yields a cell under the touch floor",
          "[tool_switcher][1784]") {
    helix::test::SampleChecks samples;
    for (const bool park : {true, false}) {
        for (int heads = 1; heads <= 16; heads++) {
            for (int w = 40; w <= 260; w += 6) {
                for (int h = 40; h <= 260; h += 6) {
                    const ToolSwitcherLayout l = decide_tool_switcher_layout(
                        heads, w, h, kMin, kGap, 18, 16, helix::ToolSwitcherActions{park, false});
                    INFO(heads << " heads on " << w << "x" << h << (park ? "" : ", no Park") << ": "
                               << mode_name(l.mode) << " " << l.cell_w << "x" << l.cell_h);
                    if (l.mode == ToolSwitcherMode::Single && (w < kMin || h < kMin)) {
                        SAMPLE_REQUIRE(samples, l.cell_w == w, heads * 100000 + w * 400 + h);
                        SAMPLE_REQUIRE(samples, l.cell_h == h, heads * 100000 + w * 400 + h);
                        continue;
                    }
                    SAMPLE_REQUIRE(samples, l.cell_w >= kMin, heads * 100000 + w * 400 + h);
                    SAMPLE_REQUIRE(samples, l.cell_h >= kMin, heads * 100000 + w * 400 + h);
                    SAMPLE_REQUIRE(samples, l.rail_h <= h, heads * 100000 + w * 400 + h);
                }
            }
        }
    }
}

TEST_CASE("Tool switcher layout: a detail block fits under a tall rail", "[tool_switcher][1784]") {
    SECTION("2x2 at 800x480 with five heads") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x2);
        REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
        CHECK(l.detail);
        CHECK(l.rail_h < k800_2x2.h);
        CHECK(l.rail_h == l.rows * l.cell_h + (l.rows - 1) * kGap);
        CHECK(l.cell_h >= kMin);
        CHECK(k800_2x2.h - l.rail_h >= 2.5 * kFonts800.small);
        // Two rows within half of 211 px: (105 - 2) / 2 = 51, under the
        // 18 * 3.4 = 61 cap.
        CHECK(l.cell_h == 51);
        CHECK(l.rail_h == 104);
    }
    SECTION("2x2 at 800x480 with one head: the row stops at body * 3.4") {
        // Half the box would allow 105 px; the body font caps the row at 61.
        const ToolSwitcherLayout l = layout_for(1, k800_2x2);
        REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
        REQUIRE(l.rows == 1);
        CHECK(l.detail);
        CHECK(l.cell_h == kFonts800.body * 34 / 10);
        CHECK(l.rail_h == 61);
    }
    SECTION("2x1 at 800x480 with five heads") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x1);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k800_2x1.h);
    }
    SECTION("a rail with no room left under it fills the box") {
        // 4 heads at 480x272 2x2 need two rows: even 48 px rows leave 20 px,
        // under the 25 px of 2.5 small lines.
        const ToolSwitcherLayout l = layout_for(4, k480_2x2, kFonts480);
        REQUIRE(l.mode == ToolSwitcherMode::Rail);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k480_2x2.h);
        CHECK(l.cell_h == (118 - kGap) / 2);
    }
    SECTION("a one-row-tall tile never shows detail, however much the rail could shrink") {
        // 2 heads + Park at 214x93: 48 px rows would leave 45 px, enough for
        // 2.5 lines of 16 px text, but 93 px is under two touch rows.
        const ToolSwitcherLayout l = layout_for(2, k800_2x1);
        REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k800_2x1.h);
        CHECK(l.cell_h == k800_2x1.h);
    }
    SECTION("the tall-box gate sits at two touch rows") {
        // One head + Park on a 130 px wide box: a single 48 px rail row.
        const ToolSwitcherLayout at = decide_tool_switcher_layout(
            1, 130, 2 * kMin, kMin, kGap, kFonts480.body, kFonts480.small, kPark);
        REQUIRE(at.mode == ToolSwitcherMode::RailWithActions);
        CHECK(at.detail);
        CHECK(at.rail_h == kMin);
        const ToolSwitcherLayout below = decide_tool_switcher_layout(
            1, 130, 2 * kMin - 1, kMin, kGap, kFonts480.body, kFonts480.small, kPark);
        REQUIRE(below.mode == ToolSwitcherMode::RailWithActions);
        CHECK_FALSE(below.detail);
        CHECK(below.rail_h == 2 * kMin - 1);
    }
    SECTION("480x272 2x2: detail exactly when the leftover holds 2.5 small lines") {
        // One head + Park on 130x118 is one 48 px row, leaving 70 px.
        const int leftover = k480_2x2.h - kMin;
        REQUIRE(leftover == 70);
        const ToolSwitcherLayout fits =
            decide_tool_switcher_layout(1, k480_2x2.w, k480_2x2.h, kMin, kGap, 12, 28, kPark);
        REQUIRE(fits.mode == ToolSwitcherMode::RailWithActions);
        CHECK(fits.detail); // 2.5 * 28 = 70
        CHECK(fits.rail_h == kMin);
        const ToolSwitcherLayout short_by_one =
            decide_tool_switcher_layout(1, k480_2x2.w, k480_2x2.h, kMin, kGap, 12, 29, kPark);
        CHECK_FALSE(short_by_one.detail); // 2.5 * 29 = 72.5
        CHECK(short_by_one.rail_h == k480_2x2.h);
        CHECK(layout_for(1, k480_2x2, kFonts480).detail);
    }
}

TEST_CASE("Tool switcher detail order: the mounted head leads, the others follow by index",
          "[tool_switcher][1784]") {
    using helix::tool_switcher_detail_order;
    CHECK(tool_switcher_detail_order(4, 2) == std::vector<int>{2, 0, 1, 3});
    CHECK(tool_switcher_detail_order(4, 0) == std::vector<int>{0, 1, 2, 3});
    // An empty carriage puts its own line where the mounted head would be.
    CHECK(tool_switcher_detail_order(3, -1) ==
          std::vector<int>{helix::kToolSwitcherEmptyCarriageRow, 0, 1, 2});
    // An index past the list names no head: no line claims to be mounted.
    CHECK(tool_switcher_detail_order(3, 7) == std::vector<int>{0, 1, 2});
    CHECK(tool_switcher_detail_order(0, -1).empty());
}

TEST_CASE("Tool switcher detail rows: a body line, then small lines, dropped whole",
          "[tool_switcher][1784]") {
    using helix::tool_switcher_detail_rows_that_fit;
    // 20 + 16 + 16 = 52: three rows fit 52 px exactly, not 51.
    CHECK(tool_switcher_detail_rows_that_fit(5, 52, 20, 16) == 3);
    CHECK(tool_switcher_detail_rows_that_fit(5, 51, 20, 16) == 2);
    // The first row is the body line: 19 px holds nothing.
    CHECK(tool_switcher_detail_rows_that_fit(5, 19, 20, 16) == 0);
    // Never more rows than there are.
    CHECK(tool_switcher_detail_rows_that_fit(2, 500, 20, 16) == 2);
}

TEST_CASE("Tool switcher strip: about 9% of the cell, never under 3 px", "[tool_switcher][1784]") {
    using helix::tool_switcher_strip_px;
    CHECK(tool_switcher_strip_px(48) == 4);  // 4.32
    CHECK(tool_switcher_strip_px(93) == 8);  // 8.37
    CHECK(tool_switcher_strip_px(100) == 9); // 9.0
    CHECK(tool_switcher_strip_px(50) == 5);  // 4.5 rounds up
    CHECK(tool_switcher_strip_px(33) == 3);  // 2.97
    CHECK(tool_switcher_strip_px(20) == 3);  // 1.8, held at the floor
    CHECK(tool_switcher_strip_px(0) == 3);
}

// ============================================================================
// Action cells: Park and the feeder
// ============================================================================

namespace {
// An eight-track strip one cell tall: 2x1 plus two more cells of 119 px pitch.
constexpr Box k800_4x1{"800x480 4x1", 214 + 2 * 119, 93};
constexpr Box k480_4x1{"480x272 4x1", 130 + 2 * 72, 52};
} // namespace

TEST_CASE("Tool switcher layout: six heads, Park and the feeder fill one row of a wide strip",
          "[tool_switcher][feeder]") {
    const ToolSwitcherLayout l = layout_with(6, k800_4x1.w, k800_4x1.h, kParkFeeder);
    REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
    CHECK(l.cols == 8);
    CHECK(l.rows == 1);
    CHECK(l.cell_w == (k800_4x1.w - 7 * kGap) / 8);
    CHECK(l.cell_w >= kMin);
    CHECK(l.cell_h == k800_4x1.h);

    // The feeder is a cell of its own: with Park alone the same heads leave
    // one cell's room spare.
    const ToolSwitcherLayout park_only = layout_with(6, k800_4x1.w, k800_4x1.h, kPark);
    REQUIRE(park_only.mode == ToolSwitcherMode::RailWithActions);
    CHECK(park_only.cols == 7);
    CHECK(park_only.cell_w > l.cell_w);
}

TEST_CASE("Tool switcher layout: the feeder cell is counted before a rail is chosen",
          "[tool_switcher][feeder]") {
    // Seven heads plus both actions are nine cells: 48 px each at 452 px.
    REQUIRE(layout_with(7, k800_4x1.w, k800_4x1.h, kParkFeeder).mode ==
            ToolSwitcherMode::RailWithActions);
    // Eight heads plus both are ten: 43 px, under the floor; eight heads
    // alone are 54 px, so the plain rail wins, reaching both actions in the
    // picker.
    const ToolSwitcherLayout eight = layout_with(8, k800_4x1.w, k800_4x1.h, kParkFeeder);
    CHECK(eight.mode == ToolSwitcherMode::Rail);
    CHECK(eight.cols == 8);

    // The same eight heads with Park alone are nine cells and keep Park.
    CHECK(layout_with(8, k800_4x1.w, k800_4x1.h, kPark).mode == ToolSwitcherMode::RailWithActions);
}

TEST_CASE("Tool switcher layout: a narrow tile with a feeder falls back like one without",
          "[tool_switcher][feeder]") {
    // 2x1 at 800x480 holds four segments at most: six heads are a face beside
    // Park, feeder or not.
    const ToolSwitcherLayout both = layout_with(6, k800_2x1.w, k800_2x1.h, kParkFeeder);
    CHECK(both.mode == ToolSwitcherMode::Pair);
    CHECK(both.cell_w == layout_with(6, k800_2x1.w, k800_2x1.h, kPark).cell_w);
    // A feeder with no Park has no Pair to make: the face takes the box.
    const ToolSwitcherLayout feeder = layout_with(6, k800_2x1.w, k800_2x1.h, kFeederOnly);
    CHECK(feeder.mode == ToolSwitcherMode::Single);
    CHECK(feeder.cell_w == k800_2x1.w);
    CHECK(feeder.cell_h == k800_2x1.h);
}

TEST_CASE("Tool switcher layout: a feeder with no Park still earns its rail cell",
          "[tool_switcher][feeder]") {
    // Two heads and the feeder are three cells on 214 px.
    const ToolSwitcherLayout l = layout_with(2, k800_2x1.w, k800_2x1.h, kFeederOnly);
    REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
    CHECK(l.cols == 3);
}

TEST_CASE("Tool switcher layout: a 2x2 tile wraps six heads, Park and the feeder and keeps detail",
          "[tool_switcher][feeder]") {
    // Eight cells on 214x211: one row is 25 px wide; two rows of four are 52 px
    // wide, leaving room for the detail block under them.
    const ToolSwitcherLayout l = layout_with(6, k800_2x2.w, k800_2x2.h, kParkFeeder);
    REQUIRE(l.mode == ToolSwitcherMode::RailWithActions);
    CHECK(l.cols == 4);
    CHECK(l.rows == 2);
    CHECK(l.cell_w == (k800_2x2.w - 3 * kGap) / 4);
    CHECK(l.cell_w >= kMin);
    CHECK(l.cell_h >= kMin);
    CHECK(l.detail);
    CHECK(l.rail_h == 2 * l.cell_h + kGap);
}

TEST_CASE("Tool switcher layout: no action offered decides exactly as before",
          "[tool_switcher][feeder]") {
    for (int heads = 1; heads <= 12; heads++) {
        for (const Box box : {k800_1x1, k800_2x1, k800_2x2, k800_4x1, k480_2x2, k480_4x1}) {
            const ToolSwitcherLayout none = layout_with(heads, box.w, box.h, {});
            const ToolSwitcherLayout no_park = layout_for(heads, box, kFonts800, false);
            INFO(heads << " heads, " << box.name);
            CHECK(none.mode == no_park.mode);
            CHECK(none.mode != ToolSwitcherMode::RailWithActions);
            CHECK(none.cols == no_park.cols);
            CHECK(none.cell_w == no_park.cell_w);
        }
    }
}

// ============================================================================
// The picker grid
// ============================================================================

TEST_CASE("Tool picker: at most three columns up to six heads, four beyond, rows balanced",
          "[tool_switcher][picker]") {
    using helix::tool_picker_columns;
    // The fewest rows the cap allows, then the fewest columns that fill them:
    // 4 is 2x2 rather than 3+1, 9 is 3x3 rather than 4+4+1.
    const int want[13] = {0, 1, 2, 3, 2, 3, 3, 4, 4, 3, 4, 4, 4};
    for (int heads = 1; heads <= 12; heads++) {
        INFO(heads << " heads");
        CHECK(tool_picker_columns(heads) == want[heads]);
    }
}

TEST_CASE("Tool picker: up to eight heads never scroll on a full screen",
          "[tool_switcher][picker]") {
    using helix::fit_tool_picker_grid;
    // 800x480 less the 10 px margins, the card's padding and a header and
    // footer of about 110 px.
    for (int heads = 1; heads <= 8; heads++) {
        const auto g = fit_tool_picker_grid(heads, 760, 350, kMin, kGap, 72);
        INFO(heads << " heads: " << g.cols << "x" << g.rows << " of " << g.cell);
        CHECK_FALSE(g.scrolls);
        CHECK(g.cell == 72);
        CHECK(g.cols == helix::tool_picker_columns(heads));
        CHECK(g.rows == (heads + g.cols - 1) / g.cols);
        CHECK(g.width == g.cols * 72 + (g.cols - 1) * kGap);
        CHECK(g.max_height == g.height);
    }
    // Twelve heads are three rows of four, which still fit at 72 px.
    const auto twelve = fit_tool_picker_grid(12, 760, 350, kMin, kGap, 72);
    CHECK(twelve.cols == 4);
    CHECK(twelve.rows == 3);
    CHECK_FALSE(twelve.scrolls);
}

TEST_CASE("Tool picker: a small screen shrinks the cells to the floor, then scrolls",
          "[tool_switcher][picker]") {
    using helix::fit_tool_picker_grid;
    // 480x272: about 440 px wide and 120 px for the grid.
    SECTION("six heads shrink to fit two rows") {
        const auto g = fit_tool_picker_grid(6, 440, 120, kMin, kGap, 72);
        CHECK(g.cols == 3);
        CHECK(g.rows == 2);
        CHECK(g.cell == (120 - kGap) / 2);
        CHECK(g.cell >= kMin);
        CHECK_FALSE(g.scrolls);
    }
    SECTION("twelve heads hold the floor and scroll whole rows") {
        const auto g = fit_tool_picker_grid(12, 440, 120, kMin, kGap, 72);
        CHECK(g.cols == 4);
        CHECK(g.rows == 3);
        CHECK(g.cell == kMin);
        CHECK(g.scrolls);
        CHECK(g.max_height == 2 * kMin + kGap);
        CHECK(g.height == 3 * kMin + 2 * kGap);
    }
    SECTION("a narrow box drops columns rather than cells under the floor") {
        const auto g = fit_tool_picker_grid(6, 2 * kMin + kGap, 400, kMin, kGap, 72);
        CHECK(g.cols == 2);
        CHECK(g.rows == 3);
        CHECK(g.cell == kMin);
    }
    SECTION("no cell is ever under the floor") {
        for (int heads = 1; heads <= 12; heads++) {
            for (int w = 40; w <= 500; w += 23) {
                for (int h = 40; h <= 400; h += 23) {
                    const auto g = fit_tool_picker_grid(heads, w, h, kMin, kGap, 72);
                    INFO(heads << " heads on " << w << "x" << h);
                    REQUIRE(g.cell >= kMin);
                    REQUIRE(g.cols * g.rows >= heads);
                }
            }
        }
    }
    CHECK(fit_tool_picker_grid(0, 440, 150, kMin, kGap, 72).cols == 0);
}

// ============================================================================
// The filament band and the cell state
// ============================================================================

TEST_CASE("Tool switcher band: labelled where a small line fits, else a plain bar",
          "[tool_switcher][band]") {
    using helix::tool_switcher_band;
    // A full-height 800x480 rail cell: 36% of 93 is 33 px, room for a 16 px
    // line and 2 px either side.
    const auto tall = tool_switcher_band(93, 16, 2);
    CHECK(tall.labelled);
    CHECK(tall.px == 33);
    // The threshold: 36% of 56 is 20 = 16 + 2 * 2; of 55 it is 19.
    CHECK(tool_switcher_band(56, 16, 2).labelled);
    CHECK_FALSE(tool_switcher_band(55, 16, 2).labelled);
    // A floor-height cell takes a plain bar about one small line tall,
    // capped at 30% of the cell.
    const auto floor = tool_switcher_band(48, 16, 2);
    CHECK_FALSE(floor.labelled);
    CHECK(floor.px == 14);
    const auto short_font = tool_switcher_band(51, 10, 2);
    CHECK(short_font.labelled); // 18 px >= 14
    // Never thinner than the strip it replaces.
    for (int h = 0; h <= 200; h++) {
        INFO("cell " << h);
        CHECK(tool_switcher_band(h, 16, 2).px >= helix::tool_switcher_strip_px(h));
    }
}

TEST_CASE("Tool switcher cell state: a changer fault marks every head but the mounted one",
          "[tool_switcher][band]") {
    using helix::tool_switcher_cell_state;
    using S = helix::ToolSwitcherCellState;
    CHECK(tool_switcher_cell_state(1, 1, false) == S::Mounted);
    CHECK(tool_switcher_cell_state(0, 1, false) == S::Idle);
    CHECK(tool_switcher_cell_state(1, 1, true) == S::Mounted);
    CHECK(tool_switcher_cell_state(0, 1, true) == S::Fault);
    CHECK(tool_switcher_cell_state(2, 1, true) == S::Fault);
    // An empty carriage has no head to spare.
    CHECK(tool_switcher_cell_state(0, -1, true) == S::Fault);
    CHECK(tool_switcher_cell_state(0, -1, false) == S::Idle);
    // The XML compares these ints.
    CHECK(static_cast<int>(S::Mounted) == 1);
    CHECK(static_cast<int>(S::Fault) == 2);
}

TEST_CASE("Tool picker: the preferred cell labels its band", "[tool_switcher][picker]") {
    using helix::tool_picker_preferred_cell;
    using helix::tool_switcher_band;
    // A 16 px small line labels a 72 px cell already: half again the floor.
    CHECK(tool_picker_preferred_cell(48, 16, 2) == 72);
    // A 22 px line needs 26 px of band, so the cell grows to 73.
    const int grown = tool_picker_preferred_cell(48, 22, 2);
    CHECK(grown > 72);
    CHECK(tool_switcher_band(grown, 22, 2).labelled);
    CHECK_FALSE(tool_switcher_band(grown - 1, 22, 2).labelled);
}

TEST_CASE("Tool picker: balancing never adds a row and uses the fewest columns that fill them",
          "[tool_switcher][picker]") {
    using helix::tool_picker_balanced_columns;
    for (int cap = 1; cap <= 4; cap++) {
        for (int heads = 1; heads <= 16; heads++) {
            const int cols = tool_picker_balanced_columns(heads, cap);
            const int rows = (heads + cols - 1) / cols;
            INFO(heads << " heads under " << cap << ": " << cols << " columns");
            CHECK(cols <= cap);
            CHECK(rows == (heads + cap - 1) / cap);
            // One column fewer could not hold every head in those rows.
            CHECK((cols - 1) * rows < heads);
        }
    }
    // A screen too narrow for the cap balances under the columns it has.
    const auto g = helix::fit_tool_picker_grid(5, 2 * kMin + kGap, 400, kMin, kGap, 72);
    CHECK(g.cols == 2);
    CHECK(g.rows == 3);
}

TEST_CASE("feeder_label_form: the fullest label that fits, else the lock alone",
          "[tool_switcher][feeder]") {
    // The action needs 80 px, "Feeder" 44.
    CHECK(helix::feeder_label_form(120, 80, 44) == helix::FeederLabelForm::Long);
    CHECK(helix::feeder_label_form(80, 80, 44) == helix::FeederLabelForm::Long);
    CHECK(helix::feeder_label_form(79, 80, 44) == helix::FeederLabelForm::Short);
    CHECK(helix::feeder_label_form(44, 80, 44) == helix::FeederLabelForm::Short);
    CHECK(helix::feeder_label_form(43, 80, 44) == helix::FeederLabelForm::Icon);
    CHECK(helix::feeder_label_form(0, 80, 44) == helix::FeederLabelForm::Icon);
}
