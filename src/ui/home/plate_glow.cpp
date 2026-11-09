// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plate_glow.h"

#include "ui_temperature_utils.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

/// Level thresholds above the burn threshold, in tenths of a degree.
constexpr int kHotDeci = 700;
constexpr int kVeryHotDeci = 900;

/// The plate's own fill, and the haze's strength where it leaves the plate.
constexpr float kPlateAlpha = 190.f;
constexpr float kHazeAlpha = 120.f;
/// The haze's sides soften over this share of the plate's width.
constexpr float kSideFeather = 0.3f;

float smoothstep(float edge0, float edge1, float v) {
    const float t = std::clamp((v - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

/// Where the column at `x` enters and leaves the quad, top first. False when
/// the column misses it.
bool column_span(const std::array<CalloutPoint, 4>& q, float x, float& top, float& bot) {
    bool any = false;
    for (size_t i = 0; i < q.size(); ++i) {
        const CalloutPoint a = q[i], b = q[(i + 1) % q.size()];
        const float x0 = float(std::min(a.x, b.x)), x1 = float(std::max(a.x, b.x));
        if (x < x0 || x > x1 || a.x == b.x)
            continue;
        const float t = (x - float(a.x)) / float(b.x - a.x);
        const float y = float(a.y) + t * float(b.y - a.y);
        top = any ? std::min(top, y) : y;
        bot = any ? std::max(bot, y) : y;
        any = true;
    }
    return any;
}

} // namespace

int bed_glow_level(int current_deci) {
    if (!ui::temperature::is_residual_hot(current_deci))
        return 0;
    return current_deci >= kVeryHotDeci ? 3 : current_deci >= kHotDeci ? 2 : 1;
}

PlateGlowMask render_plate_glow(const std::array<CalloutPoint, 4>& quad, int rise) {
    PlateGlowMask m;
    int min_x = quad[0].x, max_x = quad[0].x, min_y = quad[0].y, max_y = quad[0].y;
    for (const CalloutPoint& p : quad) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }
    if (max_x <= min_x)
        return m;
    rise = std::max(rise, 0);
    m.x = min_x;
    m.y = min_y - rise;
    m.w = max_x - min_x;
    m.h = max_y - m.y + 1;
    m.alpha.assign(size_t(m.w) * size_t(m.h), 0);

    const float feather = std::max(2.f, float(m.w) * kSideFeather);
    for (int col = 0; col < m.w; ++col) {
        const float x = float(m.x + col) + 0.5f;
        float top = 0.f, bot = 0.f;
        if (!column_span(quad, x, top, bot))
            continue;
        const float side = smoothstep(0.f, feather, std::min(x - float(min_x), float(max_x) - x));
        for (int row = 0; row < m.h; ++row) {
            const float y = float(m.y + row) + 0.5f;
            if (y > bot + 0.5f)
                break;
            // Anti-aliased plate coverage along its top and bottom edges.
            const float cover = std::clamp(std::min(y - top, bot - y) + 0.5f, 0.f, 1.f);
            const float plate = kPlateAlpha * cover;
            // The haze is full over the plate and eases out over `rise` above it.
            const float above = top - y;
            const float fade = above <= 0.f ? 1.f
                               : rise > 0   ? std::max(0.f, 1.f - above / float(rise))
                                            : 0.f;
            const float haze = kHazeAlpha * fade * fade * side;
            // Screen blend, so the plate brightens the haze rather than replacing it.
            const float a = 255.f - (255.f - plate) * (255.f - haze) / 255.f;
            m.alpha[size_t(row) * size_t(m.w) + size_t(col)] = uint8_t(std::lround(a));
        }
    }
    return m;
}

} // namespace helix
