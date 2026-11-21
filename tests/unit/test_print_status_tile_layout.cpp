// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_tile_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
PrintStatusTileLayout L(int w, int h) {
    return decide_print_status_tile_layout(w, h, 8, 48, 96);
}
} // namespace

TEST_CASE("tile layout 724x414 is side by side with a row strip", "[print_status_tile_layout]") {
    auto l = L(724, 414);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK_FALSE(l.strip_stacked);
    CHECK_FALSE(l.checklist_compact);
    CHECK(l.job_w + l.printer_w + 8 == 724);
}

TEST_CASE("tile layout 426x279 is side by side and full", "[print_status_tile_layout]") {
    auto l = L(426, 279);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK_FALSE(l.strip_stacked);
    CHECK_FALSE(l.checklist_compact);
}

TEST_CASE("tile layout 438x224 is compact", "[print_status_tile_layout]") {
    auto l = L(438, 224);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK_FALSE(l.strip_stacked);
    CHECK(l.checklist_compact);
}

TEST_CASE("tile layout 480x610 stacks", "[print_status_tile_layout]") {
    auto l = L(480, 610);
    CHECK(l.arrangement == PrintStatusArrangement::Stacked);
    CHECK(l.strip_stacked);
    CHECK(l.strip_h == 96);
    CHECK(l.job_w == 480);
    CHECK(l.job_h + l.printer_h + 8 == 610 - 96 - 8);
    CHECK(l.job_h > l.printer_h);
}

TEST_CASE("tile layout narrow panels stack the strip and go compact",
          "[print_status_tile_layout]") {
    for (int w : {304, 256}) {
        auto l = L(w, 346);
        CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
        CHECK(l.strip_stacked);
        CHECK(l.checklist_compact);
    }
}

TEST_CASE("tile layout 920x528 is side by side and full", "[print_status_tile_layout]") {
    auto l = L(920, 528);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK_FALSE(l.checklist_compact);
}

TEST_CASE("tile layout stacks only when tall enough", "[print_status_tile_layout]") {
    CHECK(L(400, 499).arrangement == PrintStatusArrangement::SideBySide);
    CHECK(L(400, 500).arrangement == PrintStatusArrangement::Stacked);
    CHECK(L(500, 500).arrangement == PrintStatusArrangement::SideBySide);
}

TEST_CASE("tile layout gives a tall stacked screen the Tune section out of the job tile",
          "[print_status_tile_layout][tune_inline]") {
    // 320x1480's content box: the tiles split 1142 as 623 job, 519 printer.
    const auto without = decide_print_status_tile_layout(304, 1254, 8, 48, 96, false, 0, 0);
    const auto l = decide_print_status_tile_layout(304, 1254, 8, 48, 96, false, 0, 400);
    REQUIRE(l.arrangement == PrintStatusArrangement::Stacked);
    CHECK(l.tune_h == 400);
    CHECK(l.job_h == 623 - 408);
    // The printer picture keeps the height it has without the section.
    CHECK(l.printer_h == 519);
    CHECK(l.printer_h == without.printer_h);
}

TEST_CASE("tile layout shows the Tune section only while the job tile keeps its compact height",
          "[print_status_tile_layout][tune_inline]") {
    // 623 - 170 = 453 to spare: a 445 section and its gap fit, 446 does not.
    CHECK(decide_print_status_tile_layout(304, 1254, 8, 48, 96, false, 0, 445).tune_h == 445);
    const auto too_tall = decide_print_status_tile_layout(304, 1254, 8, 48, 96, false, 0, 446);
    CHECK(too_tall.tune_h == 0);
    CHECK(too_tall.job_h == 623);
}

TEST_CASE("tile layout gives the lane tray its room before the Tune section",
          "[print_status_tile_layout][tune_inline]") {
    // The tray takes 120 + 8 first, leaving the job tile 325 to spare.
    const auto both = decide_print_status_tile_layout(304, 1254, 8, 48, 96, true, 0, 300);
    CHECK(both.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(both.lane_tray_h == 120);
    CHECK(both.tune_h == 300);
    CHECK(both.job_h == 623 - 128 - 308);
    CHECK(both.printer_h == 519);
    // Short of room, Tune drops out and the tray stays.
    const auto tray_only = decide_print_status_tile_layout(304, 1254, 8, 48, 96, true, 0, 330);
    CHECK(tray_only.lane_tray_h == 120);
    CHECK(tray_only.tune_h == 0);
}

TEST_CASE("tile layout leaves the Tune button where the section does not fit",
          "[print_status_tile_layout][tune_inline]") {
    // 480x800's content box: stacked, but the job tile has 96 to spare.
    const auto p480 = decide_print_status_tile_layout(448, 600, 8, 48, 96, false, 0, 300);
    REQUIRE(p480.arrangement == PrintStatusArrangement::Stacked);
    CHECK(p480.tune_h == 0);
    CHECK(p480.job_h + p480.printer_h + 8 == 600 - 96 - 8);
    // 800x480: side by side never shows it, however much room.
    CHECK(decide_print_status_tile_layout(724, 414, 8, 48, 96, false, 0, 100).tune_h == 0);
    // Not measured yet: nothing to reserve.
    CHECK(decide_print_status_tile_layout(304, 1254, 8, 48, 96, false, 0, 0).tune_h == 0);
}

// Ultrawide columns: preview square, info, printer, readouts, buttons.

namespace {
/// The Tune column, when shown, is one more column and one more gap.
int32_t columns_total(const PrintStatusColumns& c, int32_t gap) {
    const int32_t columns = c.preview_w + c.info_w + c.printer_w + c.readouts_w + c.actions_w;
    return c.tune_w > 0 ? columns + c.tune_w + 5 * gap : columns + 4 * gap;
}
} // namespace

TEST_CASE("ultrawide columns at 1480x320 make room for the Tune column",
          "[print_status_tile_layout][ultrawide][tune_inline]") {
    const auto c = decide_print_status_columns(1410, 269, 6, true);
    CHECK(c.tune_w == 295);
    // The printer picture keeps its full width; readouts and preview give
    // way to their floors.
    CHECK(c.printer_w == 376);
    CHECK(c.readouts_w == 188);
    CHECK(c.preview_w == 188);
    CHECK(c.actions_w == 129);
    CHECK(c.info_w == 204);
    CHECK(columns_total(c, 6) == 1410);
}

TEST_CASE("ultrawide columns at 1920x440 cap the Tune column's width",
          "[print_status_tile_layout][ultrawide][tune_inline]") {
    const auto c = decide_print_status_columns(1820, 372, 8, true);
    CHECK(c.tune_w == 300);
    CHECK(c.printer_w == 520);
    CHECK(c.readouts_w == 260);
    CHECK(c.preview_w == 260);
    CHECK(c.actions_w == 178);
    CHECK(c.info_w == 262);
    CHECK(columns_total(c, 8) == 1820);
}

TEST_CASE("ultrawide columns show Tune only while the info column keeps its floor",
          "[print_status_tile_layout][ultrawide][tune_inline]") {
    // Readouts and preview at their floors at 1394: the info column keeps
    // 188, 70% of 269. One pixel less and the Tune button comes back instead.
    const auto fits = decide_print_status_columns(1394, 269, 6, true);
    CHECK(fits.tune_w == 295);
    CHECK(fits.info_w == 188);
    const auto short_by_one = decide_print_status_columns(1393, 269, 6, true);
    CHECK(short_by_one.tune_w == 0);
    CHECK(short_by_one.info_w > 188);
    CHECK(columns_total(short_by_one, 6) == 1393);
}

TEST_CASE("ultrawide columns never narrow the printer picture for Tune",
          "[print_status_tile_layout][ultrawide][tune_inline]") {
    int shown = 0;
    for (int32_t h = 200; h <= 480; h += 20) {
        for (int32_t w = 2 * h; w <= 2600; w += 37) {
            const auto c = decide_print_status_columns(w, h, 8, true);
            if (c.tune_w == 0) {
                continue;
            }
            ++shown;
            INFO(w << "x" << h);
            CHECK(c.printer_w == h * 140 / 100);
            CHECK(c.info_w >= h * 70 / 100);
            CHECK(columns_total(c, 8) == w);
        }
    }
    CHECK(shown > 0);
}

TEST_CASE("ultrawide columns keep their natural widths without a running print",
          "[print_status_tile_layout][ultrawide][tune_inline]") {
    // The room the Tune column would take at 1480x320 goes back to the
    // preview and readouts: there is no column to make room for.
    const auto c = decide_print_status_columns(1410, 269, 6, false);
    CHECK(c.tune_w == 0);
    CHECK(c.preview_w == 269);
    CHECK(c.printer_w == 376);
    CHECK(c.readouts_w == 250);
    CHECK(c.actions_w == 129);
    CHECK(c.info_w == 362);
    CHECK(columns_total(c, 6) == 1410);
}

TEST_CASE("ultrawide columns narrow the printer first to keep the info column",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1700, 372, 8);
    CHECK(c.tune_w == 0);
    CHECK(c.info_w == 372);
    CHECK(c.printer_w == 401);
    CHECK(c.readouts_w == 345);
    CHECK(c.preview_w == 372);
    CHECK(columns_total(c, 8) == 1700);
}

TEST_CASE("ultrawide columns stop each column at its floor",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1200, 340, 8);
    CHECK(c.tune_w == 0);
    CHECK(c.printer_w == 340);
    CHECK(c.readouts_w == 255);
    CHECK(c.preview_w == 255);
    CHECK(c.actions_w == 163);
    CHECK(c.info_w == 155);
    CHECK(columns_total(c, 8) == 1200);
}

TEST_CASE("ultrawide tile layout sizes the job tile as the preview square",
          "[print_status_tile_layout][ultrawide]") {
    const auto l = decide_print_status_ultrawide_layout(1410, 269, 6, false, true);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK(l.job_w == 188);
    CHECK(l.job_h == 269);
    CHECK(l.printer_w == 376);
    CHECK(l.printer_h == 269);
    CHECK(l.columns.info_w == 204);
    CHECK_FALSE(l.checklist_compact);
    CHECK(decide_print_status_ultrawide_layout(900, 160, 6).checklist_compact);
}
