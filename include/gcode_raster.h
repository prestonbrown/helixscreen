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

/// Byte order of the three colour channels in a RasterTarget.
///
/// Bgra is ARGB8888 as it sits in memory on little-endian, which is what every
/// LVGL draw buffer and the ghost buffer are. Rgba is what a `glReadPixels(...,
/// GL_RGBA, ...)` readback hands back, so the 3D path's surface has red and blue
/// the other way round.
///
/// Only stroke_selection_rim() takes this. blend() and blend_coverage() are
/// ARGB8888-only: they are fed packed 0xAARRGGBB words by the line rasterizers
/// and there is no GL-side caller. Alpha is byte 3 in both layouts, which is why
/// the selection tag itself needs no such distinction.
enum class ChannelOrder { Bgra, Rgba };

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

/// Alpha value reserved to mean "this pixel belongs to the selected object".
///
/// The render cache is ARGB8888 and the alpha channel is only ever asked whether
/// it is zero (apply_ssao's edge test) or handed to the blit, so one value out of
/// 256 is free to carry a tag. Tagging costs nothing: the selected object draws
/// exactly as it always did, one pass, same color, same width, with 254 in the
/// alpha byte instead of 255. A 0.4% opacity difference is not visible.
///
/// stroke_selection_rim() then derives the white silhouette from where those
/// pixels actually landed, rather than painting white in advance and hoping a
/// later pass covers the parts that should not show.
inline constexpr uint8_t kSelectedAlpha = 254;

/// Alpha for a stroke that is NOT the selected object, with the reserved tag
/// rounded off.
///
/// Two things can land on kSelectedAlpha by accident: a Wu edge pixel whose raw
/// coverage happens to be 254, and an accumulated src-over alpha that adds up to
/// it. Either way stroke_selection_rim() would find an isolated tagged pixel,
/// see no tagged neighbour in any of its four directions, and paint it white in
/// the middle of an unselected object. 254 and 255 are indistinguishable on
/// screen, so the collision is resolved upward.
///
/// The tag itself is written by blend(), which takes the alpha byte from the
/// caller's `argb` verbatim — a tagged stroke is always drawn aliased for
/// exactly that reason, so nothing that reaches here ever means to be tagged.
inline uint8_t untagged_alpha(uint8_t a) {
    return (a == kSelectedAlpha) ? 255 : a;
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
        pixel[3] = untagged_alpha(coverage);
    } else {
        // Alpha blend: src over dst
        const uint8_t dst_a = pixel[3];
        const uint16_t inv = static_cast<uint16_t>(255 - coverage);
        pixel[0] = static_cast<uint8_t>((src_b * coverage + pixel[0] * inv) / 255);
        pixel[1] = static_cast<uint8_t>((src_g * coverage + pixel[1] * inv) / 255);
        pixel[2] = static_cast<uint8_t>((src_r * coverage + pixel[2] * inv) / 255);
        pixel[3] = untagged_alpha(static_cast<uint8_t>(coverage + (dst_a * inv) / 255));
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

/**
 * @brief Paint the white selection silhouette, in place, from the alpha tag.
 *
 * Rewrites the RGB of every kSelectedAlpha pixel lying within `rim_px` of the
 * edge of the tagged region, and leaves every other pixel alone. Alpha is never
 * touched, so the tag survives and the pass is idempotent on a given buffer.
 *
 * `gap_px` is how deep a run of untagged pixels has to be before it counts as
 * the outside. It exists because the tagged region is a stack of toolpaths, not
 * a filled polygon, and sparse infill leaves pinholes in the middle of it: with
 * gap_px = 1 every pinhole gets outlined and the interior fills with speckle.
 * Requiring the gap to be as deep as the rim is thick means a genuine hole is
 * still outlined while a one-pixel seam between two strokes is not.
 *
 * `order` says how `rgb` is unpacked into the surface. It has no default on
 * purpose: the 2D cache and ghost buffers are ARGB8888 and the 3D path's is a
 * GL_RGBA readback, and a silently-assumed order is what let a non-white outline
 * token render with red and blue swapped in 3D.
 *
 * O(w*h) with one comparison per pixel, then up to 4*(rim_px+gap_px) reads per
 * tagged pixel. Nothing is allocated.
 */
void stroke_selection_rim(const RasterTarget& t, int rim_px, int gap_px, uint32_t rgb,
                          ChannelOrder order);

} // namespace helix::gcode
