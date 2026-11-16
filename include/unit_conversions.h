// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>

namespace helix::units {

// ============================================================================
// Temperature Conversions (decidegrees = degrees × 10)
// ============================================================================

/**
 * @brief Convert Celsius to decidegrees (for UI display with 0.1° precision)
 * @param celsius Temperature in degrees Celsius
 * @return Temperature in decidegrees (1 decidegree = 0.1°C)
 *
 * Example: 25.5°C → 255 decidegrees
 */
inline int to_decidegrees(double celsius) {
    if (!std::isfinite(celsius))
        return 0;
    return static_cast<int>(celsius * 10.0);
}

/**
 * @brief Convert decidegrees back to Celsius
 * @param decidegrees Temperature in decidegrees
 * @return Temperature in degrees Celsius
 */
inline double from_decidegrees(int decidegrees) {
    return static_cast<double>(decidegrees) / 10.0;
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
