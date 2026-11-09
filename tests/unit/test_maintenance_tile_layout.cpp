// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_maintenance_tile_layout.cpp
 * @brief decide_maintenance_tile_layout(): the rung, whether the total line shows, and
 * the ring's diameter, at their boundaries.
 *
 * Run with: ./build/bin/helix-tests "[maintenance][layout]"
 */

#include "src/ui/panel_widgets/maintenance_tile_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

MaintenanceTileLayoutInput input(int w, int h) {
    MaintenanceTileLayoutInput in;
    in.content_w = w;
    in.content_h = h;
    in.needs.glance = rung_box(40, 40);
    in.needs.strip = rung_box(90, 16);
    in.needs.column = rung_box(40, 58);
    in.needs.cell = rung_box(56, 76);
    in.needs.wide = rung_box(170, 60);
    in.needs.full = rung_box(200, 180);
    in.caption_h = 18;
    in.list_w = 110;
    in.total_line_w = 120;
    in.total_line_h = 16;
    return in;
}

} // namespace

TEST_CASE("maintenance tile: each rung at its box, the next one down a pixel short",
          "[maintenance][layout]") {
    struct Row {
        int w, h;
        SizeRung rung;
    };
    const Row rows[] = {
        {40, 40, SizeRung::Glance}, {39, 40, SizeRung::Glance}, {90, 16, SizeRung::Strip},
        {40, 58, SizeRung::Column}, {56, 76, SizeRung::Cell},   {55, 76, SizeRung::Column},
        {170, 76, SizeRung::Wide},  {169, 76, SizeRung::Cell},  {200, 180, SizeRung::Full},
        {200, 179, SizeRung::Wide},
    };
    for (const Row& r : rows) {
        INFO(r.w << "x" << r.h);
        CHECK(decide_maintenance_tile_layout(input(r.w, r.h)).rung == r.rung);
    }
}

TEST_CASE("maintenance tile: fits once any rung holds", "[maintenance][layout]") {
    CHECK(decide_maintenance_tile_layout(input(40, 40)).fits);
    CHECK(decide_maintenance_tile_layout(input(90, 16)).fits);
    CHECK_FALSE(decide_maintenance_tile_layout(input(39, 39)).fits);
    CHECK_FALSE(decide_maintenance_tile_layout(input(89, 15)).fits);
}

TEST_CASE("maintenance tile: the total line shows at Full only, whole or not at all",
          "[maintenance][layout]") {
    CHECK(decide_maintenance_tile_layout(input(200, 196)).show_total);
    CHECK_FALSE(decide_maintenance_tile_layout(input(200, 195)).show_total);
    MaintenanceTileLayoutInput wide_line = input(200, 196);
    wide_line.total_line_w = 201;
    CHECK_FALSE(decide_maintenance_tile_layout(wide_line).show_total);
    CHECK_FALSE(decide_maintenance_tile_layout(input(400, 179)).show_total);
}

TEST_CASE("maintenance tile: the ring takes the largest square its rung leaves",
          "[maintenance][layout]") {
    CHECK(decide_maintenance_tile_layout(input(50, 44)).ring_d == 44);
    CHECK(decide_maintenance_tile_layout(input(44, 50)).ring_d == 44);
    // Column and Cell keep the name line under it.
    CHECK(decide_maintenance_tile_layout(input(100, 90)).ring_d == 72);
    CHECK(decide_maintenance_tile_layout(input(60, 120)).ring_d == 60);
    // Wide leaves the rows their width beside it.
    CHECK(decide_maintenance_tile_layout(input(220, 97)).ring_d == 97);
    CHECK(decide_maintenance_tile_layout(input(180, 97)).ring_d == 70);
    // Strip and Full draw no ring.
    CHECK(decide_maintenance_tile_layout(input(120, 20)).ring_d == 0);
    CHECK(decide_maintenance_tile_layout(input(300, 300)).ring_d == 0);
}

TEST_CASE("maintenance tile: the ranked rows that fit, whatever the rung's budget",
          "[maintenance][layout]") {
    MaintenanceTileLayoutInput in = input(220, 97);
    in.list_row_h = 20;
    in.full_row_h = 30;
    in.row_gap = 4;
    in.full_title_h = 20;
    CHECK(decide_maintenance_tile_layout(in).rung == SizeRung::Wide);
    CHECK(decide_maintenance_tile_layout(in).rows == 4);
    in.content_h = 76;
    CHECK(decide_maintenance_tile_layout(in).rows == 3);
    MaintenanceTileLayoutInput full = in;
    full.content_w = 200;
    full.content_h = 180;
    CHECK(decide_maintenance_tile_layout(full).rung == SizeRung::Full);
    CHECK(decide_maintenance_tile_layout(full).rows == 4);
    // Above the 3-row budget the fourth row needs its own room.
    full.content_h = 150;
    full.needs.full.h = 100;
    CHECK(decide_maintenance_tile_layout(full).rows == 3);
    // Row counts never exceed the slots, and rungs without rows report none.
    full.content_h = 900;
    CHECK(decide_maintenance_tile_layout(full).rows == maintenance::kTileRows);
    CHECK(decide_maintenance_tile_layout(input(50, 44)).rows == 0);
}
