// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file temperature_rules.h
 * @brief Temperature conversions, compact formatting and heater commands
 *
 * The rules state, network and UI code share about a temperature without any
 * of them drawing it: decidegree conversions, the compact number format every
 * reading uses, the G-code that sets a heater, and the chamber's effective
 * setpoint. Colors and widgets stay in ui_temperature_utils.h.
 */

#include "printer_temperature_state.h"
#include "unit_conversions.h"

#include <cstddef>
#include <optional>
#include <string>

namespace helix::temperature_rules {

/**
 * @brief Converts decidegrees to degrees (integer)
 *
 * PrinterState stores temperatures as decidegrees (×10) for 0.1°C resolution.
 * Use this function for integer display (e.g., "210°C").
 *
 * @param deci Temperature in decidegrees (e.g., 2100 for 210°C)
 * @return Temperature in degrees (e.g., 210)
 */
inline int deci_to_degrees(int deci) {
    return static_cast<int>(helix::units::from_decidegrees(deci));
}

/**
 * @brief Converts decidegrees to degrees (float for precision display)
 *
 * Use this function when 0.1°C precision is needed (e.g., graph data points).
 *
 * @param deci Temperature in decidegrees (e.g., 2105 for 210.5°C)
 * @return Temperature in degrees (e.g., 210.5f)
 */
inline float deci_to_degrees_f(int deci) {
    return static_cast<float>(helix::units::from_decidegrees(deci));
}

/**
 * @brief Converts degrees to decidegrees
 *
 * Use when setting temperatures from user input (e.g., keyboard entry).
 *
 * @param degrees Temperature in degrees (e.g., 210)
 * @return Temperature in decidegrees (e.g., 2100)
 */
inline int degrees_to_deci(int degrees) {
    return helix::units::to_decidegrees(static_cast<double>(degrees));
}

/**
 * @brief Format a temperature as a number with the compact-wide rule
 *
 * Formats the bare number (no unit suffix) with one decimal place, dropping the
 * decimal when the integer portion is 3 digits or more (>= 100): "60.5", "211".
 * The rule applies to the value in the display unit.
 * This is the shared rule behind format_temperature_f / format_temperature_pair_f;
 * widgets that render their own unit/label structure (e.g. a separate "°C" label)
 * call this directly so the decimal-drop behaviour stays consistent everywhere.
 *
 * @param temp Temperature in degrees (float)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temp_number(float temp, char* buffer, size_t buffer_size);

/// format_temp_number()'s decimal rule on @p value as given, with no unit
/// conversion: for a reading the user labelled with a unit of their own.
char* format_number_compact(double value, char* buffer, size_t buffer_size);

/**
 * @brief Format a temperature value with one decimal place
 *
 * Formats as "210.5°C" for precision display (graphs, PID tuning). When the integer
 * portion is 3 digits or more (>= 100), the decimal is dropped (e.g. "211°C") to keep
 * wide values compact.
 *
 * @param temp Temperature in degrees (float)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_f(float temp, char* buffer, size_t buffer_size);

/**
 * @brief Build gcode to set a heater temperature (any type)
 *
 * Handles all Klipper heater types:
 * - "temperature_fan X" → SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=X
 * - "heater_generic X"  → SET_HEATER_TEMPERATURE HEATER=X
 * - "extruder"          → SET_HEATER_TEMPERATURE HEATER=extruder
 * - "heater_bed"        → SET_HEATER_TEMPERATURE HEATER=heater_bed
 *
 * @param heater_full_name Full Klipper heater name
 * @param target_deci     Target temperature in decidegrees (x10, e.g. 2100 = 210°C)
 * @param buffer           Output buffer
 * @param buffer_size      Size of buffer
 * @param use_m141         When true, emit "M141 S{deg}" instead of a raw
 *                         SET_HEATER_TEMPERATURE/SET_TEMPERATURE_FAN_TARGET command
 * @return Pointer to buffer, or nullptr if heater_full_name is empty
 */
const char* build_heater_gcode(const std::string& heater_full_name, int target_deci, char* buffer,
                               size_t buffer_size, bool use_m141 = false);

/**
 * @brief Decide whether a chamber temperature command should route through M141.
 *
 * True when a chamber temperature command should route through the standard
 * M141 macro instead of a raw SET_HEATER_TEMPERATURE: the target heater is the
 * discovered chamber heater, the printer defines an M141 macro, and that macro is
 * not known to leave the heater alone. Some macro sets use M141 for an exhaust fan
 * alone, and routing a heater set through one of those never heats.
 *
 * @param m141_sets_heater MacroParamCache::macro_sets_target() for M141 and the
 *                         chamber heater: nullopt when its body cannot be read
 *                         (firmware that hides macro bodies), which keeps M141
 */
bool chamber_uses_m141(const std::string& heater_full_name, const std::string& chamber_heater_name,
                       bool m141_available, std::optional<bool> m141_sets_heater);

/**
 * @brief Effective chamber setpoint and control mode from the two M141 targets.
 *
 * The K2's M141 macro splits the chamber setpoint across two Klipper objects:
 * a HEATING setpoint (>40°C) lands on the heater target, while a MAINTAINING
 * setpoint (≤40°C) lands on the cooling-fan target with the heater target at 0.
 * On printers without a chamber cooling fan the fan target stays 0, so this
 * reduces to the heater target (Heating/Off only) — safe and universal.
 *
 * `fan_resting_deci` is the printer-configured cooling-fan resting value (e.g.
 * 350 = 35°C on the K2).  M141 S0 resets the cooling fan to this resting target
 * while leaving the heater at 0; a fan target equal to resting therefore means
 * Off, not a deliberate Maintaining set.
 *
 * @param heater_target_deci Chamber heater target (×10; 0 = not heating)
 * @param fan_target_deci    Chamber cooling-fan target (×10; 0 = not maintaining)
 * @param fan_resting_deci   Configured cooling-fan resting target (×10); 0 if not set
 * @return deci = effective setpoint (×10), mode = ChamberMode enum value
 */
struct ChamberSetpoint {
    int deci;
    helix::ChamberMode mode;
};

ChamberSetpoint chamber_effective_setpoint(int heater_target_deci, int fan_target_deci,
                                           int fan_resting_deci = 0);

/**
 * @brief Build gcode to turn off a heater (target=0)
 *
 * Convenience wrapper for build_heater_gcode with target=0.
 */
inline const char* build_heater_off_gcode(const std::string& heater_full_name, char* buffer,
                                          size_t buffer_size) {
    return build_heater_gcode(heater_full_name, 0, buffer, buffer_size);
}

} // namespace helix::temperature_rules
