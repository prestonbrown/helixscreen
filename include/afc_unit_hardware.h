// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

/**
 * @file afc_unit_hardware.h
 * @brief The hardware readings an AFC unit object carries beyond its topology.
 *
 * AFC DEV runs an Anycubic ACE Pro (`AFC_ACE <name>`, `AFC_ACE2 <name>`) and a Creality
 * CFS (`AFC_CFS <name>`) as units of their own. An ACE's unit object adds the hub's dryer
 * and climate to the usual lanes / extruders / hubs / buffers arrays, which
 * AmsBackendAfc::parse_afc_unit_object() reads.
 *
 * Pure functions over JSON: no LVGL, no backend state, no locking.
 */

namespace helix::afc {

/// The live hardware state an ACE Pro unit run through AFC publishes on its
/// `AFC_ACE <name>` / `AFC_ACE2 <name>` object (AFC DEV, AFC_ACE.py get_status).
/// A null value states nothing, the same as an absent key.
struct AceUnitDelta {
    /// `ace_dryer`: the dryer's status word ("drying", "heater_err", or one of
    /// AFC's idle words such as "stop"). Its key is what says the unit has a
    /// dryer AFC drives.
    std::optional<std::string> dryer_status;
    std::optional<float> dryer_target_c;    ///< `ace_dryer_target`
    std::optional<int> dryer_remaining_min; ///< `ace_dryer_remain` (seconds on the wire)
    std::optional<int> dryer_duration_min;  ///< `ace_dryer_duration`
    std::optional<float> temperature_c;     ///< `ace_temp`, the box's ambient reading
    std::optional<float> humidity_pct;      ///< `ace_humidity` (ACE 2 only)
};

/// Whether an ACE dryer status word means a cycle is running. The idle words
/// are AFC_ACE.py's `_DRYER_IDLE`.
[[nodiscard]] bool ace_dryer_running(const std::string& status);

/// What one unit object says about its hardware.
struct UnitHardwareDelta {
    /// Set when the object carries any `ace_` hardware field.
    std::optional<AceUnitDelta> ace;
};

[[nodiscard]] UnitHardwareDelta parse_unit_hardware(const nlohmann::json& data);

/// Every field a unit object is read for: its topology arrays plus what
/// parse_unit_hardware() reads. Moonraker delivers only subscribed fields
/// after the first snapshot.
[[nodiscard]] std::vector<std::string> unit_object_fields();

} // namespace helix::afc
