// include/cfs_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

#include "hv/json.hpp"

/**
 * @file cfs_status_parse.h
 * @brief The small status objects around a CFS `box` frame, read into plain
 *        structs.
 *
 * Moonraker sends deltas, so every parsed field is a std::optional and an
 * omitted or null one reads as nullopt. Pure: no locks, no AmsState.
 */
namespace helix::cfs {

/// What kind of `box` frame this is. Full frames carry `filament`/`map`/
/// `auto_refill`/`state` or a per-unit `T1`..`T4`; anything else (a bare
/// measuring_wheel tick) changes no state.
struct BoxFrameShape {
    bool has_top_level = false; ///< `filament`, `map`, `auto_refill` or `state`
    bool has_unit_data = false; ///< `T1`..`T4`
};

struct FilamentSensorDelta {
    /// Null until the sensor takes its first reading: no reading, not "no
    /// filament".
    std::optional<bool> filament_detected;
};

struct ExtruderTempDelta {
    std::optional<double> target_c;
    std::optional<double> temperature_c;
};

struct MotorControlDelta {
    std::optional<bool> motor_ready;
};

[[nodiscard]] BoxFrameShape classify_box_frame(const nlohmann::json& box);

/// Each returns nullopt when the frame carries no such object, and a delta with
/// every field it did not state left unset when it does.
[[nodiscard]] std::optional<FilamentSensorDelta>
parse_filament_sensor(const nlohmann::json& params);
[[nodiscard]] std::optional<ExtruderTempDelta> parse_extruder(const nlohmann::json& params);
[[nodiscard]] std::optional<MotorControlDelta> parse_motor_control(const nlohmann::json& params);

/// Whether printer.cfg's saved RS-485 address table names a unit. The K2 keeps
/// it in the SAVE_CONFIG block as `[auto_addr] mb_addr_table_uniids`, one entry
/// per address (`[0]` for an empty one, the unit's uuid otherwise), and it is
/// readable from configfile.settings before the bus discovery that confirms
/// it. Accepts the table parsed or as its config string; false when the
/// section or option is absent or unreadable.
[[nodiscard]] bool has_registered_unit(const nlohmann::json& configfile_settings);

} // namespace helix::cfs
