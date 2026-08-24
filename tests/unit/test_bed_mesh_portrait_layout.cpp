// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_portrait_layout.cpp
 * @brief Canvas sizing for the stacked portrait bed mesh layout
 *
 * Portrait stacks canvas / mesh info / profiles in one column. The canvas and
 * the profiles list would both flex_grow, so the split is decided here instead
 * of left to flex. Square is the target; wider-than-tall is acceptable when
 * vertical room is short, because a mesh plot squashed horizontally still
 * reads and one squashed vertically does not.
 */

#include "bed_mesh_portrait_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Canvas is square when there is room", "[bed-mesh][portrait][layout]") {
    // 480px wide, 700px of column to spend: square (480) fits with room left
    // over for the info card and profiles. Nothing else consumed the column
    // here, so avail_h == column_h.
    CHECK(bed_mesh_portrait_canvas_height(480, 700, 700) == 480);
}

TEST_CASE("Canvas goes wider than tall when vertical room is short",
          "[bed-mesh][portrait][layout]") {
    // 480px wide but only 400px of column, all of it available (nothing else
    // in the column). A square canvas would consume the whole thing and leave
    // nothing for info or profiles, so it flattens.
    const int32_t h = bed_mesh_portrait_canvas_height(480, 400, 400);
    CHECK(h < 480); // wider than tall
    CHECK(h > 0);
}

TEST_CASE("Floor share is vacuous when column_h == avail_h", "[bed-mesh][portrait][layout]") {
    // This is the shape of the ORIGINAL (2-parameter) floor test, kept because
    // it is still a true statement — but per the fix-round finding, it never
    // exercises the floor actually binding: when nothing else has consumed the
    // column, min(0.35*avail_h, avail_h) <= avail_h always, so `fitted` (which
    // is <= avail_h) already satisfies the floor. This case is trivially true,
    // not evidence the floor does anything; see the next test for that.
    const int32_t avail = 300;
    const int32_t h = bed_mesh_portrait_canvas_height(480, avail, avail);
    CHECK(h >= avail * BED_MESH_PORTRAIT_CANVAS_MIN_PCT / 100);
}

TEST_CASE("Floor actually binds when the column is mostly consumed elsewhere",
          "[bed-mesh][portrait][layout]") {
    // column_h=1000: the info card, profiles list, and gaps have eaten all but
    // avail_h=200 of it. A plain min(band_w, avail_h) would hand the canvas
    // 200px. The floor is 35% of the WHOLE column (350px), which exceeds what
    // is currently left over, so the canvas must claim more than avail_h and
    // push back against the other blocks — that push-back is the floor's
    // entire reason to exist. band_w=480 is wide enough that the square
    // ceiling does not interfere (480 > 350).
    const int32_t h = bed_mesh_portrait_canvas_height(480, 200, 1000);
    CHECK(h == 350);
    CHECK(h > 200); // exceeds the naive leftover-only fit
}

TEST_CASE("Ceiling still dominates the floor on a narrow band", "[bed-mesh][portrait][layout]") {
    // band_w=50 is narrow enough that 35% of a 1000px column (350) would blow
    // way past square. The ceiling (square=50) must win: both rules are
    // "satisfiable" only in the sense that the ceiling constrains the floor,
    // never the reverse. This is the case that would regress if the cap on
    // `effective_floor` were ever removed.
    CHECK(bed_mesh_portrait_canvas_height(50, 1000, 1000) == 50);
}

TEST_CASE("Canvas never exceeds its own width", "[bed-mesh][portrait][layout]") {
    // Taller-than-square wastes vertical room the profiles list needs and makes
    // the plot no more readable. Square is the ceiling, even with a roomy
    // column (2000px) that would otherwise inflate the floor past band_w.
    CHECK(bed_mesh_portrait_canvas_height(480, 2000, 2000) == 480);
    CHECK(bed_mesh_portrait_canvas_height(320, 2000, 2000) == 320);
}

TEST_CASE("Degenerate inputs return 0 rather than a garbage size", "[bed-mesh][portrait][layout]") {
    // Called from on_size_changed() before the first layout pass, when the
    // measured width is still 0. 0 means "cannot decide" — the caller leaves
    // the XML default in place, matching print_status_layout_decision.h.
    CHECK(bed_mesh_portrait_canvas_height(0, 700, 700) == 0);
    CHECK(bed_mesh_portrait_canvas_height(480, 0, 700) == 0);
    CHECK(bed_mesh_portrait_canvas_height(480, 700, 0) == 0);
    CHECK(bed_mesh_portrait_canvas_height(-1, -1, -1) == 0);
}
