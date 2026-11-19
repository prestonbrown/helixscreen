// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sensor_enum_names.h"

#include <string>

namespace helix::sensors {

/// @brief Role assigned to a probe sensor
enum class ProbeSensorRole {
    NONE = 0,    ///< Discovered but not assigned to a role
    Z_PROBE = 1, ///< Used as Z probe for bed leveling
};

/// @brief Type of probe sensor hardware
enum class ProbeSensorType {
    STANDARD = 1,       ///< Standard probe (Klipper "probe" section)
    BLTOUCH = 2,        ///< BLTouch probe
    SMART_EFFECTOR = 3, ///< Duet Smart Effector
    EDDY_CURRENT = 4,   ///< Eddy current probe (e.g., probe_eddy_current btt)
    CARTOGRAPHER = 5,   ///< Cartographer 3D scanning/contact probe
    BEACON = 6,         ///< Beacon eddy current probe
    TAP = 7,            ///< Voron Tap nozzle-contact probe
    KLICKY = 8,         ///< Klicky magnetic probe (macro-based)
    PRTOUCH_V2 = 9,     ///< Creality prtouch_v2 pressure-based probe (K1/K1C/K1 Max)
    LOADCELL = 10,      ///< Load cell probe (e.g., FlashForge AD5M/AD5X)
};

/// @brief Configuration for a probe sensor
struct ProbeSensorConfig {
    std::string
        klipper_name; ///< Full Klipper name (e.g., "probe", "bltouch", "probe_eddy_current btt")
    std::string sensor_name; ///< Short display name (e.g., "probe", "bltouch", "btt")
    ProbeSensorType type = ProbeSensorType::STANDARD;
    ProbeSensorRole role = ProbeSensorRole::NONE;
    bool enabled = true;

    ProbeSensorConfig() = default;

    ProbeSensorConfig(std::string klipper_name_, std::string sensor_name_, ProbeSensorType type_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          type(type_) {}
};

/// @brief Runtime state for a probe sensor
struct ProbeSensorState {
    bool triggered = false;      ///< Last QUERY_PROBE result (status key last_query)
    float last_z_result = 0.0f;  ///< Last Z probe result in mm
    float z_offset = 0.0f;       ///< Z offset in mm
    bool z_offset_known = false; ///< z_offset came from status or a config section
    bool available = false;      ///< Sensor available in current config
};

inline constexpr EnumName<ProbeSensorRole> kProbeSensorRoles[] = {
    {ProbeSensorRole::NONE, "none", "Unassigned"},
    {ProbeSensorRole::Z_PROBE, "z_probe", "Z Probe"},
};

inline constexpr EnumName<ProbeSensorType> kProbeSensorTypes[] = {
    {ProbeSensorType::STANDARD, "standard", "Probe"},
    {ProbeSensorType::BLTOUCH, "bltouch", "BLTouch"},
    {ProbeSensorType::SMART_EFFECTOR, "smart_effector", "Smart Effector"},
    {ProbeSensorType::EDDY_CURRENT, "eddy_current", "Eddy Current"},
    {ProbeSensorType::CARTOGRAPHER, "cartographer", "Cartographer"},
    {ProbeSensorType::BEACON, "beacon", "Beacon"},
    {ProbeSensorType::TAP, "tap", "Voron Tap"},
    {ProbeSensorType::KLICKY, "klicky", "Klicky"},
    {ProbeSensorType::PRTOUCH_V2, "prtouch_v2", "Creality ProTouch"},
    {ProbeSensorType::LOADCELL, "loadcell", "Load Cell"},
};

[[nodiscard]] inline std::string probe_role_to_string(ProbeSensorRole role) {
    return enum_id(kProbeSensorRoles, role);
}

[[nodiscard]] inline ProbeSensorRole probe_role_from_string(const std::string& str) {
    return enum_from_id(kProbeSensorRoles, str);
}

[[nodiscard]] inline std::string probe_role_to_display_string(ProbeSensorRole role) {
    return enum_display(kProbeSensorRoles, role);
}

[[nodiscard]] inline std::string probe_type_to_string(ProbeSensorType type) {
    return enum_id(kProbeSensorTypes, type);
}

[[nodiscard]] inline std::string probe_type_to_display_string(ProbeSensorType type) {
    return enum_display(kProbeSensorTypes, type);
}

[[nodiscard]] inline ProbeSensorType probe_type_from_string(const std::string& str) {
    return enum_from_id(kProbeSensorTypes, str);
}

} // namespace helix::sensors
