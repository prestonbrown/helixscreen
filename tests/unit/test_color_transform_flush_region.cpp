// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_transform.h"

#include <lvgl/lvgl.h>

#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// ColorTransform::select_flush_region — the slice of a flush's px_map the LUT
// walk must cover (prestonbrown/helixscreen#1592)
// ============================================================================

namespace {

lv_area_t make_area(int x1, int y1, int x2, int y2) {
    return lv_area_t{x1, y1, x2, y2};
}

// Minimal stand-in for the display's active draw buffer: the region rule only
// reads header.stride.
lv_draw_buf_t dbuf_with_stride(uint32_t stride) {
    lv_draw_buf_t db{};
    db.header.stride = stride;
    return db;
}

} // namespace

TEST_CASE("select_flush_region walks the active buffer's stride in direct mode",
          "[display][1592]") {
    // DRM dumb buffers carry a kernel-aligned pitch that exceeds width * bpp:
    // a 100px-wide XRGB8888 row is 400 bytes tight, the kernel hands back 832.
    const lv_area_t area = make_area(40, 20, 69, 39); // 30x20 dirty rect
    lv_draw_buf_t dbuf = dbuf_with_stride(832);

    const auto reg = helix::ColorTransform::select_flush_region(
        &dbuf, area, LV_COLOR_FORMAT_XRGB8888, LV_DISPLAY_RENDER_MODE_DIRECT);

    REQUIRE(reg.stride_bytes == 832);
    // Direct mode flushes the whole-screen buffer, so the dirty rect sits at
    // its absolute coordinates, not at the buffer origin.
    REQUIRE(reg.x == 40);
    REQUIRE(reg.y == 20);
}

TEST_CASE("select_flush_region covers full-render mode like direct", "[display][1592]") {
    const lv_area_t area = make_area(5, 7, 44, 30);
    lv_draw_buf_t dbuf = dbuf_with_stride(192);

    const auto reg = helix::ColorTransform::select_flush_region(&dbuf, area, LV_COLOR_FORMAT_RGB565,
                                                                LV_DISPLAY_RENDER_MODE_FULL);

    REQUIRE(reg.stride_bytes == 192);
    REQUIRE(reg.x == 5);
    REQUIRE(reg.y == 7);
}

TEST_CASE("select_flush_region offsets from the area origin in partial mode", "[display][1592]") {
    // Partial mode reshapes the draw buffer to the dirty area and flushes
    // from its origin, so the rect's pixels start at (0,0) of px_map.
    const lv_area_t area = make_area(40, 20, 69, 39);
    lv_draw_buf_t dbuf = dbuf_with_stride(64);

    const auto reg = helix::ColorTransform::select_flush_region(&dbuf, area, LV_COLOR_FORMAT_RGB565,
                                                                LV_DISPLAY_RENDER_MODE_PARTIAL);

    REQUIRE(reg.stride_bytes == 64);
    REQUIRE(reg.x == 0);
    REQUIRE(reg.y == 0);
}

TEST_CASE("select_flush_region falls back to the computed stride without a buffer",
          "[display][1592]") {
    const lv_area_t area = make_area(10, 4, 49, 23); // 40 px wide

    const auto reg = helix::ColorTransform::select_flush_region(
        nullptr, area, LV_COLOR_FORMAT_XRGB8888, LV_DISPLAY_RENDER_MODE_PARTIAL);

    REQUIRE(reg.stride_bytes == lv_draw_buf_width_to_stride(40, LV_COLOR_FORMAT_XRGB8888));
    REQUIRE(reg.x == 0);
    REQUIRE(reg.y == 0);
}

TEST_CASE("region + apply_area transform the dirty rect, not the buffer origin",
          "[display][1592]") {
    // A 100x60 XRGB8888 buffer at a kernel-style 832-byte pitch, filled with
    // mid gray. Warmth +40 lifts R and cuts B, so a transformed pixel is
    // distinguishable from an untouched one channel-wise.
    constexpr int W = 100;
    constexpr int H = 60;
    constexpr uint32_t STRIDE = 832;
    std::vector<uint8_t> buf(static_cast<size_t>(STRIDE) * H, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t* px = &buf[static_cast<size_t>(y) * STRIDE + static_cast<size_t>(x) * 4];
            px[0] = px[1] = px[2] = 0x80; // B = G = R mid gray (BGRA in memory)
            px[3] = 0xFF;
        }
    }

    helix::ColorTransform ct;
    ct.set(1.0f, 40, 0); // warmth only
    REQUIRE_FALSE(ct.is_identity());

    const lv_area_t area = make_area(40, 20, 69, 39);
    lv_draw_buf_t dbuf = dbuf_with_stride(STRIDE);
    const auto reg = helix::ColorTransform::select_flush_region(
        &dbuf, area, LV_COLOR_FORMAT_XRGB8888, LV_DISPLAY_RENDER_MODE_DIRECT);

    ct.apply_area(buf.data(), static_cast<int>(reg.stride_bytes), reg.x, reg.y,
                  lv_area_get_width(&area), lv_area_get_height(&area), LV_COLOR_FORMAT_XRGB8888);

    const auto r_at = [&](int x, int y) {
        return buf[static_cast<size_t>(y) * STRIDE + static_cast<size_t>(x) * 4 + 2];
    };
    // Inside the dirty rect, including its far corner: warm transform applied.
    REQUIRE(r_at(40, 20) > 0x80);
    REQUIRE(r_at(69, 39) > 0x80);
    // Outside: untouched — including the origin a width-derived, origin-anchored
    // walk would have recoloured instead.
    REQUIRE(r_at(0, 0) == 0x80);
    REQUIRE(r_at(99, 59) == 0x80);
}
