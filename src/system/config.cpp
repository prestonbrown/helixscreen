// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include "ui_error_reporting.h"

#include "app_constants.h"
#include "runtime_config.h"

using AppConstants::Update::PREUPDATE_CONFIG_BACKUP;
using AppConstants::Update::PREUPDATE_ENV_BACKUP;

#include <fstream>
#include <iomanip>
#include <sys/stat.h>
// C++17 filesystem - use std::filesystem if available, fall back to experimental
#if __cplusplus >= 201703L && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

using namespace helix;

Config* Config::instance{NULL};

namespace {

/// Default macro configuration - shared between init() and reset_to_defaults()
json get_default_macros() {
    return {{"load_filament", {{"label", "Load"}, {"gcode", "LOAD_FILAMENT"}}},
            {"unload_filament", {{"label", "Unload"}, {"gcode", "UNLOAD_FILAMENT"}}},
            {"macro_1", {{"label", "Clean Nozzle"}, {"gcode", "HELIX_CLEAN_NOZZLE"}}},
            {"macro_2", {{"label", "Bed Level"}, {"gcode", "HELIX_BED_MESH_IF_NEEDED"}}},
            {"cooldown", "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE "
                         "HEATER=heater_bed TARGET=0"}};
}

/// Default printer configuration - shared between init() and reset_to_defaults()
/// @param moonraker_host Host address (empty string for reset, "127.0.0.1" for new config)
json get_default_printer_config(const std::string& moonraker_host) {
    return {
        {"moonraker_api_key", false},
        {"moonraker_host", moonraker_host},
        {"moonraker_port", 7125},
        {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
        {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
        {"fans",
         {{"part", "fan"}, {"hotend", "heater_fan hotend_fan"}, {"chamber", ""}, {"exhaust", ""}}},
        {"leds",
         {{"strip", ""}, {"selected", json::array()}}}, // Empty default - wizard will auto-detect
        {"extra_sensors", json::object()},
        {"hardware",
         {{"optional", json::array()},
          {"expected", json::array()},
          {"last_snapshot", json::object()}}},
        {"default_macros", get_default_macros()}};
}

/// Default display configuration section
/// Used for both new configs and ensuring display section exists with defaults
json get_default_display_config() {
    return {{"rotate", 0},
            {"sleep_sec", 600},
            {"dim_sec", 300},
            {"dim_brightness", 30},
            {"drm_device", ""},
            {"gcode_render_mode", 0},
            {"gcode_3d_enabled", true},
            {"bed_mesh_render_mode", 0}};
}

/// Migrate legacy display settings from root level to /display/ section
/// @param data JSON config data to migrate (modified in place)
/// @return true if migration occurred, false if no migration needed
bool migrate_display_config(json& data) {
    // Check for root-level display_rotate as indicator of old format
    if (!data.contains("display_rotate")) {
        return false; // Already migrated or new config
    }

    spdlog::info("[Config] Migrating display settings to /display/ section");

    // Ensure display section exists
    if (!data.contains("display")) {
        data["display"] = json::object();
    }

    // Migrate root-level display settings (only if target key doesn't already exist)
    if (data.contains("display_rotate")) {
        if (!data["display"].contains("rotate")) {
            data["display"]["rotate"] = data["display_rotate"];
            spdlog::info("[Config] Migrated display_rotate -> /display/rotate");
        }
        data.erase("display_rotate");
    }

    if (data.contains("display_sleep_sec")) {
        if (!data["display"].contains("sleep_sec")) {
            data["display"]["sleep_sec"] = data["display_sleep_sec"];
            spdlog::info("[Config] Migrated display_sleep_sec -> /display/sleep_sec");
        }
        data.erase("display_sleep_sec");
    }

    if (data.contains("display_dim_sec")) {
        if (!data["display"].contains("dim_sec")) {
            data["display"]["dim_sec"] = data["display_dim_sec"];
            spdlog::info("[Config] Migrated display_dim_sec -> /display/dim_sec");
        }
        data.erase("display_dim_sec");
    }

    if (data.contains("display_dim_brightness")) {
        if (!data["display"].contains("dim_brightness")) {
            data["display"]["dim_brightness"] = data["display_dim_brightness"];
            spdlog::info("[Config] Migrated display_dim_brightness -> /display/dim_brightness");
        }
        data.erase("display_dim_brightness");
    }

    // Migrate touch calibration settings (only if target keys don't already exist)
    if (data.contains("touch_calibrated") || data.contains("touch_calibration")) {
        // Ensure calibration subsection exists
        if (!data["display"].contains("calibration")) {
            data["display"]["calibration"] = json::object();
        }

        if (data.contains("touch_calibrated")) {
            if (!data["display"]["calibration"].contains("valid")) {
                data["display"]["calibration"]["valid"] = data["touch_calibrated"];
                spdlog::info("[Config] Migrated touch_calibrated -> /display/calibration/valid");
            }
            data.erase("touch_calibrated");
        }

        if (data.contains("touch_calibration")) {
            const auto& cal = data["touch_calibration"];
            for (const auto& key : {"a", "b", "c", "d", "e", "f"}) {
                if (cal.contains(key) && !data["display"]["calibration"].contains(key)) {
                    data["display"]["calibration"][key] = cal[key];
                }
            }
            data.erase("touch_calibration");
            spdlog::info(
                "[Config] Migrated touch_calibration/{{a-f}} -> /display/calibration/{{a-f}}");
        }
    }

    spdlog::info("[Config] Display settings migration complete");
    return true;
}

/// Erase a value at a JSON pointer path without triggering deprecated
/// json_pointer implicit string conversion (nlohmann json 3.11+ deprecation)
void erase_at_pointer(json& data, const json::json_pointer& ptr) {
    // Navigate to parent, then erase the leaf key
    auto ptr_str = ptr.to_string();
    auto last_slash = ptr_str.rfind('/');
    if (last_slash == 0) {
        // Top-level key like "/foo"
        data.erase(ptr_str.substr(1));
    } else if (last_slash != std::string::npos) {
        // Nested key like "/foo/bar" — get parent, erase leaf
        json::json_pointer parent_ptr(ptr_str.substr(0, last_slash));
        data[parent_ptr].erase(ptr_str.substr(last_slash + 1));
    }
}

/// Migrate config keys from old paths to new paths
/// @param data JSON config data to migrate (modified in place)
/// @param migrations Vector of {from_path, to_path} pairs (JSON pointer format)
/// @return true if any migration occurred, false if no migration needed
bool migrate_config_keys(json& data,
                         const std::vector<std::pair<std::string, std::string>>& migrations) {
    bool any_migrated = false;

    for (const auto& [from_path, to_path] : migrations) {
        json::json_pointer from_ptr(from_path);
        json::json_pointer to_ptr(to_path);

        // Skip if source doesn't exist
        if (!data.contains(from_ptr)) {
            continue;
        }

        // Skip if target already exists (don't overwrite)
        if (data.contains(to_ptr)) {
            spdlog::debug("[Config] Migration skipped: {} already exists", to_path);
            erase_at_pointer(data, from_ptr);
            any_migrated = true;
            continue;
        }

        // Ensure parent path exists for target
        // For example, if to_path is "/input/calibration", ensure "/input" exists
        auto last_slash = to_path.rfind('/');
        if (last_slash != std::string::npos && last_slash > 0) {
            std::string parent_path = to_path.substr(0, last_slash);
            json::json_pointer parent_ptr(parent_path);
            if (!data.contains(parent_ptr)) {
                data[parent_ptr] = json::object();
            }
        }

        // Copy value to new location and remove from old
        data[to_ptr] = data[from_ptr];
        erase_at_pointer(data, from_ptr);
        spdlog::info("[Config] Migrated {} -> {}", from_path, to_path);
        any_migrated = true;
    }

    return any_migrated;
}

// ============================================================================
// Versioned config migrations
// ============================================================================

/// Migration v0→v1: Sound support added — default sounds OFF for existing configs.
/// Before sound actually worked, configs had sounds_enabled: true as a harmless default.
/// Force it off so upgrading users don't get surprise beeps.
static void migrate_v0_to_v1(json& config) {
    if (config.contains("sounds_enabled")) {
        config["sounds_enabled"] = false;
        spdlog::info("[Config] Migration v1: disabled sounds_enabled for existing config");
    }
}

/// Migration v1→v2: Multi-LED support — convert single LED string to array
static void migrate_v1_to_v2(json& config) {
    json::json_pointer strip_ptr("/printer/leds/strip");
    json::json_pointer selected_ptr("/printer/leds/selected");

    // If new array path already exists, nothing to do
    if (config.contains(selected_ptr)) {
        return;
    }

    // Convert old single string to array
    if (config.contains(strip_ptr)) {
        auto& strip_val = config[strip_ptr];
        if (strip_val.is_string()) {
            std::string led = strip_val.get<std::string>();
            if (!led.empty()) {
                config[selected_ptr] = json::array({led});
                spdlog::info("[Config] Migration v2: converted LED '{}' from /printer/leds/strip "
                             "to /printer/leds/selected array",
                             led);
            } else {
                config[selected_ptr] = json::array();
                spdlog::info(
                    "[Config] Migration v2: empty LED strip, created empty selected array");
            }
        }
        // Don't remove /printer/leds/strip - keep for wizard backward compat
    } else {
        // No LED configured at all - create empty array
        config[selected_ptr] = json::array();
        spdlog::info("[Config] Migration v2: no LED configured, created empty selected array");
    }
}

/// Run all versioned migrations in sequence from current version to CURRENT_CONFIG_VERSION
static void run_versioned_migrations(json& config) {
    int version = 0;
    if (config.contains("config_version")) {
        version = config["config_version"].get<int>();
    }

    if (version < 1)
        migrate_v0_to_v1(config);
    if (version < 2)
        migrate_v1_to_v2(config);

    config["config_version"] = CURRENT_CONFIG_VERSION;
}

/// Default root-level config - shared between init() and reset_to_defaults()
/// @param moonraker_host Host address for printer
/// @param include_user_prefs Include user preference fields (brightness, sounds, etc.)
json get_default_config(const std::string& moonraker_host, bool include_user_prefs) {
    // log_level intentionally absent - test_mode provides fallback to DEBUG
    json config = {{"config_version", CURRENT_CONFIG_VERSION},
                   {"log_path", "/tmp/helixscreen.log"},
                   {"dark_mode", true},
                   {"theme", {{"preset", 0}}},
                   {"display", get_default_display_config()},
                   {"gcode_viewer", {{"shading_model", "phong"}, {"tube_sides", 4}}},
                   {"input",
                    {{"scroll_throw", 25},
                     {"scroll_limit", 10},
                     {"jitter_threshold", 15},
                     {"touch_device", ""},
                     {"calibration",
                      {{"valid", false},
                       {"a", 1.0},
                       {"b", 0.0},
                       {"c", 0.0},
                       {"d", 0.0},
                       {"e", 1.0},
                       {"f", 0.0}}}}},
                   {"printer", get_default_printer_config(moonraker_host)}};

    if (include_user_prefs) {
        config["brightness"] = 50;
        config["sounds_enabled"] = false;
        config["completion_alert"] = true;
        config["wizard_completed"] = false;
        config["wifi_expected"] = false;
        config["language"] = "en";
    }

    return config;
}

} // namespace

Config::Config() {}

Config* Config::get_instance() {
    if (instance == nullptr) {
        instance = new Config();
    }
    return instance;
}

void Config::init(const std::string& config_path) {
    path = config_path;
    struct stat buffer;

    // Migration: Check for legacy config at old location (helixconfig.json in app root)
    // If new location doesn't exist but old location does, migrate it
    if (stat(config_path.c_str(), &buffer) != 0) {
        // New config doesn't exist - check for legacy locations
        const std::vector<std::string> legacy_paths = {
            "helixconfig.json",                  // Old location (app root)
            "/opt/helixscreen/helixconfig.json", // Legacy embedded install
        };

        for (const auto& legacy_path : legacy_paths) {
            if (stat(legacy_path.c_str(), &buffer) == 0) {
                spdlog::info("[Config] Found legacy config at {}, migrating to {}", legacy_path,
                             config_path);

                // Ensure config/ directory exists
                fs::path config_dir = fs::path(config_path).parent_path();
                if (!config_dir.empty() && !fs::exists(config_dir)) {
                    fs::create_directories(config_dir);
                }

                // Copy legacy config to new location, then remove old file
                try {
                    fs::copy_file(legacy_path, config_path);
                    // Remove legacy file to avoid confusion
                    fs::remove(legacy_path);
                    spdlog::info("[Config] Migration complete: {} -> {} (old file removed)",
                                 legacy_path, config_path);
                } catch (const fs::filesystem_error& e) {
                    spdlog::warn("[Config] Migration failed: {}", e.what());
                    // Fall through to create default config
                }
                break;
            }
        }

        // Recovery: check for .pre-update backup created by the in-app updater.
        // Backups live in /var/log/ (outside INSTALL_DIR) so they survive the
        // atomic swap that moves INSTALL_DIR → INSTALL_DIR.old.
        if (stat(config_path.c_str(), &buffer) != 0) {
            if (stat(PREUPDATE_CONFIG_BACKUP, &buffer) == 0) {
                spdlog::warn("[Config] Config missing after upgrade — restoring from pre-update "
                             "backup: {}",
                             PREUPDATE_CONFIG_BACKUP);

                fs::path config_dir = fs::path(config_path).parent_path();
                if (!config_dir.empty() && !fs::exists(config_dir)) {
                    std::error_code ec;
                    fs::create_directories(config_dir, ec);
                    if (ec) {
                        spdlog::error("[Config] Failed to create config dir {}: {}",
                                      config_dir.string(), ec.message());
                    }
                }

                try {
                    fs::copy_file(PREUPDATE_CONFIG_BACKUP, config_path);
                    spdlog::info("[Config] Restored config from pre-update backup");
                    fs::remove(PREUPDATE_CONFIG_BACKUP);
                } catch (const fs::filesystem_error& e) {
                    spdlog::error("[Config] Failed to restore from pre-update backup: {}",
                                  e.what());
                }
            }
        }

        // Restore helixscreen.env independently — it can be lost even if config survived
        {
            std::string env_path =
                (fs::path(config_path).parent_path() / "helixscreen.env").string();
            struct stat env_st {};
            if (stat(env_path.c_str(), &env_st) != 0 && stat(PREUPDATE_ENV_BACKUP, &env_st) == 0) {
                spdlog::warn("[Config] helixscreen.env missing after upgrade — restoring from "
                             "pre-update backup");
                try {
                    fs::copy_file(PREUPDATE_ENV_BACKUP, env_path);
                    spdlog::info("[Config] Restored helixscreen.env from pre-update backup");
                    fs::remove(PREUPDATE_ENV_BACKUP);
                } catch (const fs::filesystem_error& e) {
                    spdlog::error("[Config] Failed to restore helixscreen.env: {}", e.what());
                }
            }
        }
    }

    bool config_modified = false;

    if (stat(config_path.c_str(), &buffer) == 0) {
        // Load existing config
        spdlog::info("[Config] Loading config from {}", config_path);
        try {
            data = json::parse(std::fstream(config_path));
        } catch (const json::exception& e) {
            spdlog::error("[Config] Failed to parse {}: {}", config_path, e.what());
            spdlog::warn("[Config] Config file is corrupt — resetting to defaults");

            // Backup the corrupt file for diagnosis
            std::string backup_path = config_path + ".corrupt";
            std::rename(config_path.c_str(), backup_path.c_str());
            spdlog::info("[Config] Corrupt config backed up to {}", backup_path);

            data = get_default_config("127.0.0.1", false);
            config_modified = true;
        }

        // Run display config migration (moves root-level display_* to /display/)
        if (migrate_display_config(data)) {
            config_modified = true;
        }

        // Migrate touch settings from /display/ to /input/
        if (migrate_config_keys(data, {{"/display/calibration", "/input/calibration"},
                                       {"/display/touch_device", "/input/touch_device"}})) {
            config_modified = true;
        }

        // Run versioned migrations (v0→v1: disable sounds for existing configs, etc.)
        int version_before = data.value("config_version", 0);
        run_versioned_migrations(data);
        if (data["config_version"].get<int>() != version_before) {
            config_modified = true;
        }
    } else {
        // Create default config
        spdlog::info("[Config] Creating default config at {}", config_path);
        data = get_default_config("127.0.0.1", false);
        config_modified = true;
    }

    // Ensure printer section exists with required fields
    auto& printer = data["/printer"_json_pointer];
    if (printer.is_null()) {
        data["/printer"_json_pointer] = get_default_printer_config("127.0.0.1");
        config_modified = true;
    } else {
        // Ensure heaters exists with defaults
        auto& heaters = data[json::json_pointer(df() + "heaters")];
        if (heaters.is_null()) {
            data[json::json_pointer(df() + "heaters")] = {{"bed", "heater_bed"},
                                                          {"hotend", "extruder"}};
            config_modified = true;
        }

        // Ensure temp_sensors exists with defaults
        auto& temp_sensors = data[json::json_pointer(df() + "temp_sensors")];
        if (temp_sensors.is_null()) {
            data[json::json_pointer(df() + "temp_sensors")] = {{"bed", "heater_bed"},
                                                               {"hotend", "extruder"}};
            config_modified = true;
        }

        // Ensure fans exists with defaults
        auto& fans = data[json::json_pointer(df() + "fans")];
        if (fans.is_null()) {
            data[json::json_pointer(df() + "fans")] = {{"part", "fan"},
                                                       {"hotend", "heater_fan hotend_fan"}};
            config_modified = true;
        }

        // Ensure leds exists with defaults
        auto& leds = data[json::json_pointer(df() + "leds")];
        if (leds.is_null()) {
            data[json::json_pointer(df() + "leds")] = {{"strip", "neopixel chamber_light"}};
            config_modified = true;
        }

        // Ensure leds/selected array exists (for multi-LED support)
        auto& leds_selected = data[json::json_pointer(df() + "leds/selected")];
        if (leds_selected.is_null()) {
            // Check if there's a legacy strip value to migrate
            auto& strip = data[json::json_pointer(df() + "leds/strip")];
            if (!strip.is_null() && strip.is_string()) {
                std::string led = strip.get<std::string>();
                if (!led.empty()) {
                    data[json::json_pointer(df() + "leds/selected")] = json::array({led});
                } else {
                    data[json::json_pointer(df() + "leds/selected")] = json::array();
                }
            } else {
                data[json::json_pointer(df() + "leds/selected")] = json::array();
            }
            config_modified = true;
        }

        // Ensure extra_sensors exists (empty object for user additions)
        auto& extra_sensors = data[json::json_pointer(df() + "extra_sensors")];
        if (extra_sensors.is_null()) {
            data[json::json_pointer(df() + "extra_sensors")] = json::object();
            config_modified = true;
        }

        // Ensure hardware section exists
        auto& hardware = data[json::json_pointer(df() + "hardware")];
        if (hardware.is_null()) {
            data[json::json_pointer(df() + "hardware")] = {{"optional", json::array()},
                                                           {"expected", json::array()},
                                                           {"last_snapshot", json::object()}};
            config_modified = true;
        }

        // Ensure default_macros exists
        auto& default_macros = data[json::json_pointer(df() + "default_macros")];
        if (default_macros.is_null()) {
            data[json::json_pointer(df() + "default_macros")] = get_default_macros();
            config_modified = true;
        }
    }

    // log_level intentionally NOT migrated - absence allows test_mode fallback

    // Ensure display section exists with defaults
    if (!data.contains("display")) {
        data["display"] = get_default_display_config();
        config_modified = true;
    } else {
        // Ensure all display subsections exist with defaults
        auto display_defaults = get_default_display_config();
        auto& display = data["display"];

        for (auto& [key, value] : display_defaults.items()) {
            if (!display.contains(key)) {
                display[key] = value;
                config_modified = true;
            }
        }
    }

    // Ensure input section exists with defaults (scroll settings + touch calibration)
    if (!data.contains("input")) {
        data["input"] = {{"scroll_throw", 25},
                         {"scroll_limit", 10},
                         {"jitter_threshold", 15},
                         {"touch_device", ""},
                         {"calibration",
                          {{"valid", false},
                           {"a", 1.0},
                           {"b", 0.0},
                           {"c", 0.0},
                           {"d", 0.0},
                           {"e", 1.0},
                           {"f", 0.0}}}};
        config_modified = true;
    } else {
        // Ensure all input subsections exist with defaults
        auto& input = data["input"];

        // Ensure scroll settings exist
        if (!input.contains("scroll_throw")) {
            input["scroll_throw"] = 25;
            config_modified = true;
        }
        if (!input.contains("scroll_limit")) {
            input["scroll_limit"] = 10;
            config_modified = true;
        }
        if (!input.contains("touch_device")) {
            input["touch_device"] = "";
            config_modified = true;
        }
        if (!input.contains("jitter_threshold")) {
            input["jitter_threshold"] = 15;
            config_modified = true;
        }

        // Ensure calibration subsection exists with all required fields
        if (!input.contains("calibration")) {
            input["calibration"] = {{"valid", false}, {"a", 1.0}, {"b", 0.0}, {"c", 0.0},
                                    {"d", 0.0},       {"e", 1.0}, {"f", 0.0}};
            config_modified = true;
        } else {
            // Ensure all calibration fields exist
            auto& cal = input["calibration"];
            const json cal_defaults = {{"valid", false}, {"a", 1.0}, {"b", 0.0}, {"c", 0.0},
                                       {"d", 0.0},       {"e", 1.0}, {"f", 0.0}};
            for (auto& [key, value] : cal_defaults.items()) {
                if (!cal.contains(key)) {
                    cal[key] = value;
                    config_modified = true;
                }
            }
        }
    }

    // Save updated config with any new defaults or migrations
    if (config_modified) {
        std::ofstream o(config_path);
        o << std::setw(2) << data << std::endl;
        spdlog::debug("[Config] Saved updated config to {}", config_path);
    }

    spdlog::debug("[Config] initialized: moonraker={}:{}",
                  get<std::string>(df() + "moonraker_host"), get<int>(df() + "moonraker_port"));
}

std::string Config::df() {
    return "/printer/";
}

std::string Config::get_path() {
    return path;
}

json& Config::get_json(const std::string& json_path) {
    return data[json::json_pointer(json_path)];
}

bool Config::save() {
    if (path.empty()) {
        spdlog::trace("[Config] Skipping save (no config path set)");
        return true;
    }

    spdlog::trace("[Config] Saving config to {}", path);

    try {
        std::ofstream o(path);
        if (!o.is_open()) {
            NOTIFY_ERROR("Could not save configuration file");
            LOG_ERROR_INTERNAL("Failed to open config file for writing: {}", path);
            return false;
        }

        o << std::setw(2) << data << std::endl;

        if (!o.good()) {
            NOTIFY_ERROR("Error writing configuration file");
            LOG_ERROR_INTERNAL("Error writing to config file: {}", path);
            return false;
        }

        o.close();
        spdlog::trace("[Config] saved successfully to {}", path);
        return true;

    } catch (const std::exception& e) {
        NOTIFY_ERROR("Failed to save configuration: {}", e.what());
        LOG_ERROR_INTERNAL("Exception while saving config to {}: {}", path, e.what());
        return false;
    }
}

bool Config::is_wizard_required() {
    // Check explicit wizard completion flag
    // IMPORTANT: Use contains() first to avoid creating null entries via operator[]
    json::json_pointer ptr("/wizard_completed");

    if (data.contains(ptr)) {
        auto& wizard_completed = data[ptr];
        if (wizard_completed.is_boolean()) {
            bool is_completed = wizard_completed.get<bool>();
            spdlog::trace("[Config] Wizard completed flag = {}", is_completed);
            return !is_completed; // Wizard required if flag is false
        }
        // Key exists but wrong type - treat as not set
        spdlog::warn("[Config] wizard_completed has invalid type, treating as unset");
    }

    // No flag set - wizard has never been run
    spdlog::debug("[Config] No wizard_completed flag found, wizard required");
    return true;
}

bool Config::is_wifi_expected() {
    return get<bool>("/wifi_expected", false);
}

void Config::set_wifi_expected(bool expected) {
    set("/wifi_expected", expected);
}

std::string Config::get_language() {
    return get<std::string>("/language", "en");
}

void Config::set_language(const std::string& lang) {
    set("/language", lang);
}

bool Config::is_beta_features_enabled() {
#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
    // In test mode, default to true unless explicitly set to false
    auto* rt = get_runtime_config();
    if (rt && rt->is_test_mode()) {
        return get<bool>("/beta_features", true);
    }
#endif

    return get<bool>("/beta_features", false);
}

void Config::reset_to_defaults() {
    spdlog::info("[Config] Resetting configuration to factory defaults");

    // Reset to default configuration with empty moonraker_host (requires reconfiguration)
    // and include user preferences (brightness, sounds, etc.) with wizard_completed=false
    data = get_default_config("", true);

    spdlog::info("[Config] Configuration reset to defaults. Wizard will run on next startup.");
}

MacroConfig Config::get_macro(const std::string& key, const MacroConfig& default_val) {
    try {
        std::string path = df() + "default_macros/" + key;
        json::json_pointer ptr(path);

        if (!data.contains(ptr)) {
            spdlog::trace("[Config] Macro '{}' not found, using default", key);
            return default_val;
        }

        const auto& val = data[ptr];

        // Handle string format (backward compatibility): use as both label and gcode
        if (val.is_string()) {
            std::string macro = val.get<std::string>();
            spdlog::trace("[Config] Macro '{}' is string format: '{}'", key, macro);
            return {macro, macro};
        }

        // Handle object format: {label, gcode}
        if (val.is_object()) {
            MacroConfig result;
            result.label = val.value("label", default_val.label);
            result.gcode = val.value("gcode", default_val.gcode);
            spdlog::trace("[Config] Macro '{}': label='{}', gcode='{}'", key, result.label,
                          result.gcode);
            return result;
        }

        spdlog::warn("[Config] Macro '{}' has unexpected type, using default", key);
        return default_val;

    } catch (const std::exception& e) {
        spdlog::warn("[Config] Error reading macro '{}': {}", key, e.what());
        return default_val;
    }
}
