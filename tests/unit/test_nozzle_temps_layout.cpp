// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_temps_layout.cpp
 * @brief Unit tests for decide_nozzle_layout() — the pure layout-decision
 *        function behind the Nozzle Temps dashboard widget.
 *
 * decide_nozzle_layout() is deliberately LVGL-free, so these tests need no
 * display, font subsystem, or fixture. They lock in the column-count and
 * label-form behavior: which spelling a given column width gets, and that a
 * column too narrow for either never takes the wider one.
 *
 * Run with: ./build/bin/helix-tests "[nozzle][layout]"
 */

#include "src/ui/panel_widgets/nozzle_layout.h"

#include "../catch_amalgamated.hpp"

using helix::decide_nozzle_layout;
using helix::NozzleLayoutDecision;

TEST_CASE("Narrow 480px-panel 2x1 tile picks two short-label columns", "[nozzle][layout]") {
    // 480px panel, 2x1 tile ≈ 196px usable inner width. Short row fits twice
    // with a gap; long row does NOT fit a 92px column → short labels.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/196, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 2);
    REQUIRE(d.use_long_label == false);
}

TEST_CASE("Wide 1024px-panel 2x1 tile picks two long-label columns", "[nozzle][layout]") {
    // 500px usable → each column ≈ 244px, comfortably wider than long_row_px.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/500, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 2);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Single extruder never splits into two columns", "[nozzle][layout]") {
    // Plenty of width, but only one row → clamp to a single full-width column.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/500, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/1);
    REQUIRE(d.columns == 1);
    // col_w == avail_px == 500 >= 150 → long label fits.
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Too narrow for two short columns falls back to one column", "[nozzle][layout]") {
    // avail (150) < 2*short(90) + gap(12) = 192 → single column.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/150, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    // col_w == 150 >= long_row_px(150) → long label (just fits at the edge).
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Single full-width column shows long label when it fits", "[nozzle][layout]") {
    // Narrow enough to refuse two columns, but the full width holds the long row.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/180, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Single column too narrow for long label uses short label", "[nozzle][layout]") {
    // avail (120) < 2*short+gap → 1 column; col_w(120) < long_row_px(150) → short.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/120, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == false);
}

TEST_CASE("Degenerate zero width returns safe single-column long default", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/0, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Negative width returns safe single-column long default", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/-50, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Zero rows clamps to a single column", "[nozzle][layout]") {
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/500, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/0);
    REQUIRE(d.columns == 1);
}

// --- Exact boundaries, derived from decide_nozzle_layout()'s own arithmetic
// (columns = avail_px >= 2*min(short_row_px, long_row_px) + gap_px;
// use_long_label = col_w >= long_row_px or long_row_px <= short_row_px), not by
// running the function and recording what it printed.

TEST_CASE("Column split at the exact 2*short+gap boundary picks two columns", "[nozzle][layout]") {
    // threshold = 2*short_row_px(90) + gap_px(12) = 192, avail_px == threshold
    // -> the ">=" in decide_nozzle_layout takes the two-column branch.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/192, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 2);
    // col_w = (192 - 12) / 2 = 90 < long_row_px(150) -> short label.
    REQUIRE(d.use_long_label == false);
}

TEST_CASE("One pixel below the split boundary stays a single column", "[nozzle][layout]") {
    // Same threshold (192) as above, avail_px one pixel under it.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/191, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    // col_w == avail_px == 191 >= long_row_px(150) -> long label still fits.
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("One pixel below the long-label boundary uses the short label", "[nozzle][layout]") {
    // Single column (avail_px(149) < split threshold(192)); col_w == avail_px
    // == long_row_px(150) - 1 -> the ">=" in use_long_label just misses.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/149, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/90,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == false);
}

// --- The compact spelling is not narrower in every locale: de renders the
// nozzle name "Düse 1" and the position label "Werkzeug 1".

TEST_CASE("A compact form wider than the long form is never chosen", "[nozzle][layout]") {
    // Single column (avail_px(120) < 2*short(200) + gap(12)); col_w(120) fits
    // neither spelling, and the compact one is the wider of the two.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/120, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/200,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Equal-width forms keep the long label when neither fits", "[nozzle][layout]") {
    // The <= in the width comparison: identical widths are not a reason to swap.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/120, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/150,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("A wider compact form does not pin two columns to the long label", "[nozzle][layout]") {
    // 2*short(100) + gap(12) = 212 <= avail(400) -> two columns of 194px, which
    // fits the 150px long form on its own merits.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/400, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/100,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 2);
    REQUIRE(d.use_long_label == true);
}

TEST_CASE("Two columns are gated on the narrower spelling, not the compact one",
          "[nozzle][layout]") {
    // de: the position label ("Werkzeug 1") is wider than the nozzle name
    // ("Duse 1"), and the nozzle name is what a tight column renders. Gating on
    // the position label would refuse a second column this width does hold:
    // 2*150 + 12 = 312 <= 400, where 2*200 + 12 = 412 does not.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/400, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/200,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 2);
    // col_w = (400 - 12) / 2 = 194 >= long_row_px(150).
    CHECK(d.use_long_label == true);
}

TEST_CASE("One pixel below the narrow-spelling split boundary stays a single column",
          "[nozzle][layout]") {
    // Same shape as above, avail_px one under the 2*150 + 12 = 312 threshold.
    NozzleLayoutDecision d = decide_nozzle_layout(/*avail_px=*/311, /*gap_px=*/12,
                                                  /*long_row_px=*/150, /*short_row_px=*/200,
                                                  /*row_count=*/4);
    REQUIRE(d.columns == 1);
    CHECK(d.use_long_label == true);
}
