// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_sources.h"

#include <cstdint>
#include <optional>
#include <string>

namespace helix::ams {

/// What a lane currently shows. Computed from LaneSources, never stored back.
///
/// Every field is optional for the same reason Observation's are: a field no
/// source observed and a field observed to be blank are different answers, and
/// a sentinel standing in for the first is indistinguishable from the second.
/// A reader laying this over values somebody else built must be able to tell
/// them apart, or "nobody said" silently overwrites "the machine says PLA".
/// An engaged field holding an empty string or a zero is a reading: somebody
/// looked and found nothing there.
struct ResolvedLane {
    std::optional<bool> present;

    /// A toolhead docked in this slot, where the backend can sense docking but
    /// not the filament inside the tool. Orthogonal to `present`: a lane may
    /// answer this one and stay silent on presence.
    std::optional<bool> tool_docked;

    std::optional<uint32_t> color_rgb;
    std::optional<std::string> color_name;
    std::optional<std::string> material;
    std::optional<std::string> brand;
    std::optional<std::string> spool_name;
    std::optional<std::string> catalog_id;
    std::optional<std::string> product_name;
    std::optional<int> spoolman_id;
    std::optional<int> spoolman_filament_id;
    std::optional<int> spoolman_vendor_id;
    std::optional<std::string> spoolman_orca_filament_id;

    std::optional<float> remaining_weight_g;
    std::optional<float> total_weight_g;
};

/// The spool a lane is linked to: the id the strongest of Pending, Firmware
/// and Record states, or 0 when none links one. Spoolman's own record never
/// decides the link; it is a lookup keyed by this id.
[[nodiscard]] int linked_spool_id(const LaneSources& sources);

/// Apply the precedence table to a lane's sources. Pure: same inputs, same
/// answer, no clock, no globals, no I/O.
///
/// Identity, strongest first: Pending, Firmware, Record, then Spoolman filling
/// what the others leave unset. Weights, strongest first: Pending, Spoolman,
/// Firmware, Record, Metered. A Spoolman record counts only while it describes
/// the linked spool.
[[nodiscard]] ResolvedLane resolve(const LaneSources& sources);

} // namespace helix::ams
