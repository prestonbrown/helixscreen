// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_toolhead_badge.h"

#include "../lvgl_test_fixture.h"
#include "toolhead_art.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::vector<uint8_t> flat(int32_t w, int32_t h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(w * h * 4));
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = b;
        px[i + 1] = g;
        px[i + 2] = r;
        px[i + 3] = a;
    }
    return px;
}

struct Canvas {
    lv_draw_buf_t* buf;
    lv_obj_t* obj;
    explicit Canvas(lv_obj_t* parent) {
        buf = lv_draw_buf_create(300, 300, LV_COLOR_FORMAT_ARGB8888, 0);
        obj = lv_canvas_create(parent);
        lv_canvas_set_draw_buf(obj, buf);
    }
    ~Canvas() {
        lv_obj_delete(obj);
        lv_draw_buf_destroy(buf);
    }
    bool draw(ToolheadStyle style, std::optional<lv_color_t> filament, int32_t scale) {
        lv_canvas_fill_bg(obj, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(obj, &layer);
        const bool drawn =
            draw_toolhead_art(&layer, style, 150, 150, filament, scale, LV_OPA_COVER);
        lv_canvas_finish_layer(obj, &layer);
        return drawn;
    }
    const uint8_t* at(int32_t x, int32_t y) const {
        return buf->data + y * buf->header.stride + x * 4;
    }
};

} // namespace

TEST_CASE("toolhead art: the tint fills the mask and the shade darkens it", "[toolhead-art]") {
    auto body = flat(1, 1, 0, 0, 0, 0);
    const auto mask = flat(1, 1, 255, 255, 255, 255);
    const auto shade = flat(1, 1, 0, 0, 0, 128);
    toolhead_art_compose(body.data(), mask.data(), shade.data(), 1, 1, lv_color_hex(0xff0000));
    CHECK(body[3] == 255);
    CHECK(body[2] == Catch::Approx(127).margin(2));
    CHECK(body[1] == 0);
}

TEST_CASE("toolhead art: outside the mask the body is untouched", "[toolhead-art]") {
    auto body = flat(1, 1, 10, 20, 30, 255);
    const auto mask = flat(1, 1, 255, 255, 255, 0);
    const auto shade = flat(1, 1, 0, 0, 0, 0);
    toolhead_art_compose(body.data(), mask.data(), shade.data(), 1, 1, lv_color_hex(0xff0000));
    CHECK(body[0] == 10);
    CHECK(body[2] == 30);
}

TEST_CASE("toolhead art: resampling keeps colour and ignores transparent neighbours",
          "[toolhead-art]") {
    auto src = flat(2, 1, 0, 0, 200, 255);
    src[4] = 255; // the transparent pixel's colour must not bleed in
    src[7] = 0;
    std::vector<uint8_t> dst(4);
    toolhead_art_resample(src.data(), 2, 1, dst.data(), 1, 1);
    CHECK(dst[0] == 0);
    CHECK(dst[2] == 200);
    CHECK(dst[3] == Catch::Approx(127).margin(2));
}

TEST_CASE("toolhead art: a style without art keeps its renderer", "[toolhead-art]") {
    CHECK(toolhead_art_for(ToolheadStyle::DEFAULT) == nullptr);
    CHECK(toolhead_art_for(ToolheadStyle::AUTO) == nullptr);
    for (ToolheadStyle s : {ToolheadStyle::STEALTHBURNER, ToolheadStyle::BAMBU,
                            ToolheadStyle::ENDER3_BOWDEN, ToolheadStyle::DRAGONBURNER}) {
        REQUIRE(toolhead_art_for(s) != nullptr);
        CHECK(toolhead_art_for(s)->style == s);
    }
}

TEST_CASE("toolhead art: the frame puts the nozzle tip on the filament column", "[toolhead-art]") {
    const ToolheadArt* art = toolhead_art_for(ToolheadStyle::ENDER3_BOWDEN);
    REQUIRE(art != nullptr);
    const ToolheadArtRect r = toolhead_art_rect(*art, 100, 50, 10);
    CHECK(r.h == 80);
    CHECK(r.y == 10);
    CHECK(r.w == Catch::Approx(80 * art->aspect).margin(1));
    CHECK(100 - r.x == Catch::Approx(r.w * art->tip_x).margin(1));

    const GlyphBounds g = toolhead_bounds(ToolheadStyle::ENDER3_BOWDEN, 100, 50, 10);
    CHECK(g.left == r.x);
    CHECK(g.top == r.y);
    CHECK(g.right == r.x + r.w - 1);
    CHECK(g.bottom == r.y + r.h - 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "toolhead art: redraws reuse one composed image",
                 "[toolhead-art]") {
    Canvas c(test_screen());
    const lv_color_t red = lv_color_hex(0xff0000);
    REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, red, 11));
    const unsigned builds = toolhead_art_build_count();
    for (int i = 0; i < 5; ++i)
        REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, red, 11));
    CHECK(toolhead_art_build_count() == builds);

    REQUIRE(c.draw(ToolheadStyle::STEALTHBURNER, lv_color_hex(0x00ff00), 11));
    CHECK(toolhead_art_build_count() == builds + 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "toolhead art: drawn pixels fill the bounds, tip takes filament",
                 "[toolhead-art]") {
    Canvas c(test_screen());
    const ToolheadStyle style = ToolheadStyle::CREALITY_K2;
    REQUIRE(c.draw(style, lv_color_hex(0x00ff00), 16));
    const GlyphBounds g = toolhead_bounds(style, 150, 150, 16);

    int32_t x1 = 300, y1 = 300, x2 = -1, y2 = -1, green = 0;
    for (int32_t y = 0; y < 300; ++y) {
        for (int32_t x = 0; x < 300; ++x) {
            const uint8_t* p = c.at(x, y);
            if (p[3] <= 8)
                continue;
            x1 = std::min(x1, x);
            x2 = std::max(x2, x);
            y1 = std::min(y1, y);
            y2 = std::max(y2, y);
            green += (p[1] > 110 && p[0] < 60 && p[2] < 60) ? 1 : 0;
        }
    }
    CHECK(x1 >= g.left);
    CHECK(x2 <= g.right);
    CHECK(y1 >= g.top);
    CHECK(y2 <= g.bottom);
    CHECK(g.left >= x1 - 2);
    CHECK(g.right <= x2 + 2);
    CHECK(g.top >= y1 - 2);
    CHECK(g.bottom <= y2 + 2);
    CHECK(green > 4);
}
