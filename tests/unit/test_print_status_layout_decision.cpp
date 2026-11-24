// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_layout_decision.cpp
 * @brief Print-status exclude side-list geometry and action-row fit
 *        (prestonbrown/helixscreen#1203)
 */

#include "print_status_layout_decision.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// exclude_side_list_geometry

TEST_CASE("exclude_side_list_geometry: landscape covers the right-hand column",
          "[print-status][portrait][layout-decision]") {
    const auto g = exclude_side_list_geometry(/*portrait=*/false);
    CHECK(g.width_pct == 44); // ~= 4/9, the controls column's share of the row
    CHECK(g.height_pct == 100);
    CHECK_FALSE(g.anchor_bottom);
}

TEST_CASE("exclude_side_list_geometry: portrait covers the bottom of the stack",
          "[print-status][portrait][layout-decision]") {
    // A 44%-wide list anchored right covers 44% of *everything* in a stack and
    // none of the controls fully. Full width, bottom-anchored, sized
    // in px from the measured control stack so the object map stays visible.
    const auto g = exclude_side_list_geometry(/*portrait=*/true, /*controls_h=*/300,
                                              /*content_h=*/1000, /*gap=*/12);
    CHECK(g.width_pct == 100);
    CHECK(g.anchor_bottom);
    CHECK(g.height_px == 312); // the controls plus the column's own gap
}

TEST_CASE("exclude_side_list_geometry: landscape ignores the portrait measurements",
          "[print-status][portrait][layout-decision]") {
    // The 44%/100% rule is derived from flex_grow="4" against "5" and measured
    // pixel-exact. Feeding it measurements must not perturb it.
    const auto bare = exclude_side_list_geometry(/*portrait=*/false);
    const auto measured = exclude_side_list_geometry(/*portrait=*/false, /*controls_h=*/300,
                                                     /*content_h=*/1000, /*gap=*/12);
    CHECK(measured.width_pct == bare.width_pct);
    CHECK(measured.height_pct == bare.height_pct);
    CHECK(measured.anchor_bottom == bare.anchor_bottom);
    CHECK(measured.height_px == 0); // percentage path, never a measured height
}

TEST_CASE("portrait_side_list_height: unmeasured yields 0, not a collapsed list",
          "[print-status][portrait][layout-decision]") {
    // 0 means "fall back to height_pct". A caller that treated it as a height
    // would slide in a zero-tall list and the objects would be unreachable.
    CHECK(portrait_side_list_height(0, 1000, 12) == 0);
    CHECK(portrait_side_list_height(300, 0, 12) == 0);
    CHECK(portrait_side_list_height(-5, 1000, 12) == 0);

    const auto g = exclude_side_list_geometry(/*portrait=*/true, 0, 0, 0);
    CHECK(g.height_px == 0);
    CHECK(g.height_pct == kPortraitSideListFallbackPct);
}

TEST_CASE("portrait_side_list_height: a short control stack still gets a usable list",
          "[print-status][portrait][layout-decision]") {
    // 320x1480: controls are ~14% of the column. Sized naively that is a list
    // too short to show a row; the floor catches it.
    CHECK(portrait_side_list_height(/*controls_h=*/60, /*content_h=*/1300, /*gap=*/12) ==
          kMinSideListHeightPx);
}

TEST_CASE("portrait_side_list_height: a tall control stack never buries the map",
          "[print-status][portrait][layout-decision]") {
    // The list accompanies the object map; it must not become the reason the
    // map cannot be tapped.
    const int32_t content_h = 1000;
    const int32_t ceiling = content_h * kMaxSideListCoveragePct / 100;
    CHECK(portrait_side_list_height(/*controls_h=*/900, content_h, /*gap=*/12) == ceiling);
    CHECK(ceiling < content_h); // something is always left for the map
}

TEST_CASE("portrait_side_list_height: measured room below the card is the ceiling",
          "[print-status][portrait][layout-decision]") {
    // Controls past 55% of the column: the list still covers all of them, up to
    // the card and no further.
    CHECK(portrait_side_list_height(/*controls_h=*/600, /*content_h=*/1000, /*gap=*/12,
                                    /*room_h=*/590) == 590);
    // Short controls are still sized from the controls, not stretched to the card.
    CHECK(portrait_side_list_height(/*controls_h=*/300, /*content_h=*/1000, /*gap=*/12,
                                    /*room_h=*/590) == 312);
    const auto g = exclude_side_list_geometry(/*portrait=*/true, 600, 1000, 12, /*room_h=*/590);
    CHECK(g.height_px == 590);
}

TEST_CASE("portrait_side_list_height: an extremely short column prefers a list over a sliver",
          "[print-status][portrait][layout-decision]") {
    // When the ceiling is below the floor the two clamps disagree. The ceiling
    // wins: covering more of a tiny map beats a list that cannot show a row.
    const int32_t content_h = 200; // ceiling = 110, below kMinSideListHeightPx
    const int32_t ceiling = content_h * kMaxSideListCoveragePct / 100;
    REQUIRE(ceiling < kMinSideListHeightPx);
    CHECK(portrait_side_list_height(/*controls_h=*/10, content_h, /*gap=*/0) == ceiling);
}

TEST_CASE("action_row_fits: side-by-side widths and gaps against the row",
          "[print_status][layout]") {
    const int32_t three[] = {100, 80, 90};
    // 100 + 80 + 90 + 2 gaps of 7 = 284
    CHECK(action_row_fits(284, 7, three, 3));
    CHECK_FALSE(action_row_fits(283, 7, three, 3));
    // One button needs no gap.
    const int32_t one[] = {120};
    CHECK(action_row_fits(120, 50, one, 1));
    CHECK_FALSE(action_row_fits(119, 50, one, 1));
    // An empty row always fits.
    CHECK(action_row_fits(0, 7, nullptr, 0));
}
