// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file heater_type.h
 * @brief Shared heater configuration structure and utilities
 *
 * This module provides a unified configuration structure for heaters (nozzle, bed)
 * and helper functions to eliminate duplicate setup code across temperature panels.
 */

#pragma once

#include "lvgl/lvgl.h"
#include "preset_materials.h"

#include <array>

/**
 * @brief Heater type enumeration
 */
namespace helix {
enum class HeaterType { Nozzle = 0, Bed = 1, Chamber = 2 };
constexpr int kHeaterTypeCount = 3;

/**
 * @brief Preset target temperatures (°C) for a single heater.
 *
 * `material[i]` is the target for quick-preset slot i, parallel to
 * helix::presets::name(i). Slots are indexed, never material-named: the old
 * `{int pla; int petg; int abs;}` layout hardcoded the assumption that slot 0
 * is PLA, which stopped being true as soon as a user reassigned a slot.
 */
struct HeaterPresets {
    int off = 0;
    std::array<int, presets::kPresetCount> material{};
};
} // namespace helix

/**
 * @brief Heater configuration structure
 *
 * This structure encapsulates all configuration needed for a heater panel,
 * including display colors, temperature ranges, presets, and keypad ranges.
 */
typedef struct {
    helix::HeaterType type; ///< Heater type (nozzle or bed)
    const char* name;       ///< Short name (e.g., "nozzle", "bed")
    lv_color_t color;       ///< Theme color for this heater
    float temp_range_max;   ///< Maximum temperature for graph Y-axis
    int y_axis_increment;   ///< Y-axis label increment (e.g., 50°C, 100°C)

    helix::HeaterPresets presets; ///< Off + one target per user preset slot

    struct {
        float min; ///< Minimum keypad input value
        float max; ///< Maximum keypad input value
    } keypad_range;
} heater_config_t;
