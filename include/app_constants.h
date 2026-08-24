// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file app_constants.h
 * @brief Centralized application constants and configuration values
 *
 * This file contains application-wide constants, safety limits, and configuration
 * values shared between frontend (UI) and backend (business logic) code.
 * Centralizing these values ensures consistency and makes the codebase easier
 * to maintain.
 *
 * These constants are usable by both UI components and backend services.
 */

#pragma once

#include "lvgl.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

/**
 * @brief Application-wide constants shared between UI and backend
 */
namespace AppConstants {
/**
 * @brief Temperature-related constants
 *
 * Safety limits and default values for temperature control.
 * Used by both UI panels and backend temperature management.
 */
namespace Temperature {
/// Minimum safe temperature for extrusion operations (Klipper default)
constexpr int MIN_EXTRUSION_TEMP = 170;

/// Default maximum temperature for nozzle/hotend
constexpr int DEFAULT_NOZZLE_MAX = 500;

/// Default maximum temperature for heated bed
constexpr int DEFAULT_BED_MAX = 150;

/// Default minimum temperature (ambient)
constexpr int DEFAULT_MIN_TEMP = 0;
} // namespace Temperature

/**
 * @brief Responsive layout breakpoints
 *
 * These define the screen height thresholds for different UI layouts.
 * Use these consistently across all panels for uniform responsive behavior.
 */
namespace Responsive {
/// Tiny screens: <= 479px height
constexpr lv_coord_t BREAKPOINT_TINY_MAX = 479;

/// Small screens: 480-599px height
constexpr lv_coord_t BREAKPOINT_SMALL_MAX = 599;

/// Medium screens: 600-1023px height
constexpr lv_coord_t BREAKPOINT_MEDIUM_MAX = 1023;

/// Large screens: >= 1024px height
// (No max defined - anything above MEDIUM is large)
} // namespace Responsive

/**
 * @brief AMS/Filament loading constants
 */
namespace Ams {
/// Default preheat temperature when no material-specific temp is known (°C)
constexpr int DEFAULT_LOAD_PREHEAT_TEMP = 220;
} // namespace Ams

/**
 * @brief Startup timing constants
 *
 * Grace periods for behavior that must settle during initial boot.
 */
namespace Startup {
/// Grace period for filament sensor state stabilization after Moonraker connects
/// Allows time for initial sensor state to arrive after discovery
constexpr std::chrono::seconds SENSOR_STABILIZATION_PERIOD{5};

/// Grace period before allowing user-initiated print starts after app launch.
/// Prevents ghost touch events during startup from accidentally starting prints.
constexpr std::chrono::seconds PRINT_START_GRACE_PERIOD{1};

/// Process start timestamp for grace period calculations.
/// Initialized at static init time (before main), shared across all TUs.
inline const auto PROCESS_START_TIME = std::chrono::steady_clock::now();
} // namespace Startup

/**
 * @brief Animation timing constants for UI micro-animations
 *
 * These provide consistent animation durations across the UI.
 * Used by AnimatedValue and other animation utilities.
 */
namespace Animation {
/// Default animation duration for value changes (ms)
constexpr uint32_t DEFAULT_DURATION_MS = 300;

/// Temperature animation duration - must be SHORTER than update interval (~100-200ms)
/// to complete between updates. Using 80ms for smooth but achievable transitions.
constexpr uint32_t TEMPERATURE_DURATION_MS = 80;

/// Threshold in decidegrees to skip animation (avoids jitter on tiny fluctuations)
/// 5 decidegrees = 0.5°C
constexpr int TEMPERATURE_THRESHOLD_DECI = 5;

/// Fast animation for quick feedback (button presses, toggles)
constexpr uint32_t FAST_DURATION_MS = 150;
} // namespace Animation

/**
 * @brief Touch input gesture tuning
 *
 * Shared so every long-press gesture (home-screen edit mode, gcode viewer,
 * context menus) flips at one consistent, deliberate threshold.
 */
namespace Input {
/// Hold duration before a press registers as a long-press (ms).
/// Overrides LVGL's 400ms default; matches Android's launcher widget-edit
/// gesture (500ms) so mode-switching holds feel deliberate, not twitchy.
constexpr uint32_t LONG_PRESS_MS = 500;

/// Finger travel (in DPI-scaled px) that cancels home-grid edit-mode entry.
/// LVGL fires LONG_PRESSED on hold duration alone, regardless of movement, so
/// a press that drifts past this is treated as an accidental rest-then-linger
/// rather than a deliberate hold, and does not enter edit mode.
constexpr int EDIT_MODE_MOVE_CANCEL_DPX = 12;
} // namespace Input

/**
 * @brief Rolling config backup paths (two-tier: primary + fallback)
 *
 * Config files are backed up outside INSTALL_DIR so they survive both the
 * atomic swap (mv INSTALL_DIR -> INSTALL_DIR.old) during in-app upgrades
 * and Moonraker's shutil.rmtree() wipe of the install directory.
 *
 * Primary:  /var/lib/helixscreen/ via systemd StateDirectory=
 * Fallback: $HOME/.helixscreen/ (writable without StateDirectory)
 *
 * Config::save() maintains rolling backups; Config::init() restores from
 * them if the config directory is missing after an update.
 */
namespace Update {

/// Default primary backup directory — systemd StateDirectory (/var/lib/helixscreen/)
constexpr const char* STATE_DIR_DEFAULT = "/var/lib/helixscreen";

/// Validate that a HOME path looks sane (absolute, >1 char, no control chars).
/// Returns "/tmp" if HOME is corrupted (heap damage to environ block).
inline std::string sanitize_home(const char* home) {
    if (!home || home[0] == '\0')
        return "/tmp";
    std::string h(home);
    if (h.size() < 2 || h[0] != '/')
        return "/tmp";
    for (char c : h) {
        if (static_cast<unsigned char>(c) < 0x20)
            return "/tmp";
    }
    return h;
}

/// Fallback backup — $HOME/.helixscreen/ (writable without StateDirectory)
/// HOME is cached at first call to guard against later heap corruption
/// corrupting the environ block (observed as single-char junk directories).
namespace detail {
inline std::string& backup_fallback_dir_ref() {
    static std::string dir = [] { return sanitize_home(std::getenv("HOME")) + "/.helixscreen"; }();
    return dir;
}

/// Primary (StateDirectory) backup dir. Mutable for the same reason
/// backup_fallback_dir_ref() is: tests redirect it at a temp dir so the
/// backup-cascade cases can run on a machine that has HelixScreen installed —
/// otherwise the real /var/lib/helixscreen/settings.json.backup wins the
/// priority order and the test's own fixture backup is never consulted.
inline std::string& state_dir_ref() {
    static std::string dir = STATE_DIR_DEFAULT;
    return dir;
}
} // namespace detail

inline std::string backup_fallback_dir() {
    return detail::backup_fallback_dir_ref();
}

inline std::string state_dir() {
    return detail::state_dir_ref();
}

inline std::string config_backup_primary() {
    return state_dir() + "/settings.json.backup";
}
inline std::string env_backup_primary() {
    return state_dir() + "/helixscreen.env.backup";
}
/// Legacy primary backup path (for migration)
inline std::string legacy_config_backup_primary() {
    return state_dir() + "/helixconfig.json.backup";
}

inline std::string config_backup_fallback() {
    return backup_fallback_dir() + "/settings.json.backup";
}
inline std::string legacy_config_backup_fallback() {
    return backup_fallback_dir() + "/helixconfig.json.backup";
}
inline std::string env_backup_fallback() {
    return backup_fallback_dir() + "/helixscreen.env.backup";
}

/// Marker the installer drops beside settings.json when it deliberately keeps
/// the packaged (platform preset) config because no user config existed to
/// restore. Resolved relative to the config file's own directory.
///
/// A packaged settings.json is ambiguous on its own: byte-for-byte the same
/// document ships with a fresh install and is what Moonraker's type:web update
/// leaves behind after rmtree() destroys the user's copy. Moonraker never runs
/// the installer, and its rmtree() takes the marker with it, so an absent
/// marker identifies the clobber. Config::init() consumes the marker on the
/// first boot that reads it.
constexpr const char* FRESH_INSTALL_MARKER = ".helix-fresh-install";

/// Marker file written before _exit(0) after a successful update.
/// Watchdog checks for this to skip crash dialog on post-update restarts.
constexpr const char* UPDATE_RESTART_MARKER_PRIMARY = "/var/lib/helixscreen/update_restart";

inline std::string update_restart_marker_path() {
    // Try primary (systemd StateDirectory) first
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path primary_dir = fs::path(UPDATE_RESTART_MARKER_PRIMARY).parent_path();
    if (fs::exists(primary_dir, ec) && !ec) {
        return UPDATE_RESTART_MARKER_PRIMARY;
    }
    // Fallback to $HOME/.helixscreen/
    return backup_fallback_dir() + "/update_restart";
}
} // namespace Update
} // namespace AppConstants
