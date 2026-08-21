// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_settings_manager.h"

#include "ui_toast_manager.h"

#include "config.h"
#include "display_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "platform_capabilities.h"
#include "platform_info.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "theme_loader.h"
#include "theme_manager.h"
#include "timezone_env.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL_system.h>
#include <jni.h>

/// Push the "keep navbar visible" preference (issue #908) into HelixActivity.
/// Java side disables immersive mode + auto-hide + swipe-reveal while true.
static void android_set_navbar_always_visible(bool enabled) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;

    // Cached global ref owned by helix_activity_class() — never released here.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return;

    jmethodID method = env->GetStaticMethodID(cls, "setNavBarAlwaysVisible", "(Z)V");
    if (!method) {
        env->ExceptionClear();
        return;
    }

    env->CallStaticVoidMethod(cls, method, static_cast<jboolean>(enabled));
}
#endif // __ANDROID__

using namespace helix;

// Display dim option values (seconds) - time before screen dims to lower brightness
// Index: 0=Never, 1=30sec, 2=1min, 3=2min, 4=5min
static const int DIM_OPTIONS[] = {0, 30, 60, 120, 300, 600};
static const int DIM_OPTIONS_COUNT = sizeof(DIM_OPTIONS) / sizeof(DIM_OPTIONS[0]);

// Display sleep option values (seconds) - time before screen fully sleeps
// Index: 0=Never, 1=1min, 2=5min, 3=10min, 4=30min
static const int SLEEP_OPTIONS[] = {0, 60, 300, 600, 1200, 1800};
static const int SLEEP_OPTIONS_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);

// Timezone options: curated list of IANA timezones with friendly display names
struct TimezoneEntry {
    const char* display_name;
    const char* iana_id;
};

static const TimezoneEntry TIMEZONE_ENTRIES[] = {
    {"UTC (+0:00)", "UTC"},
    {"Hawaii (-10:00)", "Pacific/Honolulu"},
    {"Alaska (-9:00)", "America/Anchorage"},
    {"Pacific (-8:00)", "America/Los_Angeles"},
    {"Mountain (-7:00)", "America/Denver"},
    {"Central (-6:00)", "America/Chicago"},
    {"Eastern (-5:00)", "America/New_York"},
    {"Atlantic (-4:00)", "America/Halifax"},
    {"Newfoundland (-3:30)", "America/St_Johns"},
    {"Sao Paulo (-3:00)", "America/Sao_Paulo"},
    {"Cape Verde (-1:00)", "Atlantic/Cape_Verde"},
    {"London (+0:00)", "Europe/London"},
    {"Central Europe (+1:00)", "Europe/Berlin"},
    {"Eastern Europe (+2:00)", "Europe/Bucharest"},
    {"Moscow (+3:00)", "Europe/Moscow"},
    {"Iran (+3:30)", "Asia/Tehran"},
    {"Gulf (+4:00)", "Asia/Dubai"},
    {"Afghanistan (+4:30)", "Asia/Kabul"},
    {"Pakistan (+5:00)", "Asia/Karachi"},
    {"India (+5:30)", "Asia/Kolkata"},
    {"Nepal (+5:45)", "Asia/Kathmandu"},
    {"Bangladesh (+6:00)", "Asia/Dhaka"},
    {"Myanmar (+6:30)", "Asia/Yangon"},
    {"Indochina (+7:00)", "Asia/Bangkok"},
    {"China/Singapore (+8:00)", "Asia/Shanghai"},
    {"Hong Kong (+8:00)", "Asia/Hong_Kong"},
    {"Japan/Korea (+9:00)", "Asia/Tokyo"},
    {"Australia Western (+8:00)", "Australia/Perth"},
    {"Australia Central (+9:30)", "Australia/Adelaide"},
    {"Australia Eastern (+10:00)", "Australia/Sydney"},
    {"New Caledonia (+11:00)", "Pacific/Noumea"},
    {"New Zealand (+12:00)", "Pacific/Auckland"},
};
static const int TIMEZONE_COUNT = sizeof(TIMEZONE_ENTRIES) / sizeof(TIMEZONE_ENTRIES[0]);

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

    // Display dim (default: 600 seconds = 10 minutes)
    // Validate against allowed options to catch corrupt config values
    int dim_sec = config->get<int>("/display/dim_sec", 600);
    dim_sec = validate_timeout_option(dim_sec, DIM_OPTIONS, 600, "dim_sec");
    UI_MANAGED_SUBJECT_INT(display_dim_subject_, dim_sec, "settings_display_dim", subjects_);

    // Sync validated dim timeout to DisplayManager (it reads config directly at init,
    // so we need to push the corrected value if validation changed it)
    if (auto* dm = DisplayManager::instance()) {
        dm->set_dim_timeout(dim_sec);
    }

    // Display sleep (default: 1200 seconds = 20 minutes)
    // Validate against allowed options to catch corrupt config values
    int sleep_sec = config->get<int>("/display/sleep_sec", 1200);
    sleep_sec = validate_timeout_option(sleep_sec, SLEEP_OPTIONS, 1200, "sleep_sec");

    UI_MANAGED_SUBJECT_INT(display_sleep_subject_, sleep_sec, "settings_display_sleep", subjects_);

    // Brightness: Read from config (DisplayManager handles hardware)
    int brightness = config->get<int>("/brightness", 80);
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

    // Has dimming control subject (for brightness slider visibility)
    bool has_dimming = false;
    if (auto* dm = DisplayManager::instance()) {
        has_dimming = dm->has_dimming_control();
    }
    UI_MANAGED_SUBJECT_INT(has_dimming_subject_, has_dimming ? 1 : 0, "settings_has_dimming",
                           subjects_);

    // NOTE: the sleep >= dim cross-validation is deferred until after the
    // screensaver_type subject is initialized below — on no-backlight devices an
    // enabled screensaver requires the coupling too (#1049), and
    // should_couple_sleep_to_dim() reads that subject.

    // Sleep while printing (default: true = allow sleep during prints)
    bool sleep_while_printing = config->get<bool>("/display/sleep_while_printing", true);
    UI_MANAGED_SUBJECT_INT(sleep_while_printing_subject_, sleep_while_printing ? 1 : 0,
                           "settings_sleep_while_printing", subjects_);

    // Animations enabled. Default from platform tier, but software-rotated
    // displays (fbdev + rotation) can't animate smoothly, so the default is
    // forced off there (#986). An explicit user setting always wins.
    bool software_rotated = false;
    if (auto* dm = DisplayManager::instance()) {
        software_rotated = dm->is_software_rotated();
    }
    bool anim_default =
        animations_default(PlatformCapabilities::detect().supports_animations, software_rotated);
    bool animations = config->exists("/display/animations_enabled")
                          ? config->get<bool>("/display/animations_enabled", anim_default)
                          : anim_default;
    UI_MANAGED_SUBJECT_INT(animations_enabled_subject_, animations ? 1 : 0,
                           "settings_animations_enabled", subjects_);

    // System keyboard preference (default: off — use built-in LVGL keyboard)
    bool sys_kb = config->get<bool>("/display/use_system_keyboard", false);
    UI_MANAGED_SUBJECT_INT(use_system_keyboard_subject_, sys_kb ? 1 : 0,
                           "settings_use_system_keyboard", subjects_);

    // Page-scroll buttons. Desktop/embedded-Linux default: off (opt-in). ESP32
    // default: on — finger-drag scrolling is too slow on that panel, so the
    // buttons are the usable path. An explicit user setting always wins.
#if defined(ESP_PLATFORM)
    constexpr bool page_scroll_default = true;
#else
    constexpr bool page_scroll_default = false;
#endif
    bool page_scroll = config->exists("/display/page_scroll_buttons")
                           ? config->get<bool>("/display/page_scroll_buttons", page_scroll_default)
                           : page_scroll_default;
    UI_MANAGED_SUBJECT_INT(page_scroll_buttons_subject_, page_scroll ? 1 : 0,
                           "settings_page_scroll_buttons", subjects_);

    // Keep navbar onscreen preference (Android only, issue #908, default: off)
    bool keep_navbar = config->get<bool>("/display/keep_navbar_visible", false);
    UI_MANAGED_SUBJECT_INT(keep_navbar_visible_subject_, keep_navbar ? 1 : 0,
                           "settings_keep_navbar_visible", subjects_);
#ifdef __ANDROID__
    android_set_navbar_always_visible(keep_navbar);
#endif

    // Platform flag for XML conditional visibility (ephemeral, not persisted)
    int is_android = helix::is_android_platform() ? 1 : 0;
    UI_MANAGED_SUBJECT_INT(is_android_subject_, is_android, "settings_is_android", subjects_);

    // Screen rotation availability for XML row visibility (ephemeral). The
    // rotation VALUE needs no subject: it is applied once at display init and
    // the dropdown is seeded from config when the overlay activates.
    UI_MANAGED_SUBJECT_INT(rotation_available_subject_, rotation_setting_available() ? 1 : 0,
                           "settings_rotation_available", subjects_);

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

    // Timezone (default: "UTC")
    std::string tz = config->get<std::string>("/display/timezone", "UTC");
    int tz_index = 0; // Default to UTC
    bool tz_found = false;
    for (int i = 0; i < TIMEZONE_COUNT; ++i) {
        if (tz == TIMEZONE_ENTRIES[i].iana_id) {
            tz_index = i;
            tz_found = true;
            break;
        }
    }
    if (!tz_found && tz != "UTC") {
        spdlog::warn(
            "[DisplaySettingsManager] Unknown timezone '{}' in config, falling back to UTC", tz);
    }
    UI_MANAGED_SUBJECT_INT(timezone_subject_, tz_index, "settings_timezone", subjects_);

    // Apply timezone immediately. On devices without /usr/share/zoneinfo/ (notably
    // Elegoo Centauri Carbon / OpenCentauri COSMOS), this points TZDIR at the
    // bundled assets/zoneinfo/ so the zone resolves instead of silently falling
    // back to UTC.
    helix::timezone_env::apply(tz.c_str());
    spdlog::info("[DisplaySettingsManager] Timezone set to '{}' (index {})", tz, tz_index);

#ifdef HELIX_ENABLE_SCREENSAVER
    // Screensaver type — tier-aware default via supports_animations.
    // Devices that can run smooth animations (STANDARD tier on Pi 4/5, desktop)
    // default to Flying Toasters (1). BASIC (Pi 3B-class) and EMBEDDED (AD5M,
    // AD5X) default to OFF (0) because animated screensavers starve Klipper's
    // CPU budget and cause print failures.
    const int screensaver_default = PlatformCapabilities::detect().supports_animations ? 1 : 0;

    int screensaver_type = screensaver_default;
    if (config->exists("/display/screensaver_type")) {
        screensaver_type = config->get<int>("/display/screensaver_type", screensaver_default);
    } else if (config->exists("/display/screensaver_enabled")) {
        // Legacy migration: true → tier default, false → Off.
        // The v14→v15 migration will later flip Flying Toasters to Off on low tiers anyway,
        // but honouring the tier default here avoids writing a value we're about to overwrite.
        bool old_enabled = config->get<bool>("/display/screensaver_enabled", true);
        screensaver_type = old_enabled ? screensaver_default : 0;
        config->set<int>("/display/screensaver_type", screensaver_type);
        config->save();
        spdlog::info(
            "[DisplaySettingsManager] Migrated screensaver_enabled={} → screensaver_type={}",
            old_enabled, screensaver_type);
    }
    screensaver_type = std::clamp(screensaver_type, 0, 3);
    UI_MANAGED_SUBJECT_INT(screensaver_type_subject_, screensaver_type, "settings_screensaver_type",
                           subjects_);
#endif

    // Cross-validate: sleep must be >= dim when the device dims OR a screensaver
    // provides the dim-stage visual (#1049). This runs AFTER screensaver_type is
    // initialized so should_couple_sleep_to_dim() sees it. Critical for the field
    // config in bundle 3VDWMLYQ (no backlight, screensaver on, persisted
    // sleep=60 < dim=600) — it self-corrects on next launch instead of starving
    // the screensaver. Config can also be inconsistent from manual edits.
    if (should_couple_sleep_to_dim() && dim_sec > 0 && sleep_sec > 0 && sleep_sec < dim_sec) {
        spdlog::warn("[DisplaySettingsManager] Config inconsistency: sleep {}s < dim {}s, "
                     "clamping sleep to dim",
                     sleep_sec, dim_sec);
        sleep_sec = dim_sec;
        lv_subject_set_int(&display_sleep_subject_, sleep_sec);
        config->set<int>("/display/sleep_sec", sleep_sec);
        config->save();
    }

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
    // Death signal BEFORE the subjects go away: deinit frees every observer
    // node on them, so outside ObserverGuards must learn they are gone or their
    // next reset() calls lv_observer_remove() on freed memory. Replaced, not
    // cleared — an empty token reads as "dead" and would suppress removal for
    // observers registered after this teardown.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    subjects_lifetime_ = std::make_shared<bool>(true);

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

    lv_subject_set_int(&dark_mode_subject_, enabled ? 1 : 0);

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

    lv_subject_set_int(&display_dim_subject_, seconds);

    Config* config = Config::get_instance();
    config->set<int>("/display/dim_sec", seconds);
    config->save();

    // Notify DisplayManager to reload dim setting
    if (auto* dm = DisplayManager::instance()) {
        dm->set_dim_timeout(seconds);
    }

    // If dim is now > sleep, bump sleep up to match (unless sleep is disabled).
    // Enforce when the device dims OR a screensaver provides the dim-stage visual
    // (#1049) — same rationale as the sleep setter.
    int sleep_sec = get_display_sleep_sec();
    if (should_couple_sleep_to_dim() && seconds > 0 && sleep_sec > 0 && sleep_sec < seconds) {
        spdlog::info("[DisplaySettingsManager] Bumping sleep {}s up to match dim {}s", sleep_sec,
                     seconds);
        lv_subject_set_int(&display_sleep_subject_, seconds);
        config->set<int>("/display/sleep_sec", seconds);
        config->save();
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Sleep adjusted to match dim timeout"), 2000);
    }

    spdlog::debug("[DisplaySettingsManager] Display dim set to {}s", seconds);
}

int DisplaySettingsManager::get_display_sleep_sec() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&display_sleep_subject_));
}

void DisplaySettingsManager::set_display_sleep_sec(int seconds) {
    spdlog::info("[DisplaySettingsManager] set_display_sleep_sec({})", seconds);

    // Ensure sleep timeout >= dim timeout (unless sleep is disabled with 0).
    // It's nonsensical to power off before the dim/screensaver stage. Enforce
    // when the hardware dims OR a screensaver provides the dim-stage visual on a
    // no-backlight device (#1049). On binary-backlight devices with no
    // screensaver (e.g. AD5M/AD5X) the dim UI is hidden and dim_sec retains a
    // default that should NOT block short sleep times — hence the gate.
    if (seconds > 0 && should_couple_sleep_to_dim()) {
        int dim_sec = get_display_dim_sec();
        if (dim_sec > 0 && seconds < dim_sec) {
            spdlog::info("[DisplaySettingsManager] Clamping sleep {}s to dim {}s", seconds,
                         dim_sec);
            seconds = dim_sec;
            ToastManager::instance().show(ToastSeverity::INFO,
                                          lv_tr("Sleep adjusted to match dim timeout"), 2000);
        }
    }

    lv_subject_set_int(&display_sleep_subject_, seconds);

    Config* config = Config::get_instance();
    config->set<int>("/display/sleep_sec", seconds);
    config->save();
    spdlog::debug("[DisplaySettingsManager] Display sleep set to {}s", seconds);
}

int DisplaySettingsManager::get_brightness() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&brightness_subject_));
}

int DisplaySettingsManager::preview_brightness(int percent) {
    // Clamp to valid range (10-100, minimum 10% to prevent black screen)
    int clamped = std::clamp(percent, 10, 100);

    lv_subject_set_int(&brightness_subject_, clamped);

    if (auto* dm = DisplayManager::instance()) {
        dm->set_backlight_brightness(clamped);
    }

    // Deliberately no Config::save() — see the header. Logged at debug because a
    // drag emits one of these per tick.
    spdlog::debug("[DisplaySettingsManager] preview_brightness({})", clamped);
    return clamped;
}

void DisplaySettingsManager::set_brightness(int percent) {
    int clamped = preview_brightness(percent);
    spdlog::info("[DisplaySettingsManager] set_brightness({})", clamped);

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

bool DisplaySettingsManager::has_dimming_control() const {
    if (auto* dm = DisplayManager::instance()) {
        return dm->has_dimming_control();
    }
    return false;
}

bool DisplaySettingsManager::screensaver_enabled() const {
#ifdef HELIX_ENABLE_SCREENSAVER
    return get_screensaver_type() != 0;
#else
    return false;
#endif
}

bool DisplaySettingsManager::should_couple_sleep_to_dim() const {
    // Sleep (power-off) must never precede the Dim stage. The Dim stage is a
    // backlight dim on dimming-capable devices, or — on no-backlight fbdev/DRM
    // devices — an enabled screensaver. Couple in either case (#1049).
    return has_dimming_control() || screensaver_enabled();
}

bool DisplaySettingsManager::get_sleep_while_printing() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&sleep_while_printing_subject_)) != 0;
}

void DisplaySettingsManager::set_sleep_while_printing(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_sleep_while_printing({})", enabled);

    lv_subject_set_int(&sleep_while_printing_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/display/sleep_while_printing", enabled);
    config->save();
}

// =============================================================================
// SCREEN ROTATION
// =============================================================================

int DisplaySettingsManager::rotation_degrees_to_index(int degrees) {
    switch (degrees) {
    case 90:
        return 1;
    case 180:
        return 2;
    case 270:
        return 3;
    default:
        return 0; // 0 and every unrecognized value
    }
}

int DisplaySettingsManager::index_to_rotation_degrees(int index) {
    switch (index) {
    case 1:
        return 90;
    case 2:
        return 180;
    case 3:
        return 270;
    default:
        return 0; // index 0 and out-of-range
    }
}

bool DisplaySettingsManager::rotation_setting_available() {
#ifdef HELIX_DISPLAY_SDL
    return std::getenv("HELIX_SHOW_ROTATION_SETTING") != nullptr;
#else
    return true;
#endif
}

int DisplaySettingsManager::get_display_rotation() const {
    int degrees = Config::get_instance()->get<int>("/display/rotate", 0);
    if (degrees != 90 && degrees != 180 && degrees != 270) {
        // A hand-edited or corrupt settings.json can hold any integer.
        // DisplayManager only applies 90/180/270, so anything else reads as 0.
        return 0;
    }
    return degrees;
}

bool DisplaySettingsManager::set_display_rotation(int degrees) {
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        spdlog::warn("[DisplaySettingsManager] set_display_rotation({}) rejected - "
                     "must be 0, 90, 180 or 270",
                     degrees);
        return false;
    }

    bool changed = get_display_rotation() != degrees;

    Config* config = Config::get_instance();
    config->set<int>("/display/rotate", degrees);
    // Pin the first-boot probe gate in Application::run_rotation_probe_and_layout():
    // an explicit choice must survive a later auto-detect or interactive probe.
    config->set<bool>("/display/rotation_probed", true);
    config->save();

    spdlog::info("[DisplaySettingsManager] set_display_rotation({}) saved (restart required)",
                 degrees);
    return changed;
}

// =============================================================================
// UI PREFERENCES
// =============================================================================

bool DisplaySettingsManager::get_animations_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&animations_enabled_subject_)) != 0;
}

void DisplaySettingsManager::set_animations_enabled(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_animations_enabled({})", enabled);

    lv_subject_set_int(&animations_enabled_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/display/animations_enabled", enabled);
    config->save();
}

bool DisplaySettingsManager::get_use_system_keyboard() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&use_system_keyboard_subject_)) != 0;
}

void DisplaySettingsManager::set_use_system_keyboard(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_use_system_keyboard({})", enabled);

    lv_subject_set_int(&use_system_keyboard_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/display/use_system_keyboard", enabled);
    config->save();
}

bool DisplaySettingsManager::get_page_scroll_buttons() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&page_scroll_buttons_subject_)) != 0;
}

void DisplaySettingsManager::set_page_scroll_buttons(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_page_scroll_buttons({})", enabled);

    lv_subject_set_int(&page_scroll_buttons_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/display/page_scroll_buttons", enabled);
    config->save();
}

bool DisplaySettingsManager::get_keep_navbar_visible() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&keep_navbar_visible_subject_)) != 0;
}

void DisplaySettingsManager::set_keep_navbar_visible(bool enabled) {
    spdlog::info("[DisplaySettingsManager] set_keep_navbar_visible({})", enabled);

    lv_subject_set_int(&keep_navbar_visible_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/display/keep_navbar_visible", enabled);
    config->save();

#ifdef __ANDROID__
    android_set_navbar_always_visible(enabled);
#endif
}

int DisplaySettingsManager::get_bed_mesh_render_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&bed_mesh_render_mode_subject_));
}

void DisplaySettingsManager::set_bed_mesh_render_mode(int mode) {
    // Clamp to valid range (0=Auto, 1=3D, 2=2D)
    int clamped = std::clamp(mode, 0, 2);
    spdlog::info("[DisplaySettingsManager] set_bed_mesh_render_mode({})", clamped);

    lv_subject_set_int(&bed_mesh_render_mode_subject_, clamped);

    Config* config = Config::get_instance();
    config->set<int>("/display/bed_mesh_render_mode", clamped);
    config->save();

    spdlog::debug("[DisplaySettingsManager] Bed mesh render mode set to {} ({})", clamped,
                  clamped == 0 ? "Auto" : (clamped == 1 ? "3D" : "2D"));
}

int DisplaySettingsManager::get_gcode_render_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&gcode_render_mode_subject_));
}

void DisplaySettingsManager::set_gcode_render_mode(int mode) {
    // Clamp to valid range (0=Auto, 1=3D, 2=2D, 3=Thumbnail Only)
    int clamped = std::clamp(mode, 0, 3);
    spdlog::info("[DisplaySettingsManager] set_gcode_render_mode({})", clamped);

    lv_subject_set_int(&gcode_render_mode_subject_, clamped);

    Config* config = Config::get_instance();
    config->set<int>("/display/gcode_render_mode", clamped);
    // An explicit user render-mode pick re-enables the GPU path: clear the
    // persistent crash-loop block (issues #966 / #1084 / #1085) so the user can
    // retry 3D even after a prior driver crash promoted /display/gpu_3d_blocked.
    config->set<bool>("/display/gpu_3d_blocked", false);
    // Mirror for the 2D backdrop-blur block: a deliberate render-mode change is a
    // clear signal the user wants GPU features retried, so clear the blur block too.
    config->set<bool>("/display/gpu_blur_blocked", false);
    config->save();

    static const char* MODE_NAMES[] = {"Auto", "3D", "2D", "Thumbnail Only"};
    spdlog::debug("[DisplaySettingsManager] G-code render mode set to {} ({})", clamped,
                  MODE_NAMES[clamped]);
}

TimeFormat DisplaySettingsManager::get_time_format() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&time_format_subject_));
    return static_cast<TimeFormat>(std::clamp(val, 0, 1));
}

void DisplaySettingsManager::set_time_format(TimeFormat format) {
    int val = static_cast<int>(format);
    spdlog::info("[DisplaySettingsManager] set_time_format({})", val == 0 ? "12H" : "24H");

    lv_subject_set_int(&time_format_subject_, val);

    Config* config = Config::get_instance();
    config->set<int>("/display/time_format", val);
    config->save();
}

// =============================================================================
// TIMEZONE
// =============================================================================

std::string DisplaySettingsManager::get_timezone() const {
    int index = lv_subject_get_int(const_cast<lv_subject_t*>(&timezone_subject_));
    index = std::clamp(index, 0, TIMEZONE_COUNT - 1);
    return TIMEZONE_ENTRIES[index].iana_id;
}

void DisplaySettingsManager::set_timezone(const std::string& iana_id) {
    int index = 0;
    bool found = false;
    for (int i = 0; i < TIMEZONE_COUNT; ++i) {
        if (iana_id == TIMEZONE_ENTRIES[i].iana_id) {
            index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        spdlog::warn("[DisplaySettingsManager] Unknown timezone '{}', falling back to UTC",
                     iana_id);
    }
    set_timezone_by_index(index);
}

void DisplaySettingsManager::set_timezone_by_index(int index) {
    index = std::clamp(index, 0, TIMEZONE_COUNT - 1);
    const char* iana_id = TIMEZONE_ENTRIES[index].iana_id;

    spdlog::info("[DisplaySettingsManager] set_timezone({} = '{}')", index, iana_id);

    lv_subject_set_int(&timezone_subject_, index);

    // Apply timezone to process (see init_subjects for bundled-zoneinfo note).
    helix::timezone_env::apply(iana_id);

    // Persist
    Config* config = Config::get_instance();
    config->set<std::string>("/display/timezone", iana_id);
    config->save();
}

int DisplaySettingsManager::get_timezone_index() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&timezone_subject_));
}

std::string DisplaySettingsManager::get_timezone_options() {
    std::string options;
    for (int i = 0; i < TIMEZONE_COUNT; ++i) {
        if (i > 0)
            options += '\n';
        options += TIMEZONE_ENTRIES[i].display_name;
    }
    return options;
}

// =============================================================================
// SCREENSAVER
// =============================================================================

#ifdef HELIX_ENABLE_SCREENSAVER
int DisplaySettingsManager::get_screensaver_type() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&screensaver_type_subject_));
}

void DisplaySettingsManager::set_screensaver_type(int type) {
    type = std::clamp(type, 0, 3);
    spdlog::info("[DisplaySettingsManager] set_screensaver_type({})", type);

    lv_subject_set_int(&screensaver_type_subject_, type);

    Config* config = Config::get_instance();
    config->set<int>("/display/screensaver_type", type);
    config->save();
}
#endif

// =============================================================================
// CONFIG-ONLY SETTINGS (no subjects)
// =============================================================================

std::string DisplaySettingsManager::get_printer_image() const {
    Config* config = Config::get_instance();
    if (!config)
        return "";
    return config->get<std::string>(config->df() + PRINTER_IMAGE, "");
}

void DisplaySettingsManager::set_printer_image(const std::string& id) {
    Config* config = Config::get_instance();
    if (!config)
        return;
    config->set<std::string>(config->df() + PRINTER_IMAGE, id);
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

int DisplaySettingsManager::dim_seconds_to_index(int seconds) {
    for (int i = 0; i < DIM_OPTIONS_COUNT; i++) {
        if (DIM_OPTIONS[i] == seconds) {
            return i;
        }
    }
    // Default to "10 minutes" if not found
    return 5;
}

int DisplaySettingsManager::index_to_dim_seconds(int index) {
    if (index >= 0 && index < DIM_OPTIONS_COUNT) {
        return DIM_OPTIONS[index];
    }
    return 600; // Default 10 minutes
}

// =============================================================================
// DISPLAY SLEEP OPTIONS
// =============================================================================

int DisplaySettingsManager::sleep_seconds_to_index(int seconds) {
    for (int i = 0; i < SLEEP_OPTIONS_COUNT; i++) {
        if (SLEEP_OPTIONS[i] == seconds) {
            return i;
        }
    }
    // Default to "20 minutes" if not found
    return 4;
}

int DisplaySettingsManager::index_to_sleep_seconds(int index) {
    if (index >= 0 && index < SLEEP_OPTIONS_COUNT) {
        return SLEEP_OPTIONS[index];
    }
    return 1200; // Default 20 minutes
}
