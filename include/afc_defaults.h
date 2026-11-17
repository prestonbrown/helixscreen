// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <cstddef>
#include <vector>

namespace helix::printer {

/**
 * @brief Shared AFC capability flags
 *
 * Provides a single source of truth for AFC system capabilities,
 * used by both AmsBackendAfc and AmsBackendMock.
 */
struct AfcCapabilities {
    bool supports_endless_spool = true;
    bool supports_bypass = true;
    bool supports_purge = true;
    TipMethod tip_method = TipMethod::CUT;
};

/**
 * @brief Get the canonical AFC section definitions
 *
 * Returns the 7 standard AFC sections in display order.
 * Both AmsBackendAfc and AmsBackendMock use these as their
 * section definitions rather than duplicating them.
 *
 * @return Vector of DeviceSection structs ordered by display_order
 */
std::vector<DeviceSection> afc_default_sections();

/**
 * @brief Get the static AFC action definitions
 *
 * Returns actions whose metadata does not depend on runtime config.
 * Dynamic actions (per-lane calibration, config-dependent settings)
 * are added by the backends themselves.
 *
 * @return Vector of DeviceAction structs with default values
 */
std::vector<DeviceAction> afc_default_actions();

/**
 * @brief Add the guided toolhead calibration button (AFC_TOOLHEAD_CALIBRATION)
 *
 * It leads the toolhead section, whose distances it calibrates. Only for an AFC
 * that registers the command; older AFC answers it with "Unknown command".
 */
void afc_add_toolhead_calibration_action(std::vector<DeviceAction>& actions);

/**
 * @brief Get the default AFC capability flags
 * @return AfcCapabilities with standard AFC values
 */
AfcCapabilities afc_default_capabilities();

/**
 * @brief What an AFC unit reports about where its lanes go
 *
 * The lane flags come from each lane's `hub` field: "direct" / "direct_load"
 * is a direct lane, any other value names the hub it runs through. A lane whose
 * routing has not arrived yet sets neither. The counts are the unit object's
 * own `lanes`, `extruders` and `hubs` lists.
 */
struct AfcUnitRouting {
    bool has_direct_lane = false;
    bool has_hub_routed_lane = false;
    size_t lane_count = 0;
    size_t extruder_count = 0;
    size_t hub_count = 0;
    /// Extruders on the whole printer (AFC `system.num_extruders`); 0 while unknown.
    size_t printer_extruder_count = 0;
};

/**
 * @brief The path topology an AFC unit's routing describes
 *
 * Per-lane routing decides first; the extruder and hub counts decide only
 * while no lane routing is known. A unit whose known lanes are all direct is
 * PARALLEL when every lane can have an extruder of its own: several extruders,
 * or one per lane on a printer with more than one extruder, as on a toolchanger's
 * one-lane "Tools" unit. On a single-extruder printer a direct lane shares the
 * nozzle with every other unit, so it stays HUB and a load unloads first.
 */
constexpr PathTopology afc_unit_topology(const AfcUnitRouting& r) {
    if (r.has_direct_lane && r.has_hub_routed_lane) {
        return PathTopology::MIXED;
    }
    if (r.has_hub_routed_lane && r.extruder_count > 1 && r.extruder_count == r.lane_count) {
        // Each lane has its own hub AND its own extruder: independent paths
        // with hub sensors on them (an ACE Pro as lane loader through AFC:
        // 4 lanes, 4 hubs, 4 extruders, 1 unit).
        return PathTopology::PARALLEL;
    }
    if (r.has_hub_routed_lane) {
        return PathTopology::HUB;
    }
    if (r.has_direct_lane && (r.extruder_count > 1 ||
                              (r.extruder_count == r.lane_count && r.printer_extruder_count > 1))) {
        return PathTopology::PARALLEL;
    }
    if (r.hub_count > 0 && r.extruder_count <= 1) {
        return PathTopology::HUB;
    }
    if (r.extruder_count > 1) {
        return PathTopology::PARALLEL;
    }
    return PathTopology::HUB;
}

} // namespace helix::printer
