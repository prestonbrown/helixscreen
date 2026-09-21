// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::sensors {

/// @brief Role assigned to a load cell (auto-categorized during discovery)
enum class LoadCellRole {
    NONE = 0,         ///< Discovered but not assigned to a role
    SPOOL_WEIGHT = 1, ///< Spool weight
};

/// @brief Configuration for a load cell
struct LoadCellConfig {
    std::string klipper_name;               ///< Full Klipper name (e.g., "load_cell spool_weight")
    std::string sensor_name;                ///< Short name (e.g., "spool_weight")
    std::string display_name;               ///< Pretty name (e.g., "Spool Weight")
    LoadCellRole role = LoadCellRole::NONE; ///< Auto-assigned during discovery
    int priority = 100;                     ///< Lower = shown first

    LoadCellConfig() = default;

    LoadCellConfig(std::string klipper_name_, std::string sensor_name_, std::string display_name_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          display_name(std::move(display_name_)) {}
};

/// @brief Runtime state for a load cell
struct LoadCellState {
    float force_g = 0.0f;   ///< Force in grams
    bool available = false; ///< Sensor available in current config
};

/// @brief Convert role enum to config string
/// @param role The role to convert
/// @return Config-safe string for JSON storage
[[nodiscard]] inline std::string load_cell_role_to_string(LoadCellRole role) {
    switch (role) {
    case LoadCellRole::NONE:
        return "none";
    case LoadCellRole::SPOOL_WEIGHT:
        return "spool_weight";
    default:
        return "none";
    }
}

} // namespace helix::sensors
