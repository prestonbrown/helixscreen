// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_stroke_digits.h"

#include "screensaver_blur.h"
#include "screensaver_buffer.h"
#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

// Digits are strokes along lines and circular arcs, in a box one unit wide and
// kStrokeDigitPathH tall (y down). Angles are degrees, 0 pointing right and growing
// clockwise on screen, so -90 is the top of a circle.

struct Stroke {
    bool arc;
    float a, b, c, d, e; ///< line: x0 y0 x1 y1; arc: cx cy r start sweep
};

constexpr Stroke line(float x0, float y0, float x1, float y1) {
    return {false, x0, y0, x1, y1, 0.0f};
}
constexpr Stroke arc(float cx, float cy, float r, float start, float sweep) {
    return {true, cx, cy, r, start, sweep};
}

constexpr size_t kMaxStrokes = 4;

struct GlyphDef {
    size_t count;
    Stroke strokes[kMaxStrokes];
};

constexpr GlyphDef kSix = {3,
                           {arc(0.5f, 1.1f, 0.5f, 0.0f, 360.0f), line(0.0f, 1.1f, 0.0f, 0.5f),
                            arc(0.5f, 0.5f, 0.5f, 180.0f, 125.0f)}};

constexpr GlyphDef kGlyphs[10] = {
    {4,
     {arc(0.5f, 0.5f, 0.5f, 180.0f, 180.0f), arc(0.5f, 1.1f, 0.5f, 0.0f, 180.0f),
      line(0.0f, 0.5f, 0.0f, 1.1f), line(1.0f, 0.5f, 1.0f, 1.1f)}},
    {2, {line(0.55f, 0.0f, 0.55f, 1.6f), line(0.55f, 0.0f, 0.2f, 0.3f)}},
    {3,
     {arc(0.5f, 0.5f, 0.5f, 180.0f, 215.0f), line(0.91f, 0.787f, 0.0f, 1.6f),
      line(0.0f, 1.6f, 1.0f, 1.6f)}},
    {2, {arc(0.5f, 0.38f, 0.38f, 200.0f, 250.0f), arc(0.5f, 1.18f, 0.42f, 270.0f, 250.0f)}},
    {3,
     {line(0.72f, 0.0f, 0.72f, 1.6f), line(0.72f, 0.0f, 0.0f, 1.08f),
      line(0.0f, 1.08f, 1.0f, 1.08f)}},
    {3,
     {line(0.92f, 0.0f, 0.12f, 0.0f), line(0.12f, 0.0f, 0.09f, 0.813f),
      arc(0.5f, 1.1f, 0.5f, 215.0f, 280.0f)}},
    kSix,
    {2, {line(0.0f, 0.0f, 1.0f, 0.0f), line(1.0f, 0.0f, 0.32f, 1.6f)}},
    {2, {arc(0.5f, 0.38f, 0.38f, 0.0f, 360.0f), arc(0.5f, 1.18f, 0.42f, 0.0f, 360.0f)}},
    {0, {}}, // a six turned upside down
};

float dist_to_line(const Stroke& s, float px, float py) {
    const float vx = s.c - s.a;
    const float vy = s.d - s.b;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((px - s.a) * vx + (py - s.b) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(px - (s.a + t * vx), py - (s.b + t * vy));
}

float dist_to_arc(const Stroke& s, float px, float py) {
    const float dx = px - s.a;
    const float dy = py - s.b;
    const float dist = std::hypot(dx, dy);
    if (s.e >= 360.0f) {
        return std::fabs(dist - s.c);
    }
    const float angle = std::atan2(dy, dx) * 180.0f / kSaverPi;
    const float rel = std::fmod(angle - s.d + 720.0f, 360.0f);
    if (rel <= s.e) {
        return std::fabs(dist - s.c);
    }
    const float a0 = s.d * kSaverPi / 180.0f;
    const float a1 = (s.d + s.e) * kSaverPi / 180.0f;
    return std::min(std::hypot(px - (s.a + s.c * std::cos(a0)), py - (s.b + s.c * std::sin(a0))),
                    std::hypot(px - (s.a + s.c * std::cos(a1)), py - (s.b + s.c * std::sin(a1))));
}

} // namespace

float stroke_digit_distance(int digit, float px, float py) {
    // Nine is six turned half a turn about the path box's centre.
    if (digit == 9) {
        return stroke_digit_distance(6, 1.0f - px, kStrokeDigitPathH - py);
    }
    if (digit < 0 || digit > 9) {
        return 1e9f;
    }
    const GlyphDef& def = kGlyphs[digit];
    float best = 1e9f;
    for (size_t i = 0; i < def.count; i++) {
        const Stroke& s = def.strokes[i];
        best = std::min(best, s.arc ? dist_to_arc(s, px, py) : dist_to_line(s, px, py));
    }
    return best;
}

uint8_t StrokeDigitSet::at(int digit, int32_t gx, int32_t gy) const {
    if (digit < 0 || digit > 9 || gx < 0 || gy < 0 || gx >= w || gy >= h) {
        return 0;
    }
    return coverage[(static_cast<size_t>(digit) * h + gy) * w + gx];
}

size_t StrokeDigitSet::heap_bytes() const {
    return buffer_bytes(coverage);
}

void StrokeDigitSet::clear() {
    release_buffer(coverage);
    w = 0;
    h = 0;
}

void build_stroke_digits(int32_t unit, float stroke, StrokeDigitSet& out) {
    const auto u = static_cast<float>(std::max<int32_t>(1, unit));
    out.w = static_cast<int32_t>(std::lround((1.0f + stroke) * u));
    out.h = static_cast<int32_t>(std::lround((kStrokeDigitPathH + stroke) * u));
    out.coverage.assign(static_cast<size_t>(out.w) * out.h * 10, 0);
    const float half = stroke / 2.0f;
    for (int digit = 0; digit < 10; digit++) {
        uint8_t* cov = &out.coverage[static_cast<size_t>(digit) * out.w * out.h];
        for (int32_t y = 0; y < out.h; y++) {
            const float py = (static_cast<float>(y) + 0.5f) / u - half;
            for (int32_t x = 0; x < out.w; x++) {
                const float px = (static_cast<float>(x) + 0.5f) / u - half;
                const float edge = edge_coverage((half - stroke_digit_distance(digit, px, py)) * u);
                cov[static_cast<size_t>(y) * out.w + x] =
                    static_cast<uint8_t>(std::lround(edge * 255.0f));
            }
        }
    }
}

void build_stroke_halo(const StrokeDigitSet& digits, int32_t radius, StrokeDigitSet& out) {
    radius = std::max<int32_t>(1, radius);
    out.w = digits.w + 2 * radius;
    out.h = digits.h + 2 * radius;
    out.coverage.assign(static_cast<size_t>(out.w) * out.h * 10, 0);
    const int32_t grow = std::max<int32_t>(1, radius / 3);
    const int32_t soften = std::max<int32_t>(1, radius / 2);
    std::vector<float> grid(static_cast<size_t>(out.w) * out.h);
    std::vector<float> wide(grid.size());
    std::vector<float> tmp(grid.size());
    for (int digit = 0; digit < 10; digit++) {
        std::fill(grid.begin(), grid.end(), 0.0f);
        for (int32_t y = 0; y < digits.h; y++) {
            for (int32_t x = 0; x < digits.w; x++) {
                grid[static_cast<size_t>(y + radius) * out.w + x + radius] =
                    static_cast<float>(digits.at(digit, x, y)) / 255.0f;
            }
        }
        // Widen the strokes so the blur leaves a body of shade behind them, not a faint rim.
        for (int32_t y = 0; y < out.h; y++) {
            for (int32_t x = 0; x < out.w; x++) {
                float most = 0.0f;
                for (int32_t dy = -grow; dy <= grow; dy++) {
                    const int32_t yy = std::clamp(y + dy, 0, out.h - 1);
                    for (int32_t dx = -grow; dx <= grow; dx++) {
                        const int32_t xx = std::clamp(x + dx, 0, out.w - 1);
                        most = std::max(most, grid[static_cast<size_t>(yy) * out.w + xx]);
                    }
                }
                wide[static_cast<size_t>(y) * out.w + x] = most;
            }
        }
        box_blur(wide.data(), out.w, out.h, soften, false, false, tmp.data(), 3);
        uint8_t* cov = &out.coverage[static_cast<size_t>(digit) * out.w * out.h];
        for (size_t i = 0; i < wide.size(); i++) {
            cov[i] = static_cast<uint8_t>(std::lround(std::clamp(wide[i], 0.0f, 1.0f) * 255.0f));
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
