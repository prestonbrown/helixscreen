// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
#include "system/telemetry_manager.h"
#define CONFIG_RECORD_ERROR(...) TelemetryManager::instance().record_error(__VA_ARGS__)
#else
#define CONFIG_RECORD_ERROR(...) ((void)0)
#endif
#include "ui_error_reporting.h"

#include "app_constants.h"
#include "config_backup.h"
#include "config_testing.h"
#include "data_root_resolver.h"
#include "host_identity.h"
#include "json_utils.h"
#include "platform_capabilities.h"
#include "printer_detector.h"
#include "runtime_config.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
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

using AppConstants::Update::config_backup_fallback;
using AppConstants::Update::config_backup_primary;
using AppConstants::Update::env_backup_fallback;
using AppConstants::Update::env_backup_primary;
using AppConstants::Update::legacy_config_backup_fallback;
using AppConstants::Update::legacy_config_backup_primary;

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
    return {{"sleep_sec", 1200},       {"dim_sec", 600},           {"dim_brightness", 30},
            {"drm_device", ""},        {"gcode_render_mode", 0},   {"bed_mesh_render_mode", 0},
            {"gpu_3d_blocked", false}, {"gpu_blur_blocked", false}};
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

/// True when @p data has a non-null value at @p ptr. Both contains() and at()
/// are non-vivifying, so this probes without creating nodes.
///
/// The distinction matters because nlohmann's contains() answers TRUE for a key
/// whose value is null, and null keys are exactly what the pre-#1129
/// Config::get_json() probes wrote all over user configs. Treating one of those
/// as "a value is already here" makes a migration erase the real legacy source
/// and keep the garbage.
bool has_value_at(const json& data, const json::json_pointer& ptr) {
    return data.contains(ptr) && !data.at(ptr).is_null();
}

/// Migrate config keys from old paths to new paths
///
/// A null at either end counts as ABSENT, never as a value:
///   - null TARGET  → the move proceeds and overwrites the null (a null target
///                    is probe pollution, not a user setting).
///   - null SOURCE  → nothing worth moving; the source is dropped and the
///                    target is left alone rather than being overwritten with null.
///
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

        // A null source carries nothing. Drop it rather than writing null over
        // whatever the target holds.
        if (data.at(from_ptr).is_null()) {
            spdlog::debug("[Config] Migration dropped null source: {}", from_path);
            erase_at_pointer(data, from_ptr);
            any_migrated = true;
            continue;
        }

        // Skip if the target already holds a real value (don't overwrite)
        if (has_value_at(data, to_ptr)) {
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

} // end anonymous namespace

// Test injection seam: allows unit tests to force a platform tier classification
// without monkey-patching /proc. Production code never sets this.
namespace {
static std::optional<helix::PlatformTier> g_forced_tier_for_migration;
} // namespace

namespace helix::config_testing {
void set_forced_tier_for_migration(std::optional<helix::PlatformTier> tier) {
    g_forced_tier_for_migration = tier;
}
} // namespace helix::config_testing

namespace {

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
static helix::PlatformTier current_tier_for_migration() {
    if (g_forced_tier_for_migration.has_value()) {
        return *g_forced_tier_for_migration;
    }
    return helix::PlatformCapabilities::detect().tier;
}
#endif

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

/// Migration v2→v3: Reset jitter_threshold from 15 to 0 (disabled by default).
/// The jitter filter competed with LVGL's scroll_limit, adding perceptible drag delay.
/// Users with genuinely noisy panels can re-enable via config or HELIX_TOUCH_JITTER env.
static void migrate_v2_to_v3(json& config) {
    json::json_pointer ptr("/input/jitter_threshold");
    if (config.contains(ptr) && config[ptr].is_number_integer() && config[ptr].get<int>() == 15) {
        config[ptr] = 5;
        spdlog::info("[Config] Migration v3: reset jitter_threshold 15 -> 5");
    }
}

/// Migration v3→v4: Restructure single /printer to multi-printer /printers map.
/// Moves the old singular "printer" object under "printers/{slug}/" and sets active_printer_id.
/// Also moves root-level "filament", "panel_widgets" under the printer entry.
static void migrate_v3_to_v4(json& config) {
    // Skip if already has /printers (idempotent)
    if (config.contains("printers")) {
        return;
    }

    // Skip if no old /printer section exists
    if (!config.contains("printer") || !config["printer"].is_object()) {
        return;
    }

    json printer_data = config["printer"];

    // Determine the slug ID from the printer name
    std::string printer_name;
    if (printer_data.contains("printer_name") && printer_data["printer_name"].is_string()) {
        printer_name = printer_data["printer_name"].get<std::string>();
    } else if (printer_data.contains("name") && printer_data["name"].is_string()) {
        printer_name = printer_data["name"].get<std::string>();
    }
    std::string slug = printer_name.empty() ? "default" : Config::slugify(printer_name);
    if (slug.empty()) {
        slug = "default";
    }

    // Move root-level per-printer sections into the printer entry
    if (config.contains("filament") && config["filament"].is_object()) {
        printer_data["filament"] = config["filament"];
        config.erase("filament");
    }
    if (config.contains("panel_widgets") && config["panel_widgets"].is_object()) {
        printer_data["panel_widgets"] = config["panel_widgets"];
        config.erase("panel_widgets");
    }

    // Copy root-level wizard_completed into the printer entry
    if (config.contains("wizard_completed")) {
        printer_data["wizard_completed"] = config["wizard_completed"];
        // Keep root-level for backward compatibility
    }

    // Create the new printers map and set the active printer
    config["printers"] = {{slug, printer_data}};
    config["active_printer_id"] = slug;

    // Remove the old singular "printer" key
    config.erase("printer");

    // Migrate /display/printer_image to per-printer /printers/{id}/printer_image
    if (config.contains("display") && config["display"].contains("printer_image")) {
        config["printers"][slug]["printer_image"] = config["display"]["printer_image"];
        config["display"].erase("printer_image");
        spdlog::info(
            "[Config] Migration v4: moved /display/printer_image to /printers/{}/printer_image",
            slug);
    }

    spdlog::info("[Config] Migration v4: restructured /printer to /printers/{}", slug);
}

/// Default show_printer_switcher to false for single-printer configs.
/// Shared by v4→v5 and v5→v6 migrations (v6 re-runs for fresh v5 installs that had wrong default).
static void default_printer_switcher_off(json& config, int target_version) {
    if (config.contains("/printers/show_printer_switcher"_json_pointer)) {
        return;
    }

    int printer_count = 0;
    if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [key, val] : config["printers"].items()) {
            if (val.is_object()) {
                printer_count++;
            }
        }
    }

    if (printer_count <= 1) {
        config["/printers/show_printer_switcher"_json_pointer] = false;
        spdlog::info(
            "[Config] Migration v{}: disabled show_printer_switcher for single-printer config",
            target_version);
    }
}

static void migrate_v4_to_v5(json& config) {
    default_printer_switcher_off(config, 5);
}
static void migrate_v5_to_v6(json& config) {
    default_printer_switcher_off(config, 6);
}

/// Bump default brightness from 50% to 80% for users who never changed it.
static void migrate_v6_to_v7(json& config) {
    if (config.contains("brightness") && config["brightness"].is_number() &&
        config["brightness"].get<int>() == 50) {
        config["brightness"] = 80;
        spdlog::info("[Config] Migration v7: updated default brightness from 50% to 80%");
    }
}

/// Remap toolhead_style after alphabetical reorder of enum values.
/// Old: AUTO=0, DEFAULT=1, STEALTHBURNER=2, A4T=3, JABBERWOCKY=4
/// New: AUTO=0, DEFAULT=1, A4T=2, ANTHEAD=3, JABBERWOCKY=4, STEALTHBURNER=5
static void migrate_v7_to_v8(json& config) {
    auto remap_toolhead = [](json& printers_obj) {
        for (auto& [id, printer] : printers_obj.items()) {
            json::json_pointer ptr("/appearance/toolhead_style");
            if (printer.contains(ptr) && printer[ptr].is_number_integer()) {
                int old_val = printer[ptr].get<int>();
                // Only 2 (old STEALTHBURNER) and 3 (old A4T) need remapping
                if (old_val == 2) {
                    printer[ptr] = 5; // STEALTHBURNER
                    spdlog::info(
                        "[Config] Migration v8: remapped toolhead_style 2→5 (Stealthburner) "
                        "for printer {}",
                        id);
                } else if (old_val == 3) {
                    printer[ptr] = 2; // A4T
                    spdlog::info("[Config] Migration v8: remapped toolhead_style 3→2 (A4T) "
                                 "for printer {}",
                                 id);
                }
            }
        }
    };

    if (config.contains("printers") && config["printers"].is_object()) {
        remap_toolhead(config["printers"]);
    }
}

/// Re-apply brightness 50->80 bump for users whose config was written with the
/// old default of 50 after v7 migration already ran (the default in
/// get_default_config() was still 50, so new installs after v7 got 50 again).
static void migrate_v8_to_v9(json& config) {
    if (config.contains("brightness") && config["brightness"].is_number() &&
        config["brightness"].get<int>() == 50) {
        config["brightness"] = 80;
        spdlog::info("[Config] Migration v9: updated default brightness from 50% to 80%");
    }
}

/// Consolidate "power" widget into "power_device" with __all__ sentinel.
/// Scans all printers/panels/pages for widget entries with id=="power" and replaces
/// them with "power_device:N" (using the next available instance number).
static void migrate_v9_to_v10(json& config) {
    if (!config.contains("printers"))
        return;

    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object() || !printer.contains("panel_widgets"))
            continue;

        for (auto& [panel_id, panel] : printer["panel_widgets"].items()) {
            if (!panel.is_object() || !panel.contains("pages") || !panel["pages"].is_array())
                continue;

            // Find max power_device instance number across all pages
            int max_instance = 0;
            for (auto& page : panel["pages"]) {
                if (!page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    std::string id = widget.value("id", "");
                    if (id.substr(0, 13) == "power_device:") {
                        try {
                            int n = std::stoi(id.substr(13));
                            if (n > max_instance)
                                max_instance = n;
                        } catch (...) {
                        }
                    }
                }
            }

            // Migrate "power" → "power_device:N+1"
            for (auto& page : panel["pages"]) {
                if (!page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    if (widget.value("id", "") == "power") {
                        max_instance++;
                        widget["id"] = "power_device:" + std::to_string(max_instance);
                        widget["config"] = {{"device", "__all__"}, {"icon", "power_cycle"}};
                        spdlog::info("[Config] Migrated power widget to power_device:{} for "
                                     "printer '{}'",
                                     max_instance, printer_id);
                    }
                }
            }
        }
    }
}

/// Migrate PID heat rates to shared thermal path; strip heating phases from predictor entries.
static void migrate_v10_to_v11(json& config) {
    // 1. Move PID heat rates to shared thermal path
    for (const auto& heater : {"extruder", "heater_bed"}) {
        if (config.contains("calibration") && config["calibration"].contains("pid_history") &&
            config["calibration"]["pid_history"].contains(heater) &&
            config["calibration"]["pid_history"][heater].contains("heat_rate")) {
            // Copy to new location if not already present
            bool dest_exists = config.contains("thermal") && config["thermal"].contains("rates") &&
                               config["thermal"]["rates"].contains(heater) &&
                               config["thermal"]["rates"][heater].contains("heat_rate");

            if (!dest_exists) {
                config["thermal"]["rates"][heater]["heat_rate"] =
                    config["calibration"]["pid_history"][heater]["heat_rate"];
                spdlog::info("[Config] Migration v11: copied heat_rate for '{}' to /thermal/rates",
                             heater);
            }

            // Erase from old location (leave parent intact for oscillation_duration)
            config["calibration"]["pid_history"][heater].erase("heat_rate");
            spdlog::info(
                "[Config] Migration v11: removed heat_rate from /calibration/pid_history/{}",
                heater);
        }
    }

    // 2. Strip heating phases (HEATING_BED=3, HEATING_NOZZLE=4) from predictor entries
    if (config.contains("print_start_history") &&
        config["print_start_history"].contains("entries") &&
        config["print_start_history"]["entries"].is_array()) {
        int count = 0;
        for (auto& entry : config["print_start_history"]["entries"]) {
            if (entry.contains("phases") && entry["phases"].is_object()) {
                entry["phases"].erase("3"); // HEATING_BED
                entry["phases"].erase("4"); // HEATING_NOZZLE
                count++;
            }
        }
        if (count > 0) {
            spdlog::info(
                "[Config] Migration v11: stripped heating phases from {} predictor entries", count);
        }
    }
}

/// Consolidate chamber assignment keys: move `printer/chamber_sensor` → `temp_sensors/chamber`
/// and `printer/chamber_heater` → `heaters/chamber` under each printer, matching the flat
/// per-hardware-type convention used everywhere else (heaters/bed, fans/chamber, etc.).
static void migrate_v11_to_v12(json& config) {
    if (!config.contains("printers") || !config["printers"].is_object())
        return;

    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object())
            continue;
        const std::vector<std::pair<std::string, std::string>> migrations = {
            {"/printer/chamber_sensor", "/temp_sensors/chamber"},
            {"/printer/chamber_heater", "/heaters/chamber"},
        };
        if (migrate_config_keys(printer, migrations)) {
            spdlog::info("[Config] Migration v12: consolidated chamber keys for printer '{}'",
                         printer_id);
        }
        // If the now-empty /printer subkey remains, drop it.
        if (printer.contains("printer") && printer["printer"].is_object() &&
            printer["printer"].empty()) {
            printer.erase("printer");
        }
    }
}

/// Repair stale AD5X display settings from the pre-#431 preset. The original AD5X
/// preset (#235) set sleep_backlight_off=false under the assumption that backlight
/// power-off prevented wake-on-touch. #431 corrected this after hardware verification.
/// Users whose wizard ran between those commits still have the stale combination
/// (backlight never turns off at sleep).
static void migrate_v12_to_v13(json& config) {
    bool is_ad5x = false;
    if (config.value("preset", "") == "ad5x") {
        is_ad5x = true;
    } else if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [printer_id, printer] : config["printers"].items()) {
            if (printer.is_object() && printer.value("type", "") == "FlashForge Adventurer 5X") {
                is_ad5x = true;
                break;
            }
        }
    }
    if (!is_ad5x)
        return;

    if (!config.contains("display") || !config["display"].is_object())
        return;

    auto& display = config["display"];
    if (display.value("sleep_backlight_off", true) == false &&
        display.value("hardware_blank", -1) == 0) {
        display["sleep_backlight_off"] = true;
        display["hardware_blank"] = 1;
        spdlog::info("[Config] Migration v13: restored AD5X backlight-off sleep "
                     "(sleep_backlight_off=true, hardware_blank=1)");
    }
}

/// Fold legacy telemetry_config.json into settings.json. The previous
/// architecture had two sources of truth for /telemetry_enabled: TelemetryManager
/// owned telemetry_config.json; SystemSettingsManager owned settings.json's
/// /telemetry_enabled. A sync line in application.cpp clobbered the former
/// with the latter on every startup, silently disabling telemetry for users
/// whose settings.json had never had the key set. This migration preserves
/// whatever state telemetry_config.json held and then retires the file.
static void migrate_v13_to_v14(json& config, const std::string& config_path) {
    // If /telemetry_enabled is already in settings.json, no migration needed —
    // but still delete the legacy file below if it exists.
    bool has_key = config.contains("telemetry_enabled");

    if (config_path.empty()) {
        // Called from a test path without a filesystem; nothing to migrate.
        return;
    }

    fs::path legacy_path = fs::path(config_path).parent_path() / "telemetry_config.json";
    std::error_code ec;
    if (!fs::exists(legacy_path, ec)) {
        return;
    }

    try {
        std::ifstream f(legacy_path);
        json legacy;
        f >> legacy;
        if (!has_key && legacy.contains("enabled") && legacy["enabled"].is_boolean()) {
            bool legacy_enabled = legacy["enabled"].get<bool>();
            config["telemetry_enabled"] = legacy_enabled;
            spdlog::info("[Config] Migration v14: imported telemetry_enabled={} "
                         "from legacy {}",
                         legacy_enabled ? "true" : "false", legacy_path.string());
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Config] Migration v14: failed to read legacy {}: {} "
                     "(leaving file in place for retry)",
                     legacy_path.string(), e.what());
        return;
    }

    fs::remove(legacy_path, ec);
    if (!ec) {
        spdlog::debug("[Config] Migration v14: removed legacy {}", legacy_path.string());
    }
}

/// v14→v15: Re-apply AD5X sleep preset for users whose printer-identify wizard
/// ran AFTER the v12→v13 migration. The wizard (pre-fix) force-wrote the stale
/// pre-#431 display config on every confirmation, undoing the v13 restore for
/// any AD5X user who ran it. The wizard block has been removed in the same
/// release that introduces this migration. Detection condition is identical to
/// v12→v13 so the fix is idempotent if the wizard is re-run on an older build.
static void migrate_v14_to_v15(json& config) {
    bool is_ad5x = false;
    if (config.value("preset", "") == "ad5x") {
        is_ad5x = true;
    } else if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [printer_id, printer] : config["printers"].items()) {
            if (printer.is_object() && printer.value("type", "") == "FlashForge Adventurer 5X") {
                is_ad5x = true;
                break;
            }
        }
    }
    if (!is_ad5x)
        return;

    if (!config.contains("display") || !config["display"].is_object())
        return;

    auto& display = config["display"];
    if (display.value("sleep_backlight_off", true) == false &&
        display.value("hardware_blank", -1) == 0) {
        display["sleep_backlight_off"] = true;
        display["hardware_blank"] = 1;
        spdlog::info("[Config] Migration v15: re-applied AD5X backlight-off sleep "
                     "after wizard-override removal");
    }
}

/// v15 → v16: Turn the screensaver off on BASIC/EMBEDDED tiers where Flying Toasters
/// causes Klipper print failures. Queues a one-time info modal via a transient flag
/// consumed by Application post-boot. Only affects type 1 (Flying Toasters); types 2/3
/// (Starfield, Pipes 3D) are left untouched because we have no evidence they cause
/// the same problem, and silently flipping an explicit user choice is worse than
/// leaving a slightly expensive screensaver running on their preferred setting.
static void migrate_v15_to_v16(json& config) {
#if defined(HELIX_SPLASH_ONLY) || defined(HELIX_WATCHDOG)
    // Splash and watchdog don't render the screensaver and don't link the
    // PlatformCapabilities object code. The version still advances; the next
    // helix-screen run will surface the migration notice if applicable.
    (void)config;
#else
    using helix::PlatformTier;
    PlatformTier tier = current_tier_for_migration();
    if (tier != PlatformTier::BASIC && tier != PlatformTier::EMBEDDED) {
        return; // STANDARD hardware keeps its setting
    }

    if (!config.contains("display") || !config["display"].is_object()) {
        return;
    }
    auto& display = config["display"];

    int current_type = display.value("screensaver_type", 0);
    if (current_type != 1) {
        return; // only migrate Flying Toasters
    }

    display["screensaver_type"] = 0;
    display["screensaver_migration_notice_pending"] = true;
    spdlog::info("[Config] Migration v16: screensaver disabled on {} tier "
                 "(was Flying Toasters); notice queued",
                 helix::platform_tier_to_string(tier));
#endif
}

/// v16 → v17: Rename retired Voron printer image IDs (#964). The 0.2 and 2.4r2 PNGs
/// were replaced by voron-v0 / voron-v2 on disk; auto-detect users are covered by
/// the printer_database.json update, but anyone who manually picked the old image
/// in the Printer Image overlay has "shipped:voron-24r2" or "shipped:voron-0-2"
/// frozen in their per-printer printer_image setting and would render the
/// generic-CoreXY fallback after upgrade.
static void migrate_v16_to_v17(json& config) {
    static const std::vector<std::pair<std::string, std::string>> renames = {
        {"shipped:voron-24r2", "shipped:voron-v2"},
        {"shipped:voron-0-2", "shipped:voron-v0"},
    };

    if (!config.contains("printers") || !config["printers"].is_object())
        return;

    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object())
            continue;
        if (!printer.contains("printer_image") || !printer["printer_image"].is_string())
            continue;
        std::string current = printer["printer_image"].get<std::string>();
        for (const auto& [from, to] : renames) {
            if (current == from) {
                printer["printer_image"] = to;
                spdlog::info("[Config] Migration v17: renamed printer_image '{}' -> '{}' "
                             "for printer '{}'",
                             from, to, printer_id);
                break;
            }
        }
    }
}

/// v17 → v18: After the #943/#986 touch-scaling fix, the DRM/fbdev backends apply
/// evdev linear scaling to MT-only digitizers (e.g. Qidi Q2: 800x480 controller on a
/// 480x272 panel). Any affine calibration captured before the fix was computed in the
/// wrong (unscaled) coordinate space and would re-break touch on upgrade. We cannot
/// distinguish a stale capacitive affine from a legitimate resistive one in JSON alone
/// (large coefficients are valid for resistive panels), so set a one-shot
/// recheck_pending flag here; the display backend decides at boot — when it knows the
/// device's resistive/capacitive nature and live ABS range — whether to invalidate.
static void migrate_v17_to_v18(json& config) {
    // Guard ([L087]): an absent/default-constructed json is null, and writing into a
    // null via operator[] would replace it — but reading .value()/iterating a null
    // throws. Create the input/calibration objects only when missing, never overwrite
    // existing data.
    if (!config.contains("input") || !config["input"].is_object()) {
        config["input"] = json::object();
    }
    if (!config["input"].contains("calibration") || !config["input"]["calibration"].is_object()) {
        config["input"]["calibration"] = json::object();
    }

    config["input"]["calibration"]["recheck_pending"] = true;
    spdlog::info("[Config] Migration v18: flagged touch calibration for post-#943 recheck "
                 "(recheck_pending=true)");
}

/// Phase 2 offline filament picker (Task 6): /preset_materials grows from a 4-string
/// array to a 4-object array so branded filament info (id/brand/name/temps) can be
/// attached to each quick-material preset slot. Idempotent: elements already objects
/// are left untouched, so re-running against an already-migrated config never clobbers
/// branding.
static void migrate_v18_to_v19(json& config) {
    if (!config.contains("preset_materials") || !config["preset_materials"].is_array()) {
        return;
    }
    json& arr = config["preset_materials"];
    int converted = 0;
    for (auto& el : arr) {
        if (el.is_string()) {
            json obj = json::object();
            obj["type"] = el.get<std::string>();
            el = obj;
            ++converted;
        }
    }
    spdlog::info("[Config] Migration v19: preset_materials strings -> objects ({} converted)",
                 converted);
}

/// Recursively erase object members whose value is null.
///
/// Array elements are deliberately left alone: erasing one shifts every later
/// index, and no config consumer treats a null array slot as removable.
/// Audited 2026-07-25 — no config setting uses null as a meaningful tri-state
/// value; every `.is_null()` check on config data (hardware_validator,
/// panel_widget_config, config.cpp's own default-filling) means "absent, use
/// the default", which is exactly what erasing the key produces.
static int strip_null_leaves(json& node) {
    int removed = 0;
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end();) {
            if (it.value().is_null()) {
                it = node.erase(it);
                ++removed;
            } else {
                removed += strip_null_leaves(it.value());
                ++it;
            }
        }
    } else if (node.is_array()) {
        for (auto& element : node) {
            removed += strip_null_leaves(element);
        }
    }
    return removed;
}

/// True when @p node holds anything a user could have set — i.e. any leaf that
/// is not null. An empty object/array counts as nothing, and so does a tree that
/// bottoms out entirely in nulls (the shape read-only probes left behind).
/// Used to decide whether a legacy node is safe to erase.
static bool has_any_value(const json& node) {
    if (node.is_null()) {
        return false;
    }
    if (node.is_object() || node.is_array()) {
        for (const auto& child : node) {
            if (has_any_value(child)) {
                return true;
            }
        }
        return false;
    }
    return true;
}

/// Resolve which entry of /printers the config's /active_printer_id refers to,
/// applying the "empty or dangling → first printer object" fallback.
///
/// The printers map is MIXED: alongside the printer objects it holds plain
/// settings keys (`show_printer_switcher` is a bool, and the shipped template
/// adds a `_show_printer_switcher_comment` string), so an entry only counts as
/// a printer when it is an object. Every resolution site must apply that test,
/// which is why it lives here rather than being open-coded per caller.
///
/// @param preferred id to consider when /active_printer_id is absent or is not
///        a string — callers that already hold a resolved id keep it rather
///        than sliding to whichever printer happens to sort first.
/// @return the resolved printer id, or "" when the map holds no printer object.
static std::string find_active_printer_key(const json& config, const std::string& preferred = "") {
    if (!config.contains("printers") || !config["printers"].is_object()) {
        return "";
    }
    const json& printers = config["printers"];

    std::string active = preferred;
    if (config.contains("active_printer_id") && config["active_printer_id"].is_string()) {
        active = config["active_printer_id"].get<std::string>();
    }
    if (!active.empty() && printers.contains(active) && printers[active].is_object()) {
        return active;
    }

    for (const auto& [key, val] : printers.items()) {
        if (val.is_object()) {
            return key;
        }
    }
    return "";
}

/// Migration v19→v20: purge the null-node garbage that read-only probes through
/// Config::get_json() vivified into settings.json (#1129), and retire the
/// legacy top-level /led block for good.
///
/// Previously the /led → printers/<id>/leds fold lived in
/// LedController::load_config() and LedAutoState::load_config(), which run on
/// EVERY boot — so the probes there re-created the /led orphan each time and
/// erasing it would have been pointless. Doing the fold here (once, guarded by
/// the version number, using non-vivifying contains()) is what makes the
/// erase stick.
///
/// The /printer erase matters beyond tidiness: migrate_v3_to_v4() gates on
/// `config.contains("printer") && config["printer"].is_object()`, so a
/// resurrected /printer node would be split into a bogus printers/default entry
/// if that migration ever re-ran.
static void migrate_v19_to_v20(json& config) {
    // --- 1. Fold any real legacy /led values into the active printer ---
    //
    // Resolve the fold target exactly the way Config::init() resolves the active
    // printer, INCLUDING its "active_printer_id is empty or dangling → take the
    // first printer object" fallback — hence the shared helper. The migration
    // cannot defer to init()'s own resolution: that runs after
    // run_versioned_migrations(), by which point config_version is already 20
    // and this migration will never run again.
    const std::string active = find_active_printer_key(config);
    const bool have_target = !active.empty();
    bool folded = false;

    if (have_target && config.contains("led") && config["led"].is_object()) {
        const std::string base = "/printers/" + active + "/leds";
        std::vector<std::pair<std::string, std::string>> moves;
        for (const char* key :
             {"selected_strips", "last_color", "last_brightness", "last_white", "color_presets",
              "macro_devices", "led_on_at_start", "startup_brightness"}) {
            moves.emplace_back(std::string("/led/") + key, base + "/" + key);
        }
        for (const char* key : {"enabled", "mappings"}) {
            moves.emplace_back(std::string("/led/auto_state/") + key, base + "/auto_state/" + key);
        }
        // migrate_config_keys() skips (and drops) sources whose target already
        // holds a REAL value, so per-printer values already in place are never
        // clobbered — while a probe-vivified null target is correctly treated as
        // absent and gets overwritten by the legacy value.
        folded = migrate_config_keys(config, moves);
        if (folded) {
            spdlog::info("[Config] Migration v20: folded legacy /led into /printers/{}/leds",
                         active);
        }
    }

    // --- 2. Erase the orphan top-level nodes ---
    //
    // Only ever erase a node we have finished with. Erasing unconditionally
    // destroyed the whole /led block whenever the fold above was skipped (no
    // printers map at all, for instance) — the user's settings deleted with
    // nothing put in their place. If there is still something of value in there,
    // leave it: a later boot that can resolve a printer gets another chance.
    if (config.contains("led")) {
        if (folded || !has_any_value(config["led"])) {
            config.erase("led");
            spdlog::info("[Config] Migration v20: erased orphan top-level /led node");
        } else {
            spdlog::warn("[Config] Migration v20: keeping legacy /led — no printer to fold it "
                         "into yet");
        }
    }
    // /printer at this point is probe pollution: a real legacy /printer block was
    // already split out by migrate_v3_to_v4(). Erase it only when it truly holds
    // nothing, so an unexpected real one is never silently destroyed.
    if (config.contains("printer")) {
        if (!has_any_value(config["printer"])) {
            config.erase("printer");
            spdlog::info("[Config] Migration v20: erased orphan top-level /printer node");
        } else {
            spdlog::warn("[Config] Migration v20: keeping top-level /printer — it still holds "
                         "values");
        }
    }

    // --- 3. Strip the null leaves left behind by vivifying probes ---
    int removed = strip_null_leaves(config);
    if (removed > 0) {
        spdlog::info("[Config] Migration v20: removed {} null config leaf/leaves", removed);
    }
}

/// Split a '/'-separated relative config path into its segments.
static std::vector<std::string> split_config_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(path.substr(start));
            return parts;
        }
        parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
}

/// Walk a relative path without vivifying anything.
/// @return pointer to the node, or nullptr when any segment is missing or a
///         non-object stands where an object is needed.
static json* find_relative(json& node, const std::string& path) {
    json* cur = &node;
    for (const auto& segment : split_config_path(path)) {
        if (!cur->is_object()) {
            return nullptr;
        }
        const auto it = cur->find(segment);
        if (it == cur->end()) {
            return nullptr;
        }
        cur = &(*it);
    }
    return cur;
}

/// Walk a relative path, creating the intermediate objects. A non-object
/// standing in the way is replaced, since nothing can be stored beneath it.
static json& ensure_relative(json& node, const std::string& path) {
    const auto parts = split_config_path(path);
    json* cur = &node;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!cur->contains(parts[i]) || !(*cur)[parts[i]].is_object()) {
            (*cur)[parts[i]] = json::object();
        }
        cur = &(*cur)[parts[i]];
    }
    return (*cur)[parts.back()];
}

/// Erase the leaf at a relative path, then unwind any intermediate object the
/// erase left empty — so retiring /appearance/toolhead_style does not leave an
/// empty /appearance behind, while /detection survives because /detection/enabled
/// is still in it.
static void erase_relative(json& node, const std::string& path) {
    const auto parts = split_config_path(path);
    std::vector<json*> chain{&node};
    json* cur = &node;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!cur->is_object() || !cur->contains(parts[i])) {
            return;
        }
        cur = &(*cur)[parts[i]];
        chain.push_back(cur);
    }
    if (!cur->is_object()) {
        return;
    }
    cur->erase(parts.back());
    for (size_t i = chain.size(); i-- > 1;) {
        if (!chain[i]->is_object() || !chain[i]->empty()) {
            break;
        }
        chain[i - 1]->erase(parts[i - 1]);
    }
}

/// Copy a root-level value into every printer object, then retire the root key.
///
/// Used by migrate_v20_to_v21 for settings that were install-wide but describe
/// one machine. Giving every printer the old value is what keeps an upgraded
/// install behaving exactly as it did; printers the setting is meaningless on
/// simply carry an inert copy. A per-printer value already in place is never
/// overwritten.
///
/// @return number of printer objects that received a copy
static int fan_out_to_printers(json& config, const std::string& root_path,
                               const std::string& printer_path) {
    json* source = find_relative(config, root_path);
    if (source == nullptr || source->is_null()) {
        return 0;
    }
    if (!config.contains("printers") || !config["printers"].is_object()) {
        // No printers map: leave the root key alone rather than deleting the
        // value with nowhere to put it (cf. migrate_v19_to_v20's /led).
        return 0;
    }

    const json value = *source;
    int copied = 0;
    int printers_seen = 0;
    for (auto& [key, printer] : config["printers"].items()) {
        // The printers map is MIXED — `show_printer_switcher` is a plain bool
        // sibling of the printer objects, so only objects count as printers.
        if (!printer.is_object()) {
            continue;
        }
        ++printers_seen;
        json* existing = find_relative(printer, printer_path);
        if (existing != nullptr && !existing->is_null()) {
            continue;
        }
        ensure_relative(printer, printer_path) = value;
        ++copied;
    }

    // Only retire the root key once it has somewhere to live. The shipped
    // template's printers map holds nothing but `show_printer_switcher`, so a
    // fresh install really can reach here with no printer to fan out into;
    // erasing then would destroy the setting outright.
    if (printers_seen == 0) {
        return 0;
    }
    erase_relative(config, root_path);
    return copied;
}

/// The four leaves of a printer's legacy scanner/ node.
static constexpr const char* SCANNER_LEAVES[] = {"usb_vendor_product", "usb_device_name",
                                                 "bt_address", "keymap"};

/// Collapse the per-printer scanner/ nodes into one root-level /scanner.
///
/// The active printer's values are the ones kept — a barcode scanner is plugged
/// into the host, so at most one of the stored copies ever described real
/// hardware, and the active printer's is the copy the user last configured.
/// Every printer's node is then dropped, including the ones whose values were
/// not taken. That loss is intended: N divergent copies cannot become one
/// without discarding N-1 of them.
static void collapse_scanner_to_root(json& config) {
    const std::string active = find_active_printer_key(config);

    std::vector<std::string> taken;
    if (!active.empty()) {
        json* source = find_relative(config, "printers/" + active + "/scanner");
        if (source != nullptr && source->is_object()) {
            for (const char* leaf : SCANNER_LEAVES) {
                const auto it = source->find(leaf);
                if (it == source->end() || it->is_null()) {
                    continue;
                }
                const json value = *it;
                json& destination = ensure_relative(config, std::string("scanner/") + leaf);
                if (destination.is_null()) {
                    destination = value;
                    taken.emplace_back(leaf);
                }
            }
        }
    }

    int dropped = 0;
    if (config.contains("printers") && config["printers"].is_object()) {
        for (auto& [key, printer] : config["printers"].items()) {
            if (!printer.is_object()) {
                continue;
            }
            if (printer.erase("scanner") > 0) {
                ++dropped;
            }
        }
    }

    if (!taken.empty()) {
        std::string list;
        for (const auto& leaf : taken) {
            list += (list.empty() ? "" : ", ") + leaf;
        }
        spdlog::info("[Config] Migration v21: took scanner settings from active printer '{}' ({})",
                     active, list);
    }
    if (dropped > 0) {
        spdlog::info("[Config] Migration v21: dropped per-printer scanner node from {} printer(s)",
                     dropped);
    }
}

/// Migration v20→v21: store four settings in the scope that matches what they
/// actually describe.
///
/// * /appearance/toolhead_style and /detection/policy_u1 describe one machine,
///   so they move under /printers/<id>/ and fan out to EVERY printer — that is
///   what keeps an existing install looking and behaving identically after the
///   upgrade.
/// * scanner/* describes a USB or Bluetooth device attached to the host running
///   HelixScreen, not to any printer, so it moves to the root.
/// * /console/filter_user_{add,remove} stay exactly where they are. They became
///   the global layer of a two-layer read (a per-printer layer now sits beside
///   them), so no data moves.
static void migrate_v20_to_v21(json& config) {
    const int toolhead_copies =
        fan_out_to_printers(config, "appearance/toolhead_style", "appearance/toolhead_style");
    if (toolhead_copies > 0) {
        spdlog::info("[Config] Migration v21: copied /appearance/toolhead_style to {} printer(s)",
                     toolhead_copies);
    }

    const int policy_copies =
        fan_out_to_printers(config, "detection/policy_u1", "detection/policy_u1");
    if (policy_copies > 0) {
        spdlog::info("[Config] Migration v21: copied /detection/policy_u1 to {} printer(s)",
                     policy_copies);
    }

    collapse_scanner_to_root(config);
}

/// Lift a legacy root-level "preset" marker into the active printer's node.
///
/// The marker predates multi-printer support and stayed at the config root while
/// every other piece of printer configuration moved under /printers/<id>/. With
/// two printers configured the second one's preset overwrote the first one's, and
/// consumers of Config::get_preset() — the panel-widget seed loader, the wizard's
/// step collapsing — then read a marker belonging to the wrong machine (#1162).
///
/// Deliberately NOT part of the versioned chain. scripts/lib/installer/
/// printer_seed.sh writes the root key with setdefault() on --update as well as
/// on first install, i.e. into configs whose config_version is already stamped at
/// CURRENT_CONFIG_VERSION, so a `version < N` gate would never see those. This
/// runs on every boot instead; the contains() checks make it a no-op once lifted.
///
/// Ordering: init() calls this AFTER run_versioned_migrations(), so
/// migrate_v12_to_v13() and migrate_v14_to_v15() still read the root key raw for
/// their AD5X detection before it is moved away.
///
/// @return true if the config was modified
static bool lift_root_preset(json& config, const std::string& active_printer_id) {
    if (!config.contains("preset")) {
        return false;
    }
    // Only ever erase a node we have finished with (cf. migrate_v19_to_v20). With
    // no printer to lift into, leave the root key so a later boot that can resolve
    // one gets another chance.
    if (active_printer_id.empty() || !config.contains("printers") ||
        !config["printers"].is_object() || !config["printers"].contains(active_printer_id) ||
        !config["printers"][active_printer_id].is_object()) {
        return false;
    }

    const json& root = config["preset"];
    const std::string name = root.is_string() ? root.get<std::string>() : "";
    if (name.empty()) {
        config.erase("preset");
        spdlog::debug("[Config] Dropped empty root-level preset marker");
        return true;
    }

    json& printer = config["printers"][active_printer_id];
    const auto it = printer.find("preset");
    const bool already_set =
        it != printer.end() && it->is_string() && !it->get<std::string>().empty();
    if (already_set) {
        spdlog::debug("[Config] Printer '{}' already has preset '{}'; discarding root-level '{}'",
                      active_printer_id, it->get<std::string>(), name);
    } else {
        printer["preset"] = name;
        spdlog::info("[Config] Lifted root-level preset '{}' into printer '{}'", name,
                     active_printer_id);
    }
    config.erase("preset");
    return true;
}

/// Run all versioned migrations in sequence from current version to CURRENT_CONFIG_VERSION
static void run_versioned_migrations(json& config, const std::string& config_path = "") {
    int version = 0;
    if (config.contains("config_version")) {
        version = config["config_version"].get<int>();
    }

    // A config written by a NEWER build than this one — reachable as soon as
    // update channels are user-switchable, since moving from the devel channel
    // back to stable installs an older binary over a newer config.
    //
    // Every gate below is `version < N`, so none of them would fire; the damage
    // is the unconditional stamp at the end, which would rewrite config_version
    // DOWN to ours. The newer build would then re-run migrations it had already
    // applied, against data already in the new shape. Leave the document alone
    // instead: unknown keys are read-through-default everywhere, and
    // Config::save() serializes the whole in-memory document, so the newer
    // build's settings survive a round trip through this one untouched.
    if (version > CURRENT_CONFIG_VERSION) {
        spdlog::warn("[Config] config_version {} was written by a newer build (this build "
                     "understands {}) — leaving the document unmigrated and unstamped",
                     version, CURRENT_CONFIG_VERSION);
        return;
    }

    if (version < 1)
        migrate_v0_to_v1(config);
    if (version < 2)
        migrate_v1_to_v2(config);
    if (version < 3)
        migrate_v2_to_v3(config);
    if (version < 4)
        migrate_v3_to_v4(config);
    if (version < 5)
        migrate_v4_to_v5(config);
    if (version < 6)
        migrate_v5_to_v6(config);
    if (version < 7)
        migrate_v6_to_v7(config);
    if (version < 8)
        migrate_v7_to_v8(config);
    if (version < 9)
        migrate_v8_to_v9(config);
    if (version < 10)
        migrate_v9_to_v10(config);
    if (version < 11)
        migrate_v10_to_v11(config);
    if (version < 12)
        migrate_v11_to_v12(config);
    if (version < 13)
        migrate_v12_to_v13(config);
    if (version < 14)
        migrate_v13_to_v14(config, config_path);
    if (version < 15)
        migrate_v14_to_v15(config);
    if (version < 16)
        migrate_v15_to_v16(config);
    if (version < 17)
        migrate_v16_to_v17(config);
    if (version < 18)
        migrate_v17_to_v18(config);
    if (version < 19)
        migrate_v18_to_v19(config);
    if (version < 20)
        migrate_v19_to_v20(config);
    if (version < 21)
        migrate_v20_to_v21(config);

    config["config_version"] = CURRENT_CONFIG_VERSION;
}

/// Default root-level config - shared between init() and reset_to_defaults()
/// @param moonraker_host Host address for printer
/// @param include_user_prefs Include user preference fields (brightness, sounds, etc.)
json get_default_config(const std::string& moonraker_host, bool include_user_prefs) {
    // log_level intentionally absent - test_mode provides fallback to DEBUG
    std::string printer_id = "default";
    json printer_data = get_default_printer_config(moonraker_host);

    json config = {{"config_version", CURRENT_CONFIG_VERSION},
                   {"active_printer_id", printer_id},
                   {"log_path", "/tmp/helixscreen.log"},
                   {"dark_mode", true},
                   {"theme", {{"preset", 0}}},
                   {"display", get_default_display_config()},
                   {"gcode_viewer", {{"shading_model", "phong"}, {"tube_sides", 4}}},
                   {"input",
                    {{"scroll_throw", 25},
                     {"scroll_limit", 10},
                     {"long_press_time", 500},
                     {"jitter_threshold", 5},
                     {"touch_device", ""},
                     {"calibration",
                      {{"valid", false},
                       {"a", 1.0},
                       {"b", 0.0},
                       {"c", 0.0},
                       {"d", 0.0},
                       {"e", 1.0},
                       {"f", 0.0}}}}},
                   {"printers", {{"show_printer_switcher", false}, {printer_id, printer_data}}}};

    if (include_user_prefs) {
        config["brightness"] = 80;
        config["sounds_enabled"] = false;
        config["completion_alert"] = true;
        config["wizard_completed"] = false;
        config["wifi_expected"] = false;
        config["language"] = "en";
    }

    return config;
}

using helix::config_backup::find_backup;
using helix::config_backup::restore_from_backup;
using helix::config_backup::write_rolling_backup;

/// Whether the rolling-backup tiers apply to this run.
///
/// The backup tiers (/var/lib/helixscreen/, $HOME/.helixscreen/) belong to the
/// REAL printer config.  Test mode declares its own config — config/settings-test.json
/// — and must stay there, so the production tiers are off-limits in both
/// directions: reading them would restore a stale real config over a missing
/// test config, and writing them would clobber the user's rolling backup with
/// test data.  A missing test config falls back to normal defaults instead.
static bool backups_enabled() {
#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
    auto* rt = get_runtime_config();
    if (rt && rt->is_test_mode())
        return false;
#endif
    return true;
}

/// Backup search paths in priority order (primary, fallback, legacy primary, legacy fallback).
/// Used by restore_from_backup() and find_backup() calls throughout init().
/// Empty in test mode — see backups_enabled().
static std::vector<std::string> config_backup_search_paths() {
    if (!backups_enabled())
        return {};
    return {config_backup_primary(), config_backup_fallback(), legacy_config_backup_primary(),
            legacy_config_backup_fallback()};
}

/// Env-file backup search paths in priority order (primary, fallback).
/// Empty in test mode — see backups_enabled().
static std::vector<std::string> env_backup_search_paths() {
    if (!backups_enabled())
        return {};
    return {env_backup_primary(), env_backup_fallback()};
}

/// Shared recovery for a document Config::init() could not use as-is (either
/// a JSON parse failure or a present-but-unreadable file): preserve it for
/// diagnosis, try the backup chain, and fall back to defaults if nothing
/// usable is found. Callers are expected to have already logged the
/// specific cause (parse error vs. read error) before calling this, since
/// that distinction matters for telemetry but not for the recovery itself.
static void recover_config_from_backup_or_defaults(json& data, ConfigStorage& storage) {
    // Preserve the corrupt document for diagnosis
    storage.preserve_corrupt();

    // Try restoring from backup before falling back to defaults
    std::string backup_src = find_backup(config_backup_search_paths());

    bool restored = false;
    if (!backup_src.empty()) {
        try {
            // ifstream, not fstream: fstream's default openmode is in|out, so a
            // read-only backup fails to open and parses as empty input.
            data = json::parse(std::ifstream(backup_src));
            restored = true;
            spdlog::info("[Config] Restored from backup: {}", backup_src);
            NOTIFY_WARNING("Settings were corrupted — restored from backup");
        } catch (const json::exception& e2) {
            spdlog::warn("[Config] Backup also corrupt: {}", e2.what());
        }
    }

    if (!restored) {
        spdlog::warn("[Config] No valid backup — resetting to defaults");
        data = get_default_config("127.0.0.1", false);
        NOTIFY_ERROR("Settings were corrupted and could not be recovered — reset to defaults");
    }
}

} // namespace

Config::Config() {}

Config* Config::get_instance() {
    if (instance == nullptr) {
        instance = new Config();
    }
    return instance;
}

// HELIX_CONFIG_DIR override: redirect settings into a caller-chosen
// directory. Lets a read-only baseline install (e.g., the cosmos .ipk
// under /usr/share/helixscreen) persist user settings into a writable
// path — typically ~/printer_data/config/helixscreen — without first
// requiring the in-app updater to relocate the install itself. The env
// var supplies the DIRECTORY; we keep the caller's filename so the
// settings.json / settings-test.json distinction is preserved.
std::string Config::resolve_path(const std::string& config_path) {
    const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
    if (env_dir == nullptr || env_dir[0] == '\0')
        return config_path;
    return (fs::path(env_dir) / fs::path(config_path).filename()).string();
}

void Config::init(const std::string& config_path) {
    std::string resolved_path = resolve_path(config_path);
    // Keyed off the env var, not off resolved_path != config_path: pointing
    // HELIX_CONFIG_DIR at the default "config" resolves to the same string and
    // must still get the directory created.
    if (const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
        env_dir != nullptr && env_dir[0] != '\0') {
        std::error_code ec;
        fs::path base(env_dir);
        fs::create_directories(base, ec);
        if (fs::is_directory(base, ec)) {
            spdlog::info("[Config] HELIX_CONFIG_DIR override: using {}", resolved_path);
        } else {
            spdlog::warn("[Config] HELIX_CONFIG_DIR={} unusable ({}); falling back to {}", env_dir,
                         ec ? ec.message() : "not a directory", config_path);
            resolved_path = config_path;
        }
    }
    path = resolved_path;
    struct stat buffer;

    // Migration: rename helixconfig.json -> settings.json if old name exists
    fs::path old_config = fs::path(path).parent_path() / "helixconfig.json";
    if (stat(path.c_str(), &buffer) != 0 && fs::exists(old_config) && !fs::is_symlink(old_config)) {
        spdlog::info("[Config] Migrating {} -> {}", old_config.string(), path);
        std::error_code ec;
        fs::rename(old_config, path, ec);
        if (ec) {
            spdlog::warn("[Config] Migration rename failed: {} — trying copy", ec.message());
            try {
                fs::copy_file(old_config, path);
                fs::remove(old_config);
                spdlog::info("[Config] Migration complete (copy+remove)");
            } catch (const fs::filesystem_error& e) {
                spdlog::error("[Config] Migration failed: {}", e.what());
            }
        } else {
            spdlog::info("[Config] Migration complete");
        }
    } else if (stat(path.c_str(), &buffer) != 0 && fs::is_symlink(old_config)) {
        // Old config is a symlink (Pi/SonicPad: points to printer_data).
        // Don't rename symlinks — the installer handles that. Just use the
        // symlink path directly so we read/write the user's real config.
        if (stat(old_config.c_str(), &buffer) == 0) {
            path = old_config.string();
            spdlog::info("[Config] {} is a symlink — using it directly, "
                         "installer will migrate on next update",
                         old_config.string());
        } else {
            spdlog::warn("[Config] {} is a dangling symlink", old_config.string());
        }
    } else if (stat(path.c_str(), &buffer) == 0 && fs::exists(old_config)) {
        spdlog::warn("[Config] Both settings.json and helixconfig.json exist; "
                     "using settings.json (old file left in place)");
    }

    // Migrate test config unconditionally (has its own existence guard)
    fs::path old_test = fs::path(path).parent_path() / "helixconfig-test.json";
    fs::path new_test = fs::path(path).parent_path() / "settings-test.json";
    if (fs::exists(old_test) && !fs::exists(new_test)) {
        std::error_code ec;
        fs::rename(old_test, new_test, ec);
        if (!ec) {
            spdlog::info("[Config] Migrated test config: {} -> {}", old_test.string(),
                         new_test.string());
        } else {
            spdlog::warn("[Config] Test config migration failed: {}", ec.message());
        }
    }

    // Migration: Check for legacy config at old location (helixconfig.json in app root)
    // If new location doesn't exist but old location does, migrate it
    // Note: use `path` (not `config_path`) — may have been redirected to symlink above
    if (stat(path.c_str(), &buffer) != 0) {
        // Config doesn't exist - check for legacy locations
        const std::vector<std::string> legacy_paths = {
            "helixconfig.json",                  // Old location (app root)
            "/opt/helixscreen/helixconfig.json", // Legacy embedded install
        };

        for (const auto& legacy_path : legacy_paths) {
            if (stat(legacy_path.c_str(), &buffer) == 0) {
                spdlog::info("[Config] Found legacy config at {}, migrating to {}", legacy_path,
                             path);

                // Ensure config/ directory exists
                fs::path config_dir = fs::path(path).parent_path();
                if (!config_dir.empty() && !fs::exists(config_dir)) {
                    fs::create_directories(config_dir);
                }

                // Copy legacy config to new location, then remove old file
                try {
                    fs::copy_file(legacy_path, path);
                    // Remove legacy file to avoid confusion
                    fs::remove(legacy_path);
                    spdlog::info("[Config] Migration complete: {} -> {} (old file removed)",
                                 legacy_path, path);
                } catch (const fs::filesystem_error& e) {
                    spdlog::warn("[Config] Migration failed: {}", e.what());
                    // Fall through to create default config
                }
                break;
            }
        }

        // Recovery: restore config from rolling backups if missing.
        // Backups are maintained by Config::save() and survive Moonraker's
        // shutil.rmtree() wipe of the install directory.
        restore_from_backup(path, "Config", config_backup_search_paths());
    }

    // Restore helixscreen.env independently — it can be lost even if config survived
    {
        std::string env_path = (fs::path(path).parent_path() / "helixscreen.env").string();
        restore_from_backup(env_path, "helixscreen.env", env_backup_search_paths());
    }

    ensure_storage();
    bool config_modified = false;

    // A thrown load() means the document is present but unreadable (e.g.
    // permission denied) — distinct from "absent" (nullopt, no throw). Both
    // cases route into the "load existing config" branch below so a
    // present-but-unreadable config gets the same corrupt-preserve +
    // backup-restore recovery as a parse failure, instead of being silently
    // treated as first-boot and reset to defaults.
    std::optional<std::string> loaded_doc;
    bool load_read_failed = false;
    std::string load_read_error;
    try {
        loaded_doc = storage_->load();
    } catch (const std::exception& e) {
        load_read_failed = true;
        load_read_error = e.what();
    }

    if (loaded_doc || load_read_failed) {
        // Load existing config
        spdlog::info("[Config] Loading config from {}", path);

        if (load_read_failed) {
            // Route directly into the shared recovery path rather than
            // re-throwing into the json::parse try/catch below — that would
            // require widening its catch to std::exception, which would
            // also swallow an unrelated failure (e.g. bad_alloc under
            // memory pressure on RAM-constrained embedded targets) and
            // misdiagnose it as document corruption, renaming a perfectly
            // healthy settings.json to .corrupt.
            spdlog::error("[Config] Failed to read {}: {}", path, load_read_error);
            CONFIG_RECORD_ERROR("file_io", "config_read_failed",
                                fmt::format("read error: {}", load_read_error));
            recover_config_from_backup_or_defaults(data, *storage_);
            config_modified = true;
        } else {
            try {
                data = json::parse(*loaded_doc);

                // Detect tarball default that replaced user config during a Moonraker
                // web update.  Moonraker type:web does rmtree() on the install dir and
                // extracts the release tarball fresh — the tarball includes a preset-based
                // settings.json with wizard_completed=false and no config_version.  If a
                // rolling backup with real user data exists, prefer it.
                // safe_int, not .value(): a hand-edited "config_version": null throws
                // type_error.302, which lands in the catch below and destroys the
                // user's settings.json (renamed .corrupt, then reset to defaults)
                // over a single bad field.
                //
                // The packaged document alone cannot say which of the two
                // happened: a fresh install ships the identical bytes, and
                // neither the backup's age (the archive's stored mtime is the
                // release build date, newer than the backup in both cases) nor
                // its richness differs between them.  The installer settles it.
                // It leaves FRESH_INSTALL_MARKER beside settings.json whenever
                // it kept the packaged config because no user config existed to
                // restore; Moonraker's rmtree() removes the marker and the
                // re-extract does not bring it back, since it is not in the
                // archive.  Consumed here so it only ever answers for the
                // config it shipped beside.
                if (helix::json_util::safe_int(data, "config_version", 0) == 0) {
                    const fs::path fresh_marker =
                        fs::path(path).parent_path() / AppConstants::Update::FRESH_INSTALL_MARKER;
                    std::error_code marker_ec;
                    const bool installer_kept_it = fs::exists(fresh_marker, marker_ec);
                    if (installer_kept_it) {
                        spdlog::info("[Config] Packaged config kept - installer marked a fresh "
                                     "install ({})",
                                     fresh_marker.string());
                        fs::remove(fresh_marker, marker_ec);
                    }

                    std::string backup_src = installer_kept_it
                                                 ? std::string{}
                                                 : find_backup(config_backup_search_paths());
                    if (!backup_src.empty()) {
                        try {
                            auto backup_data = json::parse(std::ifstream(backup_src));
                            if (helix::json_util::safe_int(backup_data, "config_version", 0) > 0) {
                                spdlog::warn("[Config] Loaded config is a tarball default "
                                             "(no config_version) — restoring from backup: {}",
                                             backup_src);
                                data = std::move(backup_data);
                                config_modified = true;
                                NOTIFY_WARNING("Settings restored after update");
                            }
                        } catch (const json::exception& e) {
                            spdlog::warn(
                                "[Config] Backup parse failed during tarball detection: {}",
                                e.what());
                        }
                    }
                }
            } catch (const json::exception& e) {
                spdlog::error("[Config] Failed to parse {}: {}", path, e.what());
                CONFIG_RECORD_ERROR("file_io", "config_read_failed",
                                    fmt::format("parse error: {}", e.what()));
                recover_config_from_backup_or_defaults(data, *storage_);
                config_modified = true;
            }
        }

        // The migrations below get their own try/catch, deliberately separate
        // from the parse recovery above.
        //
        // They used to sit outside every handler: Config::init() has no other
        // try, and neither does its caller Application::init_config(), so a
        // single null field anywhere in a migration threw straight out of app
        // startup. But they must NOT share the parse handler either — that one
        // renames settings.json to .corrupt and resets to factory defaults,
        // which is far too destructive a response to a migration bug. A failed
        // migration should leave the user's config un-migrated and loudly
        // logged, not discarded. config_version is left unstamped, so the
        // migration is retried on the next boot.
        try {
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
            // Pass path so v13→v14 can find the legacy telemetry_config.json sidecar.
            int version_before = helix::json_util::safe_int(data, "config_version", 0);
            run_versioned_migrations(data, path);
            // safe_int, not data["config_version"] — operator[] on the non-const
            // `data` VIVIFIES a null if a migration failed to stamp the version,
            // and .get<int>() then throws on it (#1129 is the same hazard).
            if (helix::json_util::safe_int(data, "config_version", 0) != version_before) {
                config_modified = true;
            }
        } catch (const std::exception& e) {
            spdlog::error("[Config] Migration failed, continuing with un-migrated config: {}",
                          e.what());
            CONFIG_RECORD_ERROR("migration", "config_migration_failed",
                                fmt::format("migration error: {}", e.what()));
        }
    } else {
        // Create default config
        spdlog::info("[Config] Creating default config at {}", path);
        data = get_default_config("127.0.0.1", false);
        config_modified = true;
    }

    // Ensure printers map exists
    if (!data.contains("printers") || !data["printers"].is_object()) {
        data["printers"] = {{"default", get_default_printer_config("127.0.0.1")}};
        data["active_printer_id"] = "default";
        config_modified = true;
    }

    // Load the active printer ID from config (must happen before df() is used),
    // falling back to the first real printer when the stored id is empty or
    // dangling.
    if (refresh_active_printer_id()) {
        config_modified = true;
    }

    // Ensure active printer has required fields with defaults
    if (!active_printer_id_.empty()) {
        auto printer_ptr = json::json_pointer("/printers/" + active_printer_id_);
        auto& printer = data[printer_ptr];
        if (printer.is_null()) {
            data[printer_ptr] = get_default_printer_config("127.0.0.1");
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
                // Check if there's a legacy strip value to migrate. Read it
                // through the non-vivifying accessor — get_json()/operator[]
                // would leave a permanent "leds/strip": null behind (#1129).
                const json* strip = try_get_json(df() + "leds/strip");
                std::string led =
                    (strip != nullptr && strip->is_string()) ? strip->get<std::string>() : "";
                leds_selected = led.empty() ? json::array() : json::array({led});
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
    }

    // Move a legacy root-level preset marker under the active printer. Must run
    // after the active printer has been resolved and its node ensured, and after
    // run_versioned_migrations() — see lift_root_preset().
    if (lift_root_preset(data, active_printer_id_)) {
        config_modified = true;
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
                         {"long_press_time", 500},
                         {"jitter_threshold", 5},
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
            input["jitter_threshold"] = 5;
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

    // Probe for read-only storage before attempting any writes.
    read_only_mode_ = storage_->read_only();
    if (read_only_mode_) {
        spdlog::warn("[Config] Read-only storage ({}): config changes will not be persisted",
                     storage_->describe());
    }

    // Save updated config with any new defaults or migrations.
    // Goes through save() for the temp-file + fsync + rename path: this runs on
    // first boot and on the first boot after any upgrade that adds a migration,
    // so a power cut here would otherwise truncate the live settings.json.
    if (config_modified && !read_only_mode_) {
        if (save()) {
            spdlog::debug("[Config] Saved updated config to {}", path);
        } else {
            spdlog::error("[Config] Failed to persist migrated config to {}", path);
        }
    }

    // Maintain a rolling backup on startup — ensures backup freshness even if
    // the user never explicitly saves settings.  Skip when the loaded config is
    // a tarball default (wizard not yet completed, no real user data) to avoid
    // poisoning the backup with preset defaults that would break future recovery.
    if (backups_enabled() && !is_wizard_required()) {
        write_rolling_backup(path, config_backup_primary(), config_backup_fallback());
    }

    // Back up helixscreen.env outside install dir (env only changes at startup via launcher)
    if (backups_enabled()) {
        std::string env_path = (fs::path(path).parent_path() / "helixscreen.env").string();
        write_rolling_backup(env_path, env_backup_primary(), env_backup_fallback());
    }

    spdlog::debug("[Config] initialized: moonraker={}:{}",
                  get<std::string>(df() + "moonraker_host", "127.0.0.1"),
                  get<int>(df() + "moonraker_port", 7125));
}

std::string Config::df() const {
    if (active_printer_id_.empty()) {
        spdlog::warn("[Config] df() called with no active printer, using 'default'");
        return "/printers/default/";
    }
    return "/printers/" + active_printer_id_ + "/";
}

// ============================================================================
// Multi-printer support
// ============================================================================

std::string Config::get_active_printer_id() const {
    return active_printer_id_;
}

bool Config::refresh_active_printer_id() {
    const std::string resolved = find_active_printer_key(data, active_printer_id_);
    active_printer_id_ = resolved;

    if (resolved.empty()) {
        // No printer object anywhere in the map — leave /active_printer_id as
        // it stands rather than persisting a value df() cannot route to.
        return false;
    }
    if (data.contains("active_printer_id") && data["active_printer_id"].is_string() &&
        data["active_printer_id"].get<std::string>() == resolved) {
        return false;
    }

    data["active_printer_id"] = resolved;
    spdlog::info("[Config] Auto-selected active printer: {}", resolved);
    return true;
}

bool Config::set_active_printer(const std::string& printer_id) {
    if (!data.contains("printers") || !data["printers"].contains(printer_id) ||
        !data["printers"][printer_id].is_object()) {
        spdlog::error("[Config] Cannot switch to unknown printer '{}'", printer_id);
        return false;
    }
    active_printer_id_ = printer_id;
    data["active_printer_id"] = printer_id;
    spdlog::info("[Config] Switched active printer to '{}'", printer_id);
    return true;
}

std::vector<std::string> Config::get_printer_ids() const {
    std::vector<std::string> ids;
    if (data.contains("printers") && data["printers"].is_object()) {
        for (auto& [key, val] : data["printers"].items()) {
            if (!val.is_object())
                continue;
            ids.push_back(key);
        }
    }
    return ids;
}

void Config::add_printer(const std::string& printer_id, const json& printer_data) {
    if (!data.contains("printers")) {
        data["printers"] = json::object();
    }
    data["printers"][printer_id] = printer_data;
    spdlog::info("[Config] Added printer '{}'", printer_id);
}

void Config::remove_printer(const std::string& printer_id) {
    // is_object(), not just contains(): the printers map also holds plain
    // settings keys (show_printer_switcher, _show_printer_switcher_comment),
    // and erasing one of those on a mistyped id would silently drop a setting.
    if (!data.contains("printers") || !data["printers"].is_object() ||
        !data["printers"].contains(printer_id) || !data["printers"][printer_id].is_object()) {
        spdlog::warn("[Config] Cannot remove non-existent printer '{}'", printer_id);
        return;
    }

    // Prevent removing the last printer. Count printer objects rather than
    // map entries — with a single printer plus show_printer_switcher, size()
    // reports 2 and this guard would wave the last printer through.
    size_t printer_count = 0;
    for (const auto& [key, val] : data["printers"].items()) {
        if (val.is_object()) {
            printer_count++;
        }
    }
    if (printer_count <= 1) {
        spdlog::error("[Config] Cannot remove last printer '{}' — at least one printer must exist",
                      printer_id);
        return;
    }

    data["printers"].erase(printer_id);
    spdlog::info("[Config] Removed printer '{}'", printer_id);

    // If we just removed the active printer, switch to the first remaining one.
    // find_active_printer_key() skips the non-printer keys; taking
    // data["printers"].begin() instead would hand back "show_printer_switcher"
    // for any printer id sorting after it, and df() would then index a bool.
    if (active_printer_id_ == printer_id) {
        const std::string remaining_id = find_active_printer_key(data);
        if (remaining_id.empty()) {
            // Unreachable while the count guard above holds; keep the stale id
            // rather than persisting an empty one if it ever is reached.
            spdlog::error("[Config] Removed active printer '{}' with no printer left to switch to",
                          printer_id);
            return;
        }
        active_printer_id_ = remaining_id;
        data["active_printer_id"] = remaining_id;
        spdlog::info("[Config] Auto-switched to printer '{}' after removing '{}'", remaining_id,
                     printer_id);
    }
}

void Config::archive_printer(const std::string& printer_id) {
    if (!data.contains("printers") || !data["printers"].is_object() ||
        !data["printers"].contains(printer_id) || !data["printers"][printer_id].is_object()) {
        spdlog::warn("[Config] Cannot archive non-existent printer '{}'", printer_id);
        return;
    }

    // Snapshot before remove_printer() erases it. remove_printer() may decline
    // (last printer standing), so only keep the archive if the erase happened.
    json snapshot = data["printers"][printer_id];
    remove_printer(printer_id);

    if (data["printers"].contains(printer_id)) {
        return;
    }

    snapshot[ARCHIVED_AT_KEY] = next_archive_stamp();
    data["removed_printers"][printer_id] = std::move(snapshot);
    spdlog::info("[Config] Archived printer '{}' to /removed_printers", printer_id);
    prune_archived_printers();
}

int64_t Config::next_archive_stamp() const {
    int64_t stamp = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());

    // Force the stamp strictly past every existing one. Wall-clock seconds are
    // too coarse to order two archives in the same second, and a device whose
    // clock steps backwards (no RTC until NTP lands — common on these boards)
    // would otherwise stamp a new entry older than the ones it must outlive.
    if (data.contains("removed_printers") && data["removed_printers"].is_object()) {
        for (const auto& [key, val] : data["removed_printers"].items()) {
            const int64_t existing = helix::json_util::safe_int64(val, ARCHIVED_AT_KEY, 0);
            if (existing >= stamp) {
                stamp = existing + 1;
            }
        }
    }
    return stamp;
}

void Config::prune_archived_printers() {
    if (!data.contains("removed_printers") || !data["removed_printers"].is_object()) {
        return;
    }
    json& archive = data["removed_printers"];
    if (archive.size() <= MAX_ARCHIVED_PRINTERS) {
        return;
    }

    // Oldest first. Entries written before the stamp existed read as 0 and so
    // are pruned ahead of any stamped entry; the key breaks ties between them
    // so the order is deterministic rather than dependent on map layout.
    std::vector<std::pair<int64_t, std::string>> by_age;
    by_age.reserve(archive.size());
    for (const auto& [key, val] : archive.items()) {
        by_age.emplace_back(helix::json_util::safe_int64(val, ARCHIVED_AT_KEY, 0), key);
    }
    std::sort(by_age.begin(), by_age.end());

    const size_t drop_count = by_age.size() - MAX_ARCHIVED_PRINTERS;
    for (size_t i = 0; i < drop_count; i++) {
        spdlog::info("[Config] Pruned archived printer '{}' (keeping {} most recent)",
                     by_age[i].second, MAX_ARCHIVED_PRINTERS);
        archive.erase(by_age[i].second);
    }
}

std::string Config::slugify(const std::string& name) {
    std::string result;
    result.reserve(name.size());

    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            // Replace non-alphanumeric with hyphen
            if (!result.empty() && result.back() != '-') {
                result += '-';
            }
        }
    }

    // Strip trailing hyphens
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }

    // Strip leading hyphens
    size_t start = 0;
    while (start < result.size() && result[start] == '-') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    return result.empty() ? "default" : result;
}

std::string Config::get_path() {
    return path;
}

void Config::log_type_mismatch(const std::string& json_ptr, const char* stored_type,
                               const char* expected_type, const char* detail) {
    // warn, not debug: the user's setting was silently discarded, and the only
    // way they can act on it is by seeing which key and which type to correct.
    spdlog::warn("[Config] '{}' is stored as {} but must be a {} - ignoring it and using the "
                 "built-in default ({})",
                 json_ptr, stored_type, expected_type, detail);
}

void Config::log_set_failed(const std::string& json_ptr, const char* detail) {
    spdlog::warn("[Config] Could not store '{}' - the setting will not persist ({})", json_ptr,
                 detail);
}

json& Config::get_json(const std::string& json_path) {
    return data[json::json_pointer(json_path)];
}

const json* Config::try_get_json(const std::string& json_path) const {
    json::json_pointer ptr(json_path);
    if (!data.contains(ptr)) {
        return nullptr;
    }
    return &data.at(ptr);
}

std::vector<std::string> Config::get_string_array(const std::string& json_path) const {
    std::vector<std::string> out;
    const json* node = try_get_json(json_path);
    if (node == nullptr || !node->is_array()) {
        return out;
    }
    out.reserve(node->size());
    for (const auto& element : *node) {
        if (element.is_string()) {
            out.push_back(element.get<std::string>());
        }
    }
    return out;
}

void Config::ensure_storage() {
    // An injected backend (set_storage) is the caller's and is never rebuilt.
    // An auto-created one is a FileConfigStorage over `path`, so describe() is
    // that path — when they diverge, `path` has moved and the old backend would
    // keep writing to the file it was built for.
    if (storage_ && !(storage_is_default_ && storage_->describe() != path)) {
        return;
    }
    storage_ = make_file_config_storage(path);
    storage_is_default_ = true;
}

bool Config::save() {
    if (path.empty()) {
        spdlog::trace("[Config] Skipping save (no config path set)");
        return true;
    }

    if (read_only_mode_) {
        spdlog::warn("[Config] Skipping save — filesystem is read-only");
        return false;
    }

    spdlog::trace("[Config] Saving config to {}", storage_ ? storage_->describe() : path);

    ensure_storage();

    // Serialization is inside the handler: operator<< dumps with
    // error_handler_t::strict, which throws json::type_error 316 on invalid
    // UTF-8 in any stored string, and the stream buffer can throw bad_alloc on
    // a RAM-constrained target. save() has 133 call sites, many inside LVGL
    // event callbacks, where an escaping exception unwinds through a C frame.
    try {
        std::ostringstream oss;
        oss << std::setw(2) << data << std::endl;
        if (!storage_->store(oss.str())) {
            // FileConfigStorage (the default backend) already reports the specific
            // failure via NOTIFY_ERROR + CONFIG_RECORD_ERROR at the failing phase
            // (open/write/rename/exception) — don't double-toast here. Non-file
            // backends get at least this log line.
            spdlog::error("[Config] Failed to save via {}", storage_->describe());
            return false;
        }
    } catch (const std::exception& e) {
        NOTIFY_ERROR("Failed to save configuration: {}", e.what());
        LOG_ERROR_INTERNAL("Exception while saving config to {}: {}", path, e.what());
        CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                            fmt::format("exception: {}", e.what()));
        return false;
    }
    spdlog::trace("[Config] saved successfully to {}", storage_->describe());

    // Rolling backup outside the install dir (survives Moonraker wipes). Kept
    // here rather than inside the storage backend: which tiers are writable and
    // whether this document is worth preserving are Config-level policy, and
    // the backend's contract is only to move bytes durably. Same guard as the
    // startup backup in init() — a wizard-incomplete config holds preset
    // defaults, not user data, and must never overwrite the one good recovery
    // copy.
    if (backups_enabled() && !is_wizard_required()) {
        write_rolling_backup(path, config_backup_primary(), config_backup_fallback());
    }
    return true;
}

bool Config::is_read_only() const {
    return read_only_mode_;
}

bool Config::has_preset() const {
    return !get_preset().empty();
}

std::string Config::get_preset() const {
    // Per-printer, alongside every other piece of printer configuration. There is
    // deliberately no root-level fallback: a printer with no preset of its own
    // must read empty, not inherit whichever marker another printer left at the
    // root. init() lifts legacy root-level markers into the active printer, so
    // by the time anything calls this the value is where it belongs.
    const json* node = try_get_json(df() + "preset");
    if (node != nullptr && node->is_string()) {
        return node->get<std::string>();
    }
    return "";
}

void Config::set_preset(const std::string& preset_name) {
    if (preset_name.empty()) {
        return;
    }
    data[json::json_pointer(df() + "preset")] = preset_name;
    spdlog::info("[Config] Preset set to '{}' for printer '{}'", preset_name, active_printer_id_);
}

void Config::clear_preset() {
    bool cleared = false;
    if (data.contains(json::json_pointer(df() + "preset"))) {
        // df() ends in '/', which as a JSON pointer would name an empty-string
        // key inside the printer node rather than the node itself.
        std::string printer_path = df();
        printer_path.pop_back();
        data[json::json_pointer(printer_path)].erase("preset");
        cleared = true;
    }
    // Drop any legacy root-level marker too. Leaving it would let lift_root_preset()
    // put the preset straight back on the next boot, silently undoing the wizard
    // re-run path in application.cpp that calls this to restore the full wizard.
    if (data.contains("preset")) {
        data.erase("preset");
        cleared = true;
    }
    if (cleared) {
        spdlog::info("[Config] Preset marker cleared for printer '{}'", active_printer_id_);
    }
}

namespace {

/// True when HelixScreen is running on the same machine as the Moonraker at
/// `host` — i.e. this really is the printer's own embedded screen.
///
/// An absent/empty host reads as on-device, and that is deliberate: no preset
/// file carries `moonraker_host`, so a factory tarball whose baked settings.json
/// IS a preset can reach the merge with the key missing. An unset host means
/// "nobody has pointed us at a remote printer yet", not "we are remote".
bool preset_targets_this_device(const std::string& moonraker_host) {
#if defined(HELIX_SPLASH_ONLY) || defined(HELIX_WATCHDOG)
    // Neither binary can reach this: PrinterDetector is the only caller of
    // apply_preset_file() and is excluded from both object lists, and
    // host_identity.o is not linked into either. Keep the symbol out rather
    // than grow two size-sensitive embedded binaries for dead weight.
    (void)moonraker_host;
    return false;
#else
    return helix::is_moonraker_on_same_host(moonraker_host);
#endif
}

} // namespace

bool Config::apply_preset_file(const std::string& preset_name) {
    // Guard: only full-apply if wizard hasn't been completed for this printer.
    // Post-wizard, still allow a narrow migration for filament_sensors so that
    // a printer detected with an empty filament_sensors block (preset never
    // populated it at first-install, or preset was extended later) gets the
    // current preset's runout/toolhead role assignments. Without this, fixing
    // a preset only helps fresh installs — existing users stay broken even
    // after an update.
    const bool wizard_done = get<bool>(df() + "wizard_completed", false);
    if (wizard_done) {
        // Post-wizard migration window. Two cases:
        //  A) filament_sensors.sensors is empty/missing → seed from preset
        //     (original installs whose old preset didn't write the block).
        //  B) Block exists but the preset has been updated to assign RUNOUT
        //     to sensors that the user's stored copy still has at "none" —
        //     stale role=none from a prior preset version. Upgrade those
        //     specific sensors. User-edited role=runout entries are never
        //     downgraded; role=none entries the preset also wants at none
        //     are left alone.
        if (active_printer_id_.empty()) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        std::string preset_relpath = std::string("presets/") + preset_name + ".json";
        std::string preset_path = helix::find_readable(preset_relpath);
        if (!fs::exists(preset_path)) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        json preset_json;
        try {
            preset_json = json::parse(std::ifstream(preset_path));
        } catch (const json::exception&) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        if (!preset_json.contains("printer") || !preset_json["printer"].is_object() ||
            !preset_json["printer"].contains("filament_sensors")) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        const auto& preset_fs = preset_json["printer"]["filament_sensors"];

        json::json_pointer fs_ptr(df() + "filament_sensors");
        json::json_pointer sensors_ptr(df() + "filament_sensors/sensors");

        // Case A: empty/missing block — full seed.
        if (!data.contains(sensors_ptr) ||
            (data.at(sensors_ptr).is_array() && data.at(sensors_ptr).empty())) {
            auto& printer_node = data["printers"][active_printer_id_];
            if (!printer_node.is_object()) {
                printer_node = json::object();
            }
            printer_node["filament_sensors"] = preset_fs;
            spdlog::info("[Config] Migrated filament_sensors from preset '{}' "
                         "(existing block was empty)",
                         preset_name);
            save();
            return true;
        }

        // Case B: per-sensor role-upgrade from "none" → preset's role.
        if (!preset_fs.contains("sensors") || !preset_fs["sensors"].is_array()) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        if (!data.at(sensors_ptr).is_array()) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        int upgraded = 0;
        auto& user_sensors = data.at(sensors_ptr);
        for (const auto& preset_sensor : preset_fs["sensors"]) {
            if (!preset_sensor.is_object())
                continue;
            std::string preset_klipper = preset_sensor.value("klipper_name", "");
            std::string preset_role = preset_sensor.value("role", "none");
            if (preset_klipper.empty() || preset_role == "none")
                continue;
            bool found = false;
            for (auto& user_sensor : user_sensors) {
                if (!user_sensor.is_object())
                    continue;
                if (user_sensor.value("klipper_name", "") != preset_klipper)
                    continue;
                found = true;
                std::string user_role = user_sensor.value("role", "none");
                // Only upgrade when user has role=none — never overwrite an
                // explicit user assignment (runout/toolhead/entry/z_probe).
                if (user_role == "none") {
                    user_sensor["role"] = preset_role;
                    ++upgraded;
                    spdlog::info("[Config] Upgraded sensor '{}' role: none -> {} (preset '{}')",
                                 preset_klipper, preset_role, preset_name);
                }
                break;
            }
            // Sensor in preset but missing entirely from user settings → append.
            if (!found) {
                user_sensors.push_back(preset_sensor);
                ++upgraded;
                spdlog::info("[Config] Added missing sensor '{}' from preset '{}'", preset_klipper,
                             preset_name);
            }
        }
        if (upgraded > 0) {
            save();
            return true;
        }
        spdlog::info("[Config] Wizard completed, preset '{}' already applied (no upgrades)",
                     preset_name);
        return false;
    }

    // Resolve via find_readable so the writable user dir wins, then fall back
    // to the shipped read-only seed bundle at $HELIX_DATA_DIR/assets/config/presets/.
    // The seed location is where install tarballs land presets — looking only in
    // the writable config dir would miss every preset on a fresh install.
    std::string preset_relpath = std::string("presets/") + preset_name + ".json";
    std::string preset_path = helix::find_readable(preset_relpath);
    if (!fs::exists(preset_path)) {
        spdlog::warn("[Config] Preset file not found: {} (looked in writable + seed bundle)",
                     preset_path);
        return false;
    }

    // Load and parse preset JSON
    json preset_json;
    try {
        preset_json = json::parse(std::ifstream(preset_path));
    } catch (const json::exception& e) {
        spdlog::error("[Config] Failed to parse preset '{}': {}", preset_path, e.what());
        return false;
    }

    // Deep-merge the "printer" section (hardware, fans, heaters, input, etc.) into
    // the active printer subtree. Guarded by wizard_completed above, so this only
    // runs on fresh-install / pre-wizard state — safe to seed scaffolded hardware
    // defaults from the preset without a hardcoded allowlist.
    //
    // EXCEPTION: connection settings are deployment-specific and are owned by the
    // Connection wizard step, which runs BEFORE this preset is applied and saves the
    // user's real Moonraker host/port. Model presets hardcode "moonraker_host":
    // "127.0.0.1", so merging them here clobbered the user's entered IP and made
    // HelixScreen connect to localhost on the next restart. Strip those keys so the
    // preset can never overwrite them.
    if (preset_json.contains("printer") && preset_json["printer"].is_object() &&
        !active_printer_id_.empty()) {
        json patch = preset_json["printer"];
        patch.erase("moonraker_host");
        patch.erase("moonraker_port");
        patch.erase("moonraker_api_key");
        auto& printer_node = data["printers"][active_printer_id_];
        if (!printer_node.is_object()) {
            printer_node = json::object();
        }
        printer_node.merge_patch(patch);
    }

    // The device-level "display" and "input" blocks below describe the PRINTER'S
    // OWN PANEL — rotation and white balance in one, the touch calibration matrix
    // and jitter threshold in the other. Seeding them is correct only when
    // HelixScreen is the thing driving that panel. A separate host that merely
    // talks to the printer over the network (a Pi with its own touchscreen that
    // detected a Centauri Carbon during the wizard, say) would otherwise come up
    // rotated with a foreign touch matrix — and `display.rotation_probed: true`
    // then suppresses the first-boot rotation probe that would have corrected it.
    //
    // The per-printer "printer" merge above stays unconditional: that block is
    // about the printer, which is equally true from across the network.
    //
    // Both call paths have the host by this point. The wizard persists it in the
    // Connection step (3), which runs before PrinterIdentify (4) applies the
    // preset; auto-detection can only run once a connection using that same key
    // succeeded.
    const std::string moonraker_host = get<std::string>(df() + "moonraker_host", "");
    const bool on_this_device = preset_targets_this_device(moonraker_host);

    if (!on_this_device) {
        spdlog::info("[Config] Preset '{}': Moonraker host '{}' is not this machine — "
                     "leaving device display/input settings alone",
                     preset_name, moonraker_host);
    }

    // Deep-merge device-level display settings (preserves keys not in preset)
    if (on_this_device && preset_json.contains("display") && preset_json["display"].is_object()) {
        if (!data.contains("display") || !data["display"].is_object()) {
            data["display"] = json::object();
        }
        data["display"].merge_patch(preset_json["display"]);
    }

    // Deep-merge device-level input settings (preserves keys not in preset).
    // This seeds top-level /input/* — e.g. touch calibration (read from
    // /input/calibration/*) and jitter_threshold — which is distinct from the
    // per-printer "printer.input" block above. Pre-wizard only (guarded by
    // wizard_completed), so it's safe to seed scaffolded defaults.
    if (on_this_device && preset_json.contains("input") && preset_json["input"].is_object()) {
        if (!data.contains("input") || !data["input"].is_object()) {
            data["input"] = json::object();
        }
        data["input"].merge_patch(preset_json["input"]);
    }

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
    // Populate per-printer `type` from the database entry whose `preset` field matches.
    // Without this, the home panel's image widget has no printer_type to look up and
    // falls back to the generic CoreXY image. Only the main app applies presets at
    // runtime — splash/watchdog just read existing config — so PrinterDetector (which
    // pulls in the full printer database) is excluded from those binaries.
    std::string type_key = df() + helix::wizard::PRINTER_TYPE;
    if (get<std::string>(type_key, "").empty()) {
        std::string type_name = PrinterDetector::get_name_for_preset(preset_name);
        if (!type_name.empty()) {
            set<std::string>(type_key, type_name);
            spdlog::info("[Config] Preset '{}' resolved to printer type '{}'", preset_name,
                         type_name);
        } else {
            spdlog::warn("[Config] No database entry matches preset '{}' — printer type unset",
                         preset_name);
        }
    }
#endif

    spdlog::info("[Config] Applied preset '{}' to active printer", preset_name);
    save();
    return true;
}

bool Config::is_wizard_required() {
    // Check per-printer wizard_completed first (v3 config)
    if (!active_printer_id_.empty()) {
        json::json_pointer printer_ptr(df() + "wizard_completed");
        if (data.contains(printer_ptr)) {
            auto& wc = data[printer_ptr];
            if (wc.is_boolean()) {
                bool is_completed = wc.get<bool>();
                spdlog::trace("[Config] Per-printer wizard_completed = {}", is_completed);
                return !is_completed;
            }
        }
    }

    // Fall back to root-level wizard_completed (backward compat)
    json::json_pointer ptr("/wizard_completed");
    if (data.contains(ptr)) {
        auto& wizard_completed = data[ptr];
        if (wizard_completed.is_boolean()) {
            bool is_completed = wizard_completed.get<bool>();
            spdlog::trace("[Config] Root wizard_completed flag = {}", is_completed);
            return !is_completed;
        }
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

    // The defaults carry their own printer map (keyed "default"), so any
    // previously active id is now dangling — df() would route at a node that
    // does not exist and vivify it on the next set(). Callers that schedule a
    // restart never notice; the ones that stay live would.
    refresh_active_printer_id();

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
