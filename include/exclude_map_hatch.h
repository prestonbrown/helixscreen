// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Diagonal hatching for an excluded object's outline on the mini map.
//
// Pure geometry, no LVGL: the map view hands the outline in pixels and draws the
// segments that come back.

#include "sort_small.h"

#include <algorithm>
#include <cmath>
#include <glm/vec2.hpp>
#include <vector>

namespace helix {

struct HatchSegment {
    glm::vec2 a;
    glm::vec2 b;
};

/// Lines running up and to the right (x + y constant), one every @p spacing along the
/// x + y axis, clipped to the inside of @p polygon by the even-odd rule so a concave
/// outline hatches only the material. Empty for a polygon under three points or a
/// spacing that is not positive.
[[nodiscard]] inline std::vector<HatchSegment> hatch_polygon(const std::vector<glm::vec2>& polygon,
                                                             float spacing) {
    std::vector<HatchSegment> out;
    if (polygon.size() < 3 || !(spacing > 0.0f))
        return out;

    float lo = polygon.front().x + polygon.front().y;
    float hi = lo;
    for (const auto& p : polygon) {
        lo = std::min(lo, p.x + p.y);
        hi = std::max(hi, p.x + p.y);
    }

    const size_t n = polygon.size();
    std::vector<glm::vec2> hits;
    // Strictly inside the range: a line through the extreme corner has no length.
    for (float c = (std::floor(lo / spacing) + 1.0f) * spacing; c < hi; c += spacing) {
        hits.clear();
        for (size_t i = 0; i < n; ++i) {
            const glm::vec2& p = polygon[i];
            const glm::vec2& q = polygon[(i + 1) % n];
            const float up = p.x + p.y;
            const float uq = q.x + q.y;
            // Half-open on the low side, so a vertex on the line counts once.
            if ((up <= c) == (uq <= c))
                continue;
            hits.push_back(p + (q - p) * ((c - up) / (uq - up)));
        }
        sort_small(hits.begin(), hits.end(),
                   [](const glm::vec2& l, const glm::vec2& r) { return l.x < r.x; });
        for (size_t i = 0; i + 1 < hits.size(); i += 2)
            out.push_back({hits[i], hits[i + 1]});
    }
    return out;
}

} // namespace helix
