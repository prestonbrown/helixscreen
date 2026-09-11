// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A widget carrying transform_scale must render at the scaled size.
//
// Only the vg_lite and nanovg draw units read the per-draw-task matrix that
// LV_DRAW_TRANSFORM_USE_MATRIX routes transforms through; lv_draw_sw never
// does. Enabling that macro on a software-rendered build therefore sends every
// transformed widget down a path where it draws at its original size and is
// clipped by an inverse-scaled clip area instead — scaling up crops the widget
// and scaling down changes nothing, with no warning on either side.
//
// The guard is behavioural rather than a check on the macro, so it also holds
// if a future draw unit reaches the same outcome by a different route.

#include "ui_icon.h"

#include "../lvgl_ui_test_fixture.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

// Count pixels whose alpha is non-zero in an ARGB8888 snapshot of `obj`.
int count_rendered_pixels(lv_obj_t* obj) {
    lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    if (!snap)
        return -1;
    int count = 0;
    const uint8_t* data = snap->data;
    const uint32_t w = snap->header.w;
    const uint32_t h = snap->header.h;
    const uint32_t stride = snap->header.stride ? snap->header.stride : w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = data + y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            if (row[x * 4 + 3] != 0)
                count++;
        }
    }
    lv_draw_buf_destroy(snap);
    return count;
}

// An icon built the way the shipped widget builds one, so the test scales the
// same thing the nav bar scales.
lv_obj_t* nav_style_icon(lv_obj_t* parent) {
    lv_obj_t* icon = lv_label_create(parent);
    helix::ui::icon::set_size(icon, "md");
    helix::ui::icon::set_source(icon, "settings");
    helix::ui::icon::set_color(icon, lv_color_hex(0xFF0000), LV_OPA_COVER);
    lv_obj_center(icon);
    return icon;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "transform_scale changes what a widget renders",
                 "[lvgl][transform][render]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 200, 200);
    lv_obj_center(parent);

    lv_obj_t* square = nav_style_icon(parent);
    lv_obj_update_layout(parent);
    lv_refr_now(nullptr);

    const int unscaled = count_rendered_pixels(parent);
    REQUIRE(unscaled > 0);

    SECTION("scaling up covers more pixels") {
        lv_obj_set_style_transform_pivot_x(square, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(square, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_scale_x(square, 512, LV_PART_MAIN);
        lv_obj_set_style_transform_scale_y(square, 512, LV_PART_MAIN);
        lv_refr_now(nullptr);

        // 2x on both axes is 4x the area. Allow slack for edge sampling, but
        // require a clear majority of the growth so a no-op cannot pass.
        REQUIRE(count_rendered_pixels(parent) > unscaled * 3);
    }

    SECTION("scaling down covers fewer pixels") {
        lv_obj_set_style_transform_pivot_x(square, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(square, LV_PCT(50), LV_PART_MAIN);
        lv_obj_set_style_transform_scale_x(square, 128, LV_PART_MAIN);
        lv_obj_set_style_transform_scale_y(square, 128, LV_PART_MAIN);
        lv_refr_now(nullptr);

        REQUIRE(count_rendered_pixels(parent) < unscaled / 2);
    }

    lv_obj_delete(parent);
}
