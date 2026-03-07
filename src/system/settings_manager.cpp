// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_manager.h"

#include "app_globals.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "display_settings_manager.h"
#include "input_settings_manager.h"
#include "led/led_controller.h"
#include "material_settings_manager.h"
#include "moonraker_client.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "system_settings_manager.h"

#include <algorithm>
#include <cmath>

using namespace helix;

// Z movement style options (Auto=0, Bed Moves=1, Nozzle Moves=2)
static const char* Z_MOVEMENT_STYLE_OPTIONS_TEXT = "Auto\nBed Moves\nNozzle Moves";
static const char* TOOLHEAD_STYLE_OPTIONS_TEXT = "Auto\nDefault\nStealthburner\nArmored Turtle";

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

    spdlog::debug("[SettingsManager] Initializing subjects");

    Config* config = Config::get_instance();

    // Delegate to domain-specific managers
    DisplaySettingsManager::instance().init_subjects();
    SystemSettingsManager::instance().init_subjects();
    InputSettingsManager::instance().init_subjects();
    AudioSettingsManager::instance().init_subjects();
    SafetySettingsManager::instance().init_subjects();
    MaterialSettingsManager::instance().init();

    // LED state (ephemeral, not persisted - start as off)
    UI_MANAGED_SUBJECT_INT(led_enabled_subject_, 0, "settings_led_enabled", subjects_);

    // Z movement style (default: 0 = Auto)
    int z_movement_style = config->get<int>(config->df() + "z_movement_style", 0);
    z_movement_style = std::clamp(z_movement_style, 0, 2);
    UI_MANAGED_SUBJECT_INT(z_movement_style_subject_, z_movement_style, "settings_z_movement_style",
                           subjects_);

    // Apply Z movement override to printer state (ensures non-Auto setting takes
    // effect even if set_kinematics() hasn't run yet, e.g. on reconnect)
    if (z_movement_style != 0) {
        get_printer_state().apply_effective_bed_moves();
    }

    // Extrude/retract speed (default: 5 mm/s, range 1-50)
    int extrude_speed = config->get<int>(config->df() + "filament/extrude_speed", 5);
    extrude_speed = std::clamp(extrude_speed, 1, 50);
    UI_MANAGED_SUBJECT_INT(extrude_speed_subject_, extrude_speed, "settings_extrude_speed",
                           subjects_);

    // Toolhead style (default: 0 = Auto)
    int toolhead_style = config->get<int>("/appearance/toolhead_style", 0);
    toolhead_style = std::clamp(toolhead_style, 0, 3);
    UI_MANAGED_SUBJECT_INT(toolhead_style_subject_, toolhead_style, "settings_toolhead_style",
                           subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("SettingsManager",
                                                      [this]() { deinit_subjects(); });

    spdlog::debug("[SettingsManager] Subjects initialized");
}

void SettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[SettingsManager] Deinitializing subjects");

    // Use SubjectManager for RAII cleanup of all registered subjects
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::trace("[SettingsManager] Subjects deinitialized");
}

void SettingsManager::set_moonraker_client(MoonrakerClient* client) {
    moonraker_client_ = client;
    spdlog::debug("[SettingsManager] Moonraker client set: {}", client ? "connected" : "nullptr");
}

// =============================================================================
// PRINTER SETTINGS (LED — owned by SettingsManager)
// =============================================================================

bool SettingsManager::get_led_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&led_enabled_subject_)) != 0;
}

void SettingsManager::set_led_enabled(bool enabled) {
    spdlog::info("[SettingsManager] set_led_enabled({})", enabled);

    // 1. Delegate to LedController for actual hardware control
    helix::led::LedController::instance().light_set(enabled);

    // 2. Update subject (UI reacts)
    lv_subject_set_int(&led_enabled_subject_, enabled ? 1 : 0);

    // 3. Persist startup preference via LedController
    helix::led::LedController::instance().set_led_on_at_start(enabled);
    helix::led::LedController::instance().save_config();
}

// =============================================================================
// Z MOVEMENT STYLE
// =============================================================================

ZMovementStyle SettingsManager::get_z_movement_style() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&z_movement_style_subject_));
    return static_cast<ZMovementStyle>(std::clamp(val, 0, 2));
}

void SettingsManager::set_z_movement_style(ZMovementStyle style) {
    int val = static_cast<int>(style);
    val = std::clamp(val, 0, 2);
    spdlog::info("[SettingsManager] set_z_movement_style({})",
                 val == 0 ? "Auto" : (val == 1 ? "Bed Moves" : "Nozzle Moves"));

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&z_movement_style_subject_, val);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "z_movement_style", val);
    config->save();

    // 3. Apply override to printer state
    get_printer_state().apply_effective_bed_moves();
}

const char* SettingsManager::get_z_movement_style_options() {
    return Z_MOVEMENT_STYLE_OPTIONS_TEXT;
}

// =============================================================================
// TOOLHEAD STYLE
// =============================================================================

ToolheadStyle SettingsManager::get_toolhead_style() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&toolhead_style_subject_));
    return static_cast<ToolheadStyle>(std::clamp(val, 0, 3));
}

ToolheadStyle SettingsManager::get_effective_toolhead_style() const {
    auto style = get_toolhead_style();
    if (style != ToolheadStyle::AUTO) {
        return style;
    }
    if (PrinterDetector::is_voron_printer()) {
        return ToolheadStyle::STEALTHBURNER;
    }
    return ToolheadStyle::DEFAULT;
}

void SettingsManager::set_toolhead_style(ToolheadStyle style) {
    int val = static_cast<int>(style);
    val = std::clamp(val, 0, 3);
    spdlog::info("[SettingsManager] set_toolhead_style({})", val);
    lv_subject_set_int(&toolhead_style_subject_, val);
    Config* config = Config::get_instance();
    config->set<int>("/appearance/toolhead_style", val);
    config->save();
}

const char* SettingsManager::get_toolhead_style_options() {
    return TOOLHEAD_STYLE_OPTIONS_TEXT;
}

// ============================================================================
// Extrude/Retract Speed
// ============================================================================

int SettingsManager::get_extrude_speed() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&extrude_speed_subject_));
}

void SettingsManager::set_extrude_speed(int mm_per_sec) {
    mm_per_sec = std::clamp(mm_per_sec, 1, 50);
    spdlog::info("[SettingsManager] set_extrude_speed({} mm/s)", mm_per_sec);

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&extrude_speed_subject_, mm_per_sec);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "filament/extrude_speed", mm_per_sec);
    config->save();
}

// ============================================================================
// Filament Settings
// ============================================================================

std::optional<SlotInfo> SettingsManager::get_external_spool_info() const {
    Config* config = Config::get_instance();

    // Primary check: explicit assigned boolean (new format)
    bool assigned = config->get<bool>(config->df() + "filament/external_spool/assigned", false);

    // Backward compat: old configs have color_rgb but no assigned key
    if (!assigned) {
        auto color = config->get<int>(config->df() + "filament/external_spool/color_rgb", -1);
        if (color == -1) {
            return std::nullopt;
        }
        // Old format detected — treat as assigned (will be migrated on next set)
    }

    SlotInfo info;
    info.slot_index = -2; // External spool sentinel
    info.global_index = -2;
    info.color_rgb =
        static_cast<uint32_t>(config->get<int>(config->df() + "filament/external_spool/color_rgb",
                                               static_cast<int>(AMS_DEFAULT_SLOT_COLOR)));
    info.material = config->get<std::string>(config->df() + "filament/external_spool/material", "");
    info.brand = config->get<std::string>(config->df() + "filament/external_spool/brand", "");
    info.nozzle_temp_min =
        config->get<int>(config->df() + "filament/external_spool/nozzle_temp_min", 0);
    info.nozzle_temp_max =
        config->get<int>(config->df() + "filament/external_spool/nozzle_temp_max", 0);
    info.bed_temp = config->get<int>(config->df() + "filament/external_spool/bed_temp", 0);
    info.spoolman_id = config->get<int>(config->df() + "filament/external_spool/spoolman_id", 0);
    info.spool_name =
        config->get<std::string>(config->df() + "filament/external_spool/spool_name", "");
    info.remaining_weight_g =
        config->get<float>(config->df() + "filament/external_spool/remaining_weight_g", -1.0f);
    info.total_weight_g =
        config->get<float>(config->df() + "filament/external_spool/total_weight_g", -1.0f);
    info.status = SlotStatus::AVAILABLE;
    return info;
}

void SettingsManager::set_external_spool_info(const SlotInfo& info) {
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "filament/external_spool/assigned", true);
    config->set<int>(config->df() + "filament/external_spool/color_rgb",
                     static_cast<int>(info.color_rgb));
    config->set<std::string>(config->df() + "filament/external_spool/material", info.material);
    config->set<std::string>(config->df() + "filament/external_spool/brand", info.brand);
    config->set<int>(config->df() + "filament/external_spool/nozzle_temp_min",
                     info.nozzle_temp_min);
    config->set<int>(config->df() + "filament/external_spool/nozzle_temp_max",
                     info.nozzle_temp_max);
    config->set<int>(config->df() + "filament/external_spool/bed_temp", info.bed_temp);
    config->set<int>(config->df() + "filament/external_spool/spoolman_id", info.spoolman_id);
    config->set<std::string>(config->df() + "filament/external_spool/spool_name", info.spool_name);
    config->set<float>(config->df() + "filament/external_spool/remaining_weight_g",
                       info.remaining_weight_g);
    config->set<float>(config->df() + "filament/external_spool/total_weight_g",
                       info.total_weight_g);
    config->save();
}

void SettingsManager::clear_external_spool_info() {
    Config* config = Config::get_instance();
    try {
        auto& filament = config->get_json(config->df() + "filament");
        if (filament.is_object() && filament.contains("external_spool")) {
            filament.erase("external_spool");
        }
    } catch (...) {
        // /filament doesn't exist or isn't an object, nothing to clear
    }
    config->save();
}
