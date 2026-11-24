// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"
#include "screensaver_pixel_writer.h"

#include <cmath>
#include <cstdint>

/**
 * @file screensaver_analog_paint.h
 * @brief Antialiased shapes for the analog clock, each drawn from its distance field
 *
 * Every shape is a signed distance (negative inside, in pixels) evaluated per pixel over its
 * bounding box. One coverage rule draws all of them: with `grow` px added to the shape and the
 * edge spread over `soft` px, a crisp shape is (0, 1), a glow is (r, 2r) at low alpha, and a
 * drop shadow is the shape offset and softened. Angles are degrees clockwise from 12 o'clock.
 * No LVGL.
 */

namespace helix::ui {

/// A point in frame pixels.
struct APt {
    float x = 0.0f;
    float y = 0.0f;
};

/// The point `r` px from (cx, cy) at `deg` clockwise from 12 o'clock.
inline APt polar(float cx, float cy, float r, float deg) {
    const float t = deg * 0.017453292f;
    return {cx + r * std::sin(t), cy - r * std::cos(t)};
}

/// How a shape's edge is drawn: `alpha` (0 to 1) at full coverage, the shape grown by `grow` px,
/// its edge spread over `soft` px (1 is a one-pixel antialiased edge).
struct AEdge {
    float alpha = 1.0f;
    float grow = 0.0f;
    float soft = 1.0f;
};

/// Coverage (0 to 1) of a pixel at signed distance `d` from a shape drawn with `e`.
inline float edge_cover(float d, const AEdge& e) {
    const float v = 0.5f - (d - e.grow) / e.soft;
    return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v);
}

/**
 * @brief Draws shapes into a frame, clipped to one box
 *
 * Pixels are blended over what is there and dithered on RGB565, so a 16 bpp frame shows smooth
 * gradients and edges without bands.
 */
class APainter {
  public:
    APainter(const FrameTarget& frame, const DirtyRect& clip);

    /// A filled disc.
    void disc(APt c, float r, Rgb color, const AEdge& e = {});
    /// A ring between radii `r_in` and `r_out`.
    void ring(APt c, float r_in, float r_out, Rgb color, const AEdge& e = {});
    /// The part of a ring from `deg0` clockwise to `deg1` (a span of 360 or more is the whole
    /// ring), with square ends.
    void arc(APt c, float r_in, float r_out, float deg0, float deg1, Rgb color,
             const AEdge& e = {});
    /// A pie slice of radius `r` from `deg0` clockwise to `deg1`.
    void wedge(APt c, float r, float deg0, float deg1, Rgb color, const AEdge& e = {});
    /// A stroke `w` px wide from `a` to `b`, with round ends when `round` and square ends at
    /// the points otherwise.
    void segment(APt a, APt b, float w, Rgb color, bool round, const AEdge& e = {});
    /// A tapered bar from `a` (`w0` wide) to `b` (`w1` wide), square ended: a hand or a baton.
    void taper(APt a, APt b, float w0, float w1, Rgb color, const AEdge& e = {});
    /// A disc whose colour runs from `inner` at the focus (`fx`, `fy` in units of `r` from the
    /// centre) to `outer` at the rim.
    void radial_disc(APt c, float r, Rgb inner, Rgb outer, float fx, float fy);
    /// A ring shaded like brushed steel: `light` facing the top left, `dark` facing the bottom
    /// right.
    void steel_ring(APt c, float r_in, float r_out, Rgb light, Rgb dark);
    /// A sweep's afterglow: a ring band from `deg_head - span` to `deg_head` whose alpha falls
    /// from `alpha` at the head to 0 at the tail, squared.
    void trail(APt c, float r_in, float r_out, float deg_head, float span, Rgb color, float alpha);
    /// A rectangle with corners rounded by `radius`.
    void round_rect(float x1, float y1, float x2, float y2, float radius, Rgb color,
                    const AEdge& e = {});
    /// Blends `color` over every pixel of the clip at `alpha`.
    void fill(Rgb color, float alpha = 1.0f);
    /// Blends a `w` x `h` coverage map with its top left at (x, y) in `color`.
    void coverage(const uint8_t* cov, int32_t w, int32_t h, int32_t x, int32_t y, Rgb color,
                  float alpha = 1.0f);

    const DirtyRect& clip() const {
        return clip_;
    }

  private:
    /// Visits every clipped pixel of the box (x1, y1)-(x2, y2), blending `color` by the
    /// coverage `cov(px, py)` returns for the pixel centre.
    template <typename Cov>
    void shade(float x1, float y1, float x2, float y2, Rgb color, float alpha, Cov&& cov);

    FrameTarget frame_;
    DirtyRect clip_;
    PixelWriter writer_;
};

/// The pixel box of a shape spanning (x1, y1)-(x2, y2) drawn with `pad` px of edge, glow or
/// shadow around it.
DirtyRect apaint_box(float x1, float y1, float x2, float y2, float pad);

/// apaint_box() of the segment from `a` to `b`, `pad` px round it.
DirtyRect apaint_segment_box(APt a, APt b, float pad);

} // namespace helix::ui
