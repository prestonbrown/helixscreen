// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One scale for the AMS unit view and its filament path: every element (the unit's art, the
// tube, the linked-lanes bracket, the hub, the buffer capsule, the head, the sensors) is a fixed
// ratio of one unit derived from the column the view stands in, so the picture keeps its
// proportions at every screen size and every unit on one screen shares the same head.

#pragma once

#include <cstdint>

namespace helix::ui {

/// The tube core's bounds: the scale picks the gauge between them.
struct TubeLimits {
    int32_t floor = 4;
    int32_t ceil = 8;
};

/// The tube's bounds on every screen: never under 4 px, so a tube reads on the smallest
/// screen, nor over 8, so it never outweighs the head.
inline constexpr TubeLimits kPathTubeLimits{4, 8};

struct PathViewScale {
    float unit = 0.0f;          ///< pixels per scale unit; 0 = no scale (theme sizes apply)
    int32_t tube_gauge = 0;     ///< tube outer width, walls included
    int32_t sensor_r = 0;       ///< sensor dot radius
    int32_t hub_h = 0;          ///< hub box height
    int32_t cap_h = 0;          ///< buffer capsule height, never taller than the hub
    int32_t head_scale = 0;     ///< toolhead glyph scale
    int32_t bracket_h = 0;      ///< linked-lanes bracket strip height
    int32_t hub_min_w = 0;      ///< hub box width before the lane fan widens it
    int32_t spool_max = 0;      ///< the largest spool the unit art draws at
    bool buffer_in_hub = false; ///< the buffer's state rides in the hub box, no capsule row
};

/// The hub box's height in scale units.
inline constexpr float kPathHubUnits = 2.3f;

/// Column height the scale divides into units: a column this tall gets one unit per pixel.
inline constexpr float kPathViewColumnUnits = 44.0f;
/// The head is never narrower than this many tubes.
inline constexpr float kHeadMinTubes = 3.0f;

/// The scale for a view in a column @p column_h pixels tall, the tube gauge kept within
/// @p tube. @p head_width_at returns the head glyph's drawn width at a scale (the head's
/// floor is measured on the real glyph); null uses an estimate of 3.4 px per scale step.
/// @p label_h is the hub label's line height: the hub is never shorter than it and a border.
PathViewScale path_view_scale(int32_t column_h, TubeLimits tube,
                              int32_t (*head_width_at)(int32_t scale) = nullptr,
                              int32_t label_h = 0);

} // namespace helix::ui
