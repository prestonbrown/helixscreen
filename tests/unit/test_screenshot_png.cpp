// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_screenshot_png.cpp
 * @brief Guard the ARGB8888 -> PNG conversion used by `ctl screenshot <x>.png`.
 *
 * Run with: ./build/bin/helix-tests "[screenshot]"
 *
 * Two things worth pinning:
 *
 *  1. The channel swizzle. LVGL's ARGB8888 is B,G,R,A in memory; PNG wants
 *     R,G,B,A. Getting it wrong still produces a perfectly valid PNG — just one
 *     with red and blue transposed, which is easy to miss on a mostly-grey UI.
 *     argb8888_to_rgba() is tested directly so nothing else can mask it.
 *
 *  2. That write_png() emits a real PNG with the dimensions it was handed. The
 *     header is parsed by hand rather than round-tripped through lodepng's
 *     decoder: LVGL builds lodepng with its own allocators and settings, and
 *     lodepng_decode32() does not return correct pixels in-process here even
 *     for files that decode correctly everywhere else. Parsing IHDR needs no
 *     decompression and has no such dependency.
 *
 * Mutation checks: swap the R/B lines in argb8888_to_rgba() and the swizzle
 * assertions fail; make write_png() return true without writing and the header
 * test fails; hard-code alpha to 255 in argb8888_to_rgba() and the translucent
 * colour-type test fails.
 */

#include "../lvgl_test_fixture.h"
#include "screenshot.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// One ARGB8888 pixel in LVGL's memory order (B, G, R, A).
void put_bgra(std::vector<uint8_t>& buf, size_t index, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    size_t o = index * 4;
    buf[o + 0] = b;
    buf[o + 1] = g;
    buf[o + 2] = r;
    buf[o + 3] = a;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return data;
    }
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + n);
    }
    fclose(f);
    return data;
}

uint32_t be32(const std::vector<uint8_t>& d, size_t off) {
    return (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
           (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
}

} // namespace

TEST_CASE("argb8888_to_rgba swaps red and blue, leaving green and alpha alone",
          "[screenshot][png]") {
    std::vector<uint8_t> argb(4 * 4);
    // Asymmetric colours: an R/B swap must change every one of these.
    put_bgra(argb, 0, /*b=*/0, /*g=*/0, /*r=*/255, /*a=*/255);  // red
    put_bgra(argb, 1, /*b=*/0, /*g=*/255, /*r=*/0, /*a=*/255);  // green
    put_bgra(argb, 2, /*b=*/255, /*g=*/0, /*r=*/0, /*a=*/255);  // blue
    put_bgra(argb, 3, /*b=*/10, /*g=*/20, /*r=*/30, /*a=*/128); // all four differ

    std::vector<uint8_t> rgba = helix::argb8888_to_rgba(argb.data(), 4);
    REQUIRE(rgba.size() == argb.size());

    auto px = [&](size_t i, int c) { return rgba[i * 4 + static_cast<size_t>(c)]; };

    SECTION("red stays red rather than becoming blue") {
        CHECK(px(0, 0) == 255); // R
        CHECK(px(0, 1) == 0);   // G
        CHECK(px(0, 2) == 0);   // B
        CHECK(px(0, 3) == 255); // A
    }
    SECTION("green passes through untouched") {
        CHECK(px(1, 0) == 0);
        CHECK(px(1, 1) == 255);
        CHECK(px(1, 2) == 0);
    }
    SECTION("blue stays blue rather than becoming red") {
        CHECK(px(2, 0) == 0);
        CHECK(px(2, 1) == 0);
        CHECK(px(2, 2) == 255);
    }
    SECTION("alpha is preserved and all three colour channels land distinctly") {
        CHECK(px(3, 0) == 30);  // R  <- was byte 2
        CHECK(px(3, 1) == 20);  // G
        CHECK(px(3, 2) == 10);  // B  <- was byte 0
        CHECK(px(3, 3) == 128); // A
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "write_png emits a real PNG with the right dimensions",
                 "[screenshot][png]") {
    constexpr int W = 3;
    constexpr int H = 2;
    std::vector<uint8_t> argb(static_cast<size_t>(W) * H * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(W) * H; i++) {
        put_bgra(argb, i, static_cast<uint8_t>(i * 10), 20, 30, 255);
    }

    std::string path = "/tmp/helix-test-write-png.png";
    std::remove(path.c_str());

    REQUIRE(helix::write_png(path.c_str(), argb.data(), W, H));

    std::vector<uint8_t> png = read_file(path);
    REQUIRE(png.size() > 33); // signature + IHDR at minimum

    // PNG signature
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (size_t i = 0; i < 8; i++) {
        CHECK(png[i] == sig[i]);
    }

    // First chunk must be IHDR: [len][type][w][h][bitdepth][colortype]...
    CHECK(png[12] == 'I');
    CHECK(png[13] == 'H');
    CHECK(png[14] == 'D');
    CHECK(png[15] == 'R');
    CHECK(be32(png, 16) == static_cast<uint32_t>(W));
    CHECK(be32(png, 20) == static_cast<uint32_t>(H));
    CHECK(png[24] == 8); // 8 bits per channel
    // Colour type 2 = RGB. lodepng encodes with auto_convert on by default, so
    // a frame whose pixels are all opaque loses its redundant alpha channel.
    // Screenshots always are opaque, so this is the production shape.
    CHECK(png[25] == 2);

    std::remove(path.c_str());
}

TEST_CASE_METHOD(LVGLTestFixture, "write_png keeps the alpha channel when a pixel is translucent",
                 "[screenshot][png]") {
    // Counterpart to the colour-type check above: alpha is dropped only because
    // it carries no information. One translucent pixel and it has to survive,
    // which pins that write_png feeds lodepng real alpha rather than a constant.
    constexpr int W = 2;
    constexpr int H = 1;
    std::vector<uint8_t> argb(static_cast<size_t>(W) * H * 4, 0);
    put_bgra(argb, 0, 10, 20, 30, 255);
    put_bgra(argb, 1, 10, 20, 30, 64); // translucent

    std::string path = "/tmp/helix-test-write-png-alpha.png";
    std::remove(path.c_str());

    REQUIRE(helix::write_png(path.c_str(), argb.data(), W, H));

    std::vector<uint8_t> png = read_file(path);
    REQUIRE(png.size() > 33);
    CHECK(png[25] == 6); // colour type 6 = RGBA

    std::remove(path.c_str());
}

TEST_CASE_METHOD(LVGLTestFixture, "write_png rejects a degenerate size", "[screenshot][png]") {
    std::vector<uint8_t> argb(4, 0);
    CHECK_FALSE(helix::write_png("/tmp/helix-test-should-not-exist.png", argb.data(), 0, 1));
    CHECK_FALSE(helix::write_png("/tmp/helix-test-should-not-exist.png", argb.data(), 1, 0));
}

TEST_CASE_METHOD(LVGLTestFixture, "write_png reports failure on an unwritable path",
                 "[screenshot][png]") {
    std::vector<uint8_t> argb(4, 0);
    // A directory that cannot exist -> fopen fails, and write_png must say so
    // rather than reporting a successful capture that wrote nothing.
    CHECK_FALSE(helix::write_png("/nonexistent-dir-helix/shot.png", argb.data(), 1, 1));
}
