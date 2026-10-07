// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sensor_enum_names.h"

#include <string>

namespace helix::sensors {

/// @brief Role assigned to a temperature sensor (auto-categorized during discovery)
enum class TemperatureSensorRole {
    NONE = 0,           ///< Discovered but not assigned to a role
    CHAMBER = 1,        ///< Chamber temperature monitoring
    MCU = 2,            ///< MCU/board temperature
    HOST = 3,           ///< Host computer (Raspberry Pi, etc.)
    AUXILIARY = 4,      ///< Any other temperature sensor
    STEPPER_DRIVER = 5, ///< TMC stepper driver built-in temperature
};

/// @brief Type of temperature sensor in Klipper
enum class TemperatureSensorType {
    TEMPERATURE_SENSOR = 1, ///< temperature_sensor (read-only)
    TEMPERATURE_FAN = 2,    ///< temperature_fan (has target and speed)
    HEATER_GENERIC = 3,     ///< heater_generic (has target and power)
};

/// @brief Configuration for a temperature sensor
struct TemperatureSensorConfig {
    std::string klipper_name; ///< Full Klipper name (e.g., "temperature_sensor mcu_temp")
    std::string sensor_name;  ///< Short name (e.g., "mcu_temp")
    std::string display_name; ///< Pretty name (e.g., "MCU Temperature")
    TemperatureSensorType type = TemperatureSensorType::TEMPERATURE_SENSOR;
    TemperatureSensorRole role = TemperatureSensorRole::NONE; ///< Auto-assigned during discovery
    bool enabled = true;
    int priority = 100; ///< Lower = shown first

    TemperatureSensorConfig() = default;

    TemperatureSensorConfig(std::string klipper_name_, std::string sensor_name_,
                            std::string display_name_, TemperatureSensorType type_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          display_name(std::move(display_name_)), type(type_) {}
};

/// @brief Runtime state for a temperature sensor
struct TemperatureSensorState {
    float temperature = 0.0f; ///< Temperature in degrees C
    float target = 0.0f;      ///< Target temp (temperature_fan / heater_generic)
    float speed = 0.0f;       ///< Fan speed 0-1 (temperature_fan only)
    bool available = false;   ///< Sensor available in current config
};

inline constexpr EnumName<TemperatureSensorRole> kTemperatureSensorRoles[] = {
    {TemperatureSensorRole::NONE, "none", "Unassigned"},
    {TemperatureSensorRole::CHAMBER, "chamber", "Chamber"},
    {TemperatureSensorRole::MCU, "mcu", "MCU"},
    {TemperatureSensorRole::HOST, "host", "Host"},
    {TemperatureSensorRole::AUXILIARY, "auxiliary", "Auxiliary"},
    {TemperatureSensorRole::STEPPER_DRIVER, "stepper_driver", "Stepper Driver"},
};

inline constexpr EnumName<TemperatureSensorType> kTemperatureSensorTypes[] = {
    {TemperatureSensorType::TEMPERATURE_SENSOR, "temperature_sensor", "Temperature Sensor"},
    {TemperatureSensorType::TEMPERATURE_FAN, "temperature_fan", "Temperature Fan"},
    {TemperatureSensorType::HEATER_GENERIC, "heater_generic", "Generic Heater"},
};

/// Whether a Klipper temperature object carries a settable target.
[[nodiscard]] inline bool klipper_object_has_target(const std::string& klipper_name) {
    return klipper_name.rfind("heater_generic ", 0) == 0 ||
           klipper_name.rfind("temperature_fan ", 0) == 0;
}

[[nodiscard]] inline std::string temp_role_to_string(TemperatureSensorRole role) {
    return enum_id(kTemperatureSensorRoles, role);
}

[[nodiscard]] inline TemperatureSensorRole temp_role_from_string(const std::string& str) {
    return enum_from_id(kTemperatureSensorRoles, str);
}

[[nodiscard]] inline std::string temp_role_to_display_string(TemperatureSensorRole role) {
    return enum_display(kTemperatureSensorRoles, role);
}

[[nodiscard]] inline std::string temp_type_to_string(TemperatureSensorType type) {
    return enum_id(kTemperatureSensorTypes, type);
}

[[nodiscard]] inline TemperatureSensorType temp_type_from_string(const std::string& str) {
    return enum_from_id(kTemperatureSensorTypes, str);
}

} // namespace helix::sensors
