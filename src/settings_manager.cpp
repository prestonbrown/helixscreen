// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_manager.h"

#include "config.h"
#include "moonraker_client.h"
#include "spdlog/spdlog.h"
#include "ui_theme.h"

#include <algorithm>

// Display sleep option values (seconds)
// Index: 0=Never, 1=1min, 2=5min, 3=10min, 4=30min
static const int SLEEP_OPTIONS[] = {0, 60, 300, 600, 1800};
static const int SLEEP_OPTIONS_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);
static const char* SLEEP_OPTIONS_TEXT = "Never\n1 minute\n5 minutes\n10 minutes\n30 minutes";

SettingsManager& SettingsManager::instance() {
    static SettingsManager instance;
    return instance;
}

SettingsManager::SettingsManager() {
    spdlog::trace("[SettingsManager] Constructor");
}

void SettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[SettingsManager] Subjects already initialized, skipping");
        return;
    }

    spdlog::info("[SettingsManager] Initializing subjects");

    // Get initial values from Config
    Config* config = Config::get_instance();

    // Dark mode (default: true = dark)
    bool dark_mode = config->get<bool>("/dark_mode", true);
    lv_subject_init_int(&dark_mode_subject_, dark_mode ? 1 : 0);

    // Display sleep (default: 600 seconds = 10 minutes)
    int sleep_sec = config->get<int>("/display_sleep_sec", 600);
    lv_subject_init_int(&display_sleep_subject_, sleep_sec);

    // Brightness (default: 50%, range 10-100)
    int brightness = config->get<int>("/brightness", 50);
    brightness = std::max(10, std::min(100, brightness));
    lv_subject_init_int(&brightness_subject_, brightness);

    // LED state (ephemeral, not persisted - start as off)
    lv_subject_init_int(&led_enabled_subject_, 0);

    // Sounds (default: true)
    bool sounds = config->get<bool>("/sounds_enabled", true);
    lv_subject_init_int(&sounds_enabled_subject_, sounds ? 1 : 0);

    // Completion alert (default: true)
    bool completion = config->get<bool>("/completion_alert", true);
    lv_subject_init_int(&completion_alert_subject_, completion ? 1 : 0);

    // Register subjects with LVGL XML system for data binding
    lv_xml_register_subject(nullptr, "settings_dark_mode", &dark_mode_subject_);
    lv_xml_register_subject(nullptr, "settings_display_sleep", &display_sleep_subject_);
    lv_xml_register_subject(nullptr, "settings_brightness", &brightness_subject_);
    lv_xml_register_subject(nullptr, "settings_led_enabled", &led_enabled_subject_);
    lv_xml_register_subject(nullptr, "settings_sounds_enabled", &sounds_enabled_subject_);
    lv_xml_register_subject(nullptr, "settings_completion_alert", &completion_alert_subject_);

    subjects_initialized_ = true;
    spdlog::info("[SettingsManager] Subjects initialized: dark_mode={}, sleep={}s, sounds={}, "
                 "completion_alert={}",
                 dark_mode, sleep_sec, sounds, completion);
}

void SettingsManager::set_moonraker_client(MoonrakerClient* client) {
    moonraker_client_ = client;
    spdlog::debug("[SettingsManager] Moonraker client set: {}", client ? "connected" : "nullptr");
}

// =============================================================================
// APPEARANCE SETTINGS
// =============================================================================

bool SettingsManager::get_dark_mode() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&dark_mode_subject_)) != 0;
}

void SettingsManager::set_dark_mode(bool enabled) {
    spdlog::info("[SettingsManager] set_dark_mode({})", enabled);

    // 1. Update subject (UI reacts immediately via binding)
    lv_subject_set_int(&dark_mode_subject_, enabled ? 1 : 0);

    // 2. Persist to config (theme change requires restart to take effect)
    Config* config = Config::get_instance();
    config->set<bool>("/dark_mode", enabled);
    config->save();

    spdlog::debug("[SettingsManager] Dark mode {} saved (restart required)", enabled ? "enabled" : "disabled");
}

int SettingsManager::get_display_sleep_sec() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&display_sleep_subject_));
}

void SettingsManager::set_display_sleep_sec(int seconds) {
    spdlog::info("[SettingsManager] set_display_sleep_sec({})", seconds);

    // 1. Update subject
    lv_subject_set_int(&display_sleep_subject_, seconds);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/display_sleep_sec", seconds);
    config->save();

    // Note: Actual display sleep is handled by the display driver reading this value
    spdlog::debug("[SettingsManager] Display sleep set to {}s", seconds);
}

int SettingsManager::get_brightness() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&brightness_subject_));
}

void SettingsManager::set_brightness(int percent) {
    // Clamp to valid range (10-100, minimum 10% to prevent black screen)
    int clamped = std::max(10, std::min(100, percent));
    spdlog::info("[SettingsManager] set_brightness({})", clamped);

    // 1. Update subject
    lv_subject_set_int(&brightness_subject_, clamped);

    // 2. Persist
    Config* config = Config::get_instance();
    config->set<int>("/brightness", clamped);
    config->save();

    // Note: Actual brightness is applied by the display driver reading this value
    spdlog::debug("[SettingsManager] Brightness set to {}%", clamped);
}

// =============================================================================
// PRINTER SETTINGS
// =============================================================================

bool SettingsManager::get_led_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&led_enabled_subject_)) != 0;
}

void SettingsManager::set_led_enabled(bool enabled) {
    spdlog::info("[SettingsManager] set_led_enabled({})", enabled);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&led_enabled_subject_, enabled ? 1 : 0);

    // 2. Send command to printer (if connected)
    send_led_command(enabled);

    // Note: LED state is NOT persisted - it's ephemeral
}

void SettingsManager::send_led_command(bool enabled) {
    if (!moonraker_client_) {
        spdlog::warn("[SettingsManager] Cannot send LED command - no Moonraker client");
        return;
    }

    // Use common LED pin name - this should be configurable in the future
    // Common names: caselight, chamber_light, led, status_led
    std::string gcode = enabled ? "SET_PIN PIN=caselight VALUE=1" : "SET_PIN PIN=caselight VALUE=0";

    // gcode_script returns request_id (>0) on success, -1 on failure
    int result = moonraker_client_->gcode_script(gcode);
    if (result > 0) {
        spdlog::debug("[SettingsManager] LED {} command sent (request_id={})", enabled ? "on" : "off",
                      result);
    } else {
        spdlog::warn("[SettingsManager] Failed to send LED command - printer may not have caselight "
                     "pin or not connected");
    }
}

// =============================================================================
// NOTIFICATION SETTINGS
// =============================================================================

bool SettingsManager::get_sounds_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&sounds_enabled_subject_)) != 0;
}

void SettingsManager::set_sounds_enabled(bool enabled) {
    spdlog::info("[SettingsManager] set_sounds_enabled({})", enabled);

    lv_subject_set_int(&sounds_enabled_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/sounds_enabled", enabled);
    config->save();

    // Note: Actual sound playback is a placeholder - hardware TBD
}

bool SettingsManager::get_completion_alert() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&completion_alert_subject_)) != 0;
}

void SettingsManager::set_completion_alert(bool enabled) {
    spdlog::info("[SettingsManager] set_completion_alert({})", enabled);

    lv_subject_set_int(&completion_alert_subject_, enabled ? 1 : 0);

    Config* config = Config::get_instance();
    config->set<bool>("/completion_alert", enabled);
    config->save();
}

// =============================================================================
// DISPLAY SLEEP OPTIONS
// =============================================================================

const char* SettingsManager::get_display_sleep_options() {
    return SLEEP_OPTIONS_TEXT;
}

int SettingsManager::sleep_seconds_to_index(int seconds) {
    for (int i = 0; i < SLEEP_OPTIONS_COUNT; i++) {
        if (SLEEP_OPTIONS[i] == seconds) {
            return i;
        }
    }
    // Default to "10 minutes" if not found
    return 3;
}

int SettingsManager::index_to_sleep_seconds(int index) {
    if (index >= 0 && index < SLEEP_OPTIONS_COUNT) {
        return SLEEP_OPTIONS[index];
    }
    return 600; // Default 10 minutes
}
