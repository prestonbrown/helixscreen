// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

/**
 * @brief Runtime configuration for development and testing
 *
 * Controls which components use mock implementations vs real hardware.
 * In production mode (test_mode=false), NO mocks are ever used.
 * In test mode, mocks are used by default but can be overridden with --real-* flags.
 */
struct RuntimeConfig {
    bool test_mode = false; ///< Master test mode flag (--test)

    bool skip_splash = false; ///< Skip splash screen (--skip-splash, independent of test mode)

    bool use_real_wifi = false; ///< Use real WiFi backend (--real-wifi, requires --test)
    bool use_real_ethernet =
        false; ///< Use real Ethernet backend (--real-ethernet, requires --test)
    bool use_real_moonraker =
        false;                   ///< Use real Moonraker client (--real-moonraker, requires --test)
    bool use_real_files = false; ///< Use real file listing (--real-files, requires --test)

    // Print select panel options
    const char* select_file = nullptr; ///< File to auto-select in print select panel (--select-file)

    // G-code viewer options
    const char* gcode_test_file = nullptr;   ///< G-code file to load on startup (--gcode-file)
    bool gcode_camera_azimuth_set = false;   ///< Whether azimuth was set via command line
    float gcode_camera_azimuth = 0.0f;       ///< Camera azimuth angle in degrees (--gcode-az)
    bool gcode_camera_elevation_set = false; ///< Whether elevation was set via command line
    float gcode_camera_elevation = 0.0f;     ///< Camera elevation angle in degrees (--gcode-el)
    bool gcode_camera_zoom_set = false;      ///< Whether zoom was set via command line
    float gcode_camera_zoom = 1.0f;          ///< Camera zoom level (--gcode-zoom)
    bool gcode_debug_colors = false; ///< Enable per-face debug coloring (--gcode-debug-colors)

    /**
     * @brief Check if WiFi should use mock implementation
     * @return true if test mode is enabled and real WiFi is not requested
     */
    bool should_mock_wifi() const {
        return test_mode && !use_real_wifi;
    }

    /**
     * @brief Check if Ethernet should use mock implementation
     * @return true if test mode is enabled and real Ethernet is not requested
     */
    bool should_mock_ethernet() const {
        return test_mode && !use_real_ethernet;
    }

    /**
     * @brief Check if Moonraker should use mock implementation
     * @return true if test mode is enabled and real Moonraker is not requested
     */
    bool should_mock_moonraker() const {
        return test_mode && !use_real_moonraker;
    }

    /**
     * @brief Check if file list should use test data
     * @return true if test mode is enabled and real files are not requested
     */
    bool should_use_test_files() const {
        return test_mode && !use_real_files;
    }

    /**
     * @brief Check if USB should use mock implementation
     * @return true if test mode is enabled
     */
    bool should_mock_usb() const {
        return test_mode;
    }

    /**
     * @brief Check if we're in any form of test mode
     * @return true if test mode is enabled
     */
    bool is_test_mode() const {
        return test_mode;
    }

    /**
     * @brief Check if splash screen should be skipped based on command-line flags
     * @return true if --skip-splash flag set or test mode enabled
     *
     * Note: Callers should also check SettingsManager::get_skip_splash_once() for theme
     *       change restart flow. That flag is cleared after startup.
     */
    bool should_skip_splash() const {
        return skip_splash || test_mode;
    }
};

/**
 * @brief Get global runtime configuration
 * @return Reference to the global runtime configuration
 */
const RuntimeConfig& get_runtime_config();

/**
 * @brief Get mutable runtime configuration (for initialization only)
 * @return Pointer to the global runtime configuration
 */
RuntimeConfig* get_mutable_runtime_config();

#endif // RUNTIME_CONFIG_H