// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_settings_manager.h"

#include "ui_toast_manager.h"

#include "config.h"
#include "display_manager.h"
#include "display_metrics.h"
#include "helix-xml/src/xml/lv_xml_style.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "platform_capabilities.h"
#include "platform_info.h"
#include "screensaver_canvas.h"
#include "screensaver_registry.h"
#include "screensaver_setting.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "theme_loader.h"
#include "theme_manager.h"
#include "timezone_env.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

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
static const int kDimOptions[] = {0, 30, 60, 120, 300, 600};
static const int kDimOptionsCount = sizeof(kDimOptions) / sizeof(kDimOptions[0]);

// Display sleep option values (seconds) - time before screen fully sleeps
// Index: 0=Never, 1=1min, 2=5min, 3=10min, 4=20min, 5=30min, 6=60min
static const int kSleepOptions[] = {0, 60, 300, 600, 1200, 1800, 3600};
static const int kSleepOptionsCount = sizeof(kSleepOptions) / sizeof(kSleepOptions[0]);

// Timezone options: curated list of IANA timezones with friendly display names
struct TimezoneEntry {
    const char* display_name;
    const char* iana_id;
    const char* posix_tz; // the zone's TZif footer: for libcs that cannot read zoneinfo
};

// Ordered by UTC offset so the list scans west-to-east. Names are the countries
// people search for, not regional or historical groupings — #1340 was filed
// because UTC+7 was labelled "Indochina" and a user in Vietnam never found it.
//
// Same-offset zones are listed separately when their DST rules differ: picking
// America/Denver from Arizona, or Europe/Berlin from Lagos, yields the right
// time for part of the year and an hour's error for the rest.
//
// scripts/regen_zoneinfo.sh reads this table to build assets/zoneinfo/, and
// tests/shell/test_zoneinfo_bundle_gate.bats fails if the two drift. A zone
// offered here but not bundled resolves to UTC on devices without system
// tzdata — silently, with the wrong time and no error.
static const TimezoneEntry kTimezoneEntries[] = {
    {"UTC (+0:00)", "UTC", "UTC0"},
    {"Samoa (-11:00)", "Pacific/Pago_Pago", "SST11"},
    {"Hawaii (-10:00)", "Pacific/Honolulu", "HST10"},
    {"Alaska (-9:00)", "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific (-8:00)", "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Mountain (-7:00)", "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Arizona (-7:00)", "America/Phoenix", "MST7"},
    {"Central (-6:00)", "America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Mexico City (-6:00)", "America/Mexico_City", "CST6"},
    {"Saskatchewan (-6:00)", "America/Regina", "CST6"},
    {"Eastern (-5:00)", "America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Colombia/Peru (-5:00)", "America/Bogota", "<-05>5"},
    {"Atlantic (-4:00)", "America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"Venezuela (-4:00)", "America/Caracas", "<-04>4"},
    {"Chile (-4:00)", "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"Newfoundland (-3:30)", "America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Brazil - Sao Paulo (-3:00)", "America/Sao_Paulo", "<-03>3"},
    {"Argentina (-3:00)", "America/Argentina/Buenos_Aires", "<-03>3"},
    {"Fernando de Noronha (-2:00)", "America/Noronha", "<-02>2"},
    {"Azores (-1:00)", "Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Cape Verde (-1:00)", "Atlantic/Cape_Verde", "<-01>1"},
    {"Iceland (+0:00)", "Atlantic/Reykjavik", "GMT0"},
    {"London (+0:00)", "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe (+1:00)", "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"West Africa - Lagos (+1:00)", "Africa/Lagos", "WAT-1"},
    {"Eastern Europe (+2:00)", "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Egypt (+2:00)", "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"South Africa (+2:00)", "Africa/Johannesburg", "SAST-2"},
    {"Israel (+2:00)", "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Moscow (+3:00)", "Europe/Moscow", "MSK-3"},
    {"Turkey (+3:00)", "Europe/Istanbul", "<+03>-3"},
    {"Saudi Arabia (+3:00)", "Asia/Riyadh", "<+03>-3"},
    {"East Africa - Nairobi (+3:00)", "Africa/Nairobi", "EAT-3"},
    {"Iran (+3:30)", "Asia/Tehran", "<+0330>-3:30"},
    {"Gulf (+4:00)", "Asia/Dubai", "<+04>-4"},
    {"Afghanistan (+4:30)", "Asia/Kabul", "<+0430>-4:30"},
    {"Pakistan (+5:00)", "Asia/Karachi", "PKT-5"},
    {"India (+5:30)", "Asia/Kolkata", "IST-5:30"},
    {"Sri Lanka (+5:30)", "Asia/Colombo", "<+0530>-5:30"},
    {"Nepal (+5:45)", "Asia/Kathmandu", "<+0545>-5:45"},
    {"Bangladesh (+6:00)", "Asia/Dhaka", "<+06>-6"},
    {"Myanmar (+6:30)", "Asia/Yangon", "<+0630>-6:30"},
    {"Thailand (+7:00)", "Asia/Bangkok", "<+07>-7"},
    {"Vietnam (+7:00)", "Asia/Ho_Chi_Minh", "<+07>-7"},
    {"Indonesia - Jakarta (+7:00)", "Asia/Jakarta", "WIB-7"},
    {"China (+8:00)", "Asia/Shanghai", "CST-8"},
    {"Hong Kong (+8:00)", "Asia/Hong_Kong", "HKT-8"},
    {"Taiwan (+8:00)", "Asia/Taipei", "CST-8"},
    {"Singapore/Malaysia (+8:00)", "Asia/Singapore", "<+08>-8"},
    {"Philippines (+8:00)", "Asia/Manila", "PST-8"},
    {"Australia Western (+8:00)", "Australia/Perth", "AWST-8"},
    {"Japan (+9:00)", "Asia/Tokyo", "JST-9"},
    {"Korea (+9:00)", "Asia/Seoul", "KST-9"},
    {"Australia Central (+9:30)", "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Northern (+9:30)", "Australia/Darwin", "ACST-9:30"},
    {"Australia Eastern (+10:00)", "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia Queensland (+10:00)", "Australia/Brisbane", "AEST-10"},
    {"New Caledonia (+11:00)", "Pacific/Noumea", "<+11>-11"},
    {"New Zealand (+12:00)", "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Fiji (+12:00)", "Pacific/Fiji", "<+12>-12"},
    {"Tonga (+13:00)", "Pacific/Tongatapu", "<+13>-13"},
};
static const int kTimezoneCount = sizeof(kTimezoneEntries) / sizeof(kTimezoneEntries[0]);

// Dropdown row 0 is "System" where the platform has a host zone; the table follows.
static constexpr int kTimezoneRowOffset = DisplaySettingsManager::kSystemTimezoneAvailable ? 1 : 0;
static const int kTimezoneRowCount = kTimezoneCount + kTimezoneRowOffset;
static constexpr const char* kTimezoneKey = "/display/timezone";

// Table index of an offered IANA id, or -1.
static int find_timezone_entry(const std::string& iana_id) {
    for (int i = 0; i < kTimezoneCount; ++i) {
        if (iana_id == kTimezoneEntries[i].iana_id) {
            return i;
        }
    }
    return -1;
}

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

// Page-scroll buttons: opt-in on desktop/embedded Linux; on by default on ESP32,
// where finger-drag scrolling is too slow on that panel.
#if defined(ESP_PLATFORM)
static constexpr int kPageScrollDefault = 1;
#else
static constexpr int kPageScrollDefault = 0;
#endif

using settings::Scope;
// Row order is DisplaySettingsManager::Key.
static constexpr settings::PersistedSetting kDisplaySettings[] = {
    {"settings_sleep_while_printing", "/display/sleep_while_printing", Scope::Global, true, 1, 0, 1,
     "sleep_while_printing"},
    // Minimum 10% prevents a black screen; DisplayManager drives the hardware.
    {"settings_brightness", "/brightness", Scope::Global, false, 80, 10, 100, "brightness"},
    {"settings_use_system_keyboard", "/display/use_system_keyboard", Scope::Global, true, 0, 0, 1,
     "use_system_keyboard"},
    // Opt-in: an unlisted barcode scanner is a HID keyboard too.
    {"settings_hide_keyboard_with_hardware", "/display/hide_keyboard_with_hardware", Scope::Global,
     true, 0, 0, 1, "hide_keyboard_with_hardware"},
    {"settings_page_scroll_buttons", "/display/page_scroll_buttons", Scope::Global, true,
     kPageScrollDefault, 0, 1, "page_scroll_buttons"},
    {"settings_speed_flow_physical_units", "/display/speed_flow_physical_units", Scope::Global,
     true, 0, 0, 1, "speed_flow_physical_units"},
    // Print Status preview card shows the live camera instead of the preview
    {"settings_print_status_show_camera", "/display/print_status_show_camera", Scope::Global, true,
     0, 0, 1, "print_status_show_camera"},
    // Android only (#908)
    {"settings_keep_navbar_visible", "/display/keep_navbar_visible", Scope::Global, true, 0, 0, 1,
     "keep_navbar_visible"},
    // 0=Auto, 1=3D, 2=2D
    {"settings_bed_mesh_render_mode", "/display/bed_mesh_render_mode", Scope::Global, false, 0, 0,
     2, "bed_mesh_render_mode"},
    // 0=Auto, 1=3D, 2=2D, 3=Thumbnail Only
    {"settings_gcode_render_mode", "/display/gcode_render_mode", Scope::Global, false, 0, 0, 3,
     "gcode_render_mode"},
    // 0=12H, 1=24H
    {"settings_time_format", "/display/time_format", Scope::Global, false, 0, 0, 1, "time_format"},
    // The saved choice; glass shows only where the glass gate also passes.
    {"settings_glass_effects", "/display/glass_effects", Scope::Global, true, 0, 0, 1,
     "glass_effects"},
};

DisplaySettingsManager& DisplaySettingsManager::instance() {
    static HELIX_PSRAM_BSS DisplaySettingsManager instance;
    return instance;
}

settings::TableView DisplaySettingsManager::persisted_table() {
    return {kDisplaySettings, std::size(kDisplaySettings)};
}

DisplaySettingsManager::DisplaySettingsManager() : settings_(kDisplaySettings) {
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
    dim_sec = validate_timeout_option(dim_sec, kDimOptions, 600, "dim_sec");
    UI_MANAGED_SUBJECT_INT(display_dim_subject_, dim_sec, "settings_display_dim", subjects_);

    // Sync validated dim timeout to DisplayManager (it reads config directly at init,
    // so we need to push the corrected value if validation changed it)
    if (auto* dm = DisplayManager::instance()) {
        dm->set_dim_timeout(dim_sec);
    }

    // Display sleep (default: 1200 seconds = 20 minutes)
    // Validate against allowed options to catch corrupt config values
    int sleep_sec = config->get<int>("/display/sleep_sec", 1200);
    sleep_sec = validate_timeout_option(sleep_sec, kSleepOptions, 1200, "sleep_sec");

    UI_MANAGED_SUBJECT_INT(display_sleep_subject_, sleep_sec, "settings_display_sleep", subjects_);

    settings_.init(subjects_);

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

    // Drive the XML style engine's transition scale from this preference —
    // the engine has no notion of a user setting, so this is the one place
    // the two meet. lv_subject_add_observer() invokes the handler immediately
    // with the subject's current value, so the scale matches the preference
    // before any panel XML is created: a user with animations off never sees
    // one animated transition before their first toggle. The handler runs
    // inline on whichever thread writes the subject; every writer (Settings
    // panel toggles, the ctl freeze/unfreeze pair) is main-thread already,
    // matching this class's single-threaded contract above.
    transition_scale_observer_ = helix::ui::observe<int>(
        &animations_enabled_subject_, this,
        [](DisplaySettingsManager*, int enabled) {
            lv_xml_set_transition_scale(enabled ? 256 : 0);
        },
        get_subjects_lifetime(), helix::ui::Dispatch::Immediate);

    // Glass availability is ephemeral: 0 until the glass gate passes on this board this run.
    UI_MANAGED_SUBJECT_INT(glass_available_subject_, 0, "glass_available", subjects_);

    // Presence is ephemeral: it is whatever the display backend opened this run.
    UI_MANAGED_SUBJECT_INT(hardware_keyboard_present_subject_, hardware_keyboard_present_ ? 1 : 0,
                           "settings_hardware_keyboard_present", subjects_);

#ifdef __ANDROID__
    android_set_navbar_always_visible(get_keep_navbar_visible());
#endif

    // Platform flag for XML conditional visibility (ephemeral, not persisted)
    int is_android = helix::is_android_platform() ? 1 : 0;
    UI_MANAGED_SUBJECT_INT(is_android_subject_, is_android, "settings_is_android", subjects_);

    // Screen rotation availability for XML row visibility (ephemeral). The
    // rotation VALUE needs no subject: it is applied once at display init and
    // the dropdown is seeded from config when the overlay activates.
    UI_MANAGED_SUBJECT_INT(rotation_available_subject_, rotation_setting_available() ? 1 : 0,
                           "settings_rotation_available", subjects_);

    // Applied here and before logging starts (Application::init_logging); both
    // go through apply_configured_timezone(), so they cannot disagree.
    const int tz_row = apply_configured_timezone();
    UI_MANAGED_SUBJECT_INT(timezone_subject_, tz_row, "settings_timezone", subjects_);
    spdlog::info("[DisplaySettingsManager] Timezone set to '{}' (index {})",
                 get_timezone().empty() ? "system" : get_timezone(), tz_row);

#if HELIX_ENABLE_SCREENSAVER
    // Screensaver: a stored name, or a legacy type number, read leniently
    // (helix::ui::screensaver_from_setting). A fresh install, and a value naming no saver, run
    // the registry default for this build's colour depth.
    const ScreensaverType screensaver_default =
        helix::ui::default_screensaver_type(helix::ui::kSaverBuildDepth);

    ScreensaverType screensaver = screensaver_default;
    if (const json* stored = config->try_get_json(helix::ui::kScreensaverSettingPath)) {
        screensaver = helix::ui::screensaver_from_setting(stored, screensaver_default);
    } else if (config->exists("/display/screensaver_enabled")) {
        // Legacy screensaver_enabled: true reads as the default saver, false as Off.
        bool old_enabled = config->get<bool>("/display/screensaver_enabled", true);
        screensaver = old_enabled ? screensaver_default : ScreensaverType::OFF;
        config->set<std::string>(helix::ui::kScreensaverSettingPath,
                                 helix::ui::screensaver_name(screensaver));
        config->save();
        spdlog::info(
            "[DisplaySettingsManager] Migrated screensaver_enabled={} → screensaver_type={}",
            old_enabled, helix::ui::screensaver_name(screensaver));
    }
    const int screensaver_type = static_cast<int>(screensaver);
    constexpr int has_screensaver = 1;
#else
    const int screensaver_type = 0;
    constexpr int has_screensaver = 0;
#endif
    UI_MANAGED_SUBJECT_INT(screensaver_type_subject_, screensaver_type, "settings_screensaver_type",
                           subjects_);
    // Build flag for XML visibility of the screensaver picker (ephemeral)
    UI_MANAGED_SUBJECT_INT(has_screensaver_subject_, has_screensaver, "settings_has_screensaver",
                           subjects_);
    // Reads the screensaver type, so it follows that subject's init.
    UI_MANAGED_SUBJECT_INT(dim_row_kind_subject_,
                           static_cast<int>(dim_row_kind(has_dimming, screensaver_enabled())),
                           "settings_dim_row_kind", subjects_);

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
                  "dim={}s, sleep={}s, animations={}",
                  dark_mode, get_theme_name(), dim_sec, sleep_sec, animations);
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
    return config->get<std::string>("/display/theme", helix::kDefaultTheme);
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

int DisplaySettingsManager::preview_brightness(int percent) {
    // Clamp to valid range (10-100, minimum 10% to prevent black screen)
    int clamped = std::clamp(percent, 10, 100);

    lv_subject_set_int(settings_.subject(Key::Brightness), clamped);

    if (auto* dm = DisplayManager::instance()) {
        dm->set_backlight_brightness(clamped);
    }

    // Deliberately no Config::save() — see the header. Logged at debug because a
    // drag emits one of these per tick.
    spdlog::debug("[DisplaySettingsManager] preview_brightness({})", clamped);
    return clamped;
}

void DisplaySettingsManager::set_brightness(int percent) {
    settings_.set(Key::Brightness, percent);
    if (auto* dm = DisplayManager::instance()) {
        dm->set_backlight_brightness(get_brightness());
    }
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
    return get_screensaver_type() != 0;
}

bool DisplaySettingsManager::should_couple_sleep_to_dim() const {
    // Sleep (power-off) must never precede the Dim stage. The Dim stage is a
    // backlight dim on dimming-capable devices, or — on no-backlight fbdev/DRM
    // devices — an enabled screensaver. Couple in either case (#1049): exactly
    // when Display settings offers a dim-timeout row.
    return dim_row_kind(has_dimming_control(), screensaver_enabled()) != DimRowKind::Hidden;
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

void DisplaySettingsManager::set_hardware_keyboard_present(bool present) {
    spdlog::info("[DisplaySettingsManager] Hardware keyboard {}",
                 present ? "attached" : "not attached");
    hardware_keyboard_present_ = present;
    if (subjects_initialized_) {
        lv_subject_set_int(&hardware_keyboard_present_subject_, present ? 1 : 0);
    }
}

bool DisplaySettingsManager::soft_keyboard_suppressed() const {
    return subjects_initialized_ && hardware_keyboard_present_ && get_hide_keyboard_with_hardware();
}

void DisplaySettingsManager::set_glass_effects(bool enabled) {
    settings_.set(Key::GlassEffects, enabled);
}

void DisplaySettingsManager::set_glass_available(bool available) {
    if (subjects_initialized_) {
        lv_subject_set_int(&glass_available_subject_, available ? 1 : 0);
    }
}

bool DisplaySettingsManager::get_glass_available() const {
    return subjects_initialized_ &&
           lv_subject_get_int(const_cast<lv_subject_t*>(&glass_available_subject_)) != 0;
}

void DisplaySettingsManager::set_keep_navbar_visible(bool enabled) {
    settings_.set(Key::KeepNavbarVisible, enabled);

#ifdef __ANDROID__
    android_set_navbar_always_visible(enabled);
#endif
}

int DisplaySettingsManager::get_ui_scale_percent() const {
    return Config::get_instance()->get<int>("/display/ui_scale_percent",
                                            helix::DisplayMetrics::kScaleSettingAutomatic);
}

void DisplaySettingsManager::set_ui_scale_percent(int percent) {
    // Automatic, or a percentage the scale curve can actually produce. A value
    // outside the band is stored as Automatic rather than clamped, matching
    // what DisplayMetrics::scale_for_setting() does with one it reads back.
    const bool in_band = percent >= helix::DisplayMetrics::kMinScaleSettingPercent &&
                         percent <= helix::DisplayMetrics::kMaxScaleSettingPercent;
    const int stored = in_band ? percent : helix::DisplayMetrics::kScaleSettingAutomatic;

    Config* config = Config::get_instance();
    config->set<int>("/display/ui_scale_percent", stored);
    config->save();

    spdlog::info(
        "[DisplaySettingsManager] set_ui_scale_percent({}) -> {} (applies on next start)", percent,
        stored == helix::DisplayMetrics::kScaleSettingAutomatic ? std::string("automatic")
                                                                : std::to_string(stored) + "%");
}

void DisplaySettingsManager::set_gcode_render_mode(int mode) {
    // An explicit user render-mode pick re-enables the GPU paths: clear the
    // persistent crash-loop block (issues #966 / #1084 / #1085) so the user can
    // retry 3D after a driver crash promoted /display/gpu_3d_blocked, and the
    // 2D backdrop-blur block with it. set() below saves all three.
    Config* config = Config::get_instance();
    config->set<bool>("/display/gpu_3d_blocked", false);
    config->set<bool>("/display/gpu_blur_blocked", false);
    settings_.set(Key::GcodeRenderMode, mode);
}

// =============================================================================
// TIMEZONE
// =============================================================================

int DisplaySettingsManager::apply_configured_timezone() {
    Config* config = Config::get_instance();
    if (!config->exists(kTimezoneKey) && kSystemTimezoneAvailable) {
        helix::timezone_env::apply_system();
        return 0;
    }
    // On a device with no host zone, an unpicked zone is UTC (table row 0).
    const std::string tz = config->get<std::string>(kTimezoneKey, "UTC");
    int entry = find_timezone_entry(tz);
    if (entry < 0) {
        spdlog::warn(
            "[DisplaySettingsManager] Unknown timezone '{}' in config, falling back to UTC", tz);
        entry = 0;
    }
    // On devices without /usr/share/zoneinfo/ (notably Elegoo Centauri Carbon /
    // OpenCentauri COSMOS) this points TZDIR at the bundled assets/zoneinfo/ so
    // the zone resolves instead of silently falling back to UTC.
    helix::timezone_env::apply(kTimezoneEntries[entry].iana_id, kTimezoneEntries[entry].posix_tz);
    return entry + kTimezoneRowOffset;
}

std::string DisplaySettingsManager::get_timezone() const {
    int row = lv_subject_get_int(const_cast<lv_subject_t*>(&timezone_subject_));
    row = std::clamp(row, 0, kTimezoneRowCount - 1);
    if (row < kTimezoneRowOffset) {
        return "";
    }
    return kTimezoneEntries[row - kTimezoneRowOffset].iana_id;
}

void DisplaySettingsManager::set_timezone(const std::string& iana_id) {
    if (iana_id.empty() && kSystemTimezoneAvailable) {
        set_timezone_by_index(0);
        return;
    }
    int entry = find_timezone_entry(iana_id);
    if (entry < 0) {
        spdlog::warn("[DisplaySettingsManager] Unknown timezone '{}', falling back to UTC",
                     iana_id);
        entry = 0;
    }
    set_timezone_by_index(entry + kTimezoneRowOffset);
}

void DisplaySettingsManager::set_timezone_by_index(int index) {
    index = std::clamp(index, 0, kTimezoneRowCount - 1);
    lv_subject_set_int(&timezone_subject_, index);
    Config* config = Config::get_instance();

    if (index < kTimezoneRowOffset) {
        spdlog::info("[DisplaySettingsManager] set_timezone({} = system)", index);
        helix::timezone_env::apply_system();
        // System is the absence of a pick, so a later default change reaches it.
        config->get_json("/display").erase("timezone");
    } else {
        const TimezoneEntry& entry = kTimezoneEntries[index - kTimezoneRowOffset];
        spdlog::info("[DisplaySettingsManager] set_timezone({} = '{}')", index, entry.iana_id);
        helix::timezone_env::apply(entry.iana_id, entry.posix_tz);
        config->set<std::string>(kTimezoneKey, entry.iana_id);
    }
    config->save();
}

int DisplaySettingsManager::get_timezone_index() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&timezone_subject_));
}

std::string DisplaySettingsManager::get_timezone_options() {
    std::string options;
    if (kSystemTimezoneAvailable) {
        options += lv_tr("System");
    }
    for (int i = 0; i < kTimezoneCount; ++i) {
        if (!options.empty())
            options += '\n';
        options += kTimezoneEntries[i].display_name;
    }
    return options;
}

std::string DisplaySettingsManager::match_timezone(const std::string& iana_id, int jan_minutes_east,
                                                   int jul_minutes_east) {
    const int exact = find_timezone_entry(iana_id);
    if (exact >= 0) {
        return kTimezoneEntries[exact].iana_id;
    }
    for (int i = 0; i < kTimezoneCount; ++i) {
        int jan = 0;
        int jul = 0;
        if (helix::timezone_env::posix_offsets(kTimezoneEntries[i].posix_tz, jan, jul) &&
            jan == jan_minutes_east && jul == jul_minutes_east) {
            return kTimezoneEntries[i].iana_id;
        }
    }
    return "";
}

void DisplaySettingsManager::adopt_timezone_if_unset(const std::string& iana_id,
                                                     int jan_minutes_east, int jul_minutes_east) {
    if (Config::get_instance()->exists(kTimezoneKey)) {
        return;
    }
    const std::string match = match_timezone(iana_id, jan_minutes_east, jul_minutes_east);
    if (match.empty()) {
        spdlog::info("[DisplaySettingsManager] No offered timezone matches '{}' ({} / {} min)",
                     iana_id, jan_minutes_east, jul_minutes_east);
        return;
    }
    spdlog::info("[DisplaySettingsManager] Adopting timezone '{}' for reported '{}'", match,
                 iana_id);
    set_timezone(match);
}

// =============================================================================
// SCREENSAVER
// =============================================================================

int DisplaySettingsManager::get_screensaver_type() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&screensaver_type_subject_));
}

#if HELIX_ENABLE_SCREENSAVER
void DisplaySettingsManager::set_screensaver_type(int type) {
    type = helix::ui::clamp_screensaver_type(type);
    spdlog::info("[DisplaySettingsManager] set_screensaver_type({})", type);

    lv_subject_set_int(&screensaver_type_subject_, type);
    lv_subject_set_int(&dim_row_kind_subject_, static_cast<int>(dim_row_kind(
                                                   has_dimming_control(), screensaver_enabled())));

    Config* config = Config::get_instance();
    config->set<std::string>(helix::ui::kScreensaverSettingPath,
                             helix::ui::screensaver_name(static_cast<ScreensaverType>(type)));
    config->save();
}
#endif

// =============================================================================
// CONFIG-ONLY SETTINGS (no subjects)
// =============================================================================

std::string DisplaySettingsManager::get_printer_image() const {
    Config* config = Config::get_instance();
    return config->get<std::string>(config->df() + PRINTER_IMAGE, "");
}

void DisplaySettingsManager::set_printer_image(const std::string& id) {
    Config* config = Config::get_instance();
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

DisplaySettingsManager::DimRowKind DisplaySettingsManager::dim_row_kind(bool has_dimming,
                                                                        bool screensaver_enabled) {
    if (has_dimming) {
        return DimRowKind::ScreenDim;
    }
    return screensaver_enabled ? DimRowKind::ScreensaverAfter : DimRowKind::Hidden;
}

int DisplaySettingsManager::dim_seconds_to_index(int seconds) {
    for (int i = 0; i < kDimOptionsCount; i++) {
        if (kDimOptions[i] == seconds) {
            return i;
        }
    }
    // Default to "10 minutes" if not found
    return 5;
}

int DisplaySettingsManager::index_to_dim_seconds(int index) {
    if (index >= 0 && index < kDimOptionsCount) {
        return kDimOptions[index];
    }
    return 600; // Default 10 minutes
}

// =============================================================================
// DISPLAY SLEEP OPTIONS
// =============================================================================

int DisplaySettingsManager::sleep_seconds_to_index(int seconds) {
    for (int i = 0; i < kSleepOptionsCount; i++) {
        if (kSleepOptions[i] == seconds) {
            return i;
        }
    }
    // Default to "20 minutes" if not found
    return 4;
}

int DisplaySettingsManager::index_to_sleep_seconds(int index) {
    if (index >= 0 && index < kSleepOptionsCount) {
        return kSleepOptions[index];
    }
    return 1200; // Default 20 minutes
}
