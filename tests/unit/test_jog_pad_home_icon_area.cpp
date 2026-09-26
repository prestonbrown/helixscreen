// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_jog_pad_home_icon_area.cpp
 * @brief Pure geometry test for jog_pad_home_icon_area().
 *
 * lv_draw_label lays a glyph's full line height down from the top of the box
 * it is given, so a box shorter than the line height pushes the glyph past
 * the box's bottom edge. The home icon's label box therefore has to be
 * derived from the font's line height and centred on the ring's centre for
 * the glyph's visual centre to land on the ring's centre at every pad size.
 */

#include "../../include/ui_jog_pad.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

/// A font stand-in carrying only what lv_font_get_line_height() reads; the
/// area function must derive its height from the font, not from the radius.
lv_font_t font_with_line_height(int16_t line_height) {
    lv_font_t font{};
    font.line_height = line_height;
    return font;
}

} // namespace

TEST_CASE("home icon label box follows the font line height", "[jog_pad][motion]") {
    const lv_coord_t cx = 400;
    const lv_coord_t cy = 217;
    const lv_coord_t radius = 20;

    // Line height TALLER than 0.8 * radius (16px): the pairing where a
    // radius-derived box height would clip the glyph and sit it low in the
    // ring. The old half-height was 0.4 * radius = 8; the font-derived one
    // is 9, so these extents go red if the radius creeps back in.
    lv_font_t font = font_with_line_height(18);
    lv_area_t area = helix::jog_pad_home_icon_area(cx, cy, radius, &font);

    REQUIRE(area.y1 == cy - 9);
    REQUIRE(area.y2 == cy + 9);
    REQUIRE((area.y2 - area.y1 + 1) >= 18);

    // Width stays the glyph box the draw callback scales with the ring.
    REQUIRE(area.x1 == cx - 12);
    REQUIRE(area.x2 == cx + 12);

    SECTION("taller line height grows the box symmetrically") {
        lv_font_t tall = font_with_line_height(26);
        lv_area_t grown = helix::jog_pad_home_icon_area(cx, cy, radius, &tall);
        REQUIRE(grown.y1 == cy - 13);
        REQUIRE(grown.y2 == cy + 13);
        REQUIRE(grown.x1 == area.x1);
        REQUIRE(grown.x2 == area.x2);
    }

    SECTION("a line height shorter than the ring still centres the glyph") {
        lv_font_t small = font_with_line_height(10);
        lv_area_t box = helix::jog_pad_home_icon_area(cx, cy, radius, &small);
        REQUIRE(box.y1 == cy - 5);
        REQUIRE(box.y2 == cy + 5);
    }
}
