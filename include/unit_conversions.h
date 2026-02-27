// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <json.hpp> // nlohmann/json from libhv

namespace helix::units {

// ============================================================================
// Temperature Conversions (centidegrees = degrees × 10)
// ============================================================================

/**
 * @brief Convert Celsius to centidegrees (for UI display with 0.1° precision)
 * @param celsius Temperature in degrees Celsius
 * @return Temperature in centidegrees (1 centidegree = 0.1°C)
 *
 * Example: 25.5°C → 255 centidegrees
 */
inline int to_centidegrees(double celsius) {
    if (!std::isfinite(celsius))
        return 0;
    return static_cast<int>(celsius * 10.0);
}

/**
 * @brief Convert centidegrees back to Celsius
 * @param centidegrees Temperature in centidegrees
 * @return Temperature in degrees Celsius
 */
inline double from_centidegrees(int centidegrees) {
    return static_cast<double>(centidegrees) / 10.0;
}

/**
 * @brief Extract temperature from JSON and convert to centidegrees
 * @param obj JSON object containing the temperature field
 * @param key Key name for the temperature value
 * @param default_value Value to return if key is missing or not a number
 * @return Temperature in centidegrees
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_centidegrees(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_centidegrees(obj[key].get<double>());
    }
    return default_value;
}

// ============================================================================
// Percent Conversions (0.0-1.0 ratio → 0-100 integer)
// ============================================================================

/**
 * @brief Convert ratio (0.0-1.0) to percent integer (0-100)
 * @param ratio Value between 0.0 and 1.0
 * @return Percent as integer (0-100)
 *
 * Example: 0.75 → 75
 */
inline int to_percent(double ratio) {
    if (!std::isfinite(ratio))
        return 0;
    return static_cast<int>(std::round(ratio * 100.0));
}

/**
 * @brief Convert percent integer back to ratio
 * @param percent Percent value (0-100)
 * @return Ratio (0.0-1.0)
 */
inline double from_percent(int percent) {
    return static_cast<double>(percent) / 100.0;
}

/**
 * @brief Extract ratio from JSON and convert to percent
 * @param obj JSON object containing the ratio field
 * @param key Key name for the ratio value
 * @param default_value Value to return if key is missing or not a number
 * @return Percent as integer
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_percent(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_percent(obj[key].get<double>());
    }
    return default_value;
}

// ============================================================================
// Length Conversions (centimillimeters = mm × 100)
// ============================================================================

/**
 * @brief Convert millimeters to centimillimeters (for 0.01mm precision)
 * @param mm Length in millimeters
 * @return Length in centimillimeters
 *
 * Example: 1.25mm → 125 centimillimeters
 */
inline int to_centimm(double mm) {
    if (!std::isfinite(mm))
        return 0;
    return static_cast<int>(mm * 100.0);
}

/**
 * @brief Convert centimillimeters back to millimeters
 * @param centimm Length in centimillimeters
 * @return Length in millimeters
 */
inline double from_centimm(int centimm) {
    return static_cast<double>(centimm) / 100.0;
}

/**
 * @brief Extract length from JSON and convert to centimillimeters
 * @param obj JSON object containing the length field
 * @param key Key name for the length value (in mm)
 * @param default_value Value to return if key is missing or not a number
 * @return Length in centimillimeters
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_centimm(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_centimm(obj[key].get<double>());
    }
    return default_value;
}

// ============================================================================
// Speed/Rate Conversions
// ============================================================================

/**
 * @brief Convert speed factor (e.g., 0.5 for 50% speed) to percent
 * @param factor Speed factor (1.0 = 100%)
 * @return Speed as percent integer
 *
 * This is an alias for to_percent, provided for semantic clarity.
 */
inline int speed_factor_to_percent(double factor) {
    return to_percent(factor);
}

/**
 * @brief Convert mm/s to mm/min
 * @param mm_per_sec Speed in mm/s
 * @return Speed in mm/min
 */
inline int mm_per_sec_to_mm_per_min(double mm_per_sec) {
    if (!std::isfinite(mm_per_sec))
        return 0;
    return static_cast<int>(mm_per_sec * 60.0);
}

} // namespace helix::units
