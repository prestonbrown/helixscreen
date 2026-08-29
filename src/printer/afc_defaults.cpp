// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_defaults.h"

namespace helix::printer {

std::vector<DeviceSection> afc_default_sections() {
    return {
        {"setup", "Setup", 0, "Calibration, LED, and system configuration"},
        {"speed", "Speed Settings", 1, "Move speed multipliers"},
        {"toolhead", "Toolhead", 2, "Extruder distances and sensor configuration"},
        {"maintenance", "Maintenance", 3, "Lane tests and motor resets"},
        {"hub", "Hub & Cutter", 4, "Blade change and parking"},
        {"tip_forming", "Tip Forming", 5, "Tip shaping configuration"},
        {"purge", "Purge & Wipe", 6, "Purge tower and brush settings"},
    };
}

std::vector<DeviceAction> afc_default_actions() {
    std::vector<DeviceAction> actions;

    // Setup section (calibration + LED)
    actions.push_back({
        .id = "calibration_wizard",
        .label = "Run Calibration Wizard",
        .icon = "play",
        .section = "setup",
        .description = "Interactive calibration for all lanes",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "bowden_length",
        .label = "Bowden Length",
        .icon = "ruler",
        .section = "setup",
        .description = "Distance from hub to toolhead",
        .type = ActionType::SLIDER,
        .current_value = std::any(450.0f),
        .options = {},
        .min_value = 100,
        .max_value = 2000,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Speed section
    actions.push_back({
        .id = "speed_fwd",
        .label = "Forward Multiplier",
        .icon = "fast-forward",
        .section = "speed",
        .description = "Speed multiplier for forward moves",
        .type = ActionType::SLIDER,
        .current_value = std::any(1.0f),
        .options = {},
        .min_value = 0.5f,
        .max_value = 2.0f,
        .unit = "x",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "speed_rev",
        .label = "Reverse Multiplier",
        .icon = "rewind",
        .section = "speed",
        .description = "Speed multiplier for reverse moves",
        .type = ActionType::SLIDER,
        .current_value = std::any(1.0f),
        .options = {},
        .min_value = 0.5f,
        .max_value = 2.0f,
        .unit = "x",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Toolhead section (extruder distances)
    actions.push_back({
        .id = "tool_stn",
        .label = "Sensor to Nozzle",
        .icon = "ruler",
        .section = "toolhead",
        .description = "Distance from toolhead sensor to nozzle tip",
        .type = ActionType::SLIDER,
        .current_value = std::any(72.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 200.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "tool_stn_unload",
        .label = "Unload Distance",
        .icon = "ruler",
        .section = "toolhead",
        .description = "Retraction distance to clear extruder gears",
        .type = ActionType::SLIDER,
        .current_value = std::any(100.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 200.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "tool_sensor_after_extruder",
        .label = "Post-Sensor Clear",
        .icon = "ruler",
        .section = "toolhead",
        .description = "Extra distance to move once sensors clear",
        .type = ActionType::SLIDER,
        .current_value = std::any(0.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 100.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Maintenance section
    actions.push_back({
        .id = "test_lanes",
        .label = "Test All Lanes",
        .icon = "test-tube",
        .section = "maintenance",
        .description = "Run test sequence on all lanes",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "change_blade",
        .label = "Change Blade",
        .icon = "box-cutter",
        .section = "maintenance",
        .description = "Initiate blade change procedure",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "park",
        .label = "Park",
        .icon = "parking",
        .section = "maintenance",
        .description = "Park the AFC system",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "brush",
        .label = "Clean Brush",
        .icon = "broom",
        .section = "maintenance",
        .description = "Run brush cleaning sequence",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "reset_motor",
        .label = "Reset Motor Timer",
        .icon = "timer-refresh",
        .section = "maintenance",
        .description = "Reset motor run-time counter",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Setup section (LED & Modes)
    actions.push_back({
        .id = "led_toggle",
        .label = "Turn On LEDs",
        .icon = "lightbulb-on",
        .section = "setup",
        .description = "Toggle AFC LED strip",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "led_extruder",
        .label = "Toolhead LED",
        .icon = "lightbulb-on",
        .section = "setup",
        .description = "Toggle toolhead LED for an extruder",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "quiet_mode",
        .label = "Toggle Quiet Mode",
        .icon = "volume-off",
        .section = "setup",
        .description = "Enable/disable quiet operation mode",
        .type = ActionType::BUTTON,
        .current_value = {},
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Hub & Cutter section
    actions.push_back({
        .id = "hub_cut_enabled",
        .label = "Cutter Enabled",
        .icon = "content-cut",
        .section = "hub",
        .description = "Enable or disable the hub cutter",
        .type = ActionType::TOGGLE,
        .current_value = std::any(false),
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "hub_cut_dist",
        .label = "Cut Distance",
        .icon = "ruler",
        .section = "hub",
        .description = "Distance for hub cutter operation",
        .type = ActionType::SLIDER,
        .current_value = std::any(50.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 100.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "hub_bowden_length",
        .label = "Hub Bowden Length",
        .icon = "ruler",
        .section = "hub",
        .description = "Bowden tube length from hub to toolhead",
        .type = ActionType::SLIDER,
        .current_value = std::any(450.0f),
        .options = {},
        .min_value = 100.0f,
        .max_value = 2000.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "assisted_retract",
        .label = "Assisted Retract",
        .icon = "arrow-u-left-top",
        .section = "hub",
        .description = "Enable assisted retraction at hub",
        .type = ActionType::TOGGLE,
        .current_value = std::any(false),
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Tip Forming section
    actions.push_back({
        .id = "ramming_volume",
        .label = "Ramming Volume",
        .icon = "hydraulic-oil-level",
        .section = "tip_forming",
        .description = "Volume of filament used during ramming",
        .type = ActionType::SLIDER,
        .current_value = std::any(0.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 100.0f,
        .unit = "mm³",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "unloading_speed_start",
        .label = "Unloading Start Speed",
        .icon = "speedometer",
        .section = "tip_forming",
        .description = "Initial speed for filament unloading",
        .type = ActionType::SLIDER,
        .current_value = std::any(80.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 200.0f,
        .unit = "mm/s",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "cooling_tube_length",
        .label = "Cooling Tube Length",
        .icon = "thermometer-minus",
        .section = "tip_forming",
        .description = "Length of the cooling tube section",
        .type = ActionType::SLIDER,
        .current_value = std::any(15.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 100.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "cooling_tube_retraction",
        .label = "Cooling Tube Retraction",
        .icon = "thermometer-minus",
        .section = "tip_forming",
        .description = "Retraction distance in the cooling tube",
        .type = ActionType::SLIDER,
        .current_value = std::any(0.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 100.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    // Purge & Wipe section
    actions.push_back({
        .id = "purge_enabled",
        .label = "Enable Purge",
        .icon = "water",
        .section = "purge",
        .description = "Enable purging during tool changes",
        .type = ActionType::TOGGLE,
        .current_value = std::any(false),
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "purge_length",
        .label = "Purge Length",
        .icon = "ruler",
        .section = "purge",
        .description = "Length of filament to purge",
        .type = ActionType::SLIDER,
        .current_value = std::any(50.0f),
        .options = {},
        .min_value = 0.0f,
        .max_value = 200.0f,
        .unit = "mm",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    actions.push_back({
        .id = "brush_enabled",
        .label = "Enable Brush Wipe",
        .icon = "broom",
        .section = "purge",
        .description = "Enable brush wipe after purging",
        .type = ActionType::TOGGLE,
        .current_value = std::any(false),
        .options = {},
        .min_value = 0,
        .max_value = 0,
        .unit = "",
        .slot_index = -1,
        .enabled = true,
        .disable_reason = "",
    });

    return actions;
}

AfcCapabilities afc_default_capabilities() {
    return {
        .supports_endless_spool = true,
        .supports_bypass = true,
        .supports_purge = true,
        .tip_method = TipMethod::CUT,
    };
}

} // namespace helix::printer
