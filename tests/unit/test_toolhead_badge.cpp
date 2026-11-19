// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The toolhead glyph's bounds and the corner its tool badge takes.

#include "ui_toolhead_badge.h"

#include "../lvgl_test_fixture.h"
#include "filament_tube_stroker.h"
#include "nozzle_renderer_dispatch.h"
#include "toolhead_art.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::ToolheadStyle;
using namespace helix::ui;

namespace {

using DrawFn = void (*)(lv_layer_t*, int32_t, int32_t, std::optional<lv_color_t>, int32_t,
                        lv_opa_t);

struct StyleCase {
    ToolheadStyle style;
    const char* name;
    DrawFn draw;
};

const StyleCase kStyles[] = {
    {ToolheadStyle::DEFAULT, "default", draw_nozzle_bambu},
    {ToolheadStyle::A4T, "a4t", draw_nozzle_a4t},
    {ToolheadStyle::ANTHEAD, "anthead", draw_nozzle_anthead},
    {ToolheadStyle::JABBERWOCKY, "jabberwocky", draw_nozzle_jabberwocky},
    {ToolheadStyle::STEALTHBURNER, "stealthburner", draw_nozzle_stealthburner},
    {ToolheadStyle::CREALITY_K1, "k1", draw_nozzle_creality_k1},
    {ToolheadStyle::CREALITY_K2, "k2", draw_nozzle_creality_k2},
};

// Glyph scales the canvases draw at, micro through 800x480: the detail tool
// scale (2/3 of space_md), the overview row (3/4) and the single nozzle.
constexpr int32_t kScales[] = {6, 7, 8, 10, 12};

// The spool badge at micro and at 800x480 (height, minimum width), inset 2.
struct BadgeSize {
    int32_t h, w;
};
constexpr BadgeSize kSizes[] = {{14, 18}, {16, 22}};
constexpr int32_t kInset = 2;

// Tube gauge 5 plus its halo, either side of the column center.
constexpr int32_t kTubeHalf = (5 + kHaloWidthExtra + 1) / 2;

} // namespace

TEST_CASE("Toolhead badge: the lower-right corner clears the tube on every style",
          "[toolhead_badge]") {
    bool upper_left_overlaps = false;
    for (const StyleCase& sc : kStyles) {
        for (int32_t s : kScales) {
            for (const BadgeSize& b : kSizes) {
                CAPTURE(sc.name, s, b.h, b.w);
                const GlyphBounds g = toolhead_bounds(sc.style, 200, 200, s);
                // The tube runs down to the nozzle inlet, 2 scale units above center.
                const int32_t tube_end = 200 - 2 * s;
                const lv_area_t lr =
                    toolhead_badge_rect(g, b.w, b.h, kInset, BadgeCorner::LowerRight);
                CHECK(badge_clears_tube(lr, 200, tube_end, kTubeHalf));
                // Overhanging the corner, like the spool badge on its spool,
                // and clear of the nozzle's center column.
                CHECK(lr.x2 > g.right);
                CHECK(lr.y2 > g.bottom);
                CHECK(lr.x1 <= g.right);
                CHECK(lr.y1 <= g.bottom);
                CHECK(lr.x1 > (g.left + g.right) / 2);

                const lv_area_t ul =
                    toolhead_badge_rect(g, b.w, b.h, kInset, BadgeCorner::UpperLeft);
                upper_left_overlaps |= !badge_clears_tube(ul, 200, tube_end, kTubeHalf);
            }
        }
    }
    // Why the corner is lower right: upper left reaches across the tube somewhere.
    CHECK(upper_left_overlaps);
    CHECK(kToolheadBadgeCorner == BadgeCorner::LowerRight);
}

TEST_CASE("Toolhead badge: the clearance test sees a badge across the tube", "[toolhead_badge]") {
    const lv_area_t across = {195, 170, 216, 185};
    CHECK_FALSE(badge_clears_tube(across, 200, 180, 6));
    CHECK(badge_clears_tube(across, 200, 169, 6));               // below the tube's end
    CHECK(badge_clears_tube({207, 170, 228, 185}, 200, 180, 6)); // beside it
}

TEST_CASE_METHOD(LVGLTestFixture, "Toolhead bounds hold every style's drawn pixels",
                 "[toolhead_badge]") {
    lv_draw_buf_t* buf = lv_draw_buf_create(400, 400, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    lv_canvas_set_draw_buf(canvas, buf);

    for (const StyleCase& sc : kStyles) {
        for (int32_t s : {6, 10, 16}) {
            CAPTURE(sc.name, s);
            lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
            lv_layer_t layer;
            lv_canvas_init_layer(canvas, &layer);
            if (!draw_toolhead_art(&layer, sc.style, 200, 200, std::nullopt, s, LV_OPA_COVER))
                sc.draw(&layer, 200, 200, std::nullopt, s, LV_OPA_COVER);
            lv_canvas_finish_layer(canvas, &layer);

            int32_t x1 = 400, y1 = 400, x2 = -1, y2 = -1;
            for (int32_t y = 0; y < 400; y++) {
                const uint8_t* row = buf->data + y * buf->header.stride;
                for (int32_t x = 0; x < 400; x++) {
                    if (row[x * 4 + 3] > 8) {
                        x1 = std::min(x1, x);
                        x2 = std::max(x2, x);
                        y1 = std::min(y1, y);
                        y2 = std::max(y2, y);
                    }
                }
            }
            REQUIRE(x2 >= 0);
            const GlyphBounds g = toolhead_bounds(sc.style, 200, 200, s);
            // Drawn pixels inside the bounds, and the bounds no more than two
            // pixels outside them, so a corner badge sits on the glyph.
            CHECK(x1 >= g.left - 1);
            CHECK(x2 <= g.right + 1);
            CHECK(y1 >= g.top - 1);
            CHECK(y2 <= g.bottom + 1);
            CHECK(g.left >= x1 - 2);
            CHECK(g.right <= x2 + 2);
            CHECK(g.bottom <= y2 + 2);
        }
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}
