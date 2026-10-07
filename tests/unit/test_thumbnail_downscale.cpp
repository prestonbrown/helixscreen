// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl.h"
#include "thumbnail_downscale.h"

#include <cstring>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ThumbnailDims;

namespace {

std::vector<uint8_t> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

uint16_t colour_at(const std::vector<uint8_t>& out, ThumbnailDims d, int x, int y) {
    uint16_t c;
    std::memcpy(&c, out.data() + (static_cast<size_t>(y) * d.w + x) * 2, 2);
    return c;
}

uint8_t alpha_at(const std::vector<uint8_t>& out, ThumbnailDims d, int x, int y) {
    return out[static_cast<size_t>(d.w) * d.h * 2 + static_cast<size_t>(y) * d.w + x];
}

} // namespace

TEST_CASE("fit_thumbnail keeps aspect inside the box and never upscales",
          "[thumbnail][downscale]") {
    // The print-status thumbnail box at 800x480.
    constexpr int BOX_W = 377;
    constexpr int BOX_H = 260;
    const ThumbnailDims square = helix::fit_thumbnail(300, 300, BOX_W, BOX_H);
    CHECK(square.w == 260);
    CHECK(square.h == 260);
    // 16:9 fills the box's width rather than shrinking to fit a 260 square.
    const ThumbnailDims wide = helix::fit_thumbnail(640, 360, BOX_W, BOX_H);
    CHECK(wide.w == 377);
    CHECK(wide.h == 212);
    const ThumbnailDims very_wide = helix::fit_thumbnail(1000, 500, BOX_W, BOX_H);
    CHECK(very_wide.w == 377);
    CHECK(very_wide.h == 188);
    // Taller than the box's ratio: height binds.
    const ThumbnailDims tall = helix::fit_thumbnail(200, 400, BOX_W, BOX_H);
    CHECK(tall.w == 130);
    CHECK(tall.h == 260);
    // Already inside the box: kept as is.
    const ThumbnailDims small = helix::fit_thumbnail(320, 180, BOX_W, BOX_H);
    CHECK(small.w == 320);
    CHECK(small.h == 180);
    CHECK(helix::fit_thumbnail(1000, 1, BOX_W, BOX_H).h == 1);
    CHECK(helix::fit_thumbnail(0, 300, BOX_W, BOX_H).w == 0);
    CHECK(helix::rgb565a8_size({260, 260}) == 202800);
    CHECK(helix::rgb565a8_size({BOX_W, BOX_H}) == 294060);
}

TEST_CASE("downscale packs RGB565 then an alpha plane", "[thumbnail][downscale]") {
    const auto src = solid(4, 4, 255, 0, 0, 255);
    const ThumbnailDims d{2, 2};
    std::vector<uint8_t> out(helix::rgb565a8_size(d), 0xAA);
    helix::downscale_rgba_to_rgb565a8(src.data(), 4, 4, d, out.data());
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            CHECK(colour_at(out, d, x, y) == 0xF800);
            CHECK(alpha_at(out, d, x, y) == 255);
        }
    }

    const auto green = solid(2, 2, 0, 255, 0, 255);
    std::vector<uint8_t> g_out(helix::rgb565a8_size({2, 2}));
    helix::downscale_rgba_to_rgb565a8(green.data(), 2, 2, {2, 2}, g_out.data());
    CHECK(colour_at(g_out, {2, 2}, 1, 1) == 0x07E0);

    const auto blue = solid(2, 2, 0, 0, 255, 128);
    std::vector<uint8_t> b_out(helix::rgb565a8_size({1, 1}));
    helix::downscale_rgba_to_rgb565a8(blue.data(), 2, 2, {1, 1}, b_out.data());
    CHECK(colour_at(b_out, {1, 1}, 0, 0) == 0x001F);
    CHECK(alpha_at(b_out, {1, 1}, 0, 0) == 128);
}

TEST_CASE("downscale averages each box, weighting colour by alpha", "[thumbnail][downscale]") {
    // 2x1 source: an opaque white pixel next to a fully transparent black one.
    const std::vector<uint8_t> src = {255, 255, 255, 255, 0, 0, 0, 0};
    std::vector<uint8_t> out(helix::rgb565a8_size({1, 1}));
    helix::downscale_rgba_to_rgb565a8(src.data(), 2, 1, {1, 1}, out.data());
    // Colour stays white rather than greying toward the transparent neighbour.
    CHECK(colour_at(out, {1, 1}, 0, 0) == 0xFFFF);
    CHECK(alpha_at(out, {1, 1}, 0, 0) == 127);

    // Fully transparent box.
    const std::vector<uint8_t> clear = {10, 20, 30, 0, 40, 50, 60, 0};
    helix::downscale_rgba_to_rgb565a8(clear.data(), 2, 1, {1, 1}, out.data());
    CHECK(alpha_at(out, {1, 1}, 0, 0) == 0);

    // Each output pixel reads only its own box: left half black, right half white.
    std::vector<uint8_t> halves(4 * 2 * 4);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = halves.data() + (y * 4 + x) * 4;
            const uint8_t v = x < 2 ? 0 : 255;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    std::vector<uint8_t> h_out(helix::rgb565a8_size({2, 1}));
    helix::downscale_rgba_to_rgb565a8(halves.data(), 4, 2, {2, 1}, h_out.data());
    CHECK(colour_at(h_out, {2, 1}, 0, 0) == 0x0000);
    CHECK(colour_at(h_out, {2, 1}, 1, 0) == 0xFFFF);
}

TEST_CASE("an opaque downscale is what LVGL draws of the RGB565A8 image over the backdrop",
          "[thumbnail][downscale]") {
    // A 7x5 source of mixed alpha, scaled to 3x2 and placed at (1,2) in a 6x5 backdrop.
    std::mt19937 rng(7);
    std::vector<uint8_t> src(7 * 5 * 4);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>(rng());
    }
    for (size_t i = 3; i < 4 * 4; i += 4) {
        src[i] = 0; // some fully transparent pixels
    }
    std::vector<uint16_t> back(6 * 5);
    for (auto& c : back) {
        c = static_cast<uint16_t>(rng());
    }
    const ThumbnailDims d{3, 2};
    const int ox = 1, oy = 2, stride = 6;

    std::vector<uint8_t> a8(helix::rgb565a8_size(d));
    helix::downscale_rgba_to_rgb565a8(src.data(), 7, 5, d, a8.data());
    std::vector<uint16_t> want = back;
    for (int y = 0; y < d.h; ++y) {
        for (int x = 0; x < d.w; ++x) {
            uint16_t& px = want[static_cast<size_t>(oy + y) * stride + ox + x];
            px = lv_color_16_16_mix(colour_at(a8, d, x, y), px, alpha_at(a8, d, x, y));
        }
    }

    std::vector<uint16_t> got = back;
    helix::RowDownscaler scaler(7, 5, d, got.data() + oy * stride + ox, stride);
    REQUIRE(scaler.ok());
    for (int y = 0; y < 5; ++y) {
        scaler.add_row(src.data() + static_cast<size_t>(y) * 7 * 4);
    }
    CHECK(scaler.complete());
    CHECK(got == want); // pixels outside the image keep the backdrop
}
