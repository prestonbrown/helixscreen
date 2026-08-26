// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <ctime>
#include <string>

namespace helix::format {

// =============================================================================
// Constants
// =============================================================================

/**
 * @brief Unavailable/unknown value placeholder (em dash)
 *
 * Use this constant for consistent display of unavailable data across the UI.
 * Example: sensor disconnected, value not yet received, etc.
 */
inline constexpr const char* UNAVAILABLE = "—";

// =============================================================================
// Percentage Formatting
// =============================================================================

/**
 * @brief Format integer percentage with % suffix
 *
 * @param percent Percentage value (0-100 typically, but any int supported)
 * @param buf Output buffer
 * @param size Buffer size (recommended: 8)
 * @return Pointer to buffer for chaining
 */
char* format_percent(int percent, char* buf, size_t size);

/**
 * @brief Format fan speed as "Off" (when 0) or "XX%" (when > 0)
 *
 * Returns a pointer suitable for lv_label_set_text(). When speed is 0,
 * returns the static string "Off" (pass through lv_tr() for translation).
 * When speed > 0, formats "XX%" into the provided buffer.
 *
 * Typical usage:
 *   char buf[8];
 *   lv_label_set_text(label, lv_tr(format_fan_speed(speed, buf, sizeof(buf))));
 *
 * @param speed_pct Fan speed percentage (0-100)
 * @param buf Output buffer (used only when speed > 0)
 * @param size Buffer size (recommended: 8)
 * @return Pointer to "Off" literal or formatted buffer
 */
const char* format_fan_speed(int speed_pct, char* buf, size_t size);

/**
 * @brief Format percentage or show UNAVAILABLE when data is missing
 *
 * @param percent Percentage value
 * @param available Whether the data is available
 * @param buf Output buffer
 * @param size Buffer size
 * @return Pointer to buffer for chaining
 */
char* format_percent_or_unavailable(int percent, bool available, char* buf, size_t size);

/**
 * @brief Format float percentage with configurable decimal places
 *
 * @param percent Percentage value (float)
 * @param decimals Decimal places (0-2)
 * @param buf Output buffer
 * @param size Buffer size (recommended: 12)
 * @return Pointer to buffer for chaining
 */
char* format_percent_float(double percent, int decimals, char* buf, size_t size);

/**
 * @brief Format humidity from x10 value (e.g., 455 → "45%")
 *
 * Sensors often report humidity as value×10 for 0.1% resolution.
 * This function divides by 10 and displays as whole percent.
 *
 * @param humidity_x10 Humidity × 10 (e.g., 455 for 45.5%)
 * @param buf Output buffer
 * @param size Buffer size (recommended: 8)
 * @return Pointer to buffer for chaining
 */
char* format_humidity(int humidity_x10, char* buf, size_t size);

// =============================================================================
// Distance/Length Formatting
// =============================================================================

/**
 * @brief Format distance in millimeters with configurable precision
 *
 * @param mm Distance in millimeters
 * @param precision Decimal places (0-3)
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_distance_mm(double mm, int precision, char* buf, size_t size);

/**
 * @brief Format filament diameter (always 2 decimal places)
 *
 * Convenience function for common 1.75mm/2.85mm filament diameters.
 *
 * @param mm Diameter in millimeters
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_diameter_mm(float mm, char* buf, size_t size);

// =============================================================================
// Speed Formatting
// =============================================================================

/**
 * @brief Format speed in mm/s (whole number)
 *
 * @param speed Speed in mm/s
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_speed_mm_s(double speed, char* buf, size_t size);

/**
 * @brief Format speed in mm/min (whole number)
 *
 * @param speed Speed in mm/min
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_speed_mm_min(double speed, char* buf, size_t size);

/**
 * @brief Format a speed in mm/s with a chosen number of decimals.
 *
 * format_speed_mm_s() rounds to whole mm/s, which is right for velocities but
 * hides the half steps square corner velocity is tuned in -- 5.5 would render
 * as "6 mm/s". Mirrors format_percent_float() next to format_percent().
 *
 * @param speed Speed in mm/s
 * @param decimals Digits after the decimal point
 * @param buf Output buffer
 * @param size Buffer size
 * @return buf
 */
char* format_speed_mm_s_float(double speed, int decimals, char* buf, size_t size);

// =============================================================================
// Acceleration Formatting
// =============================================================================

/**
 * @brief Format acceleration in mm/s²
 *
 * @param accel Acceleration in mm/s²
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_accel_mm_s2(double accel, char* buf, size_t size);

// =============================================================================
// Frequency Formatting
// =============================================================================

/**
 * @brief Format frequency in Hz (1 decimal place)
 *
 * @param hz Frequency in Hz
 * @param buf Output buffer
 * @param size Buffer size (recommended: 16)
 * @return Pointer to buffer for chaining
 */
char* format_frequency_hz(double hz, char* buf, size_t size);

// =============================================================================
// Duration Formatting
// =============================================================================

/**
 * @brief Format duration in seconds to human-readable string
 *
 * Produces output like:
 * - "30s" for durations under 1 minute
 * - "45m" for durations under 1 hour (no seconds shown)
 * - "2h" for exact hours
 * - "2h 15m" for hours with minutes
 *
 * @param total_seconds Duration in seconds (negative values treated as 0)
 * @return Formatted string
 */
std::string duration(int total_seconds);

/**
 * @brief Format duration with "remaining" suffix for countdowns
 *
 * Produces output like:
 * - "45 min left" for durations under 1 hour
 * - "1:30 left" for durations 1 hour or more (HH:MM format)
 *
 * @param total_seconds Duration in seconds (negative values treated as 0)
 * @return Formatted string with " left" suffix
 */
std::string duration_remaining(int total_seconds);

/**
 * @brief Format print time estimate from minutes
 *
 * Produces output like:
 * - "45 min" for durations under 1 hour
 * - "2h" for exact hours
 * - "2h 15m" for hours with minutes
 *
 * @param total_minutes Duration in minutes (negative values treated as 0)
 * @return Formatted string
 */
std::string duration_from_minutes(int total_minutes);

/**
 * @brief Format duration to a fixed-size buffer (for legacy code)
 *
 * Same output format as duration() but writes to a provided buffer.
 * Useful for code that needs to avoid allocations or use C-style buffers.
 *
 * @param buf Output buffer
 * @param buf_size Size of output buffer (recommended minimum: 16)
 * @param total_seconds Duration in seconds
 * @return Number of characters written (excluding null terminator), or 0 on error
 */
size_t duration_to_buffer(char* buf, size_t buf_size, int total_seconds);

/**
 * @brief Format duration with zero-padded minutes (for progress displays)
 *
 * Produces output like:
 * - "45m" for durations under 1 hour
 * - "2h 05m" for durations 1 hour or more (minutes always 2 digits)
 *
 * @param total_seconds Duration in seconds (negative values treated as 0)
 * @return Formatted string
 */
std::string duration_padded(int total_seconds);

/**
 * @brief Format filament length in mm to human-readable string
 * @param mm Filament length in millimeters
 * @return Formatted string (e.g., "850mm", "12.5m", "1.23km")
 */
std::string format_filament_length(double mm);

// =============================================================================
// Clock Time Formatting
// =============================================================================

/**
 * @brief Format remaining seconds as an estimated finish clock time string
 *
 * Computes the wall-clock finish time by adding remaining_seconds to the
 * current local time, then formats it according to the user's time format
 * setting: "(~2:45 PM)" in 12-hour mode, "(~14:45)" in 24-hour mode.
 *
 * Returns an empty string if remaining_seconds <= 0.
 *
 * @param remaining_seconds Seconds until completion (must be > 0 to produce output)
 * @param now Current time (default: system clock). Pass explicit value for deterministic tests.
 * @param use_24h If true, format as 24-hour clock (e.g., "14:45"). Default: false (12-hour).
 * @return Formatted string like "(~2:45 PM)" or "(~14:45)", or "" if remaining_seconds <= 0
 */
std::string eta_clock_time(int remaining_seconds, std::time_t now = 0, bool use_24h = false);

/**
 * @brief Round ETA seconds for stable display
 *
 * Prevents jittery countdown by rounding to buckets:
 * - Over 2 minutes: round to nearest 30s
 * - 2 minutes or under: round to nearest 10s
 * - Zero or negative: return 0
 *
 * @param seconds Raw remaining seconds
 * @return Rounded seconds for display
 */
int round_eta_seconds(int seconds);

} // namespace helix::format
