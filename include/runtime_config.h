// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file runtime_config.h
 * @brief Test mode flags and mock backend selection at runtime
 *
 * @pattern Runtime conditionals instead of compile-time #ifdefs
 * @threading Set once at startup; read-only thereafter
 *
 * @see should_mock_wifi(), should_mock_moonraker() for mock selection pattern
 */

#pragma once

#include <cstdio>      // For snprintf in get_default_test_file_path()
#include <sys/types.h> // For pid_t

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
    pid_t splash_pid = 0;     ///< External splash process PID (--splash-pid, 0 = none)

    bool use_real_wifi = false; ///< Use real WiFi backend (--real-wifi, requires --test)
    bool use_real_ethernet =
        false; ///< Use real Ethernet backend (--real-ethernet, requires --test)
    bool use_real_moonraker =
        false;                   ///< Use real Moonraker client (--real-moonraker, requires --test)
    bool use_real_files = false; ///< Use real file listing (--real-files, requires --test)
    bool use_real_ams = false;   ///< Use real AMS backend (--real-ams, requires --test)
    bool disable_mock_ams = false; ///< Don't create mock AMS (--no-ams, requires --test)
    bool use_real_sensors = false; ///< Use real sensor data (--real-sensors, requires --test)

    bool simulate_disconnect =
        false; ///< Simulate disconnected state for testing (--disconnected, requires --test)

    // Debug/testing options
    bool test_history_api = false; ///< Test print history API on startup (--test-history)

    // Print select panel options
    const char* select_file =
        nullptr; ///< File to auto-select in print select panel (--select-file)
    bool print_select_list_mode = false; ///< Start print-select in list view (--print-select-list)

    // Mock auto-print options (for panel testing without command-line args)
    bool mock_auto_start_print = false; ///< Auto-start a print in mock mode (set internally)
    bool mock_auto_history = false; ///< Auto-generate history data in mock mode (set internally)

    /// Test G-code directory (relative to project root or build dir)
    static constexpr const char* TEST_GCODE_DIR = "assets/test_gcodes";

    /// Default test file used when auto-starting prints or generating mock history
    static constexpr const char* DEFAULT_TEST_FILE = "3DBenchy.gcode";

    /// Production config file path
    static constexpr const char* PROD_CONFIG_PATH = "config/helixconfig.json";

    /// Test mode config file path (separate from production to avoid conflicts)
    static constexpr const char* TEST_CONFIG_PATH = "config/helixconfig-test.json";

    /**
     * @brief Get full path to default test G-code file
     * @return Path like "assets/test_gcodes/3DBenchy.gcode"
     */
    static const char* get_default_test_file_path() {
        // Static buffer to hold the constructed path
        static char path_buf[256] = {};
        if (path_buf[0] == '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", TEST_GCODE_DIR, DEFAULT_TEST_FILE);
        }
        return path_buf;
    }

    // G-code viewer options
    const char* gcode_test_file = nullptr;   ///< G-code file to load on startup (--gcode-file)
    bool gcode_camera_azimuth_set = false;   ///< Whether azimuth was set via command line
    float gcode_camera_azimuth = 0.0f;       ///< Camera azimuth angle in degrees (--gcode-az)
    bool gcode_camera_elevation_set = false; ///< Whether elevation was set via command line
    float gcode_camera_elevation = 0.0f;     ///< Camera elevation angle in degrees (--gcode-el)
    bool gcode_camera_zoom_set = false;      ///< Whether zoom was set via command line
    float gcode_camera_zoom = 1.0f;          ///< Camera zoom level (--gcode-zoom)
    bool gcode_debug_colors = false; ///< Enable per-face debug coloring (--gcode-debug-colors)
    int gcode_render_mode =
        -1; ///< G-code render mode override (-1=use settings, 0=auto, 1=3D, 2=2D)

    // Mock simulation options
    double sim_speedup = 1.0;    ///< Simulation speedup factor (--sim-speed, requires --test)
    int mock_ams_gate_count = 4; ///< Number of gates for mock AMS (HELIX_AMS_GATES env var)

    // Development/debugging options
    bool show_memory_overlay = false; ///< Show memory stats overlay (--show-memory, M key toggle)
    bool mock_crash =
        false; ///< Write synthetic crash.txt on startup (--mock-crash, requires --test)

    /**
     * @brief Check if verbose subject debugging is enabled
     *
     * When enabled, subject type mismatch warnings include stack traces.
     * Can be enabled via --debug-subjects flag or HELIX_DEBUG_SUBJECTS=1 env var.
     *
     * @return true if subject debugging is enabled
     */
    static bool debug_subjects();

    /**
     * @brief Enable or disable verbose subject debugging
     * @param value true to enable stack traces on subject type mismatches
     */
    static void set_debug_subjects(bool value);

    /**
     * @brief Check if debug touch visualization is enabled
     *
     * When enabled, a ripple effect is drawn at each touch point for debugging
     * touch accuracy and hit areas. Enabled via --debug-touches flag.
     *
     * @return true if debug touch visualization is enabled
     */
    static bool debug_touches();

    /**
     * @brief Enable or disable debug touch visualization
     * @param value true to enable touch point visualization
     */
    static void set_debug_touches(bool value);

    /**
     * @brief Check if XML hot reload is enabled
     *
     * When enabled, XML files are polled for changes and re-registered live.
     * Enabled via HELIX_HOT_RELOAD=1 environment variable. Dev-only feature.
     *
     * @return true if hot reload is enabled
     */
    static bool hot_reload_enabled();

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
     * @brief Check if AMS should use mock implementation
     * @return true if test mode is enabled, real AMS is not requested, and mock not disabled
     */
    bool should_mock_ams() const {
        return test_mode && !use_real_ams && !disable_mock_ams;
    }

    /**
     * @brief Check if USB should use mock implementation
     * @return true if test mode is enabled
     */
    bool should_mock_usb() const {
        return test_mode;
    }

    /**
     * @brief Check if mDNS discovery should be skipped
     * @return true if test mode is enabled (no mDNS responders in test environments)
     */
    bool should_mock_mdns() const {
        return test_mode;
    }

    /**
     * @brief Check if sensors should use mock data
     * @return true if test mode is enabled and real sensors are not requested
     */
    bool should_mock_sensors() const {
        return test_mode && !use_real_sensors;
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

    /**
     * @brief Check if filament runout modal should be shown
     *
     * Returns false if an AMS/MMU system is present (runout during swaps is normal).
     * Can be overridden with HELIX_FORCE_RUNOUT_MODAL=1 environment variable.
     *
     * @return true if runout modal should be displayed
     */
    bool should_show_runout_modal() const;
};

/**
 * @brief Get global runtime configuration
 * @return Pointer to the global runtime configuration
 */
RuntimeConfig* get_runtime_config();