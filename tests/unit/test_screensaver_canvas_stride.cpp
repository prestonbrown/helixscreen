// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Canvas draw-buffer stride contract (prestonbrown/helixscreen#1591)
//
// lv_canvas_set_buffer() derives its own row stride (rounded to
// LV_DRAW_BUF_STRIDE_ALIGN) and sizes the canvas extent from stride * h. The
// allocation handed to it must cover that extent, or lv_canvas_fill_bg()
// overruns the buffer. These tests pin the agreement between the screensaver
// allocation and the canvas LVGL actually built.
// ============================================================================

// A width whose tight pitch differs from LVGL's stride at any alignment above
// 1 byte: 630 * 4 = 2520 rounds up to 2528 at a 16-byte align. The Android
// resize path produces such widths on fold/unfold.
namespace {
constexpr int STRAINED_W = 630;
constexpr int TEST_H = 480;

lv_obj_t* find_screensaver_canvas() {
    lv_obj_t* top = lv_layer_top();
    REQUIRE(lv_obj_get_child_count(top) > 0);
    lv_obj_t* overlay = lv_obj_get_child(top, -1); // the screensaver's overlay
    REQUIRE(lv_obj_get_child_count(overlay) >= 1);
    return lv_obj_get_child(overlay, 0); // the canvas
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "starfield canvas allocation covers LVGL's stride-derived extent",
                 "[screensaver][1591]") {
    ScopedResolution res(lv_display_get_default(), STRAINED_W, TEST_H);

    StarfieldScreensaver ss;
    ss.start();
    REQUIRE(ss.is_active());

    lv_draw_buf_t* cbuf = lv_canvas_get_draw_buf(find_screensaver_canvas());
    REQUIRE(cbuf != nullptr);
    // The walk render_frame() uses and the stride lv_canvas_set_buffer()
    // recorded must be the same number.
    REQUIRE(StarfieldScreensaverTestAccess::draw_buf_stride(ss) == cbuf->header.stride);
    // And the allocation must cover the extent LVGL will write.
    REQUIRE(StarfieldScreensaverTestAccess::draw_buf_size(ss) >= cbuf->data_size);

    ss.stop();
    REQUIRE_FALSE(ss.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "pipes canvas allocation covers LVGL's stride-derived extent",
                 "[screensaver][1591]") {
    ScopedResolution res(lv_display_get_default(), STRAINED_W, TEST_H);

    PipesScreensaver ss;
    ss.start();
    REQUIRE(ss.is_active());

    lv_draw_buf_t* cbuf = lv_canvas_get_draw_buf(find_screensaver_canvas());
    REQUIRE(cbuf != nullptr);
    REQUIRE(PipesScreensaverTestAccess::draw_buf_size(ss) >= cbuf->data_size);

    ss.stop();
    REQUIRE_FALSE(ss.is_active());
}

TEST_CASE("screensaver_canvas_stride_bytes matches lv_canvas_set_buffer's stride",
          "[screensaver][1591]") {
    // The helper must agree with LVGL's own computation at exactly the widths
    // where a tight w * 4 pitch diverges from it under a padded-stride config.
    for (int w : {1, 3, 100, 480, 630, 800, 1024}) {
        CAPTURE(w);
        REQUIRE(helix::ui::screensaver_canvas_stride_bytes(w) ==
                lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888));
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
