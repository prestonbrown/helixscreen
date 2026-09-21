// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix {

/**
 * @brief Device categories for type-aware display name formatting.
 *
 * The device type affects how names are transformed:
 * - Adds type-specific suffixes (e.g., "LED", "Fan", "Sensor")
 * - Removes redundant prefixes (e.g., "heater_bed" for HEATER becomes "Bed Heater")
 * - Applies special handling for known device patterns
 */
enum class DeviceType {
    FAN,             ///< fan, heater_fan, controller_fan, fan_generic
    HEATER,          ///< extruder, heater_bed, heater_generic
    TEMP_SENSOR,     ///< temperature_sensor, temperature_fan
    LED,             ///< neopixel, led, dotstar
    LOAD_CELL,       ///< load_cell
    FILAMENT_SENSOR, ///< filament_switch_sensor, filament_motion_sensor
    POWER_DEVICE,    ///< PSU, relay, GPIO devices
    MACRO,           ///< G-code macros
    GENERIC          ///< Fallback - no type suffix
};

/**
 * @brief Convert a technical device name to a human-readable display name.
 *
 * Applies type-aware transformation:
 * 1. Check direct mapping table (exact matches for special cases)
 * 2. Apply type-specific prefix stripping and suffix addition
 * 3. Apply snake_case to Title Case with special word handling
 *
 * @param technical_name Full Klipper object name (e.g., "heater_fan hotend_fan")
 * @param type Device category for context-aware formatting
 * @return Human-readable display name (e.g., "Hotend Fan")
 *
 * @example
 *   get_display_name("fan", DeviceType::FAN) -> "Part Cooling Fan"
 *   get_display_name("chamber", DeviceType::TEMP_SENSOR) -> "Chamber Temperature"
 *   get_display_name("chamber", DeviceType::LED) -> "Chamber LED"
 *   get_display_name("heater_bed", DeviceType::HEATER) -> "Bed Heater"
 *   get_display_name("filament_switch_sensor runout", DeviceType::FILAMENT_SENSOR) -> "Runout
 * Sensor"
 */
std::string get_display_name(const std::string& technical_name, DeviceType type);

/**
 * @brief Extract the device name suffix from a prefixed object name.
 *
 * Strips the type prefix (before space) from Klipper object names:
 *   "heater_fan hotend_fan" -> "hotend_fan"
 *   "neopixel chamber_led" -> "chamber_led"
 *   "fan" -> "fan"
 *
 * @param object_name Full Klipper object name
 * @return Suffix after the space, or full name if no space
 */
std::string extract_device_suffix(const std::string& object_name);

/**
 * @brief Convert snake_case to Title Case with special word handling.
 *
 * Handles special abbreviations:
 *   "led_strip" -> "LED Strip"
 *   "psu_control" -> "PSU Control"
 *   "usb_hub" -> "USB Hub"
 *
 * @param snake_case_name Input name with underscores
 * @return Title Case output with special words uppercased
 */
std::string prettify_name(const std::string& snake_case_name);

} // namespace helix
