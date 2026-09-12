// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_sources.h"

#include <cstdint>
#include <string>

namespace helix::ams {

/// What a lane currently shows. Computed from LaneSources, never stored back.
struct ResolvedLane {
    bool present = false;

    uint32_t color_rgb = AMS_DEFAULT_SLOT_COLOR;
    std::string color_name;
    std::string material;
    std::string brand;
    std::string spool_name;
    std::string catalog_id;
    std::string product_name;
    int spoolman_id = 0;
    int spoolman_vendor_id = 0;

    float remaining_weight_g = -1.0F;
    float total_weight_g = -1.0F;
};

/// Apply the precedence table to a lane's sources. Pure: same inputs, same
/// answer, no clock, no globals, no I/O.
[[nodiscard]] ResolvedLane resolve(const LaneSources& sources);

} // namespace helix::ams
