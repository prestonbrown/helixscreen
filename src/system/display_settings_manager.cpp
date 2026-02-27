// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_settings_manager.h"

#include "ui_toast_manager.h"

#include "config.h"
#include "display_manager.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>

using namespace helix;

// Display dim option values (seconds) - time before screen dims to lower brightness
// Index: 0=Never, 1=30sec, 2=1min, 3=2min, 4=5min
static const int DIM_OPTIONS[] = {0, 30, 60, 120, 300};
static const int DIM_OPTIONS_COUNT = sizeof(DIM_OPTIONS) / sizeof(DIM_OPTIONS[0]);
static const char* DIM_OPTIONS_TEXT = "Never\n30 seconds\n1 minute\n2 minutes\n5 minutes";

// Display sleep option values (seconds) - time before screen fully sleeps
// Index: 0=Never, 1=1min, 2=5min, 3=10min, 4=30min
static const int SLEEP_OPTIONS[] = {0, 60, 300, 600, 1800};
static const int SLEEP_OPTIONS_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);
static const char* SLEEP_OPTIONS_TEXT = "Never\n1 minute\n5 minutes\n10 minutes\n30 minutes";

// Bed mesh render mode options (Auto=0, 3D=1, 2D=2)
static const char* BED_MESH_RENDER_MODE_OPTIONS_TEXT = "Auto\n3D View\n2D Heatmap";
static const char* GCODE_RENDER_MODE_OPTIONS_TEXT = "Auto\n3D View\n2D Layers\nThumbnail Only";

// Time format options (12H=0, 24H=1)
static const char* TIME_FORMAT_OPTIONS_TEXT = "12 Hour\n24 Hour";

// Helper: Validate a timeout value against allowed options, snapping to nearest valid value
template <size_t N>
static int validate_timeout_option(int value, const int (&options)[N], int default_value,
                                   const char* setting_name) {
    // Check if value is exactly one of the valid options
    for (size_t i = 0; i < N; ++i) {
        if (options[i] == value) {
            return value; // Valid
        }
    }

    // Invalid value - find the nearest valid option
    int nearest = default_value;
    int min_diff = std::abs(value - default_value);
    for (size_t i = 0; i < N; ++i) {
        int diff = std::abs(value - options[i]);
        if (diff < min_diff) {
            min_diff = diff;
            nearest = options[i];
        }
    }

    spdlog::warn("[DisplaySettingsManager] Invalid {} value {} - snapping to nearest valid: {}",
                 setting_name, value, nearest);
    return nearest;
}

DisplaySettingsManager& DisplaySettingsManager::instance() {
    static DisplaySettingsManager instance;
    return instance;
}

DisplaySettingsManager::DisplaySettingsManager() {
    spdlog::trace("[DisplaySettingsManager] Constructor");
}

void DisplaySettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[DisplaySettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[DisplaySettingsManager] Initializing subjects");

    Config* config = Config::get_instance();

    // Dark mode (default: true = dark)
    bool dark_mode = config->get<bool>("/dark_mode", true);
    UI_MANAGED_SUBJECT_INT(dark_mode_subject_, dark_mode ? 1 : 0, "settings_dark_mode", subjects_);

    // Dark mode availability (depends on theme - updated in on_theme_changed())
    // Start with 1 (available) - will be corrected when theme is fully loaded
    UI_MANAGED_SUBJECT_INT(dark_mode_available_subject_, 1, "settings_dark_mode_available",
                           subjects_);

    // Theme index (derived from current theme name)
    int theme_index = get_theme_index();
    UI_MANAGED_SUBJECT_INT(theme_preset_subject_, theme_index, "settings_theme_preset", subjects_);

    // Display dim (default: 300 seconds = 5 minutes)
    // Validate against allowed options to catch corrupt config values
    int dim_sec = config->get<int>("/display/dim_sec", 300);
    dim_sec = validate_timeout_option(dim_sec, DIM_OPTIONS, 300, "dim_sec");
    UI_MANAGED_SUBJECT_INT(display_dim_subject_, dim_sec, "settings_display_dim", subjects_);

    // Sync validated dim timeout to DisplayManager (it reads config directly at init,
    // so we need to push the corrected value if validation changed it)
    if (auto* dm = DisplayManager::instance()) {
        dm->set_dim_timeout(dim_sec);
    }

    // Display sleep (default: 1800 seconds = 30 minutes)
    // Validate against allowed options to catch corrupt config values
    int sleep_sec = config->get<int>("/display/sleep_sec", 1800);
    sleep_sec = validate_timeout_option(sleep_sec, SLEEP_OPTIONS, 1800, "sleep_sec");
    UI_MANAGED_SUBJECT_INT(display_sleep_subject_, sleep_sec, "settings_display_sleep", subjects_);

    // Brightness: Read from config (DisplayManager handles hardware)
    int brightness = config->get<int>("/brightness", 50);
    brightness = std::clamp(brightness, 10, 100);
    UI_MANAGED_SUBJECT_INT(brightness_subject_, brightness, "settings_brightness", subjects_);
    spdlog::debug("[DisplaySettingsManager] Brightness initialized to {}%", brightness);

    // Has backlight control subject (for UI visibility) - check DisplayManager
    bool has_backlight = false;
    if (auto* dm = DisplayManager::instance()) {
        has_backlight = dm->has_backlight_control();
    }
    UI_MANAGED_SUBJECT_INT(has_backlight_subject_, has_backlight ? 1 : 0, "settings_has_backlight",
                           subjects_);

    // Sleep while printing (default: true = allow sleep during prints)
    bool sleep_while_printing = config->get<bool>("/display/sleep_while_printing", true);
    UI_MANAGED_SUBJECT_INT(sleep_while_printing_subject_, sleep_while_printing ? 1 : 0,
                           "settings_sleep_while_printing", subjects_);

    // Animations enabled (default: true)
    bool animations = config->get<bool>("/display/animations_enabled", true);
    UI_MANAGED_SUBJECT_INT(animations_enabled_subject_, animations ? 1 : 0,
                           "settings_animations_enabled", subjects_);

    // G-code 3D preview enabled (default: true)
    bool gcode_3d = config->get<bool>("/display/gcode_3d_enabled", true);
    UI_MANAGED_SUBJECT_INT(gcode_3d_enabled_subject_, gcode_3d ? 1 : 0, "settings_gcode_3d_enabled",
                           subjects_);

    // Bed mesh render mode (default: 0 = Auto)
    int bed_mesh_mode = config->get<int>("/display/bed_mesh_render_mode", 0);
    bed_mesh_mode = std::clamp(bed_mesh_mode, 0, 2);
    UI_MANAGED_SUBJECT_INT(bed_mesh_render_mode_subject_, bed_mesh_mode,
                           "settings_bed_mesh_render_mode", subjects_);

    // G-code render mode (default: 0 = Auto)
    int gcode_mode = config->get<int>("/display/gcode_render_mode", 0);
    gcode_mode = std::clamp(gcode_mode, 0, 3);
    UI_MANAGED_SUBJECT_INT(gcode_render_mode_subject_, gcode_mode, "settings_gcode_render_mode",
                           subjects_);

    // Time format (default: 0 = 12-hour)
    int time_format = config->get<int>("/display/time_format", 0);
    time_format = std::clamp(time_format, 0, 1);
    UI_MANAGED_SUBJECT_INT(time_format_subject_, time_format, "settings_time_format", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup with StaticSubjectRegistry
    StaticSubjectRegistry::instance().register_deinit(
        "DisplaySettingsManager", []() { DisplaySettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[DisplaySettingsManager] Subjects initialized: dark_mode={}, theme={}, "
                  "dim={}s, sleep={}s, brightness={}, animations={}",
                  dark_mode, get_theme_name(), dim_sec, sleep_sec, brightness, animations);
}

void DisplaySettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[DisplaySettingsManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[DisplaySettingsManager] Subjects deinitialized");
}

// =============================================================================
// DARK MODE / THEME
// =============================================================================

bool DisplaySettingsManager::get_dark_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&dark_mode_subject_)) != 0;
}

void DisplaySettingsManager::set_dark_mode(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_dark_mode({})", enabled);

    // Guard: Check if requested mode is supported
    if (enabled && !theme_manager_supports_dark_mode()) {
        spdlog::warn("[DisplaySettingsManager] Cannot enable dark mode - theme doesn't support it");
        return;
    }
    if (!enabled && !theme_manager_supports_light_mode()) {
        spdlog::warn(
            "[DisplaySettingsManager] Cannot enable light mode - theme doesn't support it");
        return;
    }

    // 1. Update subject (UI reacts immediately via binding)
    lv_subject_set_int(&dark_mode_subject_, enabled ? 1 : 0);

    // 2. Persist to config (theme change requires restart to take effect)
    Config* config = Config::get_instance();
    config->set<bool>("/dark_mode", enabled);
    config->save();

    spdlog::debug("[DisplaySettingsManager] Dark mode {} saved (restart required)",
                  enabled ? "enabled" : "disabled");
}

bool DisplaySettingsManager::is_dark_mode_available() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&dark_mode_available_subject_)) != 0;
}

void DisplaySettingsManager::on_theme_changed() {
    // Check what modes the current theme supports
    bool supports_dark = theme_manager_supports_dark_mode();
    bool supports_light = theme_manager_supports_light_mode();

    if (supports_dark && supports_light) {
        // Dual-mode theme - enable toggle
        lv_subject_set_int(&dark_mode_available_subject_, 1);
        spdlog::trace("[DisplaySettingsManager] Theme supports both modes, toggle enabled");
    } else if (supports_dark) {
        // Dark-only theme - disable toggle, force dark mode
        lv_subject_set_int(&dark_mode_available_subject_, 0);
        if (!get_dark_mode()) {
            spdlog::info("[DisplaySettingsManager] Theme is dark-only, switching to dark mode");
            // Update subject without persisting (theme controls this)
            lv_subject_set_int(&dark_mode_subject_, 1);
        }
        spdlog::debug("[DisplaySettingsManager] Theme is dark-only, toggle disabled");
    } else if (supports_light) {
        // Light-only theme - disable toggle, force light mode
        lv_subject_set_int(&dark_mode_available_subject_, 0);
        if (get_dark_mode()) {
            spdlog::info("[DisplaySettingsManager] Theme is light-only, switching to light mode");
            // Update subject without persisting (theme controls this)
            lv_subject_set_int(&dark_mode_subject_, 0);
        }
        spdlog::debug("[DisplaySettingsManager] Theme is light-only, toggle disabled");
    } else {
        // Invalid theme (no palettes) - shouldn't happen, but handle gracefully
        spdlog::warn("[DisplaySettingsManager] Theme has no valid palettes");
        lv_subject_set_int(&dark_mode_available_subject_, 0);
    }
}

std::string DisplaySettingsManager::get_theme_name() const {
    // Use the actual active theme (which respects HELIX_THEME env override)
    const auto& active = theme_manager_get_active_theme();
    if (!active.filename.empty()) {
        // Return the filename to match dropdown option matching
        return active.filename;
    }
    // Fallback to config if theme_manager not initialized yet
    Config* config = Config::get_instance();
    return config ? config->get<std::string>("/display/theme", helix::DEFAULT_THEME)
                  : helix::DEFAULT_THEME;
}

void DisplaySettingsManager::set_theme_name(const std::string& name) {
    spdlog::info("[DisplaySettingsManager] set_theme_name({})", name);

    Config* config = Config::get_instance();
    config->set<std::string>("/display/theme", name);
    config->save();
}

std::string DisplaySettingsManager::get_theme_options() const {
    auto themes = helix::discover_themes(helix::get_themes_directory());

    std::string options;
    for (size_t i = 0; i < themes.size(); ++i) {
        if (i > 0)
            options += "\n";
        options += themes[i].display_name;
    }
    return options;
}

int DisplaySettingsManager::get_theme_index() const {
    std::string current = get_theme_name();
    auto themes = helix::discover_themes(helix::get_themes_directory());

    for (size_t i = 0; i < themes.size(); ++i) {
        if (themes[i].filename == current) {
            return static_cast<int>(i);
        }
    }
    return 0; // Default to first theme
}

void DisplaySettingsManager::set_theme_by_index(int index) {
    auto themes = helix::discover_themes(helix::get_themes_directory());

    if (index >= 0 && index < static_cast<int>(themes.size())) {
        set_theme_name(themes[index].filename);

        // Update subject so UI reflects the change
        lv_subject_set_int(&theme_preset_subject_, index);
    }
}

// =============================================================================
// DISPLAY POWER / BRIGHTNESS
// =============================================================================

int DisplaySettingsManager::get_display_dim_sec() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&display_dim_subject_));
}

void DisplaySettingsManager::set_display_dim_sec(int seconds) {
    spdlog::info("[DisplaySettingsManager] set_display_dim_sec({})", seconds);

    // 1. Update subject
    lv_subject_set_int(&display_dim_subject_, seconds);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/display/dim_sec", seconds);
    config->save();

    // 3. Notify DisplayManager to reload dim setting
    DisplayManager* dm = DisplayManager::instance();
    if (dm) {
        dm->set_dim_timeout(seconds);
    }

    // 4. If dim is now > sleep, bump sleep up to match (unless sleep is disabled)
    int sleep_sec = get_display_sleep_sec();
    if (seconds > 0 && sleep_sec > 0 && sleep_sec < seconds) {
        spdlog::info("[DisplaySettingsManager] Bumping sleep {}s up to match dim {}s", sleep_sec,
                     seconds);
        // Update directly to avoid recursion through set_display_sleep_sec
        lv_subject_set_int(&display_sleep_subject_, seconds);
        Config* cfg = Config::get_instance();
        cfg->set<int>("/display/sleep_sec", seconds);
        cfg->save();
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Sleep timeout adjusted"), 2000);
    }

    spdlog::debug("[DisplaySettingsManager] Display dim set to {}s", seconds);
}

int DisplaySettingsManager::get_display_sleep_sec() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&display_sleep_subject_));
}

void DisplaySettingsManager::set_display_sleep_sec(int seconds) {
    spdlog::info("[DisplaySettingsManager] set_display_sleep_sec({})", seconds);

    // Ensure sleep timeout >= dim timeout (unless sleep is disabled with 0)
    // It's nonsensical to sleep before dimming
    if (seconds > 0) {
        int dim_sec = get_display_dim_sec();
        if (dim_sec > 0 && seconds < dim_sec) {
            spdlog::info("[DisplaySettingsManager] Clamping sleep {}s to dim {}s", seconds,
                         dim_sec);
            seconds = dim_sec;
            ToastManager::instance().show(ToastSeverity::INFO,
                                          "Sleep adjusted to match dim timeout", 2000);
        }
    }

    // 1. Update subject
    lv_subject_set_int(&display_sleep_subject_, seconds);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/display/sleep_sec", seconds);
    config->save();

    // Note: Actual display sleep is handled by the display driver reading this value
    spdlog::debug("[DisplaySettingsManager] Display sleep set to {}s", seconds);
}

int DisplaySettingsManager::get_brightness() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&brightness_subject_));
}

void DisplaySettingsManager::set_brightness(int percent) {
    // Clamp to valid range (10-100, minimum 10% to prevent black screen)
    int clamped = std::clamp(percent, 10, 100);
    spdlog::info("[DisplaySettingsManager] set_brightness({})", clamped);

    // 1. Update subject (UI reflects change immediately)
    lv_subject_set_int(&brightness_subject_, clamped);

    // 2. Apply to hardware via DisplayManager
    if (auto* dm = DisplayManager::instance()) {
        dm->set_backlight_brightness(clamped);
    }

    // 3. Persist to config
    Config* config = Config::get_instance();
    config->set<int>("/brightness", clamped);
    config->save();
}

bool DisplaySettingsManager::has_backlight_control() const {
    if (auto* dm = DisplayManager::instance()) {
        return dm->has_backlight_control();
    }
    return false;
}

bool DisplaySettingsManager::get_sleep_while_printing() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&sleep_while_printing_subject_)) != 0;
}

void DisplaySettingsManager::set_sleep_while_printing(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_sleep_while_printing({})", enabled);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&sleep_while_printing_subject_, enabled ? 1 : 0);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<bool>("/display/sleep_while_printing", enabled);
    config->save();
}

// =============================================================================
// UI PREFERENCES
// =============================================================================

bool DisplaySettingsManager::get_animations_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&animations_enabled_subject_)) != 0;
}

void DisplaySettingsManager::set_animations_enabled(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_animations_enabled({})", enabled);

    // 1. Update subject (UI reacts, animations check this dynamically)
    lv_subject_set_int(&animations_enabled_subject_, enabled ? 1 : 0);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<bool>("/display/animations_enabled", enabled);
    config->save();
}

bool DisplaySettingsManager::get_gcode_3d_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&gcode_3d_enabled_subject_)) != 0;
}

void DisplaySettingsManager::set_gcode_3d_enabled(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_gcode_3d_enabled({})", enabled);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&gcode_3d_enabled_subject_, enabled ? 1 : 0);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<bool>("/display/gcode_3d_enabled", enabled);
    config->save();
}

int DisplaySettingsManager::get_bed_mesh_render_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&bed_mesh_render_mode_subject_));
}

void DisplaySettingsManager::set_bed_mesh_render_mode(int mode) {
    // Clamp to valid range (0=Auto, 1=3D, 2=2D)
    int clamped = std::clamp(mode, 0, 2);
    spdlog::info("[DisplaySettingsManager] set_bed_mesh_render_mode({})", clamped);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&bed_mesh_render_mode_subject_, clamped);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>("/display/bed_mesh_render_mode", clamped);
    config->save();

    spdlog::debug("[DisplaySettingsManager] Bed mesh render mode set to {} ({})", clamped,
                  clamped == 0 ? "Auto" : (clamped == 1 ? "3D" : "2D"));
}

const char* DisplaySettingsManager::get_bed_mesh_render_mode_options() {
    return BED_MESH_RENDER_MODE_OPTIONS_TEXT;
}

int DisplaySettingsManager::get_gcode_render_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&gcode_render_mode_subject_));
}

void DisplaySettingsManager::set_gcode_render_mode(int mode) {
    // Clamp to valid range (0=Auto, 1=3D, 2=2D, 3=Thumbnail Only)
    int clamped = std::clamp(mode, 0, 3);
    spdlog::info("[DisplaySettingsManager] set_gcode_render_mode({})", clamped);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&gcode_render_mode_subject_, clamped);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>("/display/gcode_render_mode", clamped);
    config->save();

    static const char* MODE_NAMES[] = {"Auto", "3D", "2D", "Thumbnail Only"};
    spdlog::debug("[DisplaySettingsManager] G-code render mode set to {} ({})", clamped,
                  MODE_NAMES[clamped]);
}

const char* DisplaySettingsManager::get_gcode_render_mode_options() {
    return GCODE_RENDER_MODE_OPTIONS_TEXT;
}

TimeFormat DisplaySettingsManager::get_time_format() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&time_format_subject_));
    return static_cast<TimeFormat>(std::clamp(val, 0, 1));
}

void DisplaySettingsManager::set_time_format(TimeFormat format) {
    int val = static_cast<int>(format);
    spdlog::info("[DisplaySettingsManager] set_time_format({})", val == 0 ? "12H" : "24H");

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&time_format_subject_, val);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>("/display/time_format", val);
    config->save();
}

const char* DisplaySettingsManager::get_time_format_options() {
    return TIME_FORMAT_OPTIONS_TEXT;
}

// =============================================================================
// CONFIG-ONLY SETTINGS (no subjects)
// =============================================================================

std::string DisplaySettingsManager::get_printer_image() const {
    Config* config = Config::get_instance();
    if (!config)
        return "";
    return config->get<std::string>("/display/printer_image", "");
}

void DisplaySettingsManager::set_printer_image(const std::string& id) {
    Config* config = Config::get_instance();
    if (!config)
        return;
    config->set<std::string>("/display/printer_image", id);
    config->save();
    spdlog::info("[DisplaySettingsManager] Printer image set to: '{}'",
                 id.empty() ? "(auto-detect)" : id);
}

bool DisplaySettingsManager::get_bed_mesh_show_zero_plane() const {
    Config* config = Config::get_instance();
    return config->get<bool>("/display/bed_mesh_show_zero_plane", true);
}

// =============================================================================
// DISPLAY DIM OPTIONS
// =============================================================================

const char* DisplaySettingsManager::get_display_dim_options() {
    return DIM_OPTIONS_TEXT;
}

int DisplaySettingsManager::dim_seconds_to_index(int seconds) {
    for (int i = 0; i < DIM_OPTIONS_COUNT; i++) {
        if (DIM_OPTIONS[i] == seconds) {
            return i;
        }
    }
    // Default to "5 minutes" if not found
    return 4;
}

int DisplaySettingsManager::index_to_dim_seconds(int index) {
    if (index >= 0 && index < DIM_OPTIONS_COUNT) {
        return DIM_OPTIONS[index];
    }
    return 300; // Default 5 minutes
}

// =============================================================================
// DISPLAY SLEEP OPTIONS
// =============================================================================

const char* DisplaySettingsManager::get_display_sleep_options() {
    return SLEEP_OPTIONS_TEXT;
}

int DisplaySettingsManager::sleep_seconds_to_index(int seconds) {
    for (int i = 0; i < SLEEP_OPTIONS_COUNT; i++) {
        if (SLEEP_OPTIONS[i] == seconds) {
            return i;
        }
    }
    // Default to "10 minutes" if not found
    return 3;
}

int DisplaySettingsManager::index_to_sleep_seconds(int index) {
    if (index >= 0 && index < SLEEP_OPTIONS_COUNT) {
        return SLEEP_OPTIONS[index];
    }
    return 600; // Default 10 minutes
}
