// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <string>

namespace helix {

/** @brief Time display format (12-hour with AM/PM or 24-hour) */
enum class TimeFormat { HOUR_12 = 0, HOUR_24 = 1 };

/**
 * @brief Domain-specific manager for display/appearance settings
 *
 * Owns all display-related LVGL subjects and persistence:
 * - dark_mode (light/dark toggle)
 * - dark_mode_available (ephemeral, depends on theme)
 * - theme_preset (current theme index)
 * - display_dim (dim timeout in seconds)
 * - display_sleep (sleep timeout in seconds)
 * - brightness (0-100, clamped to 10-100)
 * - has_backlight (ephemeral, hardware detection)
 * - sleep_while_printing (allow sleep during prints)
 * - animations_enabled (UI animation toggle)
 * - gcode_3d_enabled (3D G-code preview toggle)
 * - bed_mesh_render_mode (Auto/3D/2D)
 * - gcode_render_mode (Auto/3D/2D)
 * - time_format (12H/24H)
 * - printer_image (config-only, no subject)
 * - bed_mesh_show_zero_plane (config-only, no subject)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class DisplaySettingsManager {
  public:
    static DisplaySettingsManager& instance();

    // Non-copyable
    DisplaySettingsManager(const DisplaySettingsManager&) = delete;
    DisplaySettingsManager& operator=(const DisplaySettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // DARK MODE / THEME
    // =========================================================================

    /** @brief Get dark mode state */
    bool get_dark_mode() const;

    /** @brief Set dark mode state (updates subject + persists) */
    void set_dark_mode(bool enabled);

    /** @brief Check if current theme supports dark mode toggle */
    bool is_dark_mode_available() const;

    /** @brief Called when theme changes to update mode availability */
    void on_theme_changed();

    /** @brief Get current theme filename (without .json) */
    std::string get_theme_name() const;

    /** @brief Set theme by filename, marks restart pending */
    void set_theme_name(const std::string& name);

    /** @brief Get dropdown options string for discovered themes */
    std::string get_theme_options() const;

    /** @brief Get index of current theme in options list */
    int get_theme_index() const;

    /** @brief Set theme by dropdown index */
    void set_theme_by_index(int index);

    // =========================================================================
    // DISPLAY POWER / BRIGHTNESS
    // =========================================================================

    /** @brief Get display dim timeout in seconds (0 = disabled) */
    int get_display_dim_sec() const;

    /** @brief Set display dim timeout (updates subject + persists + notifies DisplayManager) */
    void set_display_dim_sec(int seconds);

    /** @brief Get display sleep timeout in seconds (0 = disabled) */
    int get_display_sleep_sec() const;

    /** @brief Set display sleep timeout (updates subject + persists) */
    void set_display_sleep_sec(int seconds);

    /** @brief Get display brightness (10-100) */
    int get_brightness() const;

    /** @brief Set display brightness (clamped 10-100, updates subject + hardware + persists) */
    void set_brightness(int percent);

    /** @brief Check if hardware backlight control is available */
    bool has_backlight_control() const;

    /** @brief Get sleep while printing state */
    bool get_sleep_while_printing() const;

    /** @brief Set sleep while printing state (updates subject + persists) */
    void set_sleep_while_printing(bool enabled);

    // =========================================================================
    // UI PREFERENCES
    // =========================================================================

    /** @brief Get animations enabled state */
    bool get_animations_enabled() const;

    /** @brief Set animations enabled state (updates subject + persists) */
    void set_animations_enabled(bool enabled);

    /** @brief Get G-code 3D preview enabled state */
    bool get_gcode_3d_enabled() const;

    /** @brief Set G-code 3D preview enabled state (updates subject + persists) */
    void set_gcode_3d_enabled(bool enabled);

    /** @brief Get bed mesh render mode (0=Auto, 1=3D, 2=2D) */
    int get_bed_mesh_render_mode() const;

    /** @brief Set bed mesh render mode (updates subject + persists) */
    void set_bed_mesh_render_mode(int mode);

    /** @brief Get dropdown options string "Auto\n3D View\n2D Heatmap" */
    static const char* get_bed_mesh_render_mode_options();

    /** @brief Get G-code render mode (0=Auto, 1=3D, 2=2D, 3=Thumbnail Only) */
    int get_gcode_render_mode() const;

    /** @brief Set G-code render mode (updates subject + persists) */
    void set_gcode_render_mode(int mode);

    /** @brief Get dropdown options string "Auto\n3D View\n2D Layers" */
    static const char* get_gcode_render_mode_options();

    /** @brief Get time format setting */
    TimeFormat get_time_format() const;

    /** @brief Set time format (updates subject + persists) */
    void set_time_format(TimeFormat format);

    /** @brief Get dropdown options string "12 Hour\n24 Hour" */
    static const char* get_time_format_options();

    // =========================================================================
    // CONFIG-ONLY SETTINGS (no subjects)
    // =========================================================================

    /** @brief Get custom printer image ID (empty = auto-detect) */
    std::string get_printer_image() const;

    /** @brief Set custom printer image ID and persist. Empty = auto-detect. */
    void set_printer_image(const std::string& id);

    /** @brief Get bed mesh zero plane visibility */
    bool get_bed_mesh_show_zero_plane() const;

    // =========================================================================
    // DISPLAY DIM OPTIONS (for dropdown population)
    // =========================================================================

    /** @brief Get display dim options for dropdown */
    static const char* get_display_dim_options();

    /** @brief Get dropdown index for current dim seconds value */
    static int dim_seconds_to_index(int seconds);

    /** @brief Convert dropdown index to dim seconds */
    static int index_to_dim_seconds(int index);

    // =========================================================================
    // DISPLAY SLEEP OPTIONS (for dropdown population)
    // =========================================================================

    /** @brief Get display sleep options for dropdown */
    static const char* get_display_sleep_options();

    /** @brief Get dropdown index for current sleep seconds value */
    static int sleep_seconds_to_index(int seconds);

    /** @brief Convert dropdown index to sleep seconds */
    static int index_to_sleep_seconds(int index);

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Dark mode subject (integer: 0=light, 1=dark) */
    lv_subject_t* subject_dark_mode() {
        return &dark_mode_subject_;
    }

    /** @brief Dark mode available subject (integer: 0=no toggle, 1=toggle enabled) */
    lv_subject_t* subject_dark_mode_available() {
        return &dark_mode_available_subject_;
    }

    /** @brief Theme preset subject (integer: preset index) */
    lv_subject_t* subject_theme_preset() {
        return &theme_preset_subject_;
    }

    /** @brief Display dim subject (integer: seconds, 0=disabled) */
    lv_subject_t* subject_display_dim() {
        return &display_dim_subject_;
    }

    /** @brief Display sleep subject (integer: seconds, 0=disabled) */
    lv_subject_t* subject_display_sleep() {
        return &display_sleep_subject_;
    }

    /** @brief Brightness subject (integer: 10-100 percent) */
    lv_subject_t* subject_brightness() {
        return &brightness_subject_;
    }

    /** @brief Has backlight control subject (integer: 0=no, 1=yes) */
    lv_subject_t* subject_has_backlight() {
        return &has_backlight_subject_;
    }

    /** @brief Sleep while printing subject (integer: 0=inhibit, 1=allow) */
    lv_subject_t* subject_sleep_while_printing() {
        return &sleep_while_printing_subject_;
    }

    /** @brief Animations enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_animations_enabled() {
        return &animations_enabled_subject_;
    }

    /** @brief G-code 3D preview subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_gcode_3d_enabled() {
        return &gcode_3d_enabled_subject_;
    }

    /** @brief Bed mesh render mode subject (integer: 0=auto, 1=3D, 2=2D) */
    lv_subject_t* subject_bed_mesh_render_mode() {
        return &bed_mesh_render_mode_subject_;
    }

    /** @brief G-code render mode subject (integer: 0=auto, 1=3D, 2=2D, 3=thumbnail only) */
    lv_subject_t* subject_gcode_render_mode() {
        return &gcode_render_mode_subject_;
    }

    /** @brief Time format subject (integer: 0=12H, 1=24H) */
    lv_subject_t* subject_time_format() {
        return &time_format_subject_;
    }

  private:
    DisplaySettingsManager();
    ~DisplaySettingsManager() = default;

    SubjectManager subjects_;

    lv_subject_t dark_mode_subject_;
    lv_subject_t dark_mode_available_subject_;
    lv_subject_t theme_preset_subject_;
    lv_subject_t display_dim_subject_;
    lv_subject_t display_sleep_subject_;
    lv_subject_t brightness_subject_;
    lv_subject_t has_backlight_subject_;
    lv_subject_t sleep_while_printing_subject_;
    lv_subject_t animations_enabled_subject_;
    lv_subject_t gcode_3d_enabled_subject_;
    lv_subject_t bed_mesh_render_mode_subject_;
    lv_subject_t gcode_render_mode_subject_;
    lv_subject_t time_format_subject_;

    bool subjects_initialized_ = false;
};

} // namespace helix
