// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file environment_config.h
 * @brief Type-safe environment variable parsing with validation
 *
 * Provides consistent, testable parsing of environment variables with range validation.
 * Replaces scattered std::getenv() + strtol() patterns throughout the codebase.
 */

#pragma once

#include <optional>
#include <string>

namespace helix::config {

/**
 * @brief Type-safe environment variable configuration reader
 *
 * All methods are static and thread-safe (no shared state).
 * Invalid/missing values return nullopt or false rather than throwing.
 */
class EnvironmentConfig {
  public:
    // Generic type-safe parsers

    /**
     * @brief Parse integer environment variable with range validation
     *
     * @param name Environment variable name
     * @param min Minimum valid value (inclusive)
     * @param max Maximum valid value (inclusive)
     * @return Parsed value if valid, nullopt otherwise
     *
     * Returns nullopt if:
     * - Variable doesn't exist
     * - Value is empty
     * - Value contains non-numeric characters
     * - Parsed value is outside [min, max] range
     */
    static std::optional<int> get_int(const char* name, int min, int max);

    /**
     * @brief Parse integer with divisor and range validation on scaled result
     *
     * Useful for converting milliseconds to seconds, etc.
     * Result is rounded up (ceiling) before range validation.
     *
     * @param name Environment variable name
     * @param min Minimum valid scaled result (inclusive)
     * @param max Maximum valid scaled result (inclusive)
     * @param divisor Value to divide by (must be > 0)
     * @return Scaled value if valid, nullopt otherwise
     */
    static std::optional<int> get_int_scaled(const char* name, int min, int max, int divisor);

    /**
     * @brief Check if environment variable exists (regardless of value)
     *
     * @param name Environment variable name
     * @return true if variable is set (even to empty string)
     */
    static bool exists(const char* name);

    /**
     * @brief Get string value of environment variable
     *
     * @param name Environment variable name
     * @return String value if exists, nullopt otherwise
     */
    static std::optional<std::string> get_string(const char* name);

    // Application-specific helpers (HELIX_* environment variables)

    /**
     * @brief Get auto-quit timeout from HELIX_AUTO_QUIT_MS
     *
     * Converts milliseconds to seconds (ceiling division).
     * Valid range: 100ms - 3600000ms (1 hour)
     *
     * @return Timeout in seconds, or nullopt if not set/invalid
     */
    static std::optional<int> get_auto_quit_seconds();

    /**
     * @brief Check if screenshot mode is enabled via HELIX_AUTO_SCREENSHOT
     *
     * @return true if HELIX_AUTO_SCREENSHOT=1
     */
    static bool get_screenshot_enabled();

    /**
     * @brief Get mock AMS gate count from HELIX_AMS_GATES
     *
     * Valid range: 1-16
     *
     * @return Gate count, or nullopt if not set/invalid
     */
    static std::optional<int> get_mock_ams_gates();

    /**
     * @brief Check if benchmark mode is enabled via HELIX_BENCHMARK
     *
     * @return true if HELIX_BENCHMARK exists (any value)
     */
    static bool get_benchmark_mode();

    /**
     * @brief Get screen resolution override from HELIX_SCREEN_SIZE
     *
     * Accepts named presets (micro, tiny, small, medium, large, xlarge)
     * or WxH format (e.g., "480x400", "1920x1080").
     *
     * Despite the variable's name this is a pixel resolution, not a UI scale
     * factor: the value picks the display mode, and the layout tier is
     * classified from the resulting display by breakpoint_for() in
     * ui_breakpoint.h. HELIX_DPI is the separate knob for interface scale.
     *
     * @return Size string, or nullopt if not set
     */
    static std::optional<std::string> get_screen_size();

    /**
     * @brief Get data directory override from HELIX_DATA_DIR
     *
     * When set, the application uses this directory as the root for
     * runtime assets (ui_xml/, assets/, config/) instead of auto-detecting
     * from the executable path. Required for standalone deployment where
     * the binary isn't at <project>/build/bin/.
     *
     * @return Directory path, or nullopt if not set
     */
    static std::optional<std::string> get_data_dir();
};

} // namespace helix::config
