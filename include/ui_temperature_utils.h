// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "printer_temperature_state.h"
#include "unit_conversions.h"

#include <cmath>
#include <cstddef>
#include <string>

/**
 * @file ui_temperature_utils.h
 * @brief Shared temperature validation, formatting, and display utilities
 *
 * This module provides centralized temperature validation, clamping,
 * formatting, and color-coding logic used across multiple temperature-related
 * panels (controls/temp, filament, extrusion).
 *
 * ## Formatting Functions
 *
 * Use these for consistent temperature display across the UI:
 * - `format_temperature()` - Single temp: "210°C"
 * - `format_temperature_pair()` - Current/target: "210 / 245°C"
 *
 * ## Color-Coding Function
 *
 * Use `get_heating_state_color()` for consistent 4-state thermal feedback:
 * - Off (target=0): gray (text_muted)
 * - Heating (current < target-2): red (primary_color)
 * - At-temp (within ±2): green (success_color)
 * - Cooling (current > target+2): blue (info_color)
 */

namespace helix {
namespace ui {
namespace temperature {

// ============================================================================
// Unit Conversion Functions
// ============================================================================

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
 * @brief Snap a reading to the precision the UI actually renders it at.
 *
 * format_temp_number() prints one decimal below 100°C and whole degrees at or
 * above it, so at 219.6°C the card reads "220" while the raw value is still
 * 2196. Anything that classifies the reading — the heating color, the
 * Off/Heating/Ready/Cooling status word, the heater icon tint — must classify
 * THIS value, or the number and the word beside it can disagree: 222.9 against a
 * 220 target renders "223 / 220" yet truncates to 222, which is inside the
 * at-temp band and paints a green "Ready".
 *
 * The rounding matches printf's `%.0f` (half to even), so it is the same value
 * the label prints rather than an approximation of it. Below 100°C the decimal
 * is still on screen, so the reading is already exact and passes through.
 *
 * @param deci Temperature in decidegrees
 * @return The same temperature in decidegrees, snapped to displayed precision
 */
inline int displayed_deci(int deci) {
    // Mirrors format_temp_number()'s `temp >= 100.0f` switch; 1000 decidegrees
    // is exactly 100.0°C, so the two thresholds cannot drift apart.
    if (deci < 1000) {
        return deci;
    }
    return static_cast<int>(std::nearbyint(deci / 10.0)) * 10;
}

// ============================================================================
// Validation Functions
// ============================================================================

/**
 * @brief Validates and clamps a temperature value to safe limits
 *
 * If the temperature is outside the valid range, it will be clamped to
 * the nearest valid value and a warning will be logged.
 *
 * @param temp Temperature value to validate (modified in-place if clamped)
 * @param min_temp Minimum valid temperature
 * @param max_temp Maximum valid temperature
 * @param context Logging context (e.g., "Temp", "Filament", "Extrusion")
 * @param temp_type Temperature type for logging (e.g., "current", "target")
 * @return true if temperature was valid, false if it was clamped
 */
bool validate_and_clamp(int& temp, int min_temp, int max_temp, const char* context,
                        const char* temp_type);

/**
 * @brief Validates and clamps a temperature pair (current + target)
 *
 * Convenience function that validates both current and target temperatures.
 *
 * @param current Current temperature (modified in-place if clamped)
 * @param target Target temperature (modified in-place if clamped)
 * @param min_temp Minimum valid temperature
 * @param max_temp Maximum valid temperature
 * @param context Logging context (e.g., "Temp", "Filament")
 * @return true if both temperatures were valid, false if either was clamped
 */
bool validate_and_clamp_pair(int& current, int& target, int min_temp, int max_temp,
                             const char* context);

/**
 * @brief Checks if the current temperature is safe for extrusion
 *
 * Extrusion operations require the nozzle to be at or above a minimum
 * temperature (typically 170°C) to avoid damaging the extruder.
 *
 * @param current_temp Current nozzle temperature
 * @param min_extrusion_temp Minimum safe extrusion temperature
 * @return true if safe to extrude, false otherwise
 */
bool is_extrusion_safe(int current_temp, int min_extrusion_temp);

/**
 * @brief Gets a human-readable safety status message
 *
 * @param current_temp Current nozzle temperature
 * @param min_extrusion_temp Minimum safe extrusion temperature
 * @return Status message (e.g., "Ready" or "Heating (45°C below minimum)")
 */
const char* get_extrusion_safety_status(int current_temp, int min_extrusion_temp);

// ============================================================================
// Formatting Functions
// ============================================================================

/**
 * @brief Format a temperature value with degree symbol
 *
 * Formats as "210°C" for consistent display across the UI.
 *
 * @param temp Temperature in degrees (not decidegrees)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 *
 * @code{.cpp}
 * char buf[16];
 * lv_label_set_text(label, format_temperature(210, buf, sizeof(buf)));
 * @endcode
 */
char* format_temperature(int temp, char* buffer, size_t buffer_size);

/**
 * @brief Format a current/target temperature pair
 *
 * Formats as "210 / 245°C" or "210 / —°C" when target is 0 (heater off).
 *
 * @param current Current temperature in degrees
 * @param target Target temperature in degrees (0 = heater off, shows "—")
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 24)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_pair(int current, int target, char* buffer, size_t buffer_size);

/**
 * @brief Format a target temperature or "— °C" when off
 *
 * Formats as "245°C" when target > 0, or "— °C" when target is 0 (heater off).
 *
 * @param target Target temperature in degrees (0 = heater off)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_target_or_off(int target, char* buffer, size_t buffer_size);

/**
 * @brief Format a temperature as a number with the compact-wide rule
 *
 * Formats the bare number (no unit suffix) with one decimal place, dropping the
 * decimal when the integer portion is 3 digits or more (>= 100): "60.5", "211".
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
 * @brief Format a float current/target temperature pair
 *
 * Formats as "210.5 / 215.0°C" or "180.5 / —°C" when target is 0. Each value drops its
 * decimal when the integer portion is 3 digits or more (>= 100).
 *
 * @param current Current temperature in degrees (float)
 * @param target Target temperature in degrees (float, 0 = heater off)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 32)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_pair_f(float current, float target, char* buffer, size_t buffer_size);

/**
 * @brief Format a temperature range for material specs
 *
 * Formats as "200-230°C" for AMS material temperature ranges.
 *
 * @param min_temp Minimum temperature in degrees
 * @param max_temp Maximum temperature in degrees
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_range(int min_temp, int max_temp, char* buffer, size_t buffer_size);

// ============================================================================
// Display Color Functions
// ============================================================================

/** Default tolerance for "at temperature" state detection (±degrees) */
constexpr int DEFAULT_AT_TEMP_TOLERANCE = 2;

/** The same tolerance for callers that classify in decidegrees. */
constexpr int DEFAULT_AT_TEMP_TOLERANCE_DECI = DEFAULT_AT_TEMP_TOLERANCE * 10;

/**
 * @brief Thermal state of a heater, shared by every consumer of the 4-state logic.
 *
 * One classifier feeds three renderers: the temp-label color
 * (get_heating_state_color), the icon variant string (get_heating_state_variant),
 * and HeatingIconAnimator's icon tint + pulse. They must never disagree, so they
 * all switch on this rather than re-deriving the thresholds.
 */
enum class HeatState {
    Off,     ///< target <= 0 — heater disabled (text_muted / gray)
    Heating, ///< current < target - tolerance (danger / red)
    AtTemp,  ///< within +/- tolerance of target (success / green)
    Cooling, ///< current > target + tolerance (info / blue)
    Neutral  ///< chamber Maintaining, at/below the cooling ceiling (text / gray-ish neutral,
             ///< NOT the same token as Off — see classify_heat_state_with_mode())
};

/**
 * @brief Classify a heater's thermal state.
 *
 * Unit-agnostic: current, target and tolerance need only share a unit. Degree
 * callers use the default tolerance of 2; decidegree callers pass 20.
 *
 * @param current Current temperature
 * @param target Target temperature (<= 0 = heater off)
 * @param tolerance Tolerance for the at-temp band (default: 2)
 */
HeatState classify_heat_state(int current, int target, int tolerance = DEFAULT_AT_TEMP_TOLERANCE);

/**
 * @brief Classify a chamber heater's thermal state, mode-aware.
 *
 * Delegates to classify_heat_state() for Off/Heating — there, target is a genuine
 * heat goal and the plain 4-state logic already applies. In Maintaining mode the
 * target is a cooling CEILING instead: there is nothing to heat toward, so
 * Heating never applies. Above the ceiling + tolerance is Cooling (actively
 * shedding heat); at or below the ceiling is Neutral (idle/ok, not Off — the
 * heater/fan combo is still under active control, just not doing anything right
 * now).
 *
 * Both the chamber temp-label color (ui_temp_display.cpp) and the chamber icon
 * (HeatingIconAnimator, via HeaterIconBinder) call this SAME function, so they
 * can never disagree about what Maintaining means.
 *
 * @param current Current temperature
 * @param target Effective target: heat goal when Heating, cooling ceiling when Maintaining
 * @param mode Chamber control mode (Off / Heating / Maintaining)
 * @param tolerance Tolerance for the at-temp/neutral band (default: 2)
 */
HeatState classify_heat_state_with_mode(int current, int target, helix::ChamberMode mode,
                                        int tolerance = DEFAULT_AT_TEMP_TOLERANCE);

/**
 * @brief Get the theme color for an already-classified thermal state.
 *
 * The int-taking overload below is classify_heat_state() + this in one call;
 * exposed separately so a caller that classified via
 * classify_heat_state_with_mode() (chamber) can still share the same color
 * mapping instead of re-deriving it.
 *
 * @param state Already-classified thermal state
 * @return lv_color_t Theme color for that state
 */
lv_color_t get_heating_state_color(HeatState state);

/**
 * @brief Get theme color for temperature display based on 4-state heating logic
 *
 * Returns a color indicating the thermal state of a heater:
 * - **Off** (target<=0): text_muted (gray) - heater disabled
 * - **Heating** (current < target - tolerance): primary_color (red) - actively heating
 * - **At-temp** (within ±tolerance): success_color (green) - stable at target
 * - **Cooling** (current > target + tolerance): info_color (blue) - cooling down
 *
 * This function provides consistent color-coding across all temperature displays
 * (temp_display widget, filament panel, etc.). It classifies via the plain
 * (mode-unaware) classify_heat_state() — chamber callers in Maintaining mode
 * must go through classify_heat_state_with_mode() + get_heating_state_color(HeatState)
 * instead, since Maintaining can never be expressed as a single (current,target) pair.
 *
 * @param current_deg Current temperature in degrees
 * @param target_deg Target temperature in degrees (0 = heater off)
 * @param tolerance Degrees tolerance for "at temp" state (default: 2)
 * @return lv_color_t Theme color based on thermal state
 *
 * @code{.cpp}
 * lv_color_t color = get_heating_state_color(nozzle_current, nozzle_target);
 * lv_obj_set_style_text_color(temp_label, color, LV_PART_MAIN);
 * @endcode
 */
lv_color_t get_heating_state_color(int current_deg, int target_deg,
                                   int tolerance = DEFAULT_AT_TEMP_TOLERANCE);

/**
 * @brief Get the icon color-variant string for a heater's thermal state
 *
 * Mirrors get_heating_state_color()'s 4-state logic, but returns the
 * ui_icon_set_variant() string instead of a resolved color. Keeps icon tint
 * and temp-label color in lockstep (same thresholds, same inputs):
 * - target <= 0:                    "muted"   (off / gray)
 * - current < target - tolerance:   "danger"  (heating / red)
 * - current > target + tolerance:   "info"    (cooling / blue)
 * - within +/- tolerance:           "success" (at-temp / green)
 *
 * @param current_deg Current temperature in degrees
 * @param target_deg Target temperature in degrees (<= 0 = heater off)
 * @param tolerance Degrees tolerance for "at temp" state (default: 2)
 * @return Variant string suitable for ui_icon_set_variant()
 */
const char* get_heating_state_variant(int current_deg, int target_deg,
                                      int tolerance = DEFAULT_AT_TEMP_TOLERANCE);

// ============================================================================
// Heater Display
// ============================================================================

/**
 * @brief Result of formatting a heater display
 *
 * Contains all the information needed to display a heater status:
 * - temp: formatted temperature string (e.g., "150°C" or "150 / 200°C")
 * - status: semantic status ("Off", "Heating...", "Ready", or "Cooling")
 * - pct: percentage towards target (0-100, clamped)
 * - color: theme color matching the heating state (from get_heating_state_color)
 */
struct HeaterDisplayResult {
    std::string temp;
    std::string status;
    int pct;
    lv_color_t color;
};

/**
 * @brief Format heater display information from decidegree values
 *
 * Takes current and target temperatures in decidegrees (10 = 1°C) and
 * produces a consistent display result used across all heater displays.
 * Includes a color field from get_heating_state_color() for one-call convenience.
 *
 * Status logic (DEFAULT_AT_TEMP_TOLERANCE, matches get_heating_state_color):
 * - target <= 0: "Off"
 * - current < target - tolerance: "Heating..."
 * - current > target + tolerance: "Cooling"
 * - within +/- tolerance: "Ready"
 *
 * @param current_deci Current temperature in decidegrees
 * @param target_deci Target temperature in decidegrees (0 = off)
 * @return HeaterDisplayResult with formatted temp, status, percentage, and color
 */
HeaterDisplayResult heater_display(int current_deci, int target_deci);

// ============================================================================
// Heater GCode
// ============================================================================

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
 * discovered chamber heater AND the printer defines an M141 macro.
 */
bool chamber_uses_m141(const std::string& heater_full_name, const std::string& chamber_heater_name,
                       bool m141_available);

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
 * @brief Map a ChamberMode enum value to its untranslated status word.
 *
 * Returns "Heating", "Maintaining", or "Off".  Callers that display the string
 * must localise it at the call site via lv_tr().  Single source of truth shared
 * by the temperature-service display path and the chamber_status_text composer.
 */
const char* chamber_mode_word(helix::ChamberMode mode);

/**
 * @brief Build gcode to turn off a heater (target=0)
 *
 * Convenience wrapper for build_heater_gcode with target=0.
 */
inline const char* build_heater_off_gcode(const std::string& heater_full_name, char* buffer,
                                          size_t buffer_size) {
    return build_heater_gcode(heater_full_name, 0, buffer, buffer_size);
}

/**
 * @brief Compose the chamber status string from mode + thermal progress.
 *
 * Single source of truth used by both the controls panel and the temp-graph
 * overlay so they can never diverge.  Leads with the M141 control mode word
 * (Off / Maintaining / Heating), then appends thermal progress ("Ready" /
 * "Cooling") only when it adds information beyond the mode word — suppresses
 * "Heating · Heating..." and "Off · Off".
 *
 * @param current_deci  Current chamber temperature in decidegrees
 * @param target_deci   Effective chamber target in decidegrees (heater target
 *                       when Heating, fan ceiling when Maintaining, 0 when Off)
 * @param mode           ChamberMode enum value (Off / Heating / Maintaining)
 * @return Localised status string, e.g. "Maintaining", "Heating", "Maintaining · Cooling"
 */
std::string chamber_status_text(int current_deci, int target_deci, helix::ChamberMode mode);

} // namespace temperature
} // namespace ui
} // namespace helix
