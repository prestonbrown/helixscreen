// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

/**
 * @file bed_mesh_buffer.cpp
 * @brief ARGB8888 pixel buffer with drawing primitives for off-screen rendering
 *
 * All drawing operations use BGRA byte order to match LVGL's ARGB8888 format.
 * Alpha blending uses the standard formula:
 *   result = (src * src_alpha + dst * (255 - src_alpha)) / 255
 */

#include "bed_mesh_buffer.h"

#include <algorithm>
#include <cstring>

namespace helix {
namespace mesh {

// ============================================================================
// Construction
// ============================================================================

PixelBuffer::PixelBuffer(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)),
      data_(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4, 0) {}

// ============================================================================
// Pixel access
// ============================================================================

uint8_t* PixelBuffer::pixel_at(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return nullptr;
    }
    return data_.data() + (static_cast<size_t>(y) * width_ + x) * 4;
}

const uint8_t* PixelBuffer::pixel_at(int x, int y) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return nullptr;
    }
    return data_.data() + (static_cast<size_t>(y) * width_ + x) * 4;
}

// ============================================================================
// Clear
// ============================================================================

void PixelBuffer::clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (data_.empty()) {
        return;
    }

    // Write BGRA pattern to first pixel, then replicate
    uint8_t pattern[4] = {b, g, r, a};

    if (pattern[0] == pattern[1] && pattern[1] == pattern[2] && pattern[2] == pattern[3]) {
        // All bytes the same -- memset is fastest
        std::memset(data_.data(), pattern[0], data_.size());
    } else {
        // Write pattern to each pixel
        auto* dst = data_.data();
        const auto total_pixels = static_cast<size_t>(width_) * height_;
        for (size_t i = 0; i < total_pixels; i++) {
            dst[0] = pattern[0];
            dst[1] = pattern[1];
            dst[2] = pattern[2];
            dst[3] = pattern[3];
            dst += 4;
        }
    }
}

// ============================================================================
// Alpha blending
// ============================================================================

void PixelBuffer::blend_pixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 255) {
        // Fully opaque: direct write, skip blending
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = a;
        return;
    }

    // Standard alpha blend: result = (src * alpha + dst * (255 - alpha)) / 255
    const uint16_t inv_a = 255 - a;
    dst[0] = static_cast<uint8_t>((b * a + dst[0] * inv_a) / 255);
    dst[1] = static_cast<uint8_t>((g * a + dst[1] * inv_a) / 255);
    dst[2] = static_cast<uint8_t>((r * a + dst[2] * inv_a) / 255);
    // Keep destination alpha (compositing onto an existing surface)
}

// ============================================================================
// set_pixel
// ============================================================================

void PixelBuffer::set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) {
        return;
    }
    auto* p = pixel_at(x, y);
    if (p == nullptr) {
        return;
    }
    blend_pixel(p, r, g, b, a);
}

// ============================================================================
// fill_hline
// ============================================================================

void PixelBuffer::fill_hline(int x, int w, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0 || w <= 0 || y < 0 || y >= height_) {
        return;
    }

    // Clamp X range to buffer bounds
    int x_end = x + w;
    if (x < 0) {
        x = 0;
    }
    if (x_end > width_) {
        x_end = width_;
    }
    if (x >= x_end) {
        return;
    }

    auto* dst = pixel_at(x, y);
    if (a == 255) {
        // Fully opaque fast path
        for (int px = x; px < x_end; px++) {
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = a;
            dst += 4;
        }
    } else {
        const uint16_t inv_a = 255 - a;
        for (int px = x; px < x_end; px++) {
            dst[0] = static_cast<uint8_t>((b * a + dst[0] * inv_a) / 255);
            dst[1] = static_cast<uint8_t>((g * a + dst[1] * inv_a) / 255);
            dst[2] = static_cast<uint8_t>((r * a + dst[2] * inv_a) / 255);
            dst += 4;
        }
    }
}

// ============================================================================
// draw_line (Bresenham's algorithm)
// ============================================================================

void PixelBuffer::draw_line(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t a, int thickness) {
    if (a == 0 || width_ == 0 || height_ == 0) {
        return;
    }

    if (thickness <= 1) {
        // Standard Bresenham
        int dx = std::abs(x1 - x0);
        int dy = -std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx + dy;

        for (;;) {
            set_pixel(x0, y0, r, g, b, a);

            if (x0 == x1 && y0 == y1) {
                break;
            }

            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    } else {
        // Thick line: draw the center line, then offset lines perpendicular
        // For simplicity, expand by drawing parallel lines offset in the
        // perpendicular direction. For mostly-horizontal lines, offset in Y;
        // for mostly-vertical lines, offset in X.
        int dx = std::abs(x1 - x0);
        int dy = std::abs(y1 - y0);
        int half = (thickness - 1) / 2;

        if (dx >= dy) {
            // Mostly horizontal: expand in Y direction
            for (int offset = -half; offset <= half; offset++) {
                draw_line(x0, y0 + offset, x1, y1 + offset, r, g, b, a, 1);
            }
        } else {
            // Mostly vertical: expand in X direction
            for (int offset = -half; offset <= half; offset++) {
                draw_line(x0 + offset, y0, x1 + offset, y1, r, g, b, a, 1);
            }
        }
    }
}

// ============================================================================
// fill_triangle_solid (scanline rasterization)
// ============================================================================

void PixelBuffer::fill_triangle_solid(int x1, int y1, int x2, int y2, int x3, int y3, uint8_t r,
                                      uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0 || width_ == 0 || height_ == 0) {
        return;
    }

    // Sort vertices by Y coordinate (bubble sort, optimal for 3 elements)
    if (y1 > y2) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }
    if (y2 > y3) {
        std::swap(y2, y3);
        std::swap(x2, x3);
    }
    if (y1 > y2) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }

    // Skip degenerate triangles (all vertices on same scanline)
    if (y1 == y3) {
        return;
    }

    // Clamp scanline range to buffer bounds
    int y_start = std::max(y1, 0);
    int y_end = std::min(y3, height_ - 1);

    for (int y = y_start; y <= y_end; y++) {
        // Long edge: y1 -> y3
        double t_long = (y - y1) / static_cast<double>(y3 - y1);
        int x_long = x1 + static_cast<int>(t_long * (x3 - x1));

        // Short edge: split at y2
        int x_short;
        if (y < y2) {
            if (y2 == y1) {
                x_short = x1;
            } else {
                double t = (y - y1) / static_cast<double>(y2 - y1);
                x_short = x1 + static_cast<int>(t * (x2 - x1));
            }
        } else {
            if (y3 == y2) {
                x_short = x2;
            } else {
                double t = (y - y2) / static_cast<double>(y3 - y2);
                x_short = x2 + static_cast<int>(t * (x3 - x2));
            }
        }

        int xl = std::min(x_long, x_short);
        int xr = std::max(x_long, x_short);

        if (xl <= xr) {
            fill_hline(xl, xr - xl + 1, y, r, g, b, a);
        }
    }
}

// ============================================================================
// fill_triangle_gradient (scanline with per-vertex color interpolation)
// ============================================================================

namespace {

// Gradient rasterization constants (matching bed_mesh_rasterizer.h)
constexpr int GRADIENT_MIN_LINE_WIDTH = 3;
constexpr int GRADIENT_THIN_LINE_THRESHOLD = 20;
constexpr int GRADIENT_MEDIUM_LINE_THRESHOLD = 50;
constexpr int GRADIENT_THIN_SEGMENT_COUNT = 2;
constexpr int GRADIENT_MEDIUM_SEGMENT_COUNT = 3;
constexpr int GRADIENT_WIDE_SEGMENT_COUNT = 4;
constexpr double GRADIENT_SEGMENT_SAMPLE_POSITION = 0.5;

struct ColorRGB {
    uint8_t r, g, b;
};

inline ColorRGB lerp_color(const ColorRGB& a, const ColorRGB& b, double t) {
    return {static_cast<uint8_t>(a.r + t * (b.r - a.r)),
            static_cast<uint8_t>(a.g + t * (b.g - a.g)),
            static_cast<uint8_t>(a.b + t * (b.b - a.b))};
}

} // anonymous namespace

void PixelBuffer::fill_triangle_gradient(int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1, int x2,
                                         int y2, uint8_t r2, uint8_t g2, uint8_t b2, int x3, int y3,
                                         uint8_t r3, uint8_t g3, uint8_t b3, uint8_t a) {
    if (a == 0 || width_ == 0 || height_ == 0) {
        return;
    }

    // Sort vertices by Y coordinate, keeping colors aligned
    struct Vertex {
        int x, y;
        ColorRGB color;
    };
    Vertex v[3] = {{x1, y1, {r1, g1, b1}}, {x2, y2, {r2, g2, b2}}, {x3, y3, {r3, g3, b3}}};

    if (v[0].y > v[1].y)
        std::swap(v[0], v[1]);
    if (v[1].y > v[2].y)
        std::swap(v[1], v[2]);
    if (v[0].y > v[1].y)
        std::swap(v[0], v[1]);

    // Skip degenerate triangles
    if (v[0].y == v[2].y) {
        return;
    }

    // Clamp scanline range to buffer bounds
    int y_start = std::max(v[0].y, 0);
    int y_end = std::min(v[2].y, height_ - 1);

    for (int y = y_start; y <= y_end; y++) {
        // Interpolate along long edge (v[0] -> v[2])
        double t_long = (y - v[0].y) / static_cast<double>(v[2].y - v[0].y);
        int x_long = v[0].x + static_cast<int>(t_long * (v[2].x - v[0].x));
        ColorRGB c_long = lerp_color(v[0].color, v[2].color, t_long);

        // Interpolate along short edge
        int x_short;
        ColorRGB c_short;
        if (y < v[1].y) {
            if (v[1].y == v[0].y) {
                x_short = v[0].x;
                c_short = v[0].color;
            } else {
                double t = (y - v[0].y) / static_cast<double>(v[1].y - v[0].y);
                x_short = v[0].x + static_cast<int>(t * (v[1].x - v[0].x));
                c_short = lerp_color(v[0].color, v[1].color, t);
            }
        } else {
            if (v[2].y == v[1].y) {
                x_short = v[1].x;
                c_short = v[1].color;
            } else {
                double t = (y - v[1].y) / static_cast<double>(v[2].y - v[1].y);
                x_short = v[1].x + static_cast<int>(t * (v[2].x - v[1].x));
                c_short = lerp_color(v[1].color, v[2].color, t);
            }
        }

        // Ensure left/right ordering
        int xl = std::min(x_long, x_short);
        int xr = std::max(x_long, x_short);
        ColorRGB c_left = (x_long < x_short) ? c_long : c_short;
        ColorRGB c_right = (x_long < x_short) ? c_short : c_long;

        int line_width = xr - xl + 1;
        if (line_width <= 0) {
            continue;
        }

        if (line_width < GRADIENT_MIN_LINE_WIDTH) {
            // Thin line: use average color
            ColorRGB avg = lerp_color(c_left, c_right, 0.5);
            fill_hline(xl, line_width, y, avg.r, avg.g, avg.b, a);
        } else {
            // Adaptive segment count based on width
            int segment_count;
            if (line_width < GRADIENT_THIN_LINE_THRESHOLD) {
                segment_count = GRADIENT_THIN_SEGMENT_COUNT;
            } else if (line_width < GRADIENT_MEDIUM_LINE_THRESHOLD) {
                segment_count = GRADIENT_MEDIUM_SEGMENT_COUNT;
            } else {
                segment_count = GRADIENT_WIDE_SEGMENT_COUNT;
            }

            for (int si = 0; si < segment_count; si++) {
                int seg_x_start = xl + (si * line_width) / segment_count;
                int seg_x_end = xl + ((si + 1) * line_width) / segment_count - 1;
                if (seg_x_start > seg_x_end) {
                    continue;
                }

                // Sample color at segment center
                double factor = (si + GRADIENT_SEGMENT_SAMPLE_POSITION) / segment_count;
                ColorRGB seg_color = lerp_color(c_left, c_right, factor);
                fill_hline(seg_x_start, seg_x_end - seg_x_start + 1, y, seg_color.r, seg_color.g,
                           seg_color.b, a);
            }
        }
    }
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
