// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_footer_read_need.cpp
 * @brief Unit tests for footer_read_need() — what a G-code footer read is still
 *        needed for on a given open of the print detail view.
 *
 * The footer answers three independent questions: which tools the file uses,
 * what colors they print in, and how many grams each costs. ToolsUsedCache
 * persists only the first. Gating the read on that one cached answer would make
 * a cache hit skip it wholesale, leaving the other two questions unanswered for
 * the life of that open.
 *
 * On a K2 Plus, Moonraker reports filament_type for an OrcaSlicer file and no
 * filament_colors at all, so the footer is the only source of the palette.
 * First open: cache miss, read runs, real colors. Second open with the read
 * skipped: every tool falls back to the neutral stand-in and the chips render
 * "grey dot -> black dot". The viewer parse backfills the palette only in
 * full-load mode, and large files stream instead.
 */

#include "gcode_footer_summary.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::footer_read_need;

TEST_CASE("footer_read_need: a cold open needs everything", "[gcode][footer][need]") {
    const auto need = footer_read_need(/*tools_known=*/false, /*have_palette=*/false,
                                       /*have_grams=*/false);
    CHECK(need.tools);
    CHECK(need.palette);
    CHECK(need.grams);
    CHECK(need.any());
    CHECK(need.full_scan_justified());
}

TEST_CASE("footer_read_need: a cache hit still needs the palette and the grams",
          "[gcode][footer][need]") {
    // THE regression. tools_used came back from the persistent cache; nothing
    // in that cache says anything about colors or grams.
    const auto need = footer_read_need(/*tools_known=*/true, /*have_palette=*/false,
                                       /*have_grams=*/false);
    CHECK_FALSE(need.tools);
    CHECK(need.palette);
    CHECK(need.grams);
    CHECK(need.any()); // <- the read MUST still be issued
}

TEST_CASE("footer_read_need: a cache hit never justifies a whole-file scan",
          "[gcode][footer][need]") {
    // Recovering a palette is worth one small range request, never a
    // hundred-megabyte read. If the footer cannot answer, we go without.
    const auto need = footer_read_need(/*tools_known=*/true, /*have_palette=*/false,
                                       /*have_grams=*/false);
    REQUIRE(need.any());
    CHECK_FALSE(need.full_scan_justified());
}

TEST_CASE("footer_read_need: an unknown tool set justifies the full scan",
          "[gcode][footer][need]") {
    // Even when metadata already supplied the palette, the used-tool set is the
    // one answer the pre-flight gate cannot proceed without.
    const auto need = footer_read_need(/*tools_known=*/false, /*have_palette=*/true,
                                       /*have_grams=*/true);
    CHECK(need.tools);
    CHECK(need.any());
    CHECK(need.full_scan_justified());
}

TEST_CASE("footer_read_need: nothing outstanding issues no read", "[gcode][footer][need]") {
    const auto need = footer_read_need(/*tools_known=*/true, /*have_palette=*/true,
                                       /*have_grams=*/true);
    CHECK_FALSE(need.any());
    CHECK_FALSE(need.full_scan_justified());
}

TEST_CASE("footer_read_need: metadata palette alone still leaves grams outstanding",
          "[gcode][footer][need]") {
    // Moonraker's filament_weights is NOT safe to read as grams (its fallback
    // accepts filament_used, which is millimetres), so a metadata palette never
    // settles the grams question — only the footer's own `[g]` line does.
    const auto need = footer_read_need(/*tools_known=*/true, /*have_palette=*/true,
                                       /*have_grams=*/false);
    CHECK(need.grams);
    CHECK(need.any());
    CHECK_FALSE(need.full_scan_justified());
}
