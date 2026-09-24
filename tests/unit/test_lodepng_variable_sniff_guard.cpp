// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: exercises patches/lvgl_lodepng_variable_sniff_guard.patch, shipped LVGL code
// with no HelixScreen header
/**
 * @file test_lodepng_variable_sniff_guard.cpp
 * @brief A draw buffer is never taken for a PNG because of the bytes it holds.
 *
 * Run with: ./build/bin/helix-tests "[lodepng]"
 *
 * LVGL tries its decoders newest-first, so lodepng's info callback sees every
 * variable source before the bin decoder does. For those it only sniffs the
 * first eight data bytes for the PNG signature, then reads width and height
 * from bytes 16..23. lv_qrcode_set_size() hands the canvas a freshly allocated
 * buffer and clears it only afterwards, so the probe in lv_image_set_src()
 * reads whatever the heap left there. When that is PNG bytes, the widget
 * caches a size taken from them and every later draw lays the real bitmap out
 * over that area, reading far past the buffer (prestonbrown/helixscreen#1673).
 *
 * The buffers below plant the exact bytes from that crash: the signature at 0
 * and again at 16, which reads as 0x4E47 x 0x1A0A (20039 x 6666).
 *
 * Mutation check: drop the cf gate from
 * patches/lvgl_lodepng_variable_sniff_guard.patch and the two draw-buffer
 * cases fail with 20039 x 6666; the encoded-PNG case stays green.
 */

#include "../lvgl_test_fixture.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#define LODEPNG_NO_COMPILE_CPP
#include "libs/lodepng/lodepng.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/// A 200x200 I1 buffer, the size InfoQrModal asks for, carrying the stale
/// bytes the K2 crash recorded instead of a cleared palette.
lv_draw_buf_t* buffer_with_stale_png_bytes() {
    lv_draw_buf_t* buf = lv_draw_buf_create(200, 200, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
    REQUIRE(buf != nullptr);
    std::memcpy(buf->data, PNG_SIGNATURE, sizeof(PNG_SIGNATURE));
    std::memcpy(buf->data + 16, PNG_SIGNATURE, sizeof(PNG_SIGNATURE));
    return buf;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "A draw buffer holding PNG bytes reports its own header",
                 "[lodepng][image]") {
    lv_draw_buf_t* buf = buffer_with_stale_png_bytes();

    lv_image_header_t header;
    REQUIRE(lv_image_decoder_get_info(buf, &header) == LV_RESULT_OK);
    CHECK(header.w == 200);
    CHECK(header.h == 200);
    CHECK(header.cf == LV_COLOR_FORMAT_I1);

    lv_draw_buf_destroy(buf);
}

TEST_CASE_METHOD(LVGLTestFixture, "A canvas given an uncleared buffer sizes to the buffer",
                 "[lodepng][image]") {
    lv_obj_t* canvas = lv_canvas_create(lv_screen_active());
    lv_draw_buf_t* buf = buffer_with_stale_png_bytes();

    lv_canvas_set_draw_buf(canvas, buf);
    lv_obj_update_layout(canvas);

    CHECK(lv_obj_get_width(canvas) == 200);
    CHECK(lv_obj_get_height(canvas) == 200);

    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}

TEST_CASE_METHOD(LVGLTestFixture, "An encoded PNG in memory still decodes by its IHDR",
                 "[lodepng][image]") {
    std::vector<uint8_t> raw(12 * 7 * 4, 0x80);
    unsigned char* png = nullptr;
    size_t png_size = 0;
    REQUIRE(lodepng_encode_memory(&png, &png_size, raw.data(), 12, 7, LCT_RGBA, 8) == 0);

    SECTION("declared RAW_ALPHA, as LVGL's image converter emits") {
        lv_image_dsc_t dsc{};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
        dsc.data_size = static_cast<uint32_t>(png_size);
        dsc.data = png;

        lv_image_header_t header;
        REQUIRE(lv_image_decoder_get_info(&dsc, &header) == LV_RESULT_OK);
        CHECK(header.w == 12);
        CHECK(header.h == 7);
    }

    SECTION("header left zeroed") {
        lv_image_dsc_t dsc{};
        dsc.data_size = static_cast<uint32_t>(png_size);
        dsc.data = png;

        lv_image_header_t header;
        REQUIRE(lv_image_decoder_get_info(&dsc, &header) == LV_RESULT_OK);
        CHECK(header.w == 12);
        CHECK(header.h == 7);
    }

    free(png);
}
