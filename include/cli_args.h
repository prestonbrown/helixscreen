// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file cli_args.h
 * @brief Command-line argument parsing for HelixScreen
 *
 * Provides a clean interface for CLI parsing that replaces 27+ out-parameters
 * with a single structured result.
 */

#include "ui_nav_manager.h" // For ui_panel_id_t

#include <optional>
#include <string>

namespace helix {

/**
 * @brief Screen size presets (match responsive breakpoints)
 * MICRO=480x272, TINY=480x320, SMALL=480x400, MEDIUM=800x480, LARGE=1024x600, XLARGE=1280x720
 */
enum class ScreenSize { MICRO, TINY, SMALL, MEDIUM, LARGE, XLARGE };

/**
 * @brief Parsed command-line arguments
 *
 * Replaces 27+ function out-parameters with a clean struct.
 */
struct CliArgs {
    // Screen settings
    ScreenSize screen_size = ScreenSize::MEDIUM;
    bool size_was_explicit = false; ///< True if -s/--size was specified on the CLI
    int dpi = -1;                   // -1 = use default
    int display_num = -1;           // -1 = not set
    int x_pos = -1;                 // -1 = not set
    int y_pos = -1;                 // -1 = not set

    // Wizard
    bool force_wizard = false;
    int wizard_step = -1;     // -1 = not set
    bool skip_wizard = false; // --skip-wizard: suppress the first-run wizard
                              // (e.g. for screenshots/automation via helixctl)

    // Touch calibration
    bool calibrate_touch = false; ///< Force touch calibration on startup

    // Standalone dev tools (no helixctl equivalent)
    bool release_notes = false; ///< --release-notes: show update modal with fetched release notes

    // Automation
    bool screenshot_enabled = false;
    int screenshot_delay_sec = 2;
    int timeout_sec = 0;

    // Theme
    int dark_mode_cli = -1; // -1 = not set, 0 = light, 1 = dark

    // Logging
    int verbosity = 0;

    // Memory profiling (development feature)
    bool memory_report = false; // --memory-report: log memory every 30s
    bool show_memory = false;   // --show-memory: display memory overlay (M key toggle)

    // Display rotation (passed by watchdog, or CLI override)
    int rotation = 0; // 0, 90, 180, 270 degrees

    // Layout override
    std::string layout; // --layout: override auto-detected layout ("auto", "standard",
                        // "ultrawide", "portrait", "micro", "tiny", etc.)

    // Moonraker override (for testing/development)
    std::string moonraker_url; // --moonraker: override config URL (e.g., ws://192.168.1.100:7125)

    // Headless one-shot: detect printer via Moonraker REST and print JSON verdict, then exit.
    bool detect_printer = false;
    std::string detect_host = "127.0.0.1";
    int detect_port = 7125;

    // Remote control server
    bool remote_control = false;                // --remote: enable remote control server
    std::string remote_socket;                  // --remote-socket: override socket path
    std::string remote_transport = "socket";    // --remote-transport: socket|http
    std::string remote_http_bind = "127.0.0.1"; // --remote-http-bind: HTTP bind host
    int remote_http_port = 7130;                // --remote-http-port: HTTP TCP port
};

/**
 * @brief Parse command-line arguments
 *
 * @param argc Argument count
 * @param argv Argument values
 * @param args Output: parsed arguments
 * @param screen_width Output: screen width (modified based on -s flag)
 * @param screen_height Output: screen height (modified based on -s flag)
 * @return true on success, false if help was shown or error occurred
 *
 * @note Also modifies g_runtime_config for test mode flags
 */
bool parse_cli_args(int argc, char** argv, CliArgs& args, int& screen_width, int& screen_height);

/**
 * @brief Parse a screen size string (named preset or WxH format)
 *
 * Accepts named sizes: micro, tiny, small, medium, large, xlarge
 * Or custom WxH format: "480x400", "1920x1080"
 *
 * @param size_str The size string to parse
 * @param out_width Output: parsed width
 * @param out_height Output: parsed height
 * @param out_size Output: corresponding ScreenSize breakpoint
 * @return true on success, false if format is invalid
 */
bool parse_screen_size_string(const char* size_str, int& out_width, int& out_height,
                              ScreenSize& out_size);

/**
 * @brief Print test mode configuration banner
 *
 * Shows which backends are mocked vs real.
 */
void print_test_mode_banner();

} // namespace helix
