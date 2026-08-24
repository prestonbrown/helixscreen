// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "display_settings_manager.h"
#include "i_moonraker_client.h"
#include "input_settings_manager.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "material_settings_manager.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "system/telemetry_manager.h"
#include "system_settings_manager.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cmath>

using namespace helix;

// Aftermarket toolhead styles shown in dropdown (Auto + user overrides only)
// Native styles (DEFAULT, CREALITY_K1, CREALITY_K2) are auto-detected and not shown.
static const char* TOOLHEAD_STYLE_OPTIONS_TEXT = "Auto\nStealthburner\nA4T\nAntHead\nJabberWocky";

// In test mode, show all styles for debugging
static const char* TOOLHEAD_STYLE_OPTIONS_TEXT_DEBUG =
    "Auto\nDefault\nA4T\nAntHead\nJabberWocky\nStealthburner\nCreality K1\nCreality K2";

// Map dropdown index → ToolheadStyle enum value (production dropdown)
static constexpr helix::ToolheadStyle DROPDOWN_TO_STYLE[] = {
    helix::ToolheadStyle::AUTO,          // 0: Auto
    helix::ToolheadStyle::STEALTHBURNER, // 1: Stealthburner
    helix::ToolheadStyle::A4T,           // 2: A4T
    helix::ToolheadStyle::ANTHEAD,       // 3: AntHead
    helix::ToolheadStyle::JABBERWOCKY,   // 4: JabberWocky
};
static constexpr int DROPDOWN_COUNT =
    static_cast<int>(sizeof(DROPDOWN_TO_STYLE) / sizeof(DROPDOWN_TO_STYLE[0]));

// Debug dropdown: indices map directly to enum values (0=Auto, 1=Default, ...)
static constexpr helix::ToolheadStyle DROPDOWN_TO_STYLE_DEBUG[] = {
    helix::ToolheadStyle::AUTO,          // 0
    helix::ToolheadStyle::DEFAULT,       // 1
    helix::ToolheadStyle::A4T,           // 2
    helix::ToolheadStyle::ANTHEAD,       // 3
    helix::ToolheadStyle::JABBERWOCKY,   // 4
    helix::ToolheadStyle::STEALTHBURNER, // 5
    helix::ToolheadStyle::CREALITY_K1,   // 6
    helix::ToolheadStyle::CREALITY_K2,   // 7
};
static constexpr int DROPDOWN_DEBUG_COUNT =
    static_cast<int>(sizeof(DROPDOWN_TO_STYLE_DEBUG) / sizeof(DROPDOWN_TO_STYLE_DEBUG[0]));

// Convert ToolheadStyle enum to dropdown index
static int style_to_dropdown_index(helix::ToolheadStyle style) {
    auto* rc = get_runtime_config();
    bool debug = rc && rc->test_mode;
    int count = debug ? DROPDOWN_DEBUG_COUNT : DROPDOWN_COUNT;
    const auto* table = debug ? DROPDOWN_TO_STYLE_DEBUG : DROPDOWN_TO_STYLE;
    for (int i = 0; i < count; i++) {
        if (table[i] == style)
            return i;
    }
    return 0; // Unknown styles map to Auto
}

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

    // QIDI Box eject distance magnitude (default: 878 mm, range 100-2000).
    // Stored positive; negated when assembled into the FORCE_MOVE gcode.
    int qidi_eject_distance = config->get<int>(config->df() + "ams/qidi_eject_distance", 878);
    qidi_eject_distance = std::clamp(qidi_eject_distance, 100, 2000);
    UI_MANAGED_SUBJECT_INT(qidi_eject_distance_subject_, qidi_eject_distance,
                           "settings_qidi_eject_distance", subjects_);

    // QIDI Box eject velocity (default: 100 mm/s, range 10-300)
    int qidi_eject_velocity = config->get<int>(config->df() + "ams/qidi_eject_velocity", 100);
    qidi_eject_velocity = std::clamp(qidi_eject_velocity, 10, 300);
    UI_MANAGED_SUBJECT_INT(qidi_eject_velocity_subject_, qidi_eject_velocity,
                           "settings_qidi_eject_velocity", subjects_);

    // Toolhead style (default: 0 = Auto). Per-printer: the AUTO case already
    // resolves from this printer's type, so the manual override has to follow
    // the same printer or two machines share one toolhead rendering.
    int toolhead_style = config->get<int>(config->df() + "appearance/toolhead_style", 0);
    toolhead_style = std::clamp(toolhead_style, 0, 7);
    UI_MANAGED_SUBJECT_INT(toolhead_style_subject_, toolhead_style, "settings_toolhead_style",
                           subjects_);

    // Printer switcher navbar icon visibility (default: true = shown)
    bool show_printer_switcher = config->get<bool>("/printers/show_printer_switcher", false);
    UI_MANAGED_SUBJECT_INT(show_printer_switcher_subject_, show_printer_switcher ? 1 : 0,
                           "show_printer_switcher", subjects_);

    // Widget labels on icon-only home screen widgets (default: off)
    bool show_widget_labels = config->get<bool>("/appearance/show_widget_labels", false);
    UI_MANAGED_SUBJECT_INT(show_widget_labels_subject_, show_widget_labels ? 1 : 0,
                           "show_widget_labels", subjects_);

    // Auto color map for filament mapping (default: off — positional assignment)
    bool auto_color_map = config->get<bool>(config->df() + "filament/auto_color_map", false);
    UI_MANAGED_SUBJECT_INT(auto_color_map_subject_, auto_color_map ? 1 : 0, "auto_color_map",
                           subjects_);

    // AFC unload-after-print behavior (default: off — AFC leaves filament loaded
    // unless the user's end-of-print macros retract it). Per-printer setting.
    bool afc_unload_after_print =
        config->get<bool>(config->df() + "ams/afc_unload_after_print", false);
    UI_MANAGED_SUBJECT_INT(afc_unload_after_print_subject_, afc_unload_after_print ? 1 : 0,
                           "afc_unload_after_print", subjects_);

    // Always show the bypass spool on the Multi-Filament panel, even with bypass
    // disengaged (default: off). AFC exposes a virtual bypass whether or not the
    // user has one wired, so the node was drawn permanently — and painted with
    // the loaded lane's filament, which read as "there is a spool on bypass" on
    // machines that have none (#1229). Per-printer setting.
    bool ams_always_show_bypass_spool =
        config->get<bool>(config->df() + "ams/always_show_bypass_spool", false);
    UI_MANAGED_SUBJECT_INT(ams_always_show_bypass_spool_subject_,
                           ams_always_show_bypass_spool ? 1 : 0, "ams_always_show_bypass_spool",
                           subjects_);

    // Keep Spoolman spool info on a slot the firmware reports as ejected
    // (default: on — retention is the designed behavior; the eject rule only
    // arms on backends whose firmware reports spool ids). Per-printer setting.
    bool ams_keep_spool_info =
        config->get<bool>(config->df() + "ams/keep_spool_info_on_eject", true);
    UI_MANAGED_SUBJECT_INT(ams_keep_spool_info_on_eject_subject_, ams_keep_spool_info ? 1 : 0,
                           "ams_keep_spool_info_on_eject", subjects_);

    // Show the bypass controls even when the firmware reports no bypass (default:
    // off). Happy Hare's [mmu_machine] has_bypass defaults to 0 for mmu_vendor
    // "Other" — what a Qidi Box under Happy Hare reports — so machines that can
    // feed filament straight to the extruder still advertise none. Per-printer.
    bool ams_force_bypass_controls =
        config->get<bool>(config->df() + "ams/force_bypass_controls", false);
    UI_MANAGED_SUBJECT_INT(ams_force_bypass_controls_subject_, ams_force_bypass_controls ? 1 : 0,
                           "ams_force_bypass_controls", subjects_);

    // Post-filament-operation nozzle cooldown (default: on). Filament systems that
    // run their own cooldown (AFC) want ours out of the way. Per-printer setting.
    bool filament_auto_cooldown = config->get<bool>(config->df() + "filament/auto_cooldown", true);
    UI_MANAGED_SUBJECT_INT(filament_auto_cooldown_subject_, filament_auto_cooldown ? 1 : 0,
                           "filament_auto_cooldown", subjects_);

    // Console filters (defaults: both on — keeps the gcode console clean by default)
    bool filter_temps = config->get<bool>("/console/filter_temps", true);
    UI_MANAGED_SUBJECT_INT(console_filter_temps_subject_, filter_temps ? 1 : 0,
                           "console_filter_temps", subjects_);
    bool filter_firmware_noise = config->get<bool>("/console/filter_firmware_noise", true);
    UI_MANAGED_SUBJECT_INT(console_filter_firmware_noise_subject_, filter_firmware_noise ? 1 : 0,
                           "console_filter_firmware_noise", subjects_);

    // Spaghetti detection master toggle (default: true — enabled out of the box)
    bool detection_enabled = config->get<bool>("/detection/enabled", true);
    UI_MANAGED_SUBJECT_INT(detection_enabled_subject_, detection_enabled ? 1 : 0,
                           "detection_enabled", subjects_);

    // Per-source policy for Snapmaker U1 built-in detector (default: 2 =
    // DeferToSource). Per-printer: the U1's detector only exists on the U1, so
    // the policy belongs to that machine and is inert everywhere else.
    int detection_policy_u1 = config->get<int>(config->df() + "detection/policy_u1", 2);
    detection_policy_u1 = std::clamp(detection_policy_u1, 0, 2);
    UI_MANAGED_SUBJECT_INT(detection_policy_u1_subject_, detection_policy_u1, "detection_policy_u1",
                           subjects_);

    // Chamber assignment (default: "auto" = use name heuristics).
    // Legacy paths (printer/chamber_{sensor,heater}) moved to the canonical flat paths
    // by config migration v11→v12.
    chamber_heater_assignment_ =
        config->get<std::string>(config->df() + wizard::CHAMBER_HEATER, "auto");
    chamber_sensor_assignment_ =
        config->get<std::string>(config->df() + wizard::CHAMBER_SENSOR, "auto");

    // Load scanner device selection. Global: the scanner is plugged into the
    // host running HelixScreen, not into any one printer.
    scanner_device_id_ = config->get<std::string>("/scanner/usb_vendor_product", "");
    scanner_device_name_ = config->get<std::string>("/scanner/usb_device_name", "");
    if (!scanner_device_id_.empty()) {
        spdlog::info("[SettingsManager] Loaded scanner device: {} ({})", scanner_device_name_,
                     scanner_device_id_);
    }

    scanner_bt_address_ = config->get<std::string>("/scanner/bt_address", "");
    if (!scanner_bt_address_.empty()) {
        spdlog::info("[SettingsManager] Loaded scanner BT address: {}", scanner_bt_address_);
    }

    // Scanner keymap layout — default "qwerty" (US). Valid: qwerty|qwertz|azerty.
    scanner_keymap_ = config->get<std::string>("/scanner/keymap", "qwerty");
    if (scanner_keymap_ != "qwerty" && scanner_keymap_ != "qwertz" && scanner_keymap_ != "azerty") {
        spdlog::warn("[SettingsManager] Invalid scanner keymap '{}' — defaulting to qwerty",
                     scanner_keymap_);
        scanner_keymap_ = "qwerty";
    }
    spdlog::info("[SettingsManager] Loaded scanner keymap: {}", scanner_keymap_);

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

void SettingsManager::set_moonraker_client(IMoonrakerClient* client) {
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

    auto old_val = std::to_string(lv_subject_get_int(&led_enabled_subject_));

    // 1. Delegate to LedController for actual hardware control
    helix::led::LedController::instance().light_set(enabled);

    // 2. Update subject (UI reacts)
    lv_subject_set_int(&led_enabled_subject_, enabled ? 1 : 0);

    TelemetryManager::instance().notify_setting_changed("led_enabled", old_val,
                                                        std::to_string(enabled ? 1 : 0));

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

    auto old_val = std::to_string(lv_subject_get_int(&z_movement_style_subject_));

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&z_movement_style_subject_, val);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "z_movement_style", val);
    config->save();

    TelemetryManager::instance().notify_setting_changed("z_movement_style", old_val,
                                                        std::to_string(val));

    // 3. Apply override to printer state
    get_printer_state().apply_effective_bed_moves();
}

// =============================================================================
// TOOLHEAD STYLE
// =============================================================================

ToolheadStyle SettingsManager::get_toolhead_style() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&toolhead_style_subject_));
    return static_cast<ToolheadStyle>(std::clamp(val, 0, 7));
}

ToolheadStyle SettingsManager::get_effective_toolhead_style() const {
    auto style = get_toolhead_style();
    if (style != ToolheadStyle::AUTO) {
        return style;
    }

    // The printer database's native toolhead_style is authoritative. Map the DB
    // string straight to the enum so every printer that declares a style is
    // covered by one lookup (creality_k1/k2 live in the DB).
    Config* config = Config::get_instance();
    if (config) {
        std::string printer_type =
            config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
        if (!printer_type.empty()) {
            std::string db_style = PrinterDetector::get_toolhead_style(printer_type);
            if (db_style == "creality_k1")
                return ToolheadStyle::CREALITY_K1;
            if (db_style == "creality_k2")
                return ToolheadStyle::CREALITY_K2;
        }
    }

    // Fall back to heuristic detection only for printers the DB doesn't cover.
    // PFA/Anthead printers carry no toolhead_style field in the database.
    if (PrinterDetector::is_pfa_printer()) {
        return ToolheadStyle::ANTHEAD;
    }
    // CFS (Creality Filament System) is only on K2 series printers — a live
    // backend signal, not derivable from the database.
    auto* backend = AmsState::instance().get_backend();
    if (backend && backend->get_type() == AmsType::CFS) {
        return ToolheadStyle::CREALITY_K2;
    }
    return ToolheadStyle::DEFAULT;
}

void SettingsManager::set_toolhead_style(ToolheadStyle style) {
    int val = static_cast<int>(style);
    val = std::clamp(val, 0, 7);
    spdlog::info("[SettingsManager] set_toolhead_style({})", val);
    auto old_val = std::to_string(lv_subject_get_int(&toolhead_style_subject_));
    lv_subject_set_int(&toolhead_style_subject_, val);
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "appearance/toolhead_style", val);
    config->save();
    TelemetryManager::instance().notify_setting_changed("toolhead_style", old_val,
                                                        std::to_string(val));
}

const char* SettingsManager::get_toolhead_style_options() {
    auto* rc = get_runtime_config();
    if (rc && rc->test_mode) {
        return TOOLHEAD_STYLE_OPTIONS_TEXT_DEBUG;
    }
    return TOOLHEAD_STYLE_OPTIONS_TEXT;
}

int SettingsManager::toolhead_style_to_dropdown_index(ToolheadStyle style) {
    return style_to_dropdown_index(style);
}

ToolheadStyle SettingsManager::dropdown_index_to_toolhead_style(int index) {
    auto* rc = get_runtime_config();
    bool debug = rc && rc->test_mode;
    int count = debug ? DROPDOWN_DEBUG_COUNT : DROPDOWN_COUNT;
    const auto* table = debug ? DROPDOWN_TO_STYLE_DEBUG : DROPDOWN_TO_STYLE;
    if (index < 0 || index >= count)
        return ToolheadStyle::AUTO;
    return table[index];
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

    auto old_val = std::to_string(lv_subject_get_int(&extrude_speed_subject_));

    // 1. Update subject (UI reacts)
    lv_subject_set_int(&extrude_speed_subject_, mm_per_sec);

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "filament/extrude_speed", mm_per_sec);
    config->save();

    TelemetryManager::instance().notify_setting_changed("extrude_speed", old_val,
                                                        std::to_string(mm_per_sec));
}

int SettingsManager::get_qidi_eject_distance() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&qidi_eject_distance_subject_));
}

void SettingsManager::set_qidi_eject_distance(int mm) {
    mm = std::clamp(mm, 100, 2000);
    spdlog::info("[SettingsManager] set_qidi_eject_distance({} mm)", mm);

    auto old_val = std::to_string(lv_subject_get_int(&qidi_eject_distance_subject_));

    lv_subject_set_int(&qidi_eject_distance_subject_, mm);

    Config* config = Config::get_instance();
    config->set<int>(config->df() + "ams/qidi_eject_distance", mm);
    config->save();

    TelemetryManager::instance().notify_setting_changed("qidi_eject_distance", old_val,
                                                        std::to_string(mm));
}

int SettingsManager::get_qidi_eject_velocity() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&qidi_eject_velocity_subject_));
}

void SettingsManager::set_qidi_eject_velocity(int mm_per_sec) {
    mm_per_sec = std::clamp(mm_per_sec, 10, 300);
    spdlog::info("[SettingsManager] set_qidi_eject_velocity({} mm/s)", mm_per_sec);

    auto old_val = std::to_string(lv_subject_get_int(&qidi_eject_velocity_subject_));

    lv_subject_set_int(&qidi_eject_velocity_subject_, mm_per_sec);

    Config* config = Config::get_instance();
    config->set<int>(config->df() + "ams/qidi_eject_velocity", mm_per_sec);
    config->save();

    TelemetryManager::instance().notify_setting_changed("qidi_eject_velocity", old_val,
                                                        std::to_string(mm_per_sec));
}

// ============================================================================
// Printer Switcher Visibility
// ============================================================================

bool SettingsManager::get_show_printer_switcher() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&show_printer_switcher_subject_)) != 0;
}

void SettingsManager::set_show_printer_switcher(bool show) {
    spdlog::info("[SettingsManager] set_show_printer_switcher({})", show);
    lv_subject_set_int(&show_printer_switcher_subject_, show ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/printers/show_printer_switcher", show);
    config->save();
}

// ============================================================================
// Widget Labels
// ============================================================================

bool SettingsManager::get_show_widget_labels() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&show_widget_labels_subject_)) != 0;
}

void SettingsManager::set_show_widget_labels(bool show) {
    spdlog::info("[SettingsManager] set_show_widget_labels({})", show);
    lv_subject_set_int(&show_widget_labels_subject_, show ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/appearance/show_widget_labels", show);
    config->save();
}

// ============================================================================
// Auto Color Map
// ============================================================================

bool SettingsManager::get_auto_color_map() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&auto_color_map_subject_)) != 0;
}

void SettingsManager::set_auto_color_map(bool enabled) {
    spdlog::info("[SettingsManager] set_auto_color_map({})", enabled);
    lv_subject_set_int(&auto_color_map_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "filament/auto_color_map", enabled);
    config->save();
}

bool SettingsManager::get_afc_unload_after_print() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&afc_unload_after_print_subject_)) != 0;
}

void SettingsManager::set_afc_unload_after_print(bool enabled) {
    spdlog::info("[SettingsManager] set_afc_unload_after_print({})", enabled);
    lv_subject_set_int(&afc_unload_after_print_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "ams/afc_unload_after_print", enabled);
    config->save();
}

bool SettingsManager::get_ams_always_show_bypass_spool() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&ams_always_show_bypass_spool_subject_)) !=
           0;
}

void SettingsManager::set_ams_always_show_bypass_spool(bool enabled) {
    spdlog::info("[SettingsManager] set_ams_always_show_bypass_spool({})", enabled);
    lv_subject_set_int(&ams_always_show_bypass_spool_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "ams/always_show_bypass_spool", enabled);
    config->save();
}

bool SettingsManager::get_ams_keep_spool_info_on_eject() const {
    lv_subject_t* subject = const_cast<lv_subject_t*>(&ams_keep_spool_info_on_eject_subject_);
    // Before init_subjects() (app startup; plain unit tests without a fixture)
    // the subject carries no value yet — the documented default (retain)
    // applies. Otherwise an uninitialized read would report "off" and the
    // backends' eject rule would clear overrides nobody asked to clear.
    if (subject->type != LV_SUBJECT_TYPE_INT) {
        return true;
    }
    return lv_subject_get_int(subject) != 0;
}

void SettingsManager::set_ams_keep_spool_info_on_eject(bool enabled) {
    spdlog::info("[SettingsManager] set_ams_keep_spool_info_on_eject({})", enabled);
    lv_subject_set_int(&ams_keep_spool_info_on_eject_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "ams/keep_spool_info_on_eject", enabled);
    config->save();
}

bool SettingsManager::get_ams_force_bypass_controls() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&ams_force_bypass_controls_subject_)) != 0;
}

void SettingsManager::set_ams_force_bypass_controls(bool enabled) {
    spdlog::info("[SettingsManager] set_ams_force_bypass_controls({})", enabled);
    lv_subject_set_int(&ams_force_bypass_controls_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "ams/force_bypass_controls", enabled);
    config->save();
}

bool SettingsManager::get_bypass_declared() const {
    Config* config = Config::get_instance();
    return config->get<bool>(config->df() + "ams/bypass_declared", false);
}

void SettingsManager::set_bypass_declared(bool declared) {
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "ams/bypass_declared", declared);
    config->save();
}

bool SettingsManager::get_filament_auto_cooldown() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&filament_auto_cooldown_subject_)) != 0;
}

void SettingsManager::set_filament_auto_cooldown(bool enabled) {
    spdlog::info("[SettingsManager] set_filament_auto_cooldown({})", enabled);
    lv_subject_set_int(&filament_auto_cooldown_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>(config->df() + "filament/auto_cooldown", enabled);
    config->save();
}

// ============================================================================
// Console Filters
// ============================================================================

bool SettingsManager::get_console_filter_temps() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&console_filter_temps_subject_)) != 0;
}

void SettingsManager::set_console_filter_temps(bool enabled) {
    spdlog::info("[SettingsManager] set_console_filter_temps({})", enabled);
    lv_subject_set_int(&console_filter_temps_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/console/filter_temps", enabled);
    config->save();
}

bool SettingsManager::get_console_filter_firmware_noise() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&console_filter_firmware_noise_subject_)) !=
           0;
}

void SettingsManager::set_console_filter_firmware_noise(bool enabled) {
    spdlog::info("[SettingsManager] set_console_filter_firmware_noise({})", enabled);
    lv_subject_set_int(&console_filter_firmware_noise_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/console/filter_firmware_noise", enabled);
    config->save();
}

namespace {

/// Config pointer holding one layer of a user filter list.
///
/// Global lives at the root and is in force whichever printer is selected;
/// Printer lives under df() so a pattern that only makes sense on one machine
/// stays there. Both layers are read on every console rebuild.
std::string console_filter_path(const char* leaf, ConsoleFilterScope scope) {
    Config* config = Config::get_instance();
    if (scope == ConsoleFilterScope::Printer) {
        return config->df() + "console/" + leaf;
    }
    return std::string("/console/") + leaf;
}

/// Read one layer, treating a malformed list as absent rather than propagating.
std::vector<std::string> read_console_filter_layer(const char* leaf, ConsoleFilterScope scope) {
    const std::string path = console_filter_path(leaf, scope);
    try {
        return Config::get_instance()->get<std::vector<std::string>>(path,
                                                                     std::vector<std::string>{});
    } catch (const std::exception& e) {
        spdlog::warn("[SettingsManager] {} malformed, ignoring: {}", path, e.what());
        return {};
    }
}

/// Global entries first, then the active printer's, with exact duplicates
/// collapsed so a pattern present in both layers is applied once.
std::vector<std::string> merged_console_filter(const char* leaf) {
    std::vector<std::string> merged = read_console_filter_layer(leaf, ConsoleFilterScope::Global);
    for (const auto& entry : read_console_filter_layer(leaf, ConsoleFilterScope::Printer)) {
        if (std::find(merged.begin(), merged.end(), entry) == merged.end()) {
            merged.push_back(entry);
        }
    }
    return merged;
}

void write_console_filter_layer(const char* leaf, ConsoleFilterScope scope,
                                const std::vector<std::string>& patterns) {
    Config* config = Config::get_instance();
    config->set<std::vector<std::string>>(console_filter_path(leaf, scope), patterns);
    config->save();
}

constexpr const char* FILTER_USER_ADD_LEAF = "filter_user_add";
constexpr const char* FILTER_USER_REMOVE_LEAF = "filter_user_remove";

} // namespace

std::vector<std::string> SettingsManager::get_console_filter_user_add() const {
    return merged_console_filter(FILTER_USER_ADD_LEAF);
}

std::vector<std::string> SettingsManager::get_console_filter_user_remove() const {
    return merged_console_filter(FILTER_USER_REMOVE_LEAF);
}

std::vector<std::string>
SettingsManager::get_console_filter_user_add(ConsoleFilterScope scope) const {
    return read_console_filter_layer(FILTER_USER_ADD_LEAF, scope);
}

std::vector<std::string>
SettingsManager::get_console_filter_user_remove(ConsoleFilterScope scope) const {
    return read_console_filter_layer(FILTER_USER_REMOVE_LEAF, scope);
}

void SettingsManager::set_console_filter_user_add(const std::vector<std::string>& patterns,
                                                  ConsoleFilterScope scope) {
    write_console_filter_layer(FILTER_USER_ADD_LEAF, scope, patterns);
}

void SettingsManager::set_console_filter_user_remove(const std::vector<std::string>& patterns,
                                                     ConsoleFilterScope scope) {
    write_console_filter_layer(FILTER_USER_REMOVE_LEAF, scope, patterns);
}

// ============================================================================
// Macro Panel (per-printer hidden macro set)
// ============================================================================

std::vector<std::string> SettingsManager::get_hidden_macros() const {
    Config* config = Config::get_instance();
    try {
        return config->get<std::vector<std::string>>(config->df() + "macros/hidden",
                                                     std::vector<std::string>{});
    } catch (const std::exception& e) {
        spdlog::warn("[SettingsManager] {} malformed, ignoring: {}", config->df() + "macros/hidden",
                     e.what());
        return {};
    }
}

void SettingsManager::set_hidden_macros(const std::vector<std::string>& names) {
    Config* config = Config::get_instance();
    config->set<std::vector<std::string>>(config->df() + "macros/hidden", names);
    config->save();
}

bool SettingsManager::hidden_macros_key_exists() const {
    Config* config = Config::get_instance();
    return config->exists(config->df() + "macros/hidden");
}

// ============================================================================
// Spaghetti Detection Settings
// ============================================================================

bool SettingsManager::get_detection_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&detection_enabled_subject_)) != 0;
}

void SettingsManager::set_detection_enabled(bool enabled) {
    spdlog::info("[SettingsManager] set_detection_enabled({})", enabled);
    auto old_val = std::to_string(lv_subject_get_int(&detection_enabled_subject_));
    lv_subject_set_int(&detection_enabled_subject_, enabled ? 1 : 0);
    Config* config = Config::get_instance();
    config->set<bool>("/detection/enabled", enabled);
    config->save();
    TelemetryManager::instance().notify_setting_changed("detection_enabled", old_val,
                                                        std::to_string(enabled ? 1 : 0));
}

int SettingsManager::get_detection_policy_u1() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&detection_policy_u1_subject_));
}

void SettingsManager::set_detection_policy_u1(int policy) {
    policy = std::clamp(policy, 0, 2);
    spdlog::info("[SettingsManager] set_detection_policy_u1({})", policy);
    auto old_val = std::to_string(lv_subject_get_int(&detection_policy_u1_subject_));
    lv_subject_set_int(&detection_policy_u1_subject_, policy);
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "detection/policy_u1", policy);
    config->save();
    TelemetryManager::instance().notify_setting_changed("detection_policy_u1", old_val,
                                                        std::to_string(policy));
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
    // The external spool has no lane_data record, so this get/set pair is its
    // ONLY persistence — the catalog product identity has to round-trip here or
    // the editor reopens on the alphabetically-first variant of the material.
    info.catalog_id =
        config->get<std::string>(config->df() + "filament/external_spool/catalog_id", "");
    info.product_name =
        config->get<std::string>(config->df() + "filament/external_spool/product_name", "");
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
    config->set<std::string>(config->df() + "filament/external_spool/catalog_id", info.catalog_id);
    config->set<std::string>(config->df() + "filament/external_spool/product_name",
                             info.product_name);
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

// ============================================================================
// Chamber Assignment
// ============================================================================

std::string SettingsManager::get_chamber_heater_assignment() const {
    return chamber_heater_assignment_;
}

void SettingsManager::set_chamber_heater_assignment(const std::string& value) {
    chamber_heater_assignment_ = value;
    spdlog::info("[SettingsManager] set_chamber_heater_assignment({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::CHAMBER_HEATER, value);
    config->save();
}

std::string SettingsManager::get_chamber_sensor_assignment() const {
    return chamber_sensor_assignment_;
}

void SettingsManager::set_chamber_sensor_assignment(const std::string& value) {
    chamber_sensor_assignment_ = value;
    spdlog::info("[SettingsManager] set_chamber_sensor_assignment({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::CHAMBER_SENSOR, value);
    config->save();
}

// ============================================================================
// Filament Settings
// ============================================================================

void SettingsManager::clear_external_spool_info() {
    Config* config = Config::get_instance();
    // Probe first: get_json() would vivify "filament": null on every call and
    // the unconditional save() below would persist it (#1129).
    const json* existing = config->try_get_json(config->df() + "filament");
    if (existing != nullptr && existing->is_object() && existing->contains("external_spool")) {
        config->get_json(config->df() + "filament").erase("external_spool");
    }
    config->save();
}

// ============================================================================
// Barcode Scanner Settings
// ============================================================================

std::string SettingsManager::get_scanner_device_id() const {
    return scanner_device_id_;
}

void SettingsManager::set_scanner_device_id(const std::string& vendor_product) {
    spdlog::info("[SettingsManager] set_scanner_device_id({})", vendor_product);
    scanner_device_id_ = vendor_product;
    Config* config = Config::get_instance();
    config->set<std::string>("/scanner/usb_vendor_product", vendor_product);
    config->save();
}

std::string SettingsManager::get_scanner_device_name() const {
    return scanner_device_name_;
}

void SettingsManager::set_scanner_device_name(const std::string& name) {
    spdlog::info("[SettingsManager] set_scanner_device_name({})", name);
    scanner_device_name_ = name;
    Config* config = Config::get_instance();
    config->set<std::string>("/scanner/usb_device_name", name);
    config->save();
}

std::string SettingsManager::get_scanner_bt_address() const {
    return scanner_bt_address_;
}

void SettingsManager::set_scanner_bt_address(const std::string& address) {
    spdlog::info("[SettingsManager] set_scanner_bt_address({})", address);
    scanner_bt_address_ = address;
    Config* config = Config::get_instance();
    config->set<std::string>("/scanner/bt_address", address);
    config->save();
}

std::string SettingsManager::get_scanner_keymap() const {
    return scanner_keymap_;
}

void SettingsManager::set_scanner_keymap(const std::string& keymap) {
    if (keymap != "qwerty" && keymap != "qwertz" && keymap != "azerty") {
        spdlog::warn("[SettingsManager] set_scanner_keymap: rejecting invalid value '{}'", keymap);
        return;
    }
    spdlog::info("[SettingsManager] set_scanner_keymap({})", keymap);
    scanner_keymap_ = keymap;
    Config* config = Config::get_instance();
    config->set<std::string>("/scanner/keymap", keymap);
    config->save();
}
