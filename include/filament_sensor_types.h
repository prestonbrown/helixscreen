// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <string>

namespace helix {

/// How long a runout-role sensor must stay clear -- while a job holds the machine
/// and an AMS backend is present -- before the removal toast fires.
///
/// A tool change drags filament off the toolhead sensor and feeds the next lane
/// past it, which is identical to a runout at the edge and tells them apart only
/// by duration: a swap leaves the sensor clear for 26-33s and then refills, a
/// runout never refills. Anything shorter than a swap reports every tool change
/// as a runout. Detection and the runout subjects still fire on the edge; only
/// the toast waits.
constexpr std::chrono::seconds RUNOUT_TOAST_DWELL{45};

/**
 * @brief Role that a filament sensor can be assigned to.
 *
 * Each role represents a specific position in the filament path:
 * - RUNOUT: Detects filament presence anywhere in the path (triggers pause on runout)
 * - TOOLHEAD: Near the hotend, verifies filament reached nozzle during load
 * - ENTRY: At filament entry point, detects when filament is first inserted
 */
enum class FilamentSensorRole {
    NONE = 0,     ///< Sensor discovered but not assigned to a role
    RUNOUT = 1,   ///< Primary runout detection sensor
    TOOLHEAD = 2, ///< Toolhead/nozzle proximity sensor
    ENTRY = 3,    ///< Entry point detection sensor
    Z_PROBE = 10  ///< Z probing sensor (maps to Klipper "probe" object)
};

/**
 * @brief Type of filament sensor hardware.
 *
 * Determines what data is available from the sensor:
 * - SWITCH: Simple binary state (filament detected yes/no)
 * - MOTION: Encoder-based, provides motion activity data for jam detection
 */
enum class FilamentSensorType {
    SWITCH, ///< filament_switch_sensor in Klipper
    MOTION  ///< filament_motion_sensor in Klipper (encoder-based)
};

/**
 * @brief User configuration for a single filament sensor.
 *
 * Stored in settings.json and loaded at startup.
 */
struct FilamentSensorConfig {
    std::string klipper_name; ///< Full Klipper object name, e.g. "filament_switch_sensor fsensor"
    std::string sensor_name;  ///< Short name extracted from klipper_name, e.g. "fsensor"
    FilamentSensorRole role;  ///< User-assigned role
    FilamentSensorType type;  ///< Type of sensor (switch or motion)
    bool enabled;             ///< Whether this sensor is actively monitored

    FilamentSensorConfig()
        : role(FilamentSensorRole::NONE), type(FilamentSensorType::SWITCH), enabled(true) {}

    FilamentSensorConfig(const std::string& klipper_name_, const std::string& sensor_name_,
                         FilamentSensorType type_)
        : klipper_name(klipper_name_), sensor_name(sensor_name_), role(FilamentSensorRole::NONE),
          type(type_), enabled(true) {}
};

/**
 * @brief Current runtime state of a filament sensor.
 *
 * Updated from Moonraker WebSocket notifications.
 */
struct FilamentSensorState {
    bool filament_detected; ///< Whether filament is currently detected
    bool enabled;           ///< Klipper-level enabled state (motion sensors)
    int detection_count;    ///< Motion sensors: cumulative detection events
    bool available;         ///< Whether the sensor exists in current Klipper config

    FilamentSensorState()
        : filament_detected(true), enabled(true), detection_count(0), available(false) {}
    // filament_detected defaults to TRUE (optimistic "present until proven empty"). The
    // false default meant that any read before Moonraker's first status update arrived
    // saw every sensor as a runout — a false positive that hit FilamentRunoutHandler
    // on print-status panel construction within ~5s of helix-screen startup. The
    // FilamentSensorManager startup-grace-period gate (formerly in has_any_runout)
    // was a band-aid for this default; now redundant and removed.
};

/**
 * @brief Convert FilamentSensorRole to display string.
 * @param role The role to convert
 * @return Human-readable role name for UI display
 */
inline const char* role_to_display_string(FilamentSensorRole role) {
    switch (role) {
    case FilamentSensorRole::RUNOUT:
        return "Runout Sensor";
    case FilamentSensorRole::TOOLHEAD:
        return "Toolhead Sensor";
    case FilamentSensorRole::ENTRY:
        return "Entry Sensor";
    case FilamentSensorRole::Z_PROBE:
        return "Z Probe";
    case FilamentSensorRole::NONE:
    default:
        return "Unassigned";
    }
}

/**
 * @brief Convert FilamentSensorRole to config string.
 * @param role The role to convert
 * @return Config-safe string for settings.json storage
 */
inline const char* role_to_config_string(FilamentSensorRole role) {
    switch (role) {
    case FilamentSensorRole::RUNOUT:
        return "runout";
    case FilamentSensorRole::TOOLHEAD:
        return "toolhead";
    case FilamentSensorRole::ENTRY:
        return "entry";
    case FilamentSensorRole::Z_PROBE:
        return "z_probe";
    case FilamentSensorRole::NONE:
    default:
        return "none";
    }
}

/**
 * @brief Parse FilamentSensorRole from config string.
 * @param str The config string to parse
 * @return Parsed role, or NONE if unrecognized
 */
inline FilamentSensorRole role_from_config_string(const std::string& str) {
    if (str == "runout")
        return FilamentSensorRole::RUNOUT;
    if (str == "toolhead")
        return FilamentSensorRole::TOOLHEAD;
    if (str == "entry")
        return FilamentSensorRole::ENTRY;
    if (str == "z_probe")
        return FilamentSensorRole::Z_PROBE;
    return FilamentSensorRole::NONE;
}

/**
 * @brief Convert FilamentSensorType to config string.
 * @param type The type to convert
 * @return Config-safe string
 */
inline const char* type_to_config_string(FilamentSensorType type) {
    switch (type) {
    case FilamentSensorType::MOTION:
        return "motion";
    case FilamentSensorType::SWITCH:
    default:
        return "switch";
    }
}

/**
 * @brief Parse FilamentSensorType from config string.
 * @param str The config string to parse
 * @return Parsed type, defaults to SWITCH if unrecognized
 */
inline FilamentSensorType type_from_config_string(const std::string& str) {
    if (str == "motion")
        return FilamentSensorType::MOTION;
    return FilamentSensorType::SWITCH;
}

} // namespace helix
