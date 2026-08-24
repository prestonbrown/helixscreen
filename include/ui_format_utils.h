// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace helix::ui {

/**
 * @brief Format print time from minutes to human-readable string
 *
 * Converts duration into compact time format.
 * Examples: "5m", "1h30m", "8h"
 *
 * @param minutes Total print time in minutes
 * @return Formatted time string
 */
std::string format_print_time(int minutes);

/**
 * @brief Format filament weight from grams to human-readable string
 *
 * Converts weight to compact format with appropriate precision.
 * Examples: "2.5 g", "45 g", "120 g"
 *
 * @param grams Filament weight in grams
 * @return Formatted weight string
 */
std::string format_filament_weight(float grams);

/**
 * @brief Format layer count with "layers" suffix
 *
 * Converts layer count to readable format.
 * Examples: "234 layers", "1 layer", "--" (if zero/unknown)
 *
 * @param layer_count Total layer count
 * @return Formatted layer string
 */
std::string format_layer_count(uint32_t layer_count);

/**
 * @brief Format live layer progress for the print-status displays
 *
 * The single renderer behind every "Layer N / M" string. Omits the total when
 * it is unknown, marks counts that were guessed from the progress fraction with
 * a leading "~", and appends the commanded Z height when one is available.
 * Examples: "Layer 42 / 213", "Layer ~42 / 213 (24.0mm)", "Layer 7"
 *
 * @param current Current layer number
 * @param total Total layers, or 0/negative when unknown
 * @param accurate False when the layer was estimated from the progress fraction
 * @param z_centimm Commanded Z in centimillimeters, or 0/negative to omit
 * @return Formatted layer string
 */
std::string format_layer_progress(int current, int total, bool accurate, int z_centimm);

/**
 * @brief Format print height in millimeters
 *
 * Formats object height with appropriate precision.
 * Examples: "42.5 mm", "0.2 mm", "--" (if zero/unknown)
 *
 * @param height_mm Object height in millimeters
 * @return Formatted height string
 */
std::string format_print_height(double height_mm);

/**
 * @brief Format file size from bytes to human-readable string
 *
 * Converts bytes to appropriate unit (KB/MB/GB) with decimal precision.
 * Examples: "1.2 KB", "45 MB", "1.5 GB"
 *
 * @param bytes File size in bytes
 * @return Formatted size string
 */
std::string format_file_size(size_t bytes);

/**
 * @brief Format Unix timestamp to date/time string
 *
 * Converts timestamp to localized date/time format.
 * Respects user's 12/24 hour time format setting.
 * Examples: "Jan 15 2:30 PM" (12H) or "Jan 15 14:30" (24H)
 *
 * @param timestamp Unix timestamp (time_t)
 * @return Formatted date/time string
 */
std::string format_modified_date(time_t timestamp);

/**
 * @brief Format timestamp as short date (no time)
 *
 * Converts timestamp to locale-aware short date.
 * Examples: "Mar 09" (EN), "09. Mär." (DE), "3/9" (CJK)
 *
 * @param timestamp Unix timestamp (time_t)
 * @return Formatted short date string
 */
std::string format_short_date(time_t timestamp);

/**
 * @brief Format time portion only (no date)
 *
 * Returns time in user's preferred format.
 * Examples: "2:30 PM" (12H) or "14:30" (24H)
 *
 * @param tm_info Pointer to tm struct with time to format
 * @return Formatted time string
 */
std::string format_time(const struct tm* tm_info);

/**
 * @brief Format time including seconds, honoring the 12H/24H setting
 *
 * Same shape as format_time() with a :SS field appended. The temperature
 * graph caption needs this because samples are 3s apart, so minute
 * resolution cannot distinguish adjacent points.
 * Examples: "2:30:06 PM" (12H) or "14:30:06" (24H)
 *
 * @param tm_info Pointer to tm struct with time to format
 * @return Formatted time string, or helix::format::UNAVAILABLE if tm_info is null
 */
std::string format_time_with_seconds(const struct tm* tm_info);

// get_time_format_string() is intentionally NOT public. Use format_time()
// which handles POSIX-safe formatting and leading-zero stripping.

/**
 * @brief Format a relative time from milliseconds elapsed
 *
 * Translatable via lv_tr(). Handles singular/plural forms.
 * Examples: "Just now", "5 min ago", "1 hour ago", "3 days ago"
 *
 * @param elapsed_ms Milliseconds since the event
 * @return Formatted relative time string
 */
std::string format_relative_time(uint64_t elapsed_ms);

} // namespace helix::ui
