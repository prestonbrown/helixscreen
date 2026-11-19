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

// Ultrawide columns: preview square, info, printer, readouts, buttons.

namespace {
int32_t columns_total(const PrintStatusColumns& c, int32_t gap) {
    return c.preview_w + c.info_w + c.printer_w + c.readouts_w + c.actions_w + 4 * gap;
}
} // namespace

TEST_CASE("ultrawide columns at 1480x320 keep every natural width",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1410, 269, 6);
    CHECK(c.preview_w == 269);
    CHECK(c.printer_w == 376);
    CHECK(c.readouts_w == 250);
    CHECK(c.actions_w == 129);
    CHECK(c.info_w == 362);
    CHECK(columns_total(c, 6) == 1410);
}

TEST_CASE("ultrawide columns at 1920x440 keep every natural width",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1820, 372, 8);
    CHECK(c.preview_w == 372);
    CHECK(c.printer_w == 520);
    CHECK(c.readouts_w == 345);
    CHECK(c.actions_w == 178);
    CHECK(c.info_w == 373);
    CHECK(columns_total(c, 8) == 1820);
}

TEST_CASE("ultrawide columns narrow the printer first to keep the info column",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1700, 372, 8);
    CHECK(c.info_w == 372);
    CHECK(c.printer_w == 401);
    CHECK(c.readouts_w == 345);
    CHECK(c.preview_w == 372);
    CHECK(columns_total(c, 8) == 1700);
}

TEST_CASE("ultrawide columns stop each column at its floor",
          "[print_status_tile_layout][ultrawide]") {
    const auto c = decide_print_status_columns(1200, 340, 8);
    CHECK(c.printer_w == 340);
    CHECK(c.readouts_w == 255);
    CHECK(c.preview_w == 255);
    CHECK(c.actions_w == 163);
    CHECK(c.info_w == 155);
    CHECK(columns_total(c, 8) == 1200);
}

TEST_CASE("ultrawide tile layout sizes the job tile as the preview square",
          "[print_status_tile_layout][ultrawide]") {
    const auto l = decide_print_status_ultrawide_layout(1410, 269, 6);
    CHECK(l.arrangement == PrintStatusArrangement::SideBySide);
    CHECK(l.job_w == 269);
    CHECK(l.job_h == 269);
    CHECK(l.printer_w == 376);
    CHECK(l.printer_h == 269);
    CHECK(l.columns.info_w == 362);
    CHECK_FALSE(l.checklist_compact);
    CHECK(decide_print_status_ultrawide_layout(900, 160, 6).checklist_compact);
}
