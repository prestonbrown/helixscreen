// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unit_art_geometry.h"

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

float art_lane_px_per_unit(const ArtLaneGeom& g, float drawn_w) {
    // The flange's vertical radius on screen, the basis's shortest reach.
    return std::min(std::hypot(g.ey_x, g.ey_y), std::hypot(g.ez_x, g.ez_y)) * drawn_w;
}

float art_lane_coverage(const ArtLaneGeom& g, float x, float y, float radius, float px_per_unit) {
    return std::clamp((radius - art_lane_distance(g, x, y)) * px_per_unit + 0.5f, 0.0f, 1.0f);
}

} // namespace helix::ui::tray
