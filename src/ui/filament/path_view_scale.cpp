// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "path_view_scale.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {
// Each element's size in scale units.
constexpr float kTubeUnits = 0.6f;
constexpr float kSensorUnits = 0.5f;
constexpr float kHubUnits = 2.3f;
constexpr float kCapsuleOfHub = 0.75f;
constexpr float kHeadUnits = 1.1f;
constexpr float kBracketUnits = 2.1f;
constexpr float kHubWidthUnits = 5.5f;
constexpr float kSpoolUnits = 16.0f;
constexpr float kHeadWidthPerScale = 3.4f;

int32_t round_px(float v) {
    return (int32_t)std::lround(v);
}
} // namespace

PathViewScale path_view_scale(int32_t column_h, TubeLimits tube,
                              int32_t (*head_width_at)(int32_t scale), int32_t label_h) {
    PathViewScale s;
    if (column_h <= 0)
        return s;
    const float u = (float)column_h / kPathViewColumnUnits;
    s.unit = u;
    s.tube_gauge =
        std::clamp(round_px(u * kTubeUnits), tube.floor, std::max(tube.floor, tube.ceil));
    // A sensor dot stands proud of the tube on both sides.
    s.sensor_r = std::max(round_px(u * kSensorUnits), (s.tube_gauge + 3) / 2);
    s.hub_h = std::max({round_px(u * kHubUnits), 2 * s.sensor_r + 2, label_h + 2});
    s.cap_h = std::min(s.hub_h, round_px((float)s.hub_h * kCapsuleOfHub));
    // The buffer's state always rides in the hub: no capsule on the trunk.
    s.buffer_in_hub = true;
    s.head_scale = std::max<int32_t>(1, round_px(u * kHeadUnits));
    const auto head_w = [&](int32_t scale) {
        return head_width_at ? head_width_at(scale) : round_px((float)scale * kHeadWidthPerScale);
    };
    const int32_t min_head_w = (int32_t)std::ceil(kHeadMinTubes * (float)s.tube_gauge);
    while (head_w(s.head_scale) < min_head_w && s.head_scale < 64)
        ++s.head_scale;
    s.bracket_h = round_px(u * kBracketUnits);
    s.hub_min_w = round_px(u * kHubWidthUnits);
    s.spool_max = round_px(u * kSpoolUnits);
    return s;
}

} // namespace helix::ui
