// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: exercises patches/lvgl_lodepng_bpp_guard.patch — shipped LVGL code with no
// HelixScreen header
/**
 * @file test_lodepng_bitdepth_guard.cpp
 * @brief Pin the bit-depth guard on LVGL's lodepng port.
 *
 * Run with: ./build/bin/helix-tests "[lodepng]"
 *
 * LVGL replaced upstream lodepng's output allocation with a fixed ARGB8888
 * draw buffer:
 *
 *     lv_draw_buf_create_ex(..., *w, *h, LV_COLOR_FORMAT_ARGB8888, 4 * *w)
 *     postProcessScanlines(decoded->data, scanlines, *w, *h, &state->info_png)
 *
 * The buffer holds 4*w*h bytes, but postProcessScanlines writes
 * h * w * bpp/8 where bpp is the *source PNG's* bits-per-pixel, taken from
 * info_png. Upstream sized the allocation from that same colour mode; the
 * port did not re-bound the write. Every colour type at 8 bits or below
 * fits (RGBA8 exactly, everything else under), so the mismatch only shows
 * with 16-bit channels: RGB16 is bpp=48 and RGBA16 is bpp=64, i.e. a 1.5x
 * and 2x heap overflow past the end of the draw buffer.
 *
 * The conversion to the caller's requested format happens *after*
 * decodeGeneric writes, so lodepng_decode32() does not protect against it.
 *
 * These tests assert on the returned error code rather than on pixels:
 * LVGL's port puts an lv_draw_buf_t in *out, not a raw pixel array, so the
 * decoded bytes are not directly comparable here (same reason
 * test_screenshot_png.cpp parses IHDR by hand).
 *
 * Mutation check: drop the `bpp > 32` guard from
 * patches/lvgl_lodepng_bpp_guard.patch and the two 16-bit cases below fail
 * (they decode "successfully" while writing past the buffer).
 */

#include "../lvgl_test_fixture.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

// Only the C API is wanted here. The C++ wrapper declares a free
// lodepng::decode() whose overloads clash with another decode() already in
// this translation unit's include graph.
#define LODEPNG_NO_COMPILE_CPP
#include "libs/lodepng/lodepng.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Encode a solid-colour image at an arbitrary colour type / bit depth.
/// Returns the PNG bytes, or an empty vector if encoding failed.
std::vector<uint8_t> encode_png(unsigned w, unsigned h, LodePNGColorType colortype,
                                unsigned bitdepth) {
    // Bytes per pixel in the *raw* buffer lodepng_encode_memory() expects.
    unsigned channels = (colortype == LCT_RGBA) ? 4u : 3u;
    size_t raw_bytes = static_cast<size_t>(w) * h * channels * (bitdepth / 8u);

    // A recognisable non-uniform pattern; the exact values do not matter,
    // only that the encoder emits the requested colour mode.
    std::vector<uint8_t> raw(raw_bytes);
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<uint8_t>(i * 7u);
    }

    unsigned char* png = nullptr;
    size_t png_size = 0;
    unsigned err = lodepng_encode_memory(&png, &png_size, raw.data(), w, h, colortype, bitdepth);

    std::vector<uint8_t> out;
    if (err == 0 && png != nullptr) {
        out.assign(png, png + png_size);
    }
    free(png);
    return out;
}

/// Decode through the same entry point lv_lodepng.c uses and return the
/// lodepng error code. Frees whatever the decoder handed back.
unsigned decode_error(const std::vector<uint8_t>& png) {
    unsigned char* out = nullptr;
    unsigned w = 0;
    unsigned h = 0;
    unsigned err = lodepng_decode32(&out, &w, &h, png.data(), png.size());
    if (out != nullptr) {
        lv_draw_buf_destroy(reinterpret_cast<lv_draw_buf_t*>(out));
    }
    return err;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "lodepng rejects 16-bit RGBA instead of overflowing",
                 "[lodepng][security]") {
    // bpp = 64. postProcessScanlines would write 8 bytes/pixel into a
    // 4 bytes/pixel buffer -- a 2x overflow.
    std::vector<uint8_t> png = encode_png(8, 8, LCT_RGBA, 16);
    REQUIRE_FALSE(png.empty()); // encoder must actually produce a 16-bit file

    REQUIRE(decode_error(png) != 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "lodepng rejects 16-bit RGB instead of overflowing",
                 "[lodepng][security]") {
    // bpp = 48. 6 bytes/pixel into a 4 bytes/pixel buffer -- a 1.5x overflow.
    std::vector<uint8_t> png = encode_png(8, 8, LCT_RGB, 16);
    REQUIRE_FALSE(png.empty());

    REQUIRE(decode_error(png) != 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "lodepng still accepts the 8-bit depths slicers emit",
                 "[lodepng]") {
    // The guard must not cost us the formats that actually reach the cache.
    // RGBA8 is bpp=32 (exact fit); RGB8 is bpp=24 (under).
    SECTION("8-bit RGBA") {
        std::vector<uint8_t> png = encode_png(8, 8, LCT_RGBA, 8);
        REQUIRE_FALSE(png.empty());
        REQUIRE(decode_error(png) == 0);
    }

    SECTION("8-bit RGB") {
        std::vector<uint8_t> png = encode_png(8, 8, LCT_RGB, 8);
        REQUIRE_FALSE(png.empty());
        REQUIRE(decode_error(png) == 0);
    }
}
