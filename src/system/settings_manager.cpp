// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_manager.h"

#include "ui_panel_motion.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "audio_settings_manager.h"
#include "belt_tension_types.h"
#include "config.h"
#include "display_settings_manager.h"
#include "helix_psram_attr.h"
#include "i_moonraker_client.h"
#include "input_settings_manager.h"
#include "json_utils.h"
#include "lvgl.h"
#include "material_settings_manager.h"
#include "mount_feed_sequence.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "remote_screen_settings_manager.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "system/telemetry_manager.h"
#include "system_settings_manager.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace helix;

// Aftermarket toolhead styles shown in dropdown (Auto + user overrides only)
// Native styles (DEFAULT, CREALITY_K1, CREALITY_K2) are auto-detected and not shown.
// The full style list: the dropdown and the enum carry the same eight
// entries in the same order, so a style the DB auto-detects (K1/K2) or
// pins (Default) is also selectable by hand.
// Map dropdown index → ToolheadStyle enum value (production dropdown)
static constexpr helix::ToolheadStyle kDropdownToStyle[] = {
    helix::ToolheadStyle::AUTO,          // 0: Auto
    helix::ToolheadStyle::DEFAULT,       // 1: Default (Bambu-like)
    helix::ToolheadStyle::A4T,           // 2: A4T
    helix::ToolheadStyle::ANTHEAD,       // 3: AntHead
    helix::ToolheadStyle::JABBERWOCKY,   // 4: JabberWocky
    helix::ToolheadStyle::STEALTHBURNER, // 5: Stealthburner
    helix::ToolheadStyle::CREALITY_K1,   // 6: Creality K1
    helix::ToolheadStyle::CREALITY_K2,   // 7: Creality K2
};
static constexpr int kDropdownCount =
    static_cast<int>(sizeof(kDropdownToStyle) / sizeof(kDropdownToStyle[0]));

// Debug dropdown: indices map directly to enum values (0=Auto, 1=Default, ...)
static constexpr helix::ToolheadStyle kDropdownToStyleDebug[] = {
    helix::ToolheadStyle::AUTO,          // 0
    helix::ToolheadStyle::DEFAULT,       // 1
    helix::ToolheadStyle::A4T,           // 2
    helix::ToolheadStyle::ANTHEAD,       // 3
    helix::ToolheadStyle::JABBERWOCKY,   // 4
    helix::ToolheadStyle::STEALTHBURNER, // 5
    helix::ToolheadStyle::CREALITY_K1,   // 6
    helix::ToolheadStyle::CREALITY_K2,   // 7
};
static constexpr int kDropdownDebugCount =
    static_cast<int>(sizeof(kDropdownToStyleDebug) / sizeof(kDropdownToStyleDebug[0]));

// Convert ToolheadStyle enum to dropdown index
static int style_to_dropdown_index(helix::ToolheadStyle style) {
    auto* rc = get_runtime_config();
    bool debug = rc && rc->test_mode;
    int count = debug ? kDropdownDebugCount : kDropdownCount;
    const auto* table = debug ? kDropdownToStyleDebug : kDropdownToStyle;
    for (int i = 0; i < count; i++) {
        if (table[i] == style)
            return i;
    }
    return 0; // Unknown styles map to Auto
}

namespace {
/// The one place a (mode, ring) pair becomes a config key.
std::string jog_distance_key(helix::JogMode mode, bool outer) {
    const char* mode_name = "coarse";
    switch (mode) {
    case helix::JogMode::Fine:
        mode_name = "fine";
        break;
    case helix::JogMode::Coarse:
        mode_name = "coarse";
        break;
    case helix::JogMode::Turbo:
        mode_name = "turbo";
        break;
    }
    return std::string("motion/") + mode_name + (outer ? "_outer" : "_inner");
}

float jog_distance_default(helix::JogMode mode, bool outer) {
    switch (mode) {
    case helix::JogMode::Fine:
        return outer ? 1.0f : 0.1f;
    case helix::JogMode::Coarse:
        return outer ? 10.0f : 1.0f;
    case helix::JogMode::Turbo:
        return outer ? 50.0f : 10.0f;
    }
    return outer ? 10.0f : 1.0f;
}
} // namespace

using settings::Scope;
// Row order is SettingsManager::Key. PerPrinter rows follow the active printer.
static constexpr settings::PersistedSetting SETTINGS[] = {
    // 0=Auto, 1=Bed Moves, 2=Nozzle Moves
    {"settings_z_movement_style", "z_movement_style", Scope::PerPrinter, false, 0, 0, 2,
     "z_movement_style"},
    // bed_drying::EnclosureStyle: 0=Auto, 1=Enclosed, 2=Open
    {"settings_enclosure_style", "enclosure_style", Scope::PerPrinter, false, 0, 0, 2,
     "enclosure_style"},
    // mm/s
    {"settings_extrude_speed", "filament/extrude_speed", Scope::PerPrinter, false, 5, 1, 50,
     "extrude_speed"},
    // Jog feedrates in mm/min; defaults are the motion panel's shipped speeds.
    {"settings_jog_speed_xy", "motion/jog_speed_xy", Scope::PerPrinter, false, 6000, 60, 60000,
     "jog_speed_xy"},
    {"settings_jog_speed_z", "motion/jog_speed_z", Scope::PerPrinter, false, 600, 60, 60000,
     "jog_speed_z"},
    // Motion panel readout: 0=commanded, 1=actual (live) position
    {"settings_motion_show_actual_position", "motion/show_actual_position", Scope::PerPrinter, true,
     0, 0, 1, "show_actual_position"},
    // Bed tab: mm below which a move lifts Z before travelling
    {"settings_bed_map_clearance", "motion/bed_map_clearance", Scope::PerPrinter, false, 5,
     kBedMapClearanceMinMm, kBedMapClearanceMaxMm, "bed_map_clearance"},
    // MotionLayout: 0=Pad, 1=Arrows, 2=ArrowsHome, 3=AxisRows
    {"settings_motion_layout", "motion/layout", Scope::PerPrinter, false, 0, 0,
     kMotionLayoutCount - 1, "motion_layout"},
    // Lane eject distance magnitude in mm, negated when the backend assembles its
    // eject move. 0 = not chosen: the backend ejecting the lane supplies its own
    // default and range. The telemetry keys keep the analytics series' original names.
    {"settings_ams_eject_distance", "ams/eject_distance", Scope::PerPrinter, false, 0, 0, 2000,
     "qidi_eject_distance"},
    // mm/s, 0 = the backend's default
    {"settings_ams_eject_velocity", "ams/eject_velocity", Scope::PerPrinter, false, 0, 0, 300,
     "qidi_eject_velocity"},
    // Per-printer: AUTO resolves from this printer's type, so the manual
    // override follows the same printer.
    {"settings_toolhead_style", "appearance/toolhead_style", Scope::PerPrinter, false, 0, 0, 7,
     "toolhead_style"},
    {"show_printer_switcher", "/printers/show_printer_switcher", Scope::Global, true, 0, 0, 1,
     "show_printer_switcher"},
    {"show_widget_labels", "/appearance/show_widget_labels", Scope::Global, true, 0, 0, 1,
     "show_widget_labels"},
    // Off: positional filament mapping
    {"auto_color_map", "filament/auto_color_map", Scope::PerPrinter, true, 0, 0, 1,
     "auto_color_map"},
    // The telemetry key keeps the analytics series' original name.
    {"unload_after_print", "ams/unload_after_print", Scope::PerPrinter, true, 0, 0, 1,
     "afc_unload_after_print"},
    // Bit N = tool N unloads when a print ends.
    {"end_of_print_unload_tools", "ams/end_of_print_unload_tools", Scope::PerPrinter, false, 0, 0,
     255, "end_of_print_unload_tools"},
    // #1229
    {"ams_always_show_bypass_spool", "ams/always_show_bypass_spool", Scope::PerPrinter, true, 0, 0,
     1, "ams_always_show_bypass_spool"},
    // On: retention is the designed behavior
    {"ams_keep_spool_info_on_eject", "ams/keep_spool_info_on_eject", Scope::PerPrinter, true, 1, 0,
     1, "ams_keep_spool_info_on_eject"},
    {"ams_tag_links_spoolman", "ams/tag_links_spoolman", Scope::PerPrinter, true, 1, 0, 1,
     "ams_tag_links_spoolman"},
    {"ams_force_bypass_controls", "ams/force_bypass_controls", Scope::PerPrinter, true, 0, 0, 1,
     "ams_force_bypass_controls"},
    // Filament systems that run their own cooldown (AFC) want ours off.
    {"filament_auto_cooldown", "filament/auto_cooldown", Scope::PerPrinter, true, 1, 0, 1,
     "filament_auto_cooldown"},
    // #1335. Off: a popup the user did not ask for is opt-in.
    {"filament_auto_open_editor", "filament/auto_open_editor", Scope::PerPrinter, true, 0, 0, 1,
     "filament_auto_open_editor"},
    {"console_filter_temps", "/console/filter_temps", Scope::Global, true, 1, 0, 1,
     "console_filter_temps"},
    {"console_filter_firmware_noise", "/console/filter_firmware_noise", Scope::Global, true, 1, 0,
     1, "console_filter_firmware_noise"},
    {"detection_enabled", "/detection/enabled", Scope::Global, true, 1, 0, 1, "detection_enabled"},
    // Off = warn only for prints HelixScreen pauses; a firmware-paused print
    // always gets the response modal.
    {"detection_pause_on_detect", "/detection/pause_on_detect", Scope::Global, true, 1, 0, 1,
     "detection_pause_on_detect"},
    // Snapmaker U1 built-in detector: 0=Off, 1=NotifyOnly, 2=DeferToSource.
    // Per-printer: the detector only exists on the U1.
    {"detection_policy_u1", "detection/policy_u1", Scope::PerPrinter, false, 2, 0, 2,
     "detection_policy_u1"},
    // helix::zoffset::SaveMode: 0=Auto (off when each tool keeps its own Z
    // offset), 1=Show, 2=Hide. #1766
    {"settings_z_offset_save_mode", "z_offset/save_mode", Scope::PerPrinter, false, 0, 0, 2,
     "z_offset_save_mode"},
    // helix::calibration::BeltLayout: 0=not set, 1=A left, 2=A right, 3=A upper,
    // 4=A lower. Per-printer: belt routing is the machine's.
    {"settings_belt_layout", "belt_tension/layout", Scope::PerPrinter, false, 0, 0,
     helix::calibration::kBeltLayoutCount - 1, "belt_layout"},
};

SettingsManager& SettingsManager::instance() {
    static HELIX_PSRAM_BSS SettingsManager instance;
    return instance;
}

settings::TableView SettingsManager::persisted_table() {
    return {SETTINGS, std::size(SETTINGS)};
}

SettingsManager::SettingsManager() : settings_(SETTINGS) {
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
    RemoteScreenSettingsManager::instance().init_subjects();
    MaterialSettingsManager::instance().init();

    settings_.init(subjects_);

    // Apply a non-Auto Z movement override to printer state now, even if
    // set_kinematics() has not run yet (e.g. on reconnect).
    if (get_z_movement_style() != ZMovementStyle::AUTO) {
        get_printer_state().apply_effective_bed_moves();
    }

    load_jog_distances();

    // Chamber assignment (default: "auto" = use name heuristics).
    // Legacy paths (printer/chamber_{sensor,heater}) moved to the canonical flat paths
    // by config migration v11→v12.
    chamber_heater_assignment_ =
        config->get<std::string>(config->df() + wizard::CHAMBER_HEATER, "auto");
    chamber_sensor_assignment_ =
        config->get<std::string>(config->df() + wizard::CHAMBER_SENSOR, "auto");

    // Tool-changer feeder macro overrides (default: "auto" = detected default).
    feeder_open_macro_ = config->get<std::string>(config->df() + wizard::FEEDER_OPEN_MACRO, "auto");
    feeder_close_macro_ =
        config->get<std::string>(config->df() + wizard::FEEDER_CLOSE_MACRO, "auto");
    mount_feed_purge_mm_ = std::clamp(config->get<int>(config->df() + wizard::kMountFeedPurgeMm,
                                                       helix::mount_feed::kDefaultPurgeMm),
                                      0, helix::mount_feed::kMaxPurgeMm);

    // ACE bypass macro overrides (default: "auto" = detected default).
    ace_bypass_on_macro_ =
        config->get<std::string>(config->df() + wizard::kAceBypassOnMacro, "auto");
    ace_bypass_off_macro_ =
        config->get<std::string>(config->df() + wizard::kAceBypassOffMacro, "auto");

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

void SettingsManager::load_jog_distances() {
    // Jog step distances (Fine/Coarse/Turbo x inner/outer, mm). Read on every
    // jog rather than bound to a widget, so a cache is enough; the settings
    // overlay re-reads on open.
    static_assert(kJogModeCount == 3, "jog_distances_ cache is sized for three modes");
    Config* config = Config::get_instance();
    for (int m = 0; m < kJogModeCount; ++m) {
        const JogMode mode = static_cast<JogMode>(m);
        for (int outer = 0; outer < 2; ++outer) {
            float mm = config->get<float>(config->df() + jog_distance_key(mode, outer),
                                          jog_distance_default(mode, outer));
            jog_distances_[m][outer] = std::clamp(mm, 0.01f, 200.0f);
        }
    }
}

void SettingsManager::reload_from_config() {
    if (!subjects_initialized_) {
        return;
    }
    settings_.reload();
    load_jog_distances();
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
// Z MOVEMENT STYLE
// =============================================================================

void SettingsManager::set_z_movement_style(ZMovementStyle style) {
    settings_.set(Key::ZMovementStyle, static_cast<int>(style));
    get_printer_state().apply_effective_bed_moves();
}

// =============================================================================
// BED DRYING
// =============================================================================

void SettingsManager::set_enclosure_style(helix::bed_drying::EnclosureStyle style) {
    settings_.set(Key::EnclosureStyle, static_cast<int>(style));
    get_printer_state().refresh_bed_drying_capability();
}

helix::bed_drying::RunRecord SettingsManager::get_bed_drying_record() const {
    helix::bed_drying::RunRecord r;
    Config* config = Config::get_instance();
    const json j = config->get<json>(config->df() + "bed_drying", json::object());
    if (!j.is_object()) {
        return r;
    }
    r.latched = helix::json_util::safe_bool(j, "latched");
    r.start_s = helix::json_util::safe_int64(j, "start_s");
    r.end_s = helix::json_util::safe_int64(j, "end_s");
    r.bed_c = helix::json_util::safe_int(j, "bed_c");
    r.idle_restore_s = helix::json_util::safe_int(j, "idle_restore_s");
    r.appliance = helix::json_util::safe_bool(j, "appliance");
    r.chamber_c = helix::json_util::safe_int(j, "chamber_c");
    r.ended = helix::json_util::safe_bool(j, "ended");
    r.flip_notified = helix::json_util::safe_bool(j, "flip_notified");
    r.placing = helix::json_util::safe_bool(j, "placing");
    r.material = helix::json_util::safe_int(j, "material", -1);
    return r;
}

bool SettingsManager::set_bed_drying_record(const helix::bed_drying::RunRecord& r) {
    Config* config = Config::get_instance();
    config->set<json>(config->df() + "bed_drying", json{{"latched", r.latched},
                                                        {"start_s", r.start_s},
                                                        {"end_s", r.end_s},
                                                        {"bed_c", r.bed_c},
                                                        {"idle_restore_s", r.idle_restore_s},
                                                        {"appliance", r.appliance},
                                                        {"chamber_c", r.chamber_c},
                                                        {"ended", r.ended},
                                                        {"flip_notified", r.flip_notified},
                                                        {"placing", r.placing},
                                                        {"material", r.material}});
    if (!config->save()) {
        spdlog::error("[SettingsManager] Could not save the bed drying record");
        return false;
    }
    return true;
}

bool SettingsManager::clear_bed_drying_record() {
    return set_bed_drying_record(helix::bed_drying::RunRecord{});
}

// =============================================================================
// MAINTENANCE METERS
// =============================================================================

std::optional<helix::maintenance::State>
SettingsManager::get_maintenance_record(const std::string& printer_prefix) const {
    Config* config = Config::get_instance();
    const json j = config->get<json>(printer_prefix + "maintenance", json());
    if (!j.is_object()) {
        return std::nullopt;
    }
    return helix::maintenance::from_json(j);
}

bool SettingsManager::set_maintenance_record(const std::string& printer_prefix,
                                             const helix::maintenance::State& state) {
    Config* config = Config::get_instance();
    config->set<json>(printer_prefix + "maintenance", helix::maintenance::to_json(state));
    if (!config->save()) {
        spdlog::error("[SettingsManager] Could not save the maintenance meters");
        return false;
    }
    return true;
}

// =============================================================================
// TOOLHEAD STYLE
// =============================================================================

/// Maps a printer-database toolhead_style string to the enum; AUTO means "no
/// opinion".
static ToolheadStyle toolhead_style_from_name(const std::string& name) {
    if (name == "creality_k1")
        return ToolheadStyle::CREALITY_K1;
    if (name == "creality_k2")
        return ToolheadStyle::CREALITY_K2;
    if (name == "anthead")
        return ToolheadStyle::ANTHEAD;
    if (name == "default")
        return ToolheadStyle::DEFAULT;
    return ToolheadStyle::AUTO;
}

ToolheadStyle SettingsManager::get_effective_toolhead_style() const {
    auto style = get_toolhead_style();
    if (style != ToolheadStyle::AUTO) {
        return style;
    }

    // The printer database's native toolhead_style is authoritative, so one
    // lookup covers every printer that declares a style. An explicit "default"
    // pins the printer to the Bambu-like glyph regardless of what a migrated
    // config's type string says.
    Config* config = Config::get_instance();
    std::string printer_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (!printer_type.empty()) {
        auto db_style = toolhead_style_from_name(PrinterDetector::get_toolhead_style(printer_type));
        if (db_style != ToolheadStyle::AUTO)
            return db_style;
    }

    // Printers the database does not cover: the live filament backend may know.
    auto* backend = AmsState::instance().get_backend();
    if (backend) {
        auto hinted = toolhead_style_from_name(backend->toolhead_style_hint());
        if (hinted != ToolheadStyle::AUTO)
            return hinted;
    }
    return ToolheadStyle::DEFAULT;
}

std::string SettingsManager::get_toolhead_style_options() {
    // The first two are words; the rest name toolhead products.
    return std::string(lv_tr("Auto")) + "\n" + lv_tr("Default") +
           "\nA4T\nAntHead\nJabberWocky\nStealthburner\nCreality K1\nCreality K2";
}

int SettingsManager::toolhead_style_to_dropdown_index(ToolheadStyle style) {
    return style_to_dropdown_index(style);
}

ToolheadStyle SettingsManager::dropdown_index_to_toolhead_style(int index) {
    auto* rc = get_runtime_config();
    bool debug = rc && rc->test_mode;
    int count = debug ? kDropdownDebugCount : kDropdownCount;
    const auto* table = debug ? kDropdownToStyleDebug : kDropdownToStyle;
    if (index < 0 || index >= count)
        return ToolheadStyle::AUTO;
    return table[index];
}

// ============================================================================
// Jog Step Distances
// ============================================================================

float SettingsManager::get_jog_distance(JogMode mode, bool outer) const {
    // Clamp on read as well as write, so nothing reading the cache can turn a
    // bad value into a zero-length jog. Both writers (the loader and the
    // setter) clamp before the cache ever sees a value, so this binds only
    // for a third writer — do not delete it as redundant.
    return std::clamp(jog_distances_[static_cast<int>(mode)][outer ? 1 : 0], 0.01f, 200.0f);
}

void SettingsManager::set_jog_distance(JogMode mode, bool outer, float mm) {
    mm = std::clamp(mm, 0.01f, 200.0f);
    spdlog::info("[SettingsManager] set_jog_distance({} = {} mm)", jog_distance_key(mode, outer),
                 mm);

    auto old_val = std::to_string(jog_distances_[static_cast<int>(mode)][outer ? 1 : 0]);

    // 1. Update the cache (jogs read it directly)
    jog_distances_[static_cast<int>(mode)][outer ? 1 : 0] = mm;

    // 2. Persist to config
    Config* config = Config::get_instance();
    config->set<float>(config->df() + jog_distance_key(mode, outer), mm);
    config->save();

    TelemetryManager::instance().notify_setting_changed(jog_distance_key(mode, outer), old_val,
                                                        std::to_string(mm));
}

void SettingsManager::reset_jog_distances() {
    for (int m = 0; m < kJogModeCount; ++m) {
        const JogMode mode = static_cast<JogMode>(m);
        for (int outer = 0; outer < 2; ++outer)
            set_jog_distance(mode, outer, jog_distance_default(mode, outer));
    }
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
    return Config::get_instance()->get<std::vector<std::string>>(path, std::vector<std::string>{});
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

constexpr const char* kFilterUserAddLeaf = "filter_user_add";
constexpr const char* kFilterUserRemoveLeaf = "filter_user_remove";

} // namespace

std::vector<std::string> SettingsManager::get_console_filter_user_add() const {
    return merged_console_filter(kFilterUserAddLeaf);
}

std::vector<std::string> SettingsManager::get_console_filter_user_remove() const {
    return merged_console_filter(kFilterUserRemoveLeaf);
}

std::vector<std::string>
SettingsManager::get_console_filter_user_add(ConsoleFilterScope scope) const {
    return read_console_filter_layer(kFilterUserAddLeaf, scope);
}

std::vector<std::string>
SettingsManager::get_console_filter_user_remove(ConsoleFilterScope scope) const {
    return read_console_filter_layer(kFilterUserRemoveLeaf, scope);
}

void SettingsManager::set_console_filter_user_add(const std::vector<std::string>& patterns,
                                                  ConsoleFilterScope scope) {
    write_console_filter_layer(kFilterUserAddLeaf, scope, patterns);
}

void SettingsManager::set_console_filter_user_remove(const std::vector<std::string>& patterns,
                                                     ConsoleFilterScope scope) {
    write_console_filter_layer(kFilterUserRemoveLeaf, scope, patterns);
}

// ============================================================================
// Macro Panel (per-printer hidden macro set)
// ============================================================================

std::vector<std::string> SettingsManager::get_hidden_macros() const {
    Config* config = Config::get_instance();
    return config->get<std::vector<std::string>>(config->df() + "macros/hidden",
                                                 std::vector<std::string>{});
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

bool SettingsManager::is_detection_seeded() const {
    return Config::get_instance()->get<bool>("/detection/seeded", false);
}

void SettingsManager::mark_detection_seeded() {
    Config* config = Config::get_instance();
    config->set<bool>("/detection/seeded", true);
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
                                               static_cast<int>(kAmsDefaultSlotColor)));
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

std::string SettingsManager::get_feeder_open_macro() const {
    return feeder_open_macro_;
}

void SettingsManager::set_feeder_open_macro(const std::string& value) {
    feeder_open_macro_ = value;
    spdlog::info("[SettingsManager] set_feeder_open_macro({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::FEEDER_OPEN_MACRO, value);
    config->save();
}

std::string SettingsManager::get_feeder_close_macro() const {
    return feeder_close_macro_;
}

std::string SettingsManager::get_ace_bypass_on_macro() const {
    return ace_bypass_on_macro_;
}

void SettingsManager::set_ace_bypass_on_macro(const std::string& value) {
    ace_bypass_on_macro_ = value;
    spdlog::info("[SettingsManager] set_ace_bypass_on_macro({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::kAceBypassOnMacro, value);
    config->save();
}

std::string SettingsManager::get_ace_bypass_off_macro() const {
    return ace_bypass_off_macro_;
}

void SettingsManager::set_ace_bypass_off_macro(const std::string& value) {
    ace_bypass_off_macro_ = value;
    spdlog::info("[SettingsManager] set_ace_bypass_off_macro({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::kAceBypassOffMacro, value);
    config->save();
}

void SettingsManager::set_feeder_close_macro(const std::string& value) {
    feeder_close_macro_ = value;
    spdlog::info("[SettingsManager] set_feeder_close_macro({})", value);
    Config* config = Config::get_instance();
    config->set<std::string>(config->df() + wizard::FEEDER_CLOSE_MACRO, value);
    config->save();
}

int SettingsManager::get_mount_feed_purge_mm() const {
    return mount_feed_purge_mm_;
}

void SettingsManager::set_mount_feed_purge_mm(int mm) {
    mount_feed_purge_mm_ = std::clamp(mm, 0, helix::mount_feed::kMaxPurgeMm);
    spdlog::info("[SettingsManager] set_mount_feed_purge_mm({})", mount_feed_purge_mm_);
    Config* config = Config::get_instance();
    config->set<int>(config->df() + wizard::kMountFeedPurgeMm, mount_feed_purge_mm_);
    config->save();
}

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
