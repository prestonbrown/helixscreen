// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_swatch.cpp
 * @brief The swatch painter (include/ui_swatch.h): its pure decisions, its
 *        pixels on a canvas, and the object paint contract.
 */

#include "ui_swatch.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "ams_types.h"
#include "filament_variants.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/draw/lv_draw_private.h"

#include <algorithm>
#include <cmath>

#include "../catch_amalgamated.hpp"

using helix::ui::SwatchFinish;
using helix::ui::SwatchPoint;

namespace {

float dist(const SwatchPoint& p, float cx, float cy) {
    return std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy));
}

// An ARGB8888 canvas the size of one swatch, filled with a known background.
struct SwatchCanvas {
    static constexpr int32_t SIZE = 40;
    lv_draw_buf_t* buf = nullptr;
    lv_obj_t* canvas = nullptr;
    helix::ui::SwatchCache cache;
    int last_tasks = 0; ///< draw tasks the last paint() queued

    explicit SwatchCanvas(lv_obj_t* parent) {
        buf = lv_draw_buf_create(SIZE, SIZE, LV_COLOR_FORMAT_ARGB8888, 0);
        canvas = lv_canvas_create(parent);
        lv_canvas_set_draw_buf(canvas, buf);
    }
    ~SwatchCanvas() {
        lv_obj_delete(canvas);
        lv_draw_buf_destroy(buf);
    }
    void paint(helix::ui::SwatchPaint p) {
        if (!p.cache) {
            p.cache = &cache;
        }
        lv_canvas_fill_bg(canvas, lv_color_hex(0x0000FF), LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        const lv_area_t area = {0, 0, SIZE - 1, SIZE - 1};
        helix::ui::draw_swatch(&layer, area, p);
        last_tasks = 0;
        for (lv_draw_task_t* t = layer.draw_task_head; t; t = t->next) {
            ++last_tasks;
        }
        lv_canvas_finish_layer(canvas, &layer);
    }
    lv_color32_t px(int32_t x, int32_t y) const {
        return lv_canvas_get_px(canvas, x, y);
    }
};

int brightness(lv_color32_t c) {
    return c.red + c.green + c.blue;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure decisions
// ---------------------------------------------------------------------------

TEST_CASE("swatch_colors_need_edge: any colour blending into any surface asks for an edge",
          "[swatch]") {
    const std::vector<uint32_t> dark = {0x1A1B26, 0x24283B};
    CHECK(
        helix::ui::swatch_colors_need_edge({lv_color_hex(0xE53935), lv_color_hex(0x101010)}, dark));
    CHECK_FALSE(
        helix::ui::swatch_colors_need_edge({lv_color_hex(0xE53935), lv_color_hex(0xFFFFFF)}, dark));
    CHECK_FALSE(helix::ui::swatch_colors_need_edge({}, dark));
}

TEST_CASE("parse_multi_colors: '#' optional, blanks trimmed, junk skipped", "[swatch]") {
    const auto c = helix::ui::parse_multi_colors(" #FF0000 , 00FF00 ,zz,, #0000FF");
    REQUIRE(c.size() == 3);
    CHECK(lv_color_eq(c[0], lv_color_hex(0xFF0000)));
    CHECK(lv_color_eq(c[1], lv_color_hex(0x00FF00)));
    CHECK(lv_color_eq(c[2], lv_color_hex(0x0000FF)));
    CHECK(helix::ui::parse_multi_colors("").empty());
}

TEST_CASE("finish_of: a finish is a whole word of the filament name", "[swatch]") {
    using helix::FilamentFinish;
    using Finish = FilamentFinish;
    CHECK(helix::filament_finish_of("PLA Silk") == Finish::Silk);
    CHECK(helix::filament_finish_of("Silk-PLA") == Finish::Silk);
    CHECK(helix::filament_finish_of("silky gold") == Finish::Silk);
    CHECK(helix::filament_finish_of("PLA Satin") == Finish::Silk);
    CHECK(helix::filament_finish_of("PLA Matte") == Finish::Matte);
    CHECK(helix::filament_finish_of("Matt Black") == Finish::Matte);
    CHECK(helix::filament_finish_of("PLA_Marble") == Finish::Marble);
    CHECK(helix::filament_finish_of("Granite Grey") == Finish::Marble);
    CHECK(helix::filament_finish_of("Galaxy Black") == Finish::Sparkle);
    CHECK(helix::filament_finish_of("PETG (Glitter)") == Finish::Sparkle);
    CHECK(helix::filament_finish_of("Sparkle+") == Finish::Sparkle);
    CHECK(helix::filament_finish_of("Silkworm Green") == Finish::None);
    CHECK(helix::filament_finish_of("PLA") == Finish::None);
    CHECK(helix::filament_finish_of("") == Finish::None);
    // The first finish word wins.
    CHECK(helix::filament_finish_of("Matte Silk") == Finish::Matte);
}

TEST_CASE("swatch_finish_of: a slot's finish comes from its first name that has one", "[swatch]") {
    helix::SlotInfo slot;
    slot.material = "PLA";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Plain);
    slot.spool_name = "Galaxy Black";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Sparkle);
    slot.product_name = "PLA Marble";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Marble);
    slot.material = "PLA Silk";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Silk);
}

TEST_CASE("swatch_effective_finish: below the minimum area a finish paints plain", "[swatch]") {
    const int32_t m = helix::ui::kSwatchFinishMinPx;
    CHECK(helix::ui::swatch_effective_finish(SwatchFinish::Silk, m, m) == SwatchFinish::Silk);
    CHECK(helix::ui::swatch_effective_finish(SwatchFinish::Silk, m - 1, m) == SwatchFinish::Plain);
    CHECK(helix::ui::swatch_effective_finish(SwatchFinish::Marble, 8, 8) == SwatchFinish::Plain);
    // A long thin band (a spool's wound body, a tool band) keeps its finish.
    CHECK(helix::ui::swatch_effective_finish(SwatchFinish::Marble, 80, m - 4) ==
          SwatchFinish::Marble);
    CHECK(helix::ui::swatch_effective_finish(SwatchFinish::Sparkle, 80, 40) ==
          SwatchFinish::Sparkle);
}

TEST_CASE("swatch_matte_color: desaturates toward grey, keeps the hue", "[swatch]") {
    const lv_color_t red = helix::ui::swatch_matte_color(lv_color_hex(0xFF0000));
    CHECK(red.red < 255);
    CHECK(red.green > 0);
    CHECK(red.red > red.green * 3);
    const lv_color_t grey = helix::ui::swatch_matte_color(lv_color_hex(0x808080));
    CHECK(lv_color_eq(grey, lv_color_hex(0x808080)));
}

TEST_CASE("swatch_shape_polygon: a circle's outline sits on its radius", "[swatch]") {
    const lv_area_t a = {10, 20, 49, 59}; // 40 x 40
    const auto poly = helix::ui::swatch_shape_polygon(a, LV_RADIUS_CIRCLE);
    REQUIRE(poly.size() > 16);
    for (const auto& p : poly) {
        CHECK(dist(p, 30.0f, 40.0f) == Catch::Approx(20.0f).margin(0.01));
    }
    CHECK(helix::ui::swatch_polygon_area(poly) == Catch::Approx(3.14159f * 400.0f).epsilon(0.02));
    // Radius 0 is the plain rectangle.
    const auto rect = helix::ui::swatch_shape_polygon(a, 0);
    REQUIRE(rect.size() == 4);
    CHECK(helix::ui::swatch_polygon_area(rect) == Catch::Approx(1600.0f));
}

TEST_CASE("swatch_split_polygons: equal-area bands, every vertex inside the shape", "[swatch]") {
    const lv_area_t a = {0, 0, 39, 39};
    for (size_t n : {2u, 3u, 4u}) {
        CAPTURE(n);
        const auto bands = helix::ui::swatch_split_polygons(a, LV_RADIUS_CIRCLE, n);
        REQUIRE(bands.size() == n);
        const float total =
            helix::ui::swatch_polygon_area(helix::ui::swatch_shape_polygon(a, LV_RADIUS_CIRCLE));
        float sum = 0.0f;
        for (const auto& band : bands) {
            const float area = helix::ui::swatch_polygon_area(band);
            CHECK(area == Catch::Approx(total / static_cast<float>(n)).epsilon(0.01));
            sum += area;
            for (const auto& p : band) {
                CHECK(dist(p, 20.0f, 20.0f) <= 20.0f + 0.01f);
            }
        }
        CHECK(sum == Catch::Approx(total).epsilon(0.001));
    }
    // Colour 0 takes the top-left, the last colour the bottom-right.
    const auto two = helix::ui::swatch_split_polygons(a, LV_RADIUS_CIRCLE, 2);
    for (const auto& p : two[0]) {
        CHECK(p.x + p.y <= 40.0f + 0.01f);
    }
    for (const auto& p : two[1]) {
        CHECK(p.x + p.y >= 40.0f - 0.01f);
    }
    // One colour is the whole shape.
    CHECK(helix::ui::swatch_split_polygons(a, 0, 1).size() == 1);
}

TEST_CASE("swatch_split_polygons: a rounded chip's bands stay inside its corners", "[swatch]") {
    const lv_area_t a = {0, 0, 59, 29}; // 60 x 30, radius 8
    const auto bands = helix::ui::swatch_split_polygons(a, 8, 2);
    REQUIRE(bands.size() == 2);
    for (const auto& band : bands) {
        for (const auto& p : band) {
            CHECK(helix::ui::swatch_shape_contains({-1, -1, 60, 30}, 9, p.x, p.y));
        }
    }
}

TEST_CASE("swatch_texture_points: deterministic, inside the shape", "[swatch]") {
    const lv_area_t a = {0, 0, 39, 39};
    const auto p1 = helix::ui::swatch_texture_points(a, LV_RADIUS_CIRCLE, 50, 7);
    const auto p2 = helix::ui::swatch_texture_points(a, LV_RADIUS_CIRCLE, 50, 7);
    REQUIRE(p1.size() == 50);
    REQUIRE(p2.size() == 50);
    for (size_t i = 0; i < p1.size(); ++i) {
        CHECK(p1[i].x == p2[i].x);
        CHECK(p1[i].y == p2[i].y);
        CHECK(dist(p1[i], 20.0f, 20.0f) <= 19.0f + 0.01f);
    }
    const auto p3 = helix::ui::swatch_texture_points(a, LV_RADIUS_CIRCLE, 50, 8);
    CHECK((p3[0].x != p1[0].x || p3[0].y != p1[0].y));
}

TEST_CASE("swatch_texture_points: spread evenly, no bare patches", "[swatch]") {
    // 256 points over 64x64 is four per 8x8 cell on average; random sampling
    // leaves some cells with fewer than two.
    const auto points = helix::ui::swatch_texture_points({0, 0, 63, 63}, 0, 256, 7);
    int cells[8][8] = {};
    for (const auto& p : points) {
        const int cx = std::clamp(static_cast<int>(p.x) / 8, 0, 7);
        const int cy = std::clamp(static_cast<int>(p.y) / 8, 0, 7);
        ++cells[cy][cx];
    }
    int fewest = 1 << 30;
    for (const auto& row : cells) {
        for (int n : row) {
            fewest = std::min(fewest, n);
        }
    }
    CHECK(fewest >= 2);
}

// ---------------------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "draw_swatch: a circle leaves its corners to the background",
                 "[swatch]") {
    SwatchCanvas c(test_screen());
    helix::ui::SwatchPaint p;
    p.colors = {lv_color_hex(0xFF0000)};
    p.edge = false;
    c.paint(p);
    CHECK(c.px(20, 20).red == 255);
    CHECK(c.px(1, 1).blue == 255);
    CHECK(c.px(38, 38).blue == 255);
}

TEST_CASE_METHOD(LVGLTestFixture, "draw_swatch: two colours split along the diagonal", "[swatch]") {
    SwatchCanvas c(test_screen());
    helix::ui::SwatchPaint p;
    p.colors = {lv_color_hex(0xFF0000), lv_color_hex(0x00FF00)};
    p.edge = false;
    c.paint(p);
    const lv_color32_t tl = c.px(12, 12);
    const lv_color32_t br = c.px(28, 28);
    CHECK(tl.red == 255);
    CHECK(tl.green == 0);
    CHECK(br.green == 255);
    CHECK(br.red == 0);
    // The split is clipped to the circle.
    CHECK(c.px(38, 38).blue == 255);
    CHECK(c.px(1, 1).blue == 255);
}

TEST_CASE_METHOD(LVGLTestFixture, "draw_swatch: silk lays a highlight, matte mutes the colour",
                 "[swatch]") {
    SwatchCanvas c(test_screen());
    helix::ui::SwatchPaint p;
    p.colors = {lv_color_hex(0x8040C0)};
    p.edge = false;
    c.paint(p);
    const lv_color32_t plain = c.px(18, 18);

    p.finish = SwatchFinish::Silk;
    c.paint(p);
    CHECK(brightness(c.px(18, 18)) > brightness(plain) + 60);

    p.finish = SwatchFinish::Matte;
    c.paint(p);
    const lv_color32_t matte = c.px(18, 18);
    CHECK(matte.blue - matte.green < plain.blue - plain.green);
}

TEST_CASE_METHOD(LVGLTestFixture, "draw_swatch: marble and sparkle texture a large swatch only",
                 "[swatch]") {
    SwatchCanvas c(test_screen());
    helix::ui::SwatchPaint p;
    p.colors = {lv_color_hex(0x808080)};
    p.edge = false;

    auto varied = [&c]() {
        int differing = 0;
        for (int32_t y = 8; y < 32; ++y) {
            for (int32_t x = 8; x < 32; ++x) {
                if (brightness(c.px(x, y)) != 3 * 0x80) {
                    ++differing;
                }
            }
        }
        return differing;
    };

    c.paint(p);
    CHECK(varied() == 0);
    p.finish = SwatchFinish::Marble;
    c.paint(p);
    CHECK(varied() > 10);
    p.finish = SwatchFinish::Sparkle;
    c.paint(p);
    CHECK(varied() > 5);

    // A dot under the minimum size paints plain whatever its finish.
    lv_canvas_fill_bg(c.canvas, lv_color_hex(0x0000FF), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(c.canvas, &layer);
    const lv_area_t dot = {0, 0, helix::ui::kSwatchFinishMinPx - 2,
                           helix::ui::kSwatchFinishMinPx - 2};
    p.radius = 0;
    p.cache = &c.cache;
    helix::ui::draw_swatch(&layer, dot, p);
    lv_canvas_finish_layer(c.canvas, &layer);
    for (int32_t y = 0; y < helix::ui::kSwatchFinishMinPx - 1; ++y) {
        for (int32_t x = 0; x < helix::ui::kSwatchFinishMinPx - 1; ++x) {
            CHECK(brightness(c.px(x, y)) == 3 * 0x80);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "draw_swatch: a textured swatch is a few draw tasks, its texture rendered once",
                 "[swatch]") {
    SwatchCanvas c(test_screen());
    helix::ui::SwatchPaint p;
    p.colors = {lv_color_hex(0x808080)};
    for (const SwatchFinish finish : {SwatchFinish::Marble, SwatchFinish::Sparkle}) {
        CAPTURE(static_cast<int>(finish));
        helix::ui::SwatchCache cache;
        p.finish = finish;
        p.cache = &cache;
        c.paint(p);
        CHECK(cache.texture_builds() == 1);
        c.paint(p); // the second redraw reuses the texture
        CHECK(cache.texture_builds() == 1);
        // A fill, one texture blit and at most an edge.
        CHECK(c.last_tasks <= 3);
        p.radius = 6; // a new shape renders it again
        c.paint(p);
        CHECK(cache.texture_builds() == 2);
        p.radius = LV_RADIUS_CIRCLE;
    }
}

TEST_CASE("swatch_shape_coverage: whole inside, none outside, partial on the rim", "[swatch]") {
    const lv_area_t a = {0, 0, 39, 39};
    CHECK(helix::ui::swatch_shape_coverage(a, LV_RADIUS_CIRCLE, 20.0f, 20.0f) == 1.0f);
    CHECK(helix::ui::swatch_shape_coverage(a, LV_RADIUS_CIRCLE, 1.5f, 1.5f) == 0.0f);
    const float rim = helix::ui::swatch_shape_coverage(a, LV_RADIUS_CIRCLE, 20.0f, 0.0f);
    CHECK(rim > 0.0f);
    CHECK(rim < 1.0f);
    // A square keeps its corners.
    CHECK(helix::ui::swatch_shape_coverage(a, 0, 0.5f, 0.5f) == 1.0f);
}

TEST_CASE("swatch_edge_surfaces: the actual backdrop when known", "[swatch]") {
    const auto on_white = helix::ui::swatch_edge_surfaces(0xFFFFFF);
    REQUIRE(on_white.size() == 1);
    CHECK(helix::ui::swatch_colors_need_edge({lv_color_hex(0xF8F8F8)}, on_white));
    CHECK_FALSE(helix::ui::swatch_colors_need_edge({lv_color_hex(0xF8F8F8)},
                                                   helix::ui::swatch_edge_surfaces(0x101010)));
    CHECK(helix::ui::swatch_edge_surfaces().size() == 2); // no backdrop: the theme surfaces
}

TEST_CASE("SlotInfo::finish: never from the colour name", "[swatch]") {
    helix::SlotInfo slot;
    slot.material = "PLA";
    slot.color_name = "Stone Grey";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Plain);
    slot.product_name = "PLA Marble";
    CHECK(helix::ui::swatch_finish_of(slot) == SwatchFinish::Marble);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "swatch_paint: a cleared or uncoloured swatch is never left blank by a binding",
                 "[swatch]") {
    static lv_subject_t colors_subject;
    static char colors_buf[64];
    static lv_subject_t color_subject;
    lv_subject_init_string(&colors_subject, colors_buf, nullptr, sizeof(colors_buf),
                           "#FF0000,#0000FF");
    lv_subject_init_color(&color_subject, lv_color_hex(0x00FF00));
    lv_xml_register_subject(nullptr, "test_swatch_colors", &colors_subject);
    lv_xml_register_subject(nullptr, "test_swatch_color", &color_subject);

    // A colours binding falling below two colours keeps the colour C++ set.
    const char* multi_attrs[] = {"bind_colors", "test_swatch_colors", "width", "40", "height", "40",
                                 nullptr};
    auto* multi = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "swatch_paint", multi_attrs));
    REQUIRE(multi != nullptr);
    helix::ui::apply_swatch_color(multi, 0xAA5500, "");
    lv_subject_set_string(&colors_subject, "");
    auto colors = helix::ui::swatch_paint_colors(multi);
    REQUIRE(colors.size() == 1);
    CHECK(lv_color_eq(colors[0], lv_color_hex(0xAA5500)));

    // A colour binding after a clear paints again.
    const char* bound_attrs[] = {"bind_color", "test_swatch_color", "width", "40", "height", "40",
                                 nullptr};
    auto* bound = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "swatch_paint", bound_attrs));
    REQUIRE(bound != nullptr);
    lv_obj_update_layout(bound);
    auto center_alpha = [bound]() {
        lv_draw_buf_t* snap = lv_snapshot_take(bound, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(snap != nullptr);
        const int alpha = (snap->data + 20 * snap->header.stride + 20 * 4)[3];
        lv_draw_buf_destroy(snap);
        return alpha;
    };
    CHECK(center_alpha() == 255);
    helix::ui::clear_swatch_color(bound);
    CHECK(center_alpha() == 0);
    lv_subject_set_color(&color_subject, lv_color_hex(0x0000FF));
    CHECK(center_alpha() == 255);

    lv_obj_delete(multi);
    lv_obj_delete(bound);
    lv_xml_unregister_subject(nullptr, "test_swatch_colors");
    lv_xml_unregister_subject(nullptr, "test_swatch_color");
    lv_subject_deinit(&colors_subject);
    lv_subject_deinit(&color_subject);
}

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "apply_swatch_color: one colour, or the multi-colour list",
                 "[swatch]") {
    lv_obj_t* sw = lv_obj_create(test_screen());

    helix::ui::apply_swatch_color(sw, 0x123456, "");
    REQUIRE(helix::ui::has_swatch_paint(sw));
    auto colors = helix::ui::swatch_paint_colors(sw);
    REQUIRE(colors.size() == 1);
    CHECK(lv_color_eq(colors[0], lv_color_hex(0x123456)));
    // The style background is the painter's; bg_color carries the colour for
    // click handlers.
    CHECK(lv_obj_get_style_bg_opa(sw, LV_PART_MAIN) == LV_OPA_TRANSP);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(sw, LV_PART_MAIN), lv_color_hex(0x123456)));

    // A single listed colour is not a split: the primary paints.
    helix::ui::apply_swatch_color(sw, 0x00FF00, "#AABBCC");
    colors = helix::ui::swatch_paint_colors(sw);
    REQUIRE(colors.size() == 1);
    CHECK(lv_color_eq(colors[0], lv_color_hex(0x00FF00)));

    helix::ui::apply_swatch_color(sw, 0x000000, "#202020,#F0F0F0", SwatchFinish::Silk);
    CHECK(helix::ui::swatch_paint_colors(sw).size() == 2);
    CHECK(helix::ui::swatch_paint_finish(sw) == SwatchFinish::Silk);
}

TEST_CASE_METHOD(LVGLTestFixture, "apply_swatch_color: hooks install once and die with the object",
                 "[swatch]") {
    lv_obj_t* sw = lv_obj_create(test_screen());
    const uint32_t baseline = lv_obj_get_event_count(sw);
    helix::ui::apply_swatch_color(sw, 0x000000, "#202020,#F0F0F0");
    const uint32_t after_first = lv_obj_get_event_count(sw);
    CHECK(after_first > baseline);
    helix::ui::apply_swatch_color(sw, 0x445566, "");
    helix::ui::apply_swatch_color(sw, 0x000000, "#101010,#808080,#FFFFFF");
    CHECK(lv_obj_get_event_count(sw) == after_first);

    lv_obj_delete(sw);
    CHECK_FALSE(helix::ui::has_swatch_paint(sw));

    helix::ui::apply_swatch_color(nullptr, 0x123456, "#202020,#F0F0F0");
}

TEST_CASE_METHOD(LVGLTestFixture, "apply_swatch_color: a slot paints its colours and finish",
                 "[swatch]") {
    lv_obj_t* sw = lv_obj_create(test_screen());
    helix::SlotInfo slot;
    slot.color_rgb = 0xD4AF37;
    slot.multi_color_hexes = "#D4AF37,#C0C0C0,#B87333";
    slot.material = "PLA Silk";
    helix::ui::apply_swatch_color(sw, slot);
    CHECK(helix::ui::swatch_paint_colors(sw).size() == 3);
    CHECK(helix::ui::swatch_paint_finish(sw) == SwatchFinish::Silk);
}

TEST_CASE_METHOD(LVGLTestFixture, "clear_swatch_color: an emptied swatch paints nothing",
                 "[swatch]") {
    lv_obj_t* sw = lv_obj_create(test_screen());
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 40, 40);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_update_layout(sw);

    auto center_alpha = [sw]() {
        lv_draw_buf_t* snap = lv_snapshot_take(sw, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(snap != nullptr);
        const uint8_t* px = snap->data + 20 * snap->header.stride + 20 * 4;
        const int alpha = px[3];
        lv_draw_buf_destroy(snap);
        return alpha;
    };

    helix::ui::apply_swatch_color(sw, 0xFF0000, "");
    CHECK(center_alpha() == 255);
    helix::ui::clear_swatch_color(sw);
    CHECK(center_alpha() == 0);
    helix::ui::apply_swatch_color(sw, 0x00FF00, "");
    CHECK(center_alpha() == 255);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "swatch_paint_finish_pixels: silk sheens the shape corner to corner", "[swatch]") {
    lv_draw_buf_t* buf = lv_draw_buf_create(80, 40, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);
    for (int32_t y = 0; y < 40; ++y) {
        auto* row = reinterpret_cast<lv_color32_t*>(buf->data + y * buf->header.stride);
        for (int32_t x = 0; x < 80; ++x) {
            row[x] = {100, 100, 100, 255};
        }
    }
    helix::ui::swatch_paint_finish_pixels(buf, {0, 0, 79, 39}, 0, helix::ui::SwatchFinish::Silk);
    auto at = [&](int32_t x, int32_t y) {
        return reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride)[x];
    };
    // A tall region (a spool body) needs sheen at its ends, not a band across the middle.
    CHECK(at(3, 2).green > 105);
    CHECK(at(76, 37).green > 105);
    CHECK(at(40, 20).green > at(3, 2).green);
    lv_draw_buf_destroy(buf);
}
