// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plate_glow.h"

#include "ui_temperature_utils.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

/// A bed's and a chamber's glow levels: past `warm` is 1, from `hot` 2, from `very_hot` 3.
struct GlowScale {
    int warm;
    int hot;
    int very_hot;
};
constexpr GlowScale kBedScale{ui::temperature::kResidualHeatThresholdDeci, 700, 900};
constexpr GlowScale kChamberScale{300, 450, 600};

int glow_level(int current_deci, const GlowScale& s) {
    if (current_deci <= s.warm)
        return 0;
    return current_deci >= s.very_hot ? 3 : current_deci >= s.hot ? 2 : 1;
}

/// The plate's own fill, and the haze's strength where it leaves the plate.
constexpr float kPlateAlpha = 110.f;
constexpr float kHazeAlpha = 120.f;
/// The haze's sides soften over this share of the plate's width.
constexpr float kSideFeather = 0.3f;
/// How far a region's edge fades in, in pixels.
constexpr float kRegionFeatherPx = 3.f;

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

float signed_area(const GlowQuad& q) {
    float area = 0.f;
    for (size_t i = 0; i < q.size(); ++i) {
        const CalloutPoint& a = q[i];
        const CalloutPoint& b = q[(i + 1) % q.size()];
        area += float(a.x) * float(b.y) - float(b.x) * float(a.y);
    }
    return area / 2.f;
}

/// A convex quad's edges as lines nx*x + ny*y + c, positive on its inner side.
struct QuadEdges {
    std::array<float, 4> nx{}, ny{}, c{};
    size_t count = 0;
};

QuadEdges edges_of(const GlowQuad& q) {
    QuadEdges e;
    const float winding = signed_area(q) >= 0.f ? 1.f : -1.f;
    for (size_t i = 0; i < q.size(); ++i) {
        const CalloutPoint& a = q[i];
        const CalloutPoint& b = q[(i + 1) % q.size()];
        const float ex = float(b.x - a.x), ey = float(b.y - a.y);
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len <= 0.f)
            continue;
        e.nx[e.count] = -winding * ey / len;
        e.ny[e.count] = winding * ex / len;
        e.c[e.count] = -(e.nx[e.count] * float(a.x) + e.ny[e.count] * float(a.y));
        ++e.count;
    }
    return e;
}

/// 1 well inside the quad, easing to 0 at its edge, 0 outside.
float quad_cover(const QuadEdges& e, float x, float y) {
    float nearest = kRegionFeatherPx;
    for (size_t i = 0; i < e.count; ++i) {
        const float d = e.nx[i] * x + e.ny[i] * y + e.c[i];
        if (d <= 0.f)
            return 0.f;
        nearest = std::min(nearest, d);
    }
    return nearest / kRegionFeatherPx;
}

} // namespace

int bed_glow_level(int current_deci) {
    return glow_level(current_deci, kBedScale);
}

int chamber_glow_level(int current_deci) {
    return glow_level(current_deci, kChamberScale);
}

PlateGlowMask render_region_glow(const std::vector<GlowQuad>& quads, CalloutPoint source,
                                 float reach, float floor, float peak,
                                 const std::vector<GlowQuad>& cutouts) {
    PlateGlowMask m;
    if (quads.empty())
        return m;
    int min_x = quads[0][0].x, max_x = min_x, min_y = quads[0][0].y, max_y = min_y;
    float area = 0.f;
    for (const GlowQuad& q : quads) {
        area = std::max(area, std::abs(signed_area(q)));
        for (const CalloutPoint& p : q) {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
    }
    if (area <= 0.f || max_x <= min_x || max_y <= min_y)
        return m;
    const int full_w = max_x - min_x;
    const int full_h = max_y - min_y;
    int step = 1;
    while (size_t((full_w + step - 1) / step) * size_t((full_h + step - 1) / step) >
           kMaxGlowMaskBytes)
        ++step;
    m.x = min_x;
    m.y = min_y;
    m.step = step;
    m.w = (full_w + step - 1) / step;
    m.h = (full_h + step - 1) / step;
    m.alpha.assign(size_t(m.w) * size_t(m.h), 0);
    std::vector<QuadEdges> edges;
    edges.reserve(quads.size());
    for (const GlowQuad& q : quads)
        edges.push_back(edges_of(q));
    std::vector<QuadEdges> cut_edges;
    cut_edges.reserve(cutouts.size());
    for (const GlowQuad& q : cutouts)
        cut_edges.push_back(edges_of(q));

    for (int row = 0; row < m.h; ++row) {
        const float y = float(m.y) + (float(row) + 0.5f) * float(step);
        for (int col = 0; col < m.w; ++col) {
            const float x = float(m.x) + (float(col) + 0.5f) * float(step);
            float cover = 0.f;
            for (const QuadEdges& e : edges)
                cover = std::max(cover, quad_cover(e, x, y));
            float cut = 0.f;
            for (const QuadEdges& e : cut_edges)
                cut = std::max(cut, quad_cover(e, x, y));
            cover *= 1.f - cut;
            if (cover <= 0.f)
                continue;
            const float dx = x - float(source.x), dy = y - float(source.y);
            const float t = 1.f - std::clamp(std::sqrt(dx * dx + dy * dy) / reach, 0.f, 1.f);
            const float light = floor + (1.f - floor) * smoothstep(0.f, 1.f, t);
            m.alpha[size_t(row) * size_t(m.w) + size_t(col)] =
                uint8_t(std::lround(peak * cover * light));
        }
    }
    return m;
}

PlateGlowMask render_disc_glow(CalloutPoint center, int radius) {
    PlateGlowMask m;
    if (radius <= 0)
        return m;
    m.x = center.x - radius;
    m.y = center.y - radius;
    m.w = m.h = radius * 2;
    m.alpha.assign(size_t(m.w) * size_t(m.h), 0);
    for (int row = 0; row < m.h; ++row) {
        for (int col = 0; col < m.w; ++col) {
            const float dx = float(col) + 0.5f - float(radius);
            const float dy = float(row) + 0.5f - float(radius);
            const float d = std::sqrt(dx * dx + dy * dy) / float(radius);
            m.alpha[size_t(row) * size_t(m.w) + size_t(col)] =
                uint8_t(std::lround(255.f * smoothstep(0.f, 1.f, 1.f - d)));
        }
    }
    return m;
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
