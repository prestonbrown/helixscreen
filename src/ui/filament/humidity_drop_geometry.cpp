// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "humidity_drop_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::humidity_drop {

namespace {

// Single precision throughout: the ESP32-S3's FPU has no double unit.
constexpr float kTwoPi = 6.28318530718f;

// The MDI "water" glyph in its 24-unit box: a half circle of radius 6 under a
// cubic from the equator's left end (6,14), control (6,10), to the tip
// (12,3.25). In radii from the circle's centre the tip sits 10.75/6 above it.
// The second control sits just below and outside the tip, so the sides pinch
// in slightly on the way up, a classic teardrop, and still meet in a point.
constexpr float kTipAbove = 10.75f / 6.0f;
constexpr float kCtrlAbove = 4.0f / 6.0f;
constexpr float kTipCtrlOut = 0.08f;
constexpr float kTipCtrlBelow = 0.35f;

// The drop's share of its box's height, the rest left as air.
constexpr float kHeightShare = 0.84f;
constexpr float kWidthShare = 0.92f;

/// The upper curve at parameter t: half width u and height above the centre v,
/// both in radii. t=0 is the equator, t=1 the tip; v falls monotonically.
void upper_curve(float t, float& u, float& v) {
    const float s = 1.0f - t;
    u = s * s * (1.0f + 2.0f * t) + 3.0f * s * t * t * kTipCtrlOut;
    v = 3.0f * s * s * t * kCtrlAbove + 3.0f * s * t * t * (kTipAbove - kTipCtrlBelow) +
        t * t * t * kTipAbove;
}

/// Half width in radii at height @p above (radii above the centre), 0..kTipAbove.
float upper_half_width(float above) {
    float lo = 0.0f;
    float hi = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
    for (int i = 0; i < 18; ++i) {
        const float mid = 0.5f * (lo + hi);
        upper_curve(mid, u, v);
        if (v < above)
            lo = mid;
        else
            hi = mid;
    }
    upper_curve(0.5f * (lo + hi), u, v);
    return u;
}

/// What a row needs to measure distance across it, so a raster pays the curve
/// inversion once per row rather than per pixel.
struct Row {
    enum Kind { AboveTip, Upper, Round } kind;
    float hw;       ///< half width (Upper)
    float inv_norm; ///< 1/sqrt(1 + slope^2) of the edge (Upper)
};

Row row_profile(const DropShape& s, float y) {
    if (y >= s.cy)
        return {Row::Round, 0, 0};
    if (y <= s.tip_y)
        return {Row::AboveTip, 0, 0};
    const float hw = half_width(s, y);
    const float e = 0.5f;
    const float slope = (half_width(s, std::min(y + e, s.cy)) - half_width(s, y - e)) /
                        (std::min(y + e, s.cy) - (y - e));
    return {Row::Upper, hw, 1.0f / std::sqrt(1.0f + slope * slope)};
}

float row_distance(const DropShape& s, const Row& row, float x, float y) {
    const float dx = std::fabs(x - s.cx);
    switch (row.kind) {
    case Row::Round: {
        const float dy = y - s.cy;
        return std::sqrt(dx * dx + dy * dy) - s.r;
    }
    case Row::AboveTip: {
        const float dy = s.tip_y - y;
        return std::sqrt(dx * dx + dy * dy);
    }
    case Row::Upper:
    default:
        return (dx - row.hw) * row.inv_norm;
    }
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/// Premultiplied colour accumulator for one pixel, layered back to front.
struct Acc {
    float r = 0, g = 0, b = 0, a = 0;
    void over(const Rgb& c, float alpha) {
        if (alpha <= 0.0f)
            return;
        const float k = 1.0f - alpha;
        r = c.r * alpha + r * k;
        g = c.g * alpha + g * k;
        b = c.b * alpha + b * k;
        a = alpha + a * k;
    }
};

// Translucent layers, as alpha.
constexpr float kTintAlpha = 0.14f;
constexpr float kHighlightAlpha = 0.45f;
constexpr float kUnknownOutlineAlpha = 0.45f;

} // namespace

DropShape fit_drop(int w, int h) {
    const float height_units = 1.0f + kTipAbove;
    float r = (h * kHeightShare) / height_units;
    r = std::min(r, w * kWidthShare * 0.5f);
    DropShape s;
    s.r = r;
    s.cx = w * 0.5f;
    s.tip_y = (h - r * height_units) * 0.5f;
    s.cy = s.tip_y + r * kTipAbove;
    return s;
}

float half_width(const DropShape& s, float y) {
    if (s.r <= 0.0f || y < s.tip_y || y > s.bottom())
        return 0.0f;
    if (y >= s.cy) {
        const float dy = y - s.cy;
        return std::sqrt(std::max(0.0f, s.r * s.r - dy * dy));
    }
    return s.r * upper_half_width((s.cy - y) / s.r);
}

float signed_distance(const DropShape& s, float x, float y) {
    return row_distance(s, row_profile(s, y), x, y);
}

float stroke_width(const DropShape& s) {
    return std::clamp(s.r * 0.17f, 1.0f, 3.5f);
}

float wave_amplitude(const DropShape& s) {
    // 10% of the radius at icon size, rising to 17% from r = 20px up.
    const float grow = std::clamp((s.r - 6.0f) / 14.0f, 0.0f, 1.0f);
    return std::max(0.75f, s.r * (0.10f + 0.07f * grow));
}

float fill_height(float fraction) {
    return clamp01(fraction);
}

float amplitude_at(const DropShape& s, float fraction) {
    const float h = fill_height(fraction);
    // Flat at the floor and the tip, so an empty drop shows no water and a
    // nearly full one no notch.
    const float calm = std::min(clamp01(h / 0.1f), clamp01((1.0f - h) / 0.25f));
    return wave_amplitude(s) * calm;
}

float fill_fraction(int humidity_x10) {
    if (humidity_x10 < 0)
        return -1.0f;
    return std::min(1.0f, humidity_x10 / 1000.0f);
}

float surface_level(const DropShape& s, float fraction) {
    return s.bottom() - (s.bottom() - s.tip_y) * fill_height(fraction);
}

float wave_y(const DropShape& s, float level, float amplitude, float phase, float x) {
    if (s.r <= 0.0f)
        return level;
    // Two crests a drop's width, under a swell one drop's width long that lifts
    // one and lowers the other, so they never stand level like a sine.
    // Whole-number multiples of the phase keep the shape seamless as it wraps.
    const float t = kTwoPi * (x - s.cx) / (2.0f * s.r);
    return level + amplitude * (0.65f * std::sin(2.0f * t + 2.0f * phase) +
                                0.35f * std::sin(t + phase - 0.785f));
}

float back_phase(float front_phase) {
    return 3.6f - front_phase;
}

namespace {

struct Hsv {
    float h, s, v; // h in 0..6 (sextants), s and v in 0..1
};

Hsv to_hsv(Rgb c) {
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 0.0f) {
        if (mx == r)
            h = std::fmod((g - b) / d + 6.0f, 6.0f);
        else if (mx == g)
            h = (b - r) / d + 2.0f;
        else
            h = (r - g) / d + 4.0f;
    }
    return {h, mx > 0.0f ? d / mx : 0.0f, mx};
}

Rgb to_rgb(Hsv c) {
    const float f = c.h - std::floor(c.h);
    const float p = c.v * (1.0f - c.s);
    const float q = c.v * (1.0f - c.s * f);
    const float t = c.v * (1.0f - c.s * (1.0f - f));
    float r, g, b;
    switch (static_cast<int>(c.h) % 6) {
    case 0:
        r = c.v, g = t, b = p;
        break;
    case 1:
        r = q, g = c.v, b = p;
        break;
    case 2:
        r = p, g = c.v, b = t;
        break;
    case 3:
        r = p, g = q, b = c.v;
        break;
    case 4:
        r = t, g = p, b = c.v;
        break;
    default:
        r = c.v, g = p, b = q;
        break;
    }
    auto byte = [](float x) { return static_cast<uint8_t>(clamp01(x) * 255.0f + 0.5f); };
    return {byte(r), byte(g), byte(b)};
}

Rgb mix(Rgb a, Rgb b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

} // namespace

DropColors water_colors(Rgb water, Rgb background) {
    Hsv base = to_hsv(water);
    // A tinted colour too grey to read as water is lifted; a true grey (a theme
    // that chose no hue) stays grey.
    if (base.s >= 0.12f)
        base.s = std::clamp(base.s, 0.55f, 0.90f);
    const bool dark_bg = to_hsv(background).v < 0.5f;

    const Hsv front{base.h, base.s * 0.85f, std::clamp(base.v, 0.75f, 0.95f)};
    const Hsv back{base.h, std::min(1.0f, base.s * 1.05f), front.v * 0.62f};
    DropColors c;
    c.front = to_rgb(front);
    c.back = to_rgb(back);
    c.outline = dark_bg ? c.front : to_rgb({base.h, base.s, back.v * 0.85f});
    c.highlight = mix(c.front, {255, 255, 255}, 0.75f);
    return c;
}

bool should_animate(bool animations_enabled, bool visible, int humidity_x10) {
    return animations_enabled && visible && humidity_x10 > 0 && humidity_x10 < 1000;
}

void rasterize(uint8_t* px, int w, int h, int stride, const DropColors& colors, int humidity_x10,
               float phase) {
    if (!px || w <= 0 || h <= 0)
        return;

    const DropShape s = fit_drop(w, h);
    const float sw = stroke_width(s);
    const float fraction = fill_fraction(humidity_x10);
    const bool known = fraction >= 0.0f;
    const bool has_fill = known && fraction > 0.0f;

    const float amp = amplitude_at(s, fraction);
    const float level = surface_level(s, fraction);
    // The back wave rides a little higher so its crests show between the front's.
    const float back_level = level - 0.6f * amp;
    const float back_ph = back_phase(phase);
    // Rows wholly above both waves hold no water; rows wholly below are full.
    const float wave_top = back_level - amp - 1.0f;
    const float wave_bottom = level + amp + 1.0f;

    // A specular spot up and left on the round part sells the bulge; too small to
    // read below a few pixels of radius.
    const bool highlight = known && s.r >= 6.0f;
    const float hl_cx = s.cx - s.r * 0.45f;
    const float hl_cy = s.cy - s.r * 0.30f;
    const float hl_rx = std::max(1.0f, s.r * 0.15f);
    const float hl_ry = std::max(1.5f, s.r * 0.36f);

    const float outline_alpha = known ? 1.0f : kUnknownOutlineAlpha;

    for (int j = 0; j < h; ++j) {
        uint8_t* out = px + j * stride;
        const float y = j + 0.5f;
        const Row row = row_profile(s, y);
        for (int i = 0; i < w; ++i, out += 4) {
            const float x = i + 0.5f;
            const float d = row_distance(s, row, x, y);
            if (d >= 1.0f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }

            Acc acc;
            // The water keeps clear of the outline's outer half.
            const float inner = clamp01(0.5f - (d + sw * 0.5f));
            if (known && inner > 0.0f) {
                acc.over(colors.front, inner * kTintAlpha);
                if (has_fill && y >= wave_top) {
                    float back_cov;
                    float front_cov;
                    if (y > wave_bottom) {
                        back_cov = front_cov = 1.0f;
                    } else {
                        back_cov = clamp01(y - wave_y(s, back_level, amp, back_ph, x) + 0.5f);
                        front_cov = clamp01(y - wave_y(s, level, amp, phase, x) + 0.5f);
                    }
                    acc.over(colors.back, inner * back_cov);
                    acc.over(colors.front, inner * front_cov);
                }
                if (highlight) {
                    const float ex = (x - hl_cx) / hl_rx;
                    const float ey = (y - hl_cy) / hl_ry;
                    // Distance outside the ellipse, roughly in pixels, for a soft edge.
                    const float e = (std::sqrt(ex * ex + ey * ey) - 1.0f) * hl_rx;
                    acc.over(colors.highlight, inner * kHighlightAlpha * clamp01(0.5f - e));
                }
            }

            const float ring = clamp01(std::min(0.5f - d, 0.5f + d + sw));
            acc.over(colors.outline, ring * outline_alpha);

            if (acc.a <= 0.0f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }
            const float inv = 1.0f / acc.a;
            out[0] = static_cast<uint8_t>(std::min(255.0f, acc.b * inv + 0.5f));
            out[1] = static_cast<uint8_t>(std::min(255.0f, acc.g * inv + 0.5f));
            out[2] = static_cast<uint8_t>(std::min(255.0f, acc.r * inv + 0.5f));
            out[3] = static_cast<uint8_t>(std::min(255.0f, acc.a * 255.0f + 0.5f));
        }
    }
}

} // namespace helix::ui::humidity_drop
