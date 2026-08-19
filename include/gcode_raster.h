// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file gcode_raster.h
/// Software rasterizers for the G-code preview.
///
/// These bypass the LVGL draw API deliberately: the AD5M's draw pipeline cannot
/// render the segment counts a sliced model produces, and the ghost pass runs on
/// a background thread where LVGL APIs are off limits entirely. Both the solid
/// layer cache and the raw ghost buffer are plain ARGB8888 surfaces, so one unit
/// serves both — the caller supplies a RasterTarget describing whichever buffer
/// it owns.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace helix::gcode {

/// A writable ARGB8888 surface. `stride` is bytes per row, which need not equal
/// `w * 4` (LVGL aligns its draw buffers). A default-constructed target has a
/// null `data` and every write against it is a no-op.
struct RasterTarget {
    uint8_t* data = nullptr;
    size_t stride = 0;
    int w = 0;
    int h = 0;
};

/// Whether a line is drawn with Bresenham (Off) or Xiaolin Wu antialiasing (On).
enum class Aa { Off, On };

/// Upper bound on the offsets a single thick line can produce. Extrusion pixel
/// width is clamped to MAX_EXTRUSION_PIXEL_WIDTH (8) upstream, so this is
/// generous headroom rather than a real limit.
inline constexpr int kMaxThickLineOffsets = 32;

/// Minimum segment length for which a perpendicular direction is meaningful.
/// Shorter than this and `dx/len` is noise, so the segment degenerates to a
/// single thin line.
///
/// This used to be declared once at 0.001f and then shadowed by a local 0.5f
/// inside the AA thick-line variant, so only the AA path applied the coarser
/// cutoff. The shadow was accidental; 0.001f is the declared intent and is what
/// both paths use now.
inline constexpr float kMinLineLength = 0.001f;

/// Perpendicular offsets, in pixels, for the parallel lines making up a thick
/// line of `width`. Writes the count into `out`, which must hold
/// kMaxThickLineOffsets entries, and returns it. The count is max(1, width) and
/// the offsets are symmetric about 0 (they sum to 0).
int thick_line_offsets(int width, float* out);

/// floor(v + 0.5f) — rounds halves toward +infinity.
///
/// Not std::round, which rounds halves AWAY from zero. An even width's offsets
/// are half-integers, and under std::round the -0.5 and +0.5 pair became -1 and
/// +1, skipping 0 entirely and drawing a hollow line.
inline int round_offset(float v) {
    return static_cast<int>(std::floor(v + 0.5f));
}

/// Overwrite one pixel. Not a composite despite the name, which is historical:
/// all four channels are written unconditionally from `argb`.
inline void blend(const RasterTarget& t, int x, int y, uint32_t argb) {
    if (t.data == nullptr || x < 0 || x >= t.w || y < 0 || y >= t.h) {
        return;
    }

    // ARGB8888 in memory is B, G, R, A on little-endian.
    uint8_t* pixel = t.data + static_cast<size_t>(y) * t.stride + static_cast<size_t>(x) * 4;
    pixel[0] = static_cast<uint8_t>(argb & 0xFF);         // B
    pixel[1] = static_cast<uint8_t>((argb >> 8) & 0xFF);  // G
    pixel[2] = static_cast<uint8_t>((argb >> 16) & 0xFF); // R
    pixel[3] = static_cast<uint8_t>((argb >> 24) & 0xFF); // A
}

/// Coverage-weighted src-over composite of `rgb` (alpha bits ignored) onto one
/// pixel. Zero coverage is a no-op.
inline void blend_coverage(const RasterTarget& t, int x, int y, uint32_t rgb, uint8_t coverage) {
    if (t.data == nullptr || x < 0 || x >= t.w || y < 0 || y >= t.h) {
        return;
    }
    if (coverage == 0) {
        return;
    }

    uint8_t* pixel = t.data + static_cast<size_t>(y) * t.stride + static_cast<size_t>(x) * 4;

    const uint8_t src_b = static_cast<uint8_t>(rgb & 0xFF);
    const uint8_t src_g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    const uint8_t src_r = static_cast<uint8_t>((rgb >> 16) & 0xFF);

    if (coverage == 255 || pixel[3] == 0) {
        // Full coverage or empty destination: just write
        pixel[0] = src_b;
        pixel[1] = src_g;
        pixel[2] = src_r;
        pixel[3] = coverage;
    } else {
        // Alpha blend: src over dst
        const uint8_t dst_a = pixel[3];
        const uint16_t inv = static_cast<uint16_t>(255 - coverage);
        pixel[0] = static_cast<uint8_t>((src_b * coverage + pixel[0] * inv) / 255);
        pixel[1] = static_cast<uint8_t>((src_g * coverage + pixel[1] * inv) / 255);
        pixel[2] = static_cast<uint8_t>((src_r * coverage + pixel[2] * inv) / 255);
        pixel[3] = static_cast<uint8_t>(coverage + (dst_a * inv) / 255);
    }
}

/// Draw a one-pixel line. Aa::Off is Bresenham writing through blend(); Aa::On
/// is Xiaolin Wu writing through blend_coverage(), which sets alpha per pixel
/// from coverage and so ignores the alpha bits of `argb`.
void line(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb, Aa aa);

/// Draw a line `width` pixels thick, as `width` parallel lines offset along the
/// segment normal. AA and non-AA use the same offsets, so a given width is the
/// same thickness either way.
void thick_line(const RasterTarget& t, int x0, int y0, int x1, int y1, uint32_t argb, int width,
                Aa aa);

} // namespace helix::gcode
