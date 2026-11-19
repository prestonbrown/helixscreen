// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unit_art_geometry.h"

#include "ui_ams_slot_layout.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::tray {

float wound_radius(float core, float full, float fill) {
    return core + (full - core) * std::clamp(fill, 0.0f, 1.0f);
}

float art_lane_distance(const ArtLaneGeom& g, float x, float y) {
    // Into the flange basis, where the spool's discs are circles of radius 1.
    const float det = g.ey_x * g.ez_y - g.ey_y * g.ez_x;
    if (det == 0.0f)
        return INFINITY;
    auto to_basis = [&](float px, float py, float& u, float& v) {
        u = (px * g.ez_y - py * g.ez_x) / det;
        v = (g.ey_x * py - g.ey_y * px) / det;
    };
    float pu, pv, du, dv;
    to_basis(x - g.far_x, y - g.far_y, pu, pv);
    to_basis(g.near_x - g.far_x, g.near_y - g.far_y, du, dv);
    const float len2 = du * du + dv * dv;
    const float t = len2 > 0 ? std::clamp((pu * du + pv * dv) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(pu - t * du, pv - t * dv);
}

float art_lane_flange_distance(const ArtLaneGeom& g, float x, float y, bool near_flange) {
    const float det = g.ey_x * g.ez_y - g.ey_y * g.ez_x;
    if (det == 0.0f)
        return INFINITY;
    const float px = x - (near_flange ? g.near_x : g.far_x);
    const float py = y - (near_flange ? g.near_y : g.far_y);
    return std::hypot((px * g.ez_y - py * g.ez_x) / det, (g.ey_x * py - g.ey_y * px) / det);
}

FlangeTurn flange_turn(float degrees) {
    const float rad = degrees * 3.14159265f / 180.0f;
    return {std::cos(rad), std::sin(rad)};
}

float art_lane_px_per_unit(const ArtLaneGeom& g, float drawn_w) {
    // The flange's vertical radius on screen, the basis's shortest reach.
    return std::min(std::hypot(g.ey_x, g.ey_y), std::hypot(g.ez_x, g.ez_y)) * drawn_w;
}

float art_lane_coverage(const ArtLaneGeom& g, float x, float y, float radius, float px_per_unit) {
    return std::clamp((radius - art_lane_distance(g, x, y)) * px_per_unit + 0.5f, 0.0f, 1.0f);
}

bool unit_view_fits(const UnitArtGeom& geom) {
    const int n = std::clamp(geom.lanes, 0, kMaxArtLanes);
    // The bands are integer pixels, so the drawing is measured at a width fine enough that
    // rounding cannot move a point across a boundary.
    constexpr float kScale = 10000.0f;
    constexpr float kMinOutletGap = 0.01f; ///< of the drawing's width, about a tube apart
    auto px = [&](float v) { return static_cast<int32_t>(std::lround(v * kScale)); };
    AmsLaneLine lines[kMaxArtLanes];
    for (int i = 0; i < n; ++i) {
        const ArtLaneGeom& g = geom.lane[i];
        lines[i] = {px((g.box_x0 + g.box_x1) / 2), px((g.box_y0 + g.box_y1) / 2), px(g.outlet_x),
                    px(g.outlet_y)};
        for (int j = 0; j < i; ++j) {
            if (std::abs(geom.lane[j].outlet_x - g.outlet_x) < kMinOutletGap)
                return false;
        }
    }
    // Each slot spans the whole drawing, so only the boundaries between lanes trim its band.
    const int32_t x1 = px(-1.0f), x2 = px(2.0f);
    for (int i = 0; i < n; ++i) {
        const AmsSlotHitBand band = ams_slot_lane_hit_band(lines, i, n, x1, 0, x2);
        const AmsLaneLine& l = lines[i];
        if (!ams_slot_hit_band_holds(band, x1, 0, x2, l.x, l.y) ||
            !ams_slot_hit_band_holds(band, x1, 0, x2, l.out_x, l.out_y))
            return false;
    }
    return true;
}

} // namespace helix::ui::tray
