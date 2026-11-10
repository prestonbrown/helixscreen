// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Both flanges are vented: the front's slots show the filament wound behind them,
// or the dark inside of an empty spool, and a spool too small to read them keeps
// solid flanges. Pixels are read straight from the canvas buffer.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "spool_vents.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

namespace {

lv_color32_t px(lv_obj_t* canvas, int32_t x, int32_t y) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    return reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride)[x];
}

bool reddish(lv_color32_t c) {
    return c.red > c.green + 40 && c.red > c.blue + 40;
}

bool grey(lv_color32_t c) {
    return std::abs(c.red - c.green) < 12 && std::abs(c.green - c.blue) < 12;
}

int brightness(lv_color32_t c) {
    return c.red + c.green + c.blue;
}

// An 80 px spool draws slots but no dots. Its front flange is centred at
// x = 51, y = 40 and its back flange at x = 29; the top slot's centre is
// 0.25 * 80 = 20 px above that, and 4 px right of it is solid flange.
constexpr int32_t kSpool = 80;
constexpr int32_t kSlotX = 51;
constexpr int32_t kSlotY = 20;
constexpr int32_t kFlangeX = 55;
constexpr int32_t kBackSlotX = 29;

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: a full spool shows its filament through the slots",
                 "[spool_canvas][spool_vents]") {
    REQUIRE(helix::ui::spool_vent_detail(kSpool) == helix::ui::SpoolVentDetail::Slots);
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    CHECK(reddish(px(canvas, kSlotX, kSlotY)));
    CHECK(grey(px(canvas, kFlangeX, kSlotY)));
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: an empty spool's slots are dark, front and back",
                 "[spool_canvas][spool_vents]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 0.02f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    const lv_color32_t slot = px(canvas, kSlotX, kSlotY);
    CHECK(grey(slot));
    CHECK(brightness(slot) + 60 < brightness(px(canvas, kFlangeX, kSlotY)));
    CHECK(brightness(px(canvas, kBackSlotX, kSlotY)) + 40 <
          brightness(px(canvas, kBackSlotX + 4, kSlotY)));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: an empty spool shows its core between the flanges",
                 "[spool_canvas]") {
    // An 80 px spool's back flange is centred at x = 26; the core spans
    // y = 32..48 there, and the front flange's left edge is at x = 39. A
    // nearly empty spool of the same colour renders first, so a cached render
    // of it must not stand in for the empty one.
    lv_obj_t* low = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(low != nullptr);
    ui_spool_canvas_set_color(low, lv_color_hex(0xD02020));
    ui_spool_canvas_set_fill_level(low, 0.02f);
    REQUIRE(reddish(px(low, 32, 40)));

    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 0.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    const lv_color32_t core = px(canvas, 32, 40);
    const lv_color32_t back_flange = px(canvas, 32, 26);
    CHECK(grey(core));
    CHECK(brightness(core) > brightness(back_flange) + 30);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: a small spool keeps a solid flange",
                 "[spool_canvas][spool_vents]") {
    // 28 px: the front flange is centred at (18, 14), its top slot 7 px above.
    REQUIRE(helix::ui::spool_vent_detail(28) == helix::ui::SpoolVentDetail::None);
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 28);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    CHECK(grey(px(canvas, 18, 7)));
}

// The front view looks at the spool face-on, scaled so its flange fills the
// canvas: an 80 px spool's flange has radius 39 and the vent geometry's size
// is 39 / 0.42. The top slot's centre is near (40, 17), (67, 40) is flange
// between the 0 degree neighbours, (19, 28) the upper-left slot and (60, 51)
// the lower-right one.
namespace {
constexpr int32_t kFrontSlotX = 40;
constexpr int32_t kFrontSlotY = 17;
constexpr int32_t kFrontWebX = 67;
constexpr int32_t kFrontWebY = 40;

lv_obj_t* front_spool(lv_obj_t* parent, float fill) {
    lv_obj_t* canvas = ui_spool_canvas_create(parent, kSpool);
    REQUIRE(canvas != nullptr);
    helix::ui::spool_canvas_set_view(canvas, helix::ui::SpoolView::Front);
    REQUIRE(helix::ui::spool_canvas_get_view(canvas) == helix::ui::SpoolView::Front);
    ui_spool_canvas_set_fill_level(canvas, fill);
    return canvas;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: the front view smokes the flange over the filament",
                 "[spool_canvas][spool_vents]") {
    lv_obj_t* canvas = front_spool(test_screen(), 1.0f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    const lv_color32_t slot = px(canvas, kFrontSlotX, kFrontSlotY);
    const lv_color32_t smoke = px(canvas, kFrontWebX, kFrontWebY);
    CHECK(reddish(slot));
    CHECK(smoke.red > smoke.green + 15); // the filament reads through the smoke
    CHECK(slot.red > smoke.red);         // and reads clearest through a slot
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: the front view's slots go dark past the filament",
                 "[spool_canvas][spool_vents]") {
    lv_obj_t* canvas = front_spool(test_screen(), 0.1f);
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xD02020));

    CHECK(grey(px(canvas, kFrontSlotX, kFrontSlotY)));
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: a front view keeps the multi-colour split",
                 "[spool_canvas][spool_vents]") {
    lv_obj_t* canvas = front_spool(test_screen(), 1.0f);
    helix::ui::spool_canvas_set_filament(canvas, 0xFF0000, "#FF0000,#0000FF",
                                         helix::ui::SwatchFinish::Plain);

    const lv_color32_t upper_left = px(canvas, 19, 28);
    const lv_color32_t lower_right = px(canvas, 60, 51);
    CHECK(upper_left.red > upper_left.blue + 40);
    CHECK(lower_right.blue > lower_right.red + 40);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: the view is part of the rendered spool",
                 "[spool_canvas][spool_vents]") {
    // Same size, colour and fill: a side render cached first must not be
    // served to the front view.
    lv_obj_t* side = ui_spool_canvas_create(test_screen(), kSpool);
    REQUIRE(side != nullptr);
    ui_spool_canvas_set_fill_level(side, 1.0f);
    ui_spool_canvas_set_color(side, lv_color_hex(0xD02020));

    lv_obj_t* front = front_spool(test_screen(), 1.0f);
    ui_spool_canvas_set_color(front, lv_color_hex(0xD02020));

    // (67, 40) is solid flange from the side and smoked filament face-on.
    CHECK(grey(px(side, kFrontWebX, kFrontWebY)));
    CHECK_FALSE(grey(px(front, kFrontWebX, kFrontWebY)));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "spool_canvas: the key light reaches the tube floor short of the rim's shadow",
                 "[spool_canvas][spool_vents]") {
    // 240 px: the hub hole is centred at (155, 120), 10 px across and 24 px
    // tall. Six rows below the axis, the right of the hole sees the tube wall
    // 2 px deep, in the light; the left sees it 18 px deep, past the 12 px the
    // 45 degree light reaches before the top rim shades it.
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 240);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);

    const lv_color32_t lit = px(canvas, 163, 126);
    const lv_color32_t shaded = px(canvas, 147, 126);
    CHECK(brightness(lit) > brightness(shaded) + 60);
}
