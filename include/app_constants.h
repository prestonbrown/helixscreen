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
#include <string>
#include <sys/stat.h>

/**
 * @brief Application-wide constants shared between UI and backend
 */
namespace helix::app_constants {
/**
 * @brief Temperature-related constants
 *
 * Safety limits and default values for temperature control.
 * Used by both UI panels and backend temperature management.
 */
namespace temperature {
/// Minimum safe temperature for extrusion operations (Klipper default)
constexpr int kMinExtrusionTemp = 170;

/// Default maximum temperature for nozzle/hotend
constexpr int kDefaultNozzleMax = 500;

/// Default maximum temperature for heated bed
constexpr int kDefaultBedMax = 150;

/// Default minimum temperature (ambient)
constexpr int kDefaultMinTemp = 0;
} // namespace temperature

/**
 * @brief Responsive layout breakpoints
 *
 * These define the screen height thresholds for different UI layouts.
 * Use these consistently across all panels for uniform responsive behavior.
 */
namespace responsive {
/// Tiny screens: <= 479px height
constexpr int32_t kBreakpointTinyMax = 479;

/// Small screens: 480-599px height
constexpr int32_t kBreakpointSmallMax = 599;

/// Medium screens: 600-1023px height
constexpr int32_t kBreakpointMediumMax = 1023;

/// Large screens: >= 1024px height
// (No max defined - anything above MEDIUM is large)
} // namespace responsive

/**
 * @brief AMS/Filament loading constants
 */
namespace ams {
/// Default preheat temperature when no material-specific temp is known (°C)
constexpr int kDefaultLoadPreheatTemp = 220;
} // namespace ams

/**
 * @brief Startup timing constants
 *
 * Grace periods for behavior that must settle during initial boot.
 */
namespace startup {
/// Grace period for filament sensor state stabilization after Moonraker connects
/// Allows time for initial sensor state to arrive after discovery
constexpr std::chrono::seconds kSensorStabilizationPeriod{5};

/// Grace period before allowing user-initiated print starts after app launch.
/// Prevents ghost touch events during startup from accidentally starting prints.
constexpr std::chrono::seconds kPrintStartGracePeriod{1};

/// Process start timestamp for grace period calculations.
/// Initialized at static init time (before main), shared across all TUs.
inline const auto kProcessStartTime = std::chrono::steady_clock::now();
} // namespace startup

namespace transfer {
/// A transfer, or a wait on file reads, that makes no progress for this long
/// is treated as stalled and given up.
constexpr uint32_t STALL_MS = 30000;
} // namespace transfer

/**
 * @brief Animation timing constants for UI micro-animations
 *
 * These provide consistent animation durations across the UI.
 * Used by AnimatedValue and other animation utilities.
 */
namespace animation {
/// Default animation duration for value changes (ms)
constexpr uint32_t kDefaultDurationMs = 300;

/// Temperature animation duration - must be SHORTER than update interval (~100-200ms)
/// to complete between updates. Using 80ms for smooth but achievable transitions.
constexpr uint32_t kTemperatureDurationMs = 80;

/// Threshold in decidegrees to skip animation (avoids jitter on tiny fluctuations)
/// 5 decidegrees = 0.5°C
constexpr int kTemperatureThresholdDeci = 5;

/// Fast animation for quick feedback (button presses, toggles)
constexpr uint32_t kFastDurationMs = 150;
} // namespace animation

/**
 * @brief Touch input gesture tuning
 *
 * Shared so every long-press gesture (home-screen edit mode, gcode viewer,
 * context menus) flips at one consistent, deliberate threshold.
 */
namespace input {
/// Hold duration before a press registers as a long-press (ms).
/// Overrides LVGL's 400ms default; matches Android's launcher widget-edit
/// gesture (500ms) so mode-switching holds feel deliberate, not twitchy.
constexpr uint32_t kLongPressMs = 500;

/// Finger travel (in DPI-scaled px) that cancels home-grid edit-mode entry.
/// LVGL fires LONG_PRESSED on hold duration alone, regardless of movement, so
/// a press that drifts past this is treated as an accidental rest-then-linger
/// rather than a deliberate hold, and does not enter edit mode.
constexpr int kEditModeMoveCancelDpx = 12;
} // namespace input

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
namespace update {

/// Default primary backup directory — systemd StateDirectory (/var/lib/helixscreen/)
constexpr const char* kStateDirDefault = "/var/lib/helixscreen";

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
    static std::string dir = kStateDirDefault;
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
constexpr const char* kFreshInstallMarker = ".helix-fresh-install";

/// Marker file written before _exit(0) after a successful update.
/// Watchdog checks for this to skip crash dialog on post-update restarts.
constexpr const char* kUpdateRestartMarkerPrimary = "/var/lib/helixscreen/update_restart";

inline std::string update_restart_marker_path() {
    // Try primary (systemd StateDirectory) first
    // A plain stat, not helix::fs: this header is included too widely to bring
    // that namespace into files that alias std::filesystem as `fs`.
    const std::string primary(kUpdateRestartMarkerPrimary);
    struct stat st {};
    if (::stat(primary.substr(0, primary.rfind('/')).c_str(), &st) == 0) {
        return kUpdateRestartMarkerPrimary;
    }
    // Fallback to $HOME/.helixscreen/
    return backup_fallback_dir() + "/update_restart";
}
} // namespace update
} // namespace helix::app_constants
