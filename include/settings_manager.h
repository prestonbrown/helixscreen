// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef __HELIX_SETTINGS_MANAGER_H__
#define __HELIX_SETTINGS_MANAGER_H__

#include "lvgl/lvgl.h"
#include <functional>
#include <string>

class MoonrakerClient;

/**
 * @brief Application settings manager with reactive UI binding
 *
 * Coordinates persistence (Config), reactive subjects (lv_subject_t), immediate
 * effects (theme changes, Moonraker commands), and user preferences.
 *
 * Architecture:
 * ```
 * SettingsManager
 * ├── Persistence Layer (wraps Config)
 * │   └── JSON storage in helixconfig.json
 * ├── Reactive Layer (lv_subject_t)
 * │   └── UI automatically updates when settings change
 * ├── Effect Layer (immediate actions)
 * │   └── Apply dark mode, set LED, adjust sleep timer
 * └── Remote Layer (Moonraker API)
 *     └── LED commands, calibration triggers
 * ```
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 *
 * Usage:
 * ```cpp
 * auto& settings = SettingsManager::instance();
 * settings.init_subjects();  // Before XML creation
 *
 * // Toggle dark mode (updates UI, applies theme, persists)
 * settings.set_dark_mode(true);
 *
 * // Get subject for XML binding
 * lv_subject_t* subject = settings.subject_dark_mode();
 * ```
 */
class SettingsManager {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to global SettingsManager
     */
    static SettingsManager& instance();

    // Prevent copying
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    /**
     * @brief Initialize LVGL subjects
     *
     * MUST be called BEFORE creating XML components that bind to settings subjects.
     * Loads initial values from Config and registers subjects with LVGL XML system.
     */
    void init_subjects();

    /**
     * @brief Set Moonraker client reference for remote commands
     *
     * Required for LED control and other printer-dependent settings.
     * Call after MoonrakerClient is initialized.
     *
     * @param client Pointer to active MoonrakerClient (can be nullptr to disable)
     */
    void set_moonraker_client(MoonrakerClient* client);

    // =========================================================================
    // APPEARANCE SETTINGS
    // =========================================================================

    /**
     * @brief Get dark mode state
     * @return true if dark mode enabled
     */
    bool get_dark_mode() const;

    /**
     * @brief Set dark mode state
     *
     * Updates subject (UI reacts) and persists to Config.
     * Note: Theme change requires application restart to take effect.
     *
     * @param enabled true for dark mode, false for light mode
     */
    void set_dark_mode(bool enabled);

    /**
     * @brief Get display sleep timeout in seconds
     * @return Sleep timeout (0 = disabled)
     */
    int get_display_sleep_sec() const;

    /**
     * @brief Set display sleep timeout
     *
     * Updates subject and persists. Effect applied elsewhere (display driver).
     *
     * @param seconds Sleep timeout (0 to disable)
     */
    void set_display_sleep_sec(int seconds);

    /**
     * @brief Get display brightness (0-100)
     * @return Brightness percentage
     */
    int get_brightness() const;

    /**
     * @brief Set display brightness
     *
     * Updates subject and persists. Effect applied elsewhere (display driver).
     *
     * @param percent Brightness percentage (0-100, clamped to 10-100 minimum)
     */
    void set_brightness(int percent);

    // =========================================================================
    // PRINTER SETTINGS
    // =========================================================================

    /**
     * @brief Get LED enabled state
     * @return true if LED is on
     */
    bool get_led_enabled() const;

    /**
     * @brief Set LED enabled state
     *
     * Updates subject and sends Moonraker command if client is connected.
     * Note: Does NOT persist - LED state is ephemeral.
     *
     * @param enabled true to turn on, false to turn off
     */
    void set_led_enabled(bool enabled);

    // =========================================================================
    // NOTIFICATION SETTINGS (placeholders for future hardware)
    // =========================================================================

    /**
     * @brief Get sound enabled state
     * @return true if sounds enabled
     */
    bool get_sounds_enabled() const;

    /**
     * @brief Set sound enabled state
     *
     * Placeholder - hardware support TBD. Updates subject and persists.
     *
     * @param enabled true to enable sounds
     */
    void set_sounds_enabled(bool enabled);

    /**
     * @brief Get completion alert enabled state
     * @return true if print completion alerts enabled
     */
    bool get_completion_alert() const;

    /**
     * @brief Set completion alert enabled state
     * @param enabled true to enable completion alerts
     */
    void set_completion_alert(bool enabled);

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Dark mode subject (integer: 0=light, 1=dark) */
    lv_subject_t* subject_dark_mode() { return &dark_mode_subject_; }

    /** @brief Display sleep subject (integer: seconds, 0=disabled) */
    lv_subject_t* subject_display_sleep() { return &display_sleep_subject_; }

    /** @brief Brightness subject (integer: 10-100 percent) */
    lv_subject_t* subject_brightness() { return &brightness_subject_; }

    /** @brief LED enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_led_enabled() { return &led_enabled_subject_; }

    /** @brief Sounds enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_sounds_enabled() { return &sounds_enabled_subject_; }

    /** @brief Completion alert subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_completion_alert() { return &completion_alert_subject_; }

    // =========================================================================
    // DISPLAY SLEEP OPTIONS (for dropdown population)
    // =========================================================================

    /**
     * @brief Get display sleep options for dropdown
     * @return Newline-separated string of options (e.g., "Never\n1 minute\n5 minutes")
     */
    static const char* get_display_sleep_options();

    /**
     * @brief Get dropdown index for current sleep seconds value
     * @param seconds Current sleep timeout in seconds
     * @return Dropdown index (0-based)
     */
    static int sleep_seconds_to_index(int seconds);

    /**
     * @brief Convert dropdown index to sleep seconds
     * @param index Dropdown index (0-based)
     * @return Sleep timeout in seconds
     */
    static int index_to_sleep_seconds(int index);

  private:
    SettingsManager();
    ~SettingsManager() = default;

    // Apply immediate effects
    void send_led_command(bool enabled);

    // LVGL subjects
    lv_subject_t dark_mode_subject_;
    lv_subject_t display_sleep_subject_;
    lv_subject_t brightness_subject_;
    lv_subject_t led_enabled_subject_;
    lv_subject_t sounds_enabled_subject_;
    lv_subject_t completion_alert_subject_;

    // External references
    MoonrakerClient* moonraker_client_ = nullptr;

    // State
    bool subjects_initialized_ = false;
};

#endif // __HELIX_SETTINGS_MANAGER_H__
