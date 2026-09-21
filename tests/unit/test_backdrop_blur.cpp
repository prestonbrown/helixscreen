// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../wait_finish_spy.h"
#include "backdrop_blur.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::detail;

// ============================================================================
// box_blur_argb8888 Tests
// ============================================================================

TEST_CASE("box_blur: 1x1 white pixel stays white", "[backdrop_blur][box_blur]") {
    // ARGB8888: A=0xFF, R=0xFF, G=0xFF, B=0xFF
    uint8_t pixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    box_blur_argb8888(pixel, 1, 1, 3);
    REQUIRE(pixel[0] == 0xFF);
    REQUIRE(pixel[1] == 0xFF);
    REQUIRE(pixel[2] == 0xFF);
    REQUIRE(pixel[3] == 0xFF);
}

TEST_CASE("box_blur: solid color is unchanged", "[backdrop_blur][box_blur]") {
    // 4x4 solid red (ARGB = FF, 00, 00, FF in LVGL ARGB8888 byte order)
    constexpr int w = 4, h = 4;
    std::vector<uint8_t> buf(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = 0xFF; // B
        buf[i * 4 + 1] = 0x00; // G
        buf[i * 4 + 2] = 0x00; // R
        buf[i * 4 + 3] = 0xFF; // A
    }

    auto original = buf;
    box_blur_argb8888(buf.data(), w, h, 3);

    // Every pixel should still be the same solid color
    REQUIRE(buf == original);
}

TEST_CASE("box_blur: smooths sharp edges", "[backdrop_blur][box_blur]") {
    // 8x1 strip: left half black, right half white
    constexpr int w = 8, h = 1;
    std::vector<uint8_t> buf(w * h * 4, 0);
    for (int x = 0; x < w; x++) {
        uint8_t val = (x >= w / 2) ? 0xFF : 0x00;
        buf[x * 4 + 0] = val;  // B
        buf[x * 4 + 1] = val;  // G
        buf[x * 4 + 2] = val;  // R
        buf[x * 4 + 3] = 0xFF; // A
    }

    box_blur_argb8888(buf.data(), w, h, 1);

    // Pixel at the boundary (index 3, last black) should have been pulled up
    // toward white, and pixel at index 4 (first white) pulled down.
    // The boundary pixels should no longer be 0 or 255.
    uint8_t left_of_edge = buf[3 * 4 + 0];  // B channel of pixel 3
    uint8_t right_of_edge = buf[4 * 4 + 0]; // B channel of pixel 4

    // After blur, the edge should be softened
    REQUIRE(left_of_edge > 0);
    REQUIRE(right_of_edge < 255);
    // And left side should be darker than right
    REQUIRE(left_of_edge < right_of_edge);
}

TEST_CASE("box_blur: preserves buffer dimensions", "[backdrop_blur][box_blur]") {
    constexpr int w = 16, h = 8;
    std::vector<uint8_t> buf(w * h * 4, 128);
    size_t original_size = buf.size();

    box_blur_argb8888(buf.data(), w, h, 3);

    // Size unchanged (we operate in-place)
    REQUIRE(buf.size() == original_size);
}

TEST_CASE("box_blur: handles zero iterations gracefully", "[backdrop_blur][box_blur]") {
    constexpr int w = 4, h = 4;
    std::vector<uint8_t> buf(w * h * 4, 0x80);
    auto original = buf;

    box_blur_argb8888(buf.data(), w, h, 0);
    REQUIRE(buf == original);
}

// ============================================================================
// downscale_2x_argb8888 Tests
// ============================================================================

TEST_CASE("downscale_2x: 4x4 to 2x2 with correct averaging", "[backdrop_blur][downscale]") {
    constexpr int sw = 4, sh = 4;
    std::vector<uint8_t> src(sw * sh * 4, 0);

    // Fill top-left 2x2 block with value 100 on all channels
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            int idx = (y * sw + x) * 4;
            src[idx + 0] = 100; // B
            src[idx + 1] = 100; // G
            src[idx + 2] = 100; // R
            src[idx + 3] = 255; // A
        }
    }

    // Fill top-right 2x2 block with value 200
    for (int y = 0; y < 2; y++) {
        for (int x = 2; x < 4; x++) {
            int idx = (y * sw + x) * 4;
            src[idx + 0] = 200;
            src[idx + 1] = 200;
            src[idx + 2] = 200;
            src[idx + 3] = 255;
        }
    }

    constexpr int dw = sw / 2, dh = sh / 2;
    std::vector<uint8_t> dst(dw * dh * 4, 0);

    downscale_2x_argb8888(src.data(), dst.data(), sw, sh, sw * 4);

    // Top-left output pixel should be average of (100,100,100,100) = 100
    REQUIRE(dst[0] == 100); // B
    REQUIRE(dst[1] == 100); // G
    REQUIRE(dst[2] == 100); // R
    REQUIRE(dst[3] == 255); // A

    // Top-right output pixel should be average of (200,200,200,200) = 200
    REQUIRE(dst[4] == 200); // B
    REQUIRE(dst[5] == 200); // G
    REQUIRE(dst[6] == 200); // R
    REQUIRE(dst[7] == 255); // A
}

TEST_CASE("downscale_2x: 2x2 to 1x1 averages all four pixels", "[backdrop_blur][downscale]") {
    // Four pixels: (0,0,0), (100,100,100), (200,200,200), (40,40,40)
    // Average B/G/R = (0+100+200+40)/4 = 85
    uint8_t src[4 * 4] = {
        0,   0,   0,   255, // pixel (0,0)
        100, 100, 100, 255, // pixel (1,0)
        200, 200, 200, 255, // pixel (0,1)
        40,  40,  40,  255, // pixel (1,1)
    };

    uint8_t dst[4] = {};
    downscale_2x_argb8888(src, dst, 2, 2, 2 * 4);

    REQUIRE(dst[0] == 85);  // B
    REQUIRE(dst[1] == 85);  // G
    REQUIRE(dst[2] == 85);  // R
    REQUIRE(dst[3] == 255); // A
}

TEST_CASE("downscale_2x: handles padded source stride", "[backdrop_blur][downscale]") {
    // 2x2 source with stride 16 (padded from tight stride of 8)
    // Tight stride = 2 * 4 = 8, padded stride = 16 (8 bytes of padding per row)
    constexpr int sw = 2, sh = 2;
    constexpr int padded_stride = 16;
    uint8_t src[padded_stride * sh] = {};

    // Row 0: pixel(0,0)=(10,20,30,255), pixel(1,0)=(50,60,70,255), then 8 bytes padding
    src[0] = 10;
    src[1] = 20;
    src[2] = 30;
    src[3] = 255;
    src[4] = 50;
    src[5] = 60;
    src[6] = 70;
    src[7] = 255;
    // Row 1 at offset 16: pixel(0,1)=(90,100,110,255), pixel(1,1)=(130,140,150,255)
    src[16] = 90;
    src[17] = 100;
    src[18] = 110;
    src[19] = 255;
    src[20] = 130;
    src[21] = 140;
    src[22] = 150;
    src[23] = 255;

    uint8_t dst[4] = {};
    downscale_2x_argb8888(src, dst, sw, sh, padded_stride);

    // Average: (10+50+90+130)/4=70, (20+60+100+140)/4=80, (30+70+110+150)/4=90
    REQUIRE(dst[0] == 70);
    REQUIRE(dst[1] == 80);
    REQUIRE(dst[2] == 90);
    REQUIRE(dst[3] == 255);
}

// ============================================================================
// Circuit Breaker Tests
// ============================================================================

TEST_CASE("circuit breaker: disabled after null parent failure",
          "[backdrop_blur][circuit_breaker]") {
    // Reset state from any previous test
    reset_circuit_breaker();
    REQUIRE_FALSE(is_blur_disabled());

    // Calling with nullptr parent should fail and trip the circuit breaker
    lv_obj_t* result = helix::ui::create_blurred_backdrop(nullptr, 180);
    REQUIRE(result == nullptr);
    REQUIRE(is_blur_disabled());

    // Subsequent calls also return nullptr without trying
    result = helix::ui::create_blurred_backdrop(nullptr, 180);
    REQUIRE(result == nullptr);
    REQUIRE(is_blur_disabled());
}

TEST_CASE("circuit breaker: cleanup keeps blur disabled", "[backdrop_blur][circuit_breaker]") {
    reset_circuit_breaker();

    // Trip the breaker
    helix::ui::create_blurred_backdrop(nullptr, 180);
    REQUIRE(is_blur_disabled());

    // Cleanup keeps blur disabled (pending stability testing)
    helix::ui::backdrop_blur_cleanup();
    REQUIRE(is_blur_disabled());
}

// ============================================================================
// Teardown drain (#1673)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "backdrop image delete drains the draw units before freeing",
                 "[backdrop_blur][drawbuf][1673]") {
    // The blurred/darkened backdrop is a live image source until the modal
    // teardown deletes its widget. The free must go through
    // helix::safe_draw_buf_destroy() so no in-flight blend on the render
    // thread reads the buffer after it is gone - the same failure shape as
    // the K1 crash, where a blend source was munmap'd mid-draw.
    reset_circuit_breaker();

    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* img = helix::ui::create_blurred_backdrop(parent, 180);
    REQUIRE(img != nullptr);

    WaitForFinishSpy spy;
    lv_obj_delete(img);

    CHECK(spy.waits() >= 1);
}
