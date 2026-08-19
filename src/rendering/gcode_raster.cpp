// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_raster.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace helix::gcode {

namespace {

/// Bresenham's line algorithm. Uses no LVGL APIs, so it is safe on the
/// background ghost render thread.
void line_bresenham(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        blend(t, x0, y0, argb);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy) {
            if (x0 == x1) {
                break;
            }
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            if (y0 == y1) {
                break;
            }
            err += dx;
            y0 += sy;
        }
    }
}

/// Xiaolin Wu's anti-aliased line algorithm.
void line_wu(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb) {
    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) {
        std::swap(x0, y0);
        std::swap(x1, y1);
    }
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    float gradient = (dx < 0.001f) ? 1.0f : dy / dx;

    // Strip alpha from the color — it is set per-pixel from coverage instead.
    uint32_t base_color = argb & 0x00FFFFFF;

    // First endpoint
    float yend = static_cast<float>(y0);
    float intery = yend + gradient;

    if (steep) {
        blend_coverage(t, static_cast<int>(yend), x0, base_color, 255);
    } else {
        blend_coverage(t, x0, static_cast<int>(yend), base_color, 255);
    }

    // Second endpoint
    if (steep) {
        blend_coverage(t, y1, x1, base_color, 255);
    } else {
        blend_coverage(t, x1, y1, base_color, 255);
    }

    // Main loop — draw pixels with fractional coverage for AA
    for (int x = x0 + 1; x < x1; x++) {
        int iy = static_cast<int>(intery);
        float frac = intery - static_cast<float>(iy);
        uint8_t coverage_lo = static_cast<uint8_t>((1.0f - frac) * 255);
        uint8_t coverage_hi = static_cast<uint8_t>(frac * 255);

        if (steep) {
            blend_coverage(t, iy, x, base_color, coverage_lo);
            blend_coverage(t, iy + 1, x, base_color, coverage_hi);
        } else {
            blend_coverage(t, x, iy, base_color, coverage_lo);
            blend_coverage(t, x, iy + 1, base_color, coverage_hi);
        }
        intery += gradient;
    }
}

} // namespace

int thick_line_offsets(int width, float* out) {
    // Clamped rather than trusted: the caller's array is kMaxThickLineOffsets
    // long. Extrusion widths are clamped to 8 upstream so this never bites.
    const int count = std::clamp(width, 1, kMaxThickLineOffsets);

    // Symmetric about 0, `count` of them: offsets run -(count-1)/2 .. +(count-1)/2
    // in unit steps. An even count therefore lands on half-integers, which is
    // what round_offset()'s floor(v + 0.5f) is there to map without a gap.
    const float half = static_cast<float>(count - 1) * 0.5f;
    for (int i = 0; i < count; ++i) {
        out[i] = static_cast<float>(i) - half;
    }
    return count;
}

void line(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb, Aa aa) {
    if (aa == Aa::On) {
        line_wu(t, x0, y0, x1, y1, argb);
    } else {
        line_bresenham(t, x0, y0, x1, y1, argb);
    }
}

void thick_line(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb, int width,
                Aa aa) {
    if (width <= 1) {
        line(t, x0, y0, x1, y1, argb, aa);
        return;
    }

    const float dx = static_cast<float>(x1 - x0);
    const float dy = static_cast<float>(y1 - y0);
    const float len = std::sqrt(dx * dx + dy * dy);

    if (len < kMinLineLength) {
        line(t, x0, y0, x1, y1, argb, aa);
        return;
    }

    // Perpendicular unit vector (the segment direction rotated 90 degrees)
    const float px = -dy / len;
    const float py = dx / len;

    float offsets[kMaxThickLineOffsets];
    const int count = thick_line_offsets(width, offsets);
    for (int i = 0; i < count; ++i) {
        const int ox = round_offset(px * offsets[i]);
        const int oy = round_offset(py * offsets[i]);
        line(t, x0 + ox, y0 + oy, x1 + ox, y1 + oy, argb, aa);
    }
}

} // namespace helix::gcode
