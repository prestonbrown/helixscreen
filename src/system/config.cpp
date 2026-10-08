// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include "completion_alert_mode.h"

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
#include "system/telemetry_manager.h"
#define CONFIG_RECORD_ERROR(...) TelemetryManager::instance().record_error(__VA_ARGS__)
#else
#define CONFIG_RECORD_ERROR(...) ((void)0)
#endif
#include "ui_error_reporting.h"

#include "app_constants.h"
#include "config_backup.h"
#include "config_migrations.h"
#include "config_testing.h"
#include "data_root_resolver.h"
#include "helix_fs.h"
#include "host_identity.h"
#include "input_defaults.h"
#include "json_utils.h"
#include "printer_detector.h"
#include "runtime_config.h"
#include "text_io.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sys/stat.h>

namespace hfs = helix::fs;

using namespace helix;

namespace tio = helix::text_io;

using namespace helix::config_detail;

using AppConstants::Update::config_backup_fallback;
using AppConstants::Update::config_backup_primary;
using AppConstants::Update::env_backup_fallback;
using AppConstants::Update::env_backup_primary;
using AppConstants::Update::legacy_config_backup_fallback;
using AppConstants::Update::legacy_config_backup_primary;

Config* Config::instance{NULL};

namespace helix {

const char* const kDefaultCooldownGcode = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                                          "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0";

bool is_default_cooldown_gcode(const std::string& gcode) {
    return gcode == kDefaultCooldownGcode;
}

} // namespace helix

namespace helix::config_detail {

/// The unescaped reference tokens of @p ptr, or nullopt where
/// json::json_pointer's constructor would throw: a non-empty pointer without a
/// leading '/', or a '~' not followed by '0' or '1'.
static std::optional<std::vector<std::string>> pointer_tokens(const std::string& ptr) {
    std::vector<std::string> tokens;
    if (ptr.empty()) {
        return tokens;
    }
    if (ptr[0] != '/') {
        return std::nullopt;
    }
    std::string token;
    for (size_t i = 1; i <= ptr.size(); ++i) {
        if (i == ptr.size() || ptr[i] == '/') {
            tokens.push_back(std::move(token));
            token.clear();
        } else if (ptr[i] == '~') {
            if (i + 1 >= ptr.size() || (ptr[i + 1] != '0' && ptr[i + 1] != '1')) {
                return std::nullopt;
            }
            token += ptr[i + 1] == '0' ? '~' : '/';
            ++i;
        } else {
            token += ptr[i];
        }
    }
    return tokens;
}

/// An array index token as nlohmann accepts one: "0", or digits with no
/// leading zero, fitting size_t. "-" (one past the end) is not an index.
static std::optional<size_t> array_token_index(const std::string& token) {
    if (token.empty() || token[0] < '0' || token[0] > '9' ||
        (token.size() > 1 && token[0] == '0')) {
        return std::nullopt;
    }
    return tio::parse_int<size_t>(token);
}

/// The node @p root[json_pointer(ptr)] would assign to, creating missing
/// objects on the way, or nullptr where nlohmann would throw: a malformed
/// pointer, a path through a scalar, or an array token that is not an index.
/// @p why names the refusal. The whole path is checked before any of it is
/// created, so a refusal leaves @p root untouched.
json* node_for_write(json& root, const std::string& ptr, const char** why) {
    const auto tokens = pointer_tokens(ptr);
    if (!tokens) {
        *why = "malformed JSON pointer";
        return nullptr;
    }
    // Past the first missing component everything is created fresh, and
    // nlohmann can always create that.
    const json* node = &root;
    for (const auto& token : *tokens) {
        if (node->is_null()) {
            break;
        }
        if (node->is_object()) {
            const auto it = node->find(token);
            if (it == node->end()) {
                break;
            }
            node = &*it;
        } else if (node->is_array()) {
            const auto idx = array_token_index(token);
            if (!idx) {
                *why = "not an index into an array";
                return nullptr;
            }
            if (*idx >= node->size()) {
                break;
            }
            node = &(*node)[*idx];
        } else {
            *why = "a path component holds a value, not an object";
            return nullptr;
        }
    }
    return &root[json::json_pointer(ptr)];
}

} // namespace helix::config_detail

namespace {

/// Default macro configuration - shared between init() and reset_to_defaults()
json get_default_macros() {
    return {{"load_filament", {{"label", "Load"}, {"gcode", "LOAD_FILAMENT"}}},
            {"unload_filament", {{"label", "Unload"}, {"gcode", "UNLOAD_FILAMENT"}}},
            {"macro_1", {{"label", "Clean Nozzle"}, {"gcode", "HELIX_CLEAN_NOZZLE"}}},
            {"macro_2", {{"label", "Bed Level"}, {"gcode", "HELIX_BED_MESH_IF_NEEDED"}}},
            {"cooldown", kDefaultCooldownGcode}};
}

/// default_macros text a shipped preset used to persist in an earlier
/// release, keyed by preset name then macro key. apply_preset_file()'s
/// post-wizard migration replaces a stored macro with the preset's CURRENT
/// text only when it byte-matches an entry here, so a user's own edit is
/// never touched and an already-migrated printer is a no-op.
const std::map<std::string, std::map<std::string, std::string>>& stale_default_macro_text() {
    static const std::map<std::string, std::map<std::string, std::string>> table = {
        {"k2",
         {{"cooldown", "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                       "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0\n"
                       "SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=0"}}},
    };
    return table;
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
        {"motion",
         {{"jog_speed_xy", 6000},
          {"jog_speed_z", 600},
          {"show_actual_position", false},
          {"bed_map_clearance", 5},
          {"fine_inner", 0.1f},
          {"fine_outer", 1.0f},
          {"coarse_inner", 1.0f},
          {"coarse_outer", 10.0f},
          {"turbo_inner", 10.0f},
          {"turbo_outer", 50.0f}}},
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
    } else if (!data["display"].is_object()) {
        spdlog::warn("[Config] /display is not an object - leaving display settings unmigrated");
        return false;
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
        } else if (!data["display"]["calibration"].is_object()) {
            spdlog::warn("[Config] /display/calibration is not an object - leaving touch "
                         "calibration unmigrated");
            return true;
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

} // namespace

namespace helix::config_detail {

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
        // A target built from a printer id carries whatever the id holds.
        if (!pointer_tokens(from_path) || !pointer_tokens(to_path)) {
            spdlog::warn("[Config] Migration of {} -> {} skipped: malformed path", from_path,
                         to_path);
            continue;
        }
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

        // Copy value to new location (creating its parents) and remove from old
        json value = data.at(from_ptr);
        const char* why = nullptr;
        json* target = node_for_write(data, to_path, &why);
        if (target == nullptr) {
            spdlog::warn("[Config] Migration of {} -> {} skipped: {}", from_path, to_path, why);
            continue;
        }
        *target = std::move(value);
        erase_at_pointer(data, from_ptr);
        spdlog::info("[Config] Migrated {} -> {}", from_path, to_path);
        any_migrated = true;
    }

    return any_migrated;
}

} // namespace helix::config_detail

namespace {

/// Config::init()'s display-migration step, as one callable unit.
///
/// Both halves always run: an already-/display/-shaped config still needs its
/// touch keys moved to /input/, so the second migration must NOT be gated on the
/// first one having found anything.
///
/// @param data JSON config data to migrate (modified in place)
/// @return true if either migration changed @p data
bool run_display_migrations(json& data) {
    bool changed = migrate_display_config(data);
    if (migrate_config_keys(data, {{"/display/calibration", "/input/calibration"},
                                   {"/display/touch_device", "/input/touch_device"}})) {
        changed = true;
    }
    return changed;
}

} // end anonymous namespace

namespace helix::config_testing {
bool run_display_migrations_for_test(nlohmann::json& data) {
    return ::run_display_migrations(data);
}
} // namespace helix::config_testing

namespace helix::config_detail {

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
std::string find_active_printer_key(const json& config, const std::string& preferred) {
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

} // namespace helix::config_detail

namespace {

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
                   {"gcode_viewer", {{"tube_sides", 4}}},
                   {"input",
                    {{"scroll_throw", helix::input_defaults::SCROLL_THROW},
                     {"scroll_limit", 10},
                     {"long_press_time", 500},
                     {"touch_device", ""},
                     {"calibration",
                      {{"valid", false},
                       {"a", 1.0},
                       {"b", 0.0},
                       {"c", 0.0},
                       {"d", 0.0},
                       {"e", 1.0},
                       {"f", 0.0}}},
                     // The evdev ABS range and axis swap a three-point calibration
                     // solved for (#1259, #1276). valid=false means "use whatever
                     // range the kernel declared", which is what every install did
                     // before this key existed - so no migration is needed and an
                     // uncalibrated device behaves exactly as it always has.
                     {"touch_range",
                      {{"valid", false},
                       {"swap_axes", false},
                       {"min_x", 0},
                       {"max_x", 0},
                       {"min_y", 0},
                       {"max_y", 0}}}}},
                   {"printers", {{"show_printer_switcher", false}, {printer_id, printer_data}}}};

    if (include_user_prefs) {
        config["brightness"] = 80;
        config["sounds_enabled"] = false;
        config["completion_alert"] = static_cast<int>(helix::CompletionAlertMode::ALERT);
        config["wizard_completed"] = false;
        config["wifi_expected"] = false;
        config["language"] = "en";
    }

    return config;
}

/// Whether a document holds only keys the installer seeded: no config_version,
/// no single /printer, and no printer object under /printers. That is a fresh
/// install, not a config to migrate - the chain would treat the seeded keys as
/// old data and leave the defaults they lack (log_path, gcode_viewer) unset.
static bool is_installer_seed_document(const json& config) {
    if (helix::json_util::safe_int(config, "config_version", 0) != 0) {
        return false;
    }
    if (config.contains("printer") && config["printer"].is_object()) {
        return false;
    }
    const auto printers = config.find("printers");
    if (printers != config.end() && printers->is_object()) {
        for (const auto& [key, value] : printers->items()) {
            if (value.is_object()) {
                return false;
            }
        }
    }
    return true;
}

using helix::config_backup::find_backup;
using helix::config_backup::remove_backups;
using helix::config_backup::restore_from_backup;
using helix::config_backup::write_backup_file;
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
        json backup = json::parse(tio::read_file(backup_src).value_or(""), nullptr, false);
        if (backup.is_object()) {
            data = std::move(backup);
            restored = true;
            spdlog::info("[Config] Restored from backup: {}", backup_src);
            NOTIFY_WARNING("Settings were corrupted — restored from backup");
        } else {
            spdlog::warn("[Config] Backup also corrupt: {}", backup_src);
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
    return hfs::join_path(env_dir, hfs::filename(config_path));
}

/// Copy what must survive @p old_doc being replaced by defaults into @p fresh:
/// the active printer's Moonraker connection, so an install whose printer is on
/// another host still reaches it, and /update/channel, so an install the
/// installer put on beta does not fall back to stable.
static void carry_across_migration_floor(const json& old_doc, json& fresh) {
    if (const auto update = old_doc.find("update");
        update != old_doc.end() && update->is_object()) {
        const auto channel = update->find("channel");
        if (channel != update->end() && channel->is_number_integer()) {
            fresh["update"]["channel"] = *channel;
        }
    }

    const json* printer = nullptr;
    if (const auto printers = old_doc.find("printers");
        printers != old_doc.end() && printers->is_object()) {
        const auto it = printers->find(find_active_printer_key(old_doc));
        if (it != printers->end()) {
            printer = &*it;
        }
    } else if (const auto single = old_doc.find("printer");
               single != old_doc.end() && single->is_object()) {
        printer = &*single;
    }
    if (printer == nullptr) {
        return;
    }
    json& target =
        fresh["printers"][helix::json_util::safe_string(fresh, "active_printer_id", "default")];
    for (const char* key : {"moonraker_host", "moonraker_port", "moonraker_api_key"}) {
        const auto it = printer->find(key);
        if (it != printer->end() && !it->is_null()) {
            target[key] = *it;
        }
    }
}

void Config::init(const std::string& config_path) {
    std::string resolved_path = resolve_path(config_path);
    // Keyed off the env var, not off resolved_path != config_path: pointing
    // HELIX_CONFIG_DIR at the default "config" resolves to the same string and
    // must still get the directory created.
    if (const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
        env_dir != nullptr && env_dir[0] != '\0') {
        if (hfs::create_directories(env_dir)) {
            spdlog::info("[Config] HELIX_CONFIG_DIR override: using {}", resolved_path);
        } else {
            spdlog::warn("[Config] HELIX_CONFIG_DIR={} unusable ({}); falling back to {}", env_dir,
                         std::strerror(errno), config_path);
            resolved_path = config_path;
        }
    }
    path = resolved_path;
    struct stat buffer;

    // A Pi/SonicPad install may still reach its config through a
    // helixconfig.json symlink into printer_data. Use it directly; the
    // installer renames it on the next update.
    const std::string old_config = hfs::join_path(hfs::parent_path(path), "helixconfig.json");
    if (stat(path.c_str(), &buffer) != 0 && hfs::is_symlink(old_config)) {
        if (stat(old_config.c_str(), &buffer) == 0) {
            path = old_config;
            spdlog::info("[Config] {} is a symlink — using it directly, "
                         "installer will migrate on next update",
                         old_config);
        } else {
            spdlog::warn("[Config] {} is a dangling symlink", old_config);
        }
    }

    if (stat(path.c_str(), &buffer) != 0) {
        // Recovery: restore config from rolling backups if missing.
        // Backups are maintained by Config::save() and survive Moonraker's
        // shutil.rmtree() wipe of the install directory.
        restore_from_backup(path, "Config", config_backup_search_paths());
    }

    // Restore helixscreen.env independently — it can be lost even if config survived
    {
        std::string env_path = hfs::join_path(hfs::parent_path(path), "helixscreen.env");
        restore_from_backup(env_path, "helixscreen.env", env_backup_search_paths());
    }

    ensure_storage();
    bool config_modified = false;

    // Probe for read-only storage before attempting any writes.
    read_only_mode_ = storage_->read_only();
    if (read_only_mode_) {
        spdlog::warn("[Config] Read-only storage ({}): config changes will not be persisted",
                     storage_->describe());
    }

    // A read error means the document is present but unreadable (e.g.
    // permission denied), distinct from "absent" (nullopt, no error). Both
    // route into the "load existing config" branch below so a
    // present-but-unreadable config gets the same corrupt-preserve +
    // backup-restore recovery as a parse failure, instead of being silently
    // treated as first-boot and reset to defaults.
    std::optional<std::string> loaded_doc;
    // True while `data` is the document parsed from `path`, whatever put it
    // there (a backup restored onto a missing file counts).
    bool data_is_on_disk_doc = false;
    std::string load_read_error;
    loaded_doc = storage_->load(load_read_error);
    const bool load_read_failed = !loaded_doc && !load_read_error.empty();

    if (loaded_doc || load_read_failed) {
        // Load existing config
        spdlog::info("[Config] Loading config from {}", path);

        if (load_read_failed) {
            spdlog::error("[Config] Failed to read {}: {}", path, load_read_error);
            CONFIG_RECORD_ERROR("file_io", "config_read_failed",
                                fmt::format("read error: {}", load_read_error));
            recover_config_from_backup_or_defaults(data, *storage_);
            config_modified = true;
        } else if (json parsed = json::parse(*loaded_doc, nullptr, false); !parsed.is_object()) {
            // Everything downstream indexes the document by key, so a
            // well-formed array or scalar is as unusable as a syntax error.
            const char* problem = parsed.is_discarded() ? "not valid JSON" : "not a JSON object";
            spdlog::error("[Config] Failed to parse {}: {}", path, problem);
            CONFIG_RECORD_ERROR("file_io", "config_read_failed",
                                fmt::format("parse error: {}", problem));
            data_is_on_disk_doc = false;
            recover_config_from_backup_or_defaults(data, *storage_);
            config_modified = true;
        } else {
            data = std::move(parsed);
            data_is_on_disk_doc = true;

            // Detect tarball default that replaced user config during a Moonraker
            // web update.  Moonraker type:web does rmtree() on the install dir and
            // extracts the release tarball fresh — the tarball includes a preset-based
            // settings.json with wizard_completed=false and no config_version.  If a
            // rolling backup with real user data exists, prefer it.
            // safe_int, not .value(): a hand-edited "config_version": null has
            // to read as 0 here, not fail the whole document over one field.
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
                const std::string fresh_marker = hfs::join_path(
                    hfs::parent_path(path), AppConstants::Update::FRESH_INSTALL_MARKER);
                const bool installer_kept_it = hfs::exists(fresh_marker);
                if (installer_kept_it) {
                    spdlog::info("[Config] Packaged config kept - installer marked a fresh "
                                 "install ({})",
                                 fresh_marker);
                    hfs::remove(fresh_marker);
                }

                std::string backup_src =
                    installer_kept_it ? std::string{} : find_backup(config_backup_search_paths());
                if (!backup_src.empty()) {
                    auto backup_data =
                        json::parse(tio::read_file(backup_src).value_or(""), nullptr, false);
                    if (!backup_data.is_object()) {
                        spdlog::warn("[Config] Backup parse failed during tarball detection: {}",
                                     backup_src);
                    } else if (helix::json_util::safe_int(backup_data, "config_version", 0) > 0) {
                        spdlog::warn("[Config] Loaded config is a tarball default "
                                     "(no config_version) — restoring from backup: {}",
                                     backup_src);
                        data = std::move(backup_data);
                        data_is_on_disk_doc = false;
                        config_modified = true;
                        NOTIFY_WARNING("Settings restored after update");
                    }
                }
            }
        }

        // A document holding only installer-seeded keys starts from the fresh
        // defaults, with the seeded keys laid over them. Its legacy display keys
        // move first: once the defaults fill /input/calibration, a calibration
        // still under /display/ has nowhere to go. Runs after the tarball
        // detection above, so a rolling backup still replaces such a document.
        if (is_installer_seed_document(data)) {
            spdlog::info("[Config] Config holds no printer and no version: starting from "
                         "defaults under its {} seeded key(s)",
                         data.size());
            run_display_migrations(data);
            json fresh = get_default_config("127.0.0.1", false);
            fresh.merge_patch(data);
            fresh["config_version"] = CURRENT_CONFIG_VERSION;
            data = std::move(fresh);
            config_modified = true;
        }

        // With exceptions, the migrations run under their own catch-all,
        // separate from the parse recovery above: that one renames
        // settings.json to .corrupt and resets to factory defaults, far too
        // destructive a response to a migration bug. A failed migration leaves
        // the config un-migrated and logged, with config_version unstamped so
        // it is retried on the next boot. Without exceptions (ESP32) there is
        // nothing to catch, so every step has to be non-throwing by itself.
#if defined(__cpp_exceptions)
        try {
#endif
            // Moves root-level display_* to /display/, then the touch keys from
            // /display/ to /input/. Shared with the config_testing seam so tests
            // drive this exact sequence instead of restating it.
            if (run_display_migrations(data)) {
                config_modified = true;
            }

            // Run versioned migrations (v0→v1: disable sounds for existing configs, etc.)
            // Pass path so v13→v14 can find the legacy telemetry_config.json sidecar.
            int version_before = helix::json_util::safe_int(data, "config_version", 0);
            // The save() after migrating also refreshes the rolling backup, so
            // without this copy nothing keeps the pre-upgrade document. It is a
            // fixed sibling of settings.json that no restore path searches, and
            // a file copy, so it applies only when the storage is that file.
            // A snapshot already at version_before is kept: the file on disk
            // may be a partly migrated document that still carries that
            // version, and the first copy is the original.
            // One generation only - the next migrating boot from a
            // different version overwrites it; keep a versioned name per
            // migration if older ones are wanted.
            //
            // A document below the migration floor is replaced by defaults, and
            // it may have come from a rolling backup rather than from path (a
            // corrupt settings.json, or a tarball default after a Moonraker web
            // update). The snapshot is then its only copy, so it is written
            // from memory whatever the source.
            const std::string snapshot = path + ".pre-migration";
            const bool below_floor =
                version_before > 0 && version_before < MIN_MIGRATABLE_CONFIG_VERSION;
            const bool copy_from_file = data_is_on_disk_doc && storage_->describe() == path;
            // A small partition cannot spare a second copy of settings.json for
            // a manual recovery nobody can perform there. A below-floor document
            // still gets one: it is about to be replaced and has no other copy.
            const bool small = storage_->small_footprint();
            if (small && !below_floor) {
                std::remove(snapshot.c_str());
            }
            bool snapshot_kept = false;
            if (version_before > 0 && version_before < CURRENT_CONFIG_VERSION &&
                ((copy_from_file && !small) || below_floor) && !read_only_mode_) {
                // Absent or unreadable parses as discarded, which reads as 0:
                // nothing worth keeping.
                const int snapshot_version = helix::json_util::safe_int(
                    json::parse(tio::read_file(snapshot).value_or(""), nullptr, false),
                    "config_version", 0);
                if (snapshot_version == version_before) {
                    spdlog::debug("[Config] Keeping existing v{} pre-migration copy: {}",
                                  version_before, snapshot);
                    snapshot_kept = true;
                } else if (copy_from_file
                               ? write_backup_file(path, snapshot)
                               : tio::write_file_atomic(
                                     snapshot, helix::json_util::safe_dump(data, small ? -1 : 2))) {
                    spdlog::info("[Config] Saved v{} config before migrating: {}", version_before,
                                 snapshot);
                    snapshot_kept = true;
                } else {
                    spdlog::warn("[Config] Could not save pre-migration copy to {}", snapshot);
                }
            }
            if (below_floor && !snapshot_kept && !read_only_mode_) {
                // Replacing it now would leave no copy anywhere. Left unmigrated
                // and unstamped, it is retried on the next boot.
                spdlog::error("[Config] config_version {} is older than this build migrates "
                              "(oldest: {}) and could not be copied to {}; leaving it unmigrated",
                              version_before, MIN_MIGRATABLE_CONFIG_VERSION, snapshot);
            } else if (below_floor) {
                spdlog::warn("[Config] config_version {} is older than this build migrates "
                             "(oldest: {}); starting from defaults, previous config kept at {}",
                             version_before, MIN_MIGRATABLE_CONFIG_VERSION, snapshot);
                json fresh = get_default_config("127.0.0.1", false);
                carry_across_migration_floor(data, fresh);
                data = std::move(fresh);
                config_modified = true;
            } else {
                helix::config_detail::run_versioned_migrations(data, path);
            }
            // safe_int, not data["config_version"] — operator[] on the non-const
            // `data` VIVIFIES a null if a migration failed to stamp the version,
            // and .get<int>() then throws on it (#1129 is the same hazard).
            if (helix::json_util::safe_int(data, "config_version", 0) != version_before) {
                config_modified = true;
            }
#if defined(__cpp_exceptions)
        } catch (const std::exception& e) {
            spdlog::error("[Config] Migration failed, continuing with un-migrated config: {}",
                          e.what());
            CONFIG_RECORD_ERROR("migration", "config_migration_failed",
                                fmt::format("migration error: {}", e.what()));
        }
#endif
    } else {
        // Create default config
        spdlog::info("[Config] Creating default config at {}", path);
        data = get_default_config("127.0.0.1", false);
        config_modified = true;
    }

    // Ensure the printers map holds a printer. A versionless document naming none
    // still reaches here with a printers object: normalize_versionless_document()
    // gives it /printers/show_printer_switcher, which is not a printer. A config
    // from a newer build is exempt: its printers may be in a shape this build
    // does not read, and it is left as written (see run_versioned_migrations).
    const bool from_newer_build =
        helix::json_util::safe_int(data, "config_version", 0) > CURRENT_CONFIG_VERSION;
    if (!from_newer_build) {
        if (!data.contains("printers") || !data["printers"].is_object()) {
            data["printers"] = json::object();
        }
        if (get_printer_ids().empty()) {
            data["printers"]["default"] = get_default_printer_config("127.0.0.1");
            data["active_printer_id"] = "default";
            config_modified = true;
        }
    }

    // Load the active printer ID from config (must happen before df() is used),
    // falling back to the first real printer when the stored id is empty or
    // dangling.
    if (refresh_active_printer_id()) {
        config_modified = true;
    }

    // Ensure active printer has required fields with defaults. Addressed by key,
    // not through a df() pointer: the id is a key of the printers object (made
    // an object above), whatever characters it holds.
    if (!active_printer_id_.empty()) {
        json& printer = data["printers"][active_printer_id_];
        if (printer.is_null()) {
            printer = get_default_printer_config("127.0.0.1");
            config_modified = true;
        } else if (printer.is_object()) {
            auto ensure = [&](const char* key, json value) {
                if (!printer.contains(key) || printer[key].is_null()) {
                    printer[key] = std::move(value);
                    config_modified = true;
                }
            };
            ensure("heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}});
            ensure("temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}});
            ensure("fans", {{"part", "fan"}, {"hotend", "heater_fan hotend_fan"}});
            ensure("leds", {{"strip", "neopixel chamber_light"}});

            // Ensure leds/selected array exists (for multi-LED support)
            json& leds = printer["leds"];
            if (leds.is_object() && (!leds.contains("selected") || leds["selected"].is_null())) {
                // Seed it from a legacy strip value when there is one.
                const auto strip = leds.find("strip");
                std::string led =
                    (strip != leds.end() && strip->is_string()) ? strip->get<std::string>() : "";
                leds["selected"] = led.empty() ? json::array() : json::array({led});
                config_modified = true;
            }

            ensure("extra_sensors", json::object());
            ensure("hardware", {{"optional", json::array()},
                                {"expected", json::array()},
                                {"last_snapshot", json::object()}});
            ensure("default_macros", get_default_macros());
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
    if (!data.contains("display") || !data["display"].is_object()) {
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
    if (!data.contains("input") || !data["input"].is_object()) {
        data["input"] = {{"scroll_throw", helix::input_defaults::SCROLL_THROW},
                         {"scroll_limit", 10},
                         {"long_press_time", 500},
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
            input["scroll_throw"] = helix::input_defaults::SCROLL_THROW;
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

        // Ensure calibration subsection exists with all required fields
        if (!input.contains("calibration") || !input["calibration"].is_object()) {
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
        std::string env_path = hfs::join_path(hfs::parent_path(path), "helixscreen.env");
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

std::string Config::next_printer_id() const {
    const auto existing = get_printer_ids();
    int counter = static_cast<int>(existing.size()) + 1;
    std::string id;
    do {
        id = "printer-" + std::to_string(counter++);
    } while (std::find(existing.begin(), existing.end(), id) != existing.end());
    return id;
}

std::string Config::find_printer_by_host(const std::string& host, int port) const {
    for (const auto& id : get_printer_ids()) {
        const std::string base = "/printers/" + id + "/";
        if (get<std::string>(base + "moonraker_host", "") == host &&
            get<int>(base + "moonraker_port", 7125) == port) {
            return id;
        }
    }
    return {};
}

std::string Config::get_printer_display_name(const std::string& printer_id,
                                             const std::string& fallback) const {
    const std::string base = "/printers/" + printer_id + "/";
    for (const char* key : {"printer_name", "type", "moonraker_host"}) {
        std::string value = get<std::string>(base + key, "");
        if (!value.empty()) {
            return value;
        }
    }
    return fallback;
}

std::string Config::get_active_printer_name() const {
    return get_printer_display_name(active_printer_id_, active_printer_id_);
}

void Config::add_printer(const std::string& printer_id, const json& printer_data) {
    if (!data.contains("printers")) {
        data["printers"] = json::object();
    } else if (!data["printers"].is_object()) {
        spdlog::error("[Config] Cannot add printer '{}': /printers is not an object", printer_id);
        return;
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
    if (printer_removed_hook_) {
        printer_removed_hook_(printer_id);
    }

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
    // The printer is already gone from /printers, so a malformed archive is
    // replaced rather than losing the snapshot too.
    if (!data.contains("removed_printers") || !data["removed_printers"].is_object()) {
        data["removed_printers"] = json::object();
    }
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
                               const char* expected_type) {
    // warn, not debug: the user's setting was silently discarded, and the only
    // way they can act on it is by seeing which key and which type to correct.
    spdlog::warn("[Config] '{}' is stored as {} but must be a {} - ignoring it and using the "
                 "built-in default",
                 json_ptr, stored_type, expected_type);
}

void Config::log_set_failed(const std::string& json_ptr, const char* detail) {
    spdlog::warn("[Config] Could not store '{}' - the setting will not persist ({})", json_ptr,
                 detail);
}

json* Config::create_path(const std::string& json_ptr) {
    const char* why = nullptr;
    json* node = node_for_write(data, json_ptr, &why);
    if (node == nullptr) {
        log_set_failed(json_ptr, why);
    }
    return node;
}

json& Config::get_json(const std::string& json_path) {
    if (json* node = create_path(json_path)) {
        return *node;
    }
    discarded_write_ = nullptr;
    return discarded_write_;
}

const json* Config::try_get_json(const std::string& json_path) const {
    const auto tokens = pointer_tokens(json_path);
    if (!tokens) {
        return nullptr;
    }
    const json* node = &data;
    for (const auto& token : *tokens) {
        if (node->is_object()) {
            const auto it = node->find(token);
            if (it == node->end()) {
                return nullptr;
            }
            node = &*it;
        } else if (node->is_array()) {
            const auto idx = array_token_index(token);
            if (!idx || *idx >= node->size()) {
                return nullptr;
            }
            node = &(*node)[*idx];
        } else {
            return nullptr;
        }
    }
    return node;
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

    // safe_dump() replaces invalid UTF-8 rather than throwing on it, so a stray
    // byte in a printer name or SSID costs those bytes and not the user's whole
    // save. With exceptions, the try catches what serialization can still throw
    // — bad_alloc on a RAM-constrained target — because save() has 133 call
    // sites, many inside LVGL event callbacks, where an escaping exception
    // unwinds through a C frame.
#if defined(__cpp_exceptions)
    try {
#endif
        if (!storage_->store(
                helix::json_util::safe_dump(data, storage_->small_footprint() ? -1 : 2) + "\n")) {
            // FileConfigStorage (the default backend) already reports the specific
            // failure via NOTIFY_ERROR + CONFIG_RECORD_ERROR at the failing phase
            // (open/write/rename/exception) — don't double-toast here. Non-file
            // backends get at least this log line.
            spdlog::error("[Config] Failed to save via {}", storage_->describe());
            return false;
        }
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        NOTIFY_ERROR("Failed to save configuration: {}", e.what());
        LOG_ERROR_INTERNAL("Exception while saving config to {}: {}", path, e.what());
        CONFIG_RECORD_ERROR("file_io", "config_write_failed",
                            fmt::format("exception: {}", e.what()));
        return false;
    }
#endif
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
    set(df() + "preset", preset_name);
    spdlog::info("[Config] Preset set to '{}' for printer '{}'", preset_name, active_printer_id_);
}

void Config::clear_preset() {
    bool cleared = false;
    const std::string id = active_printer_id_.empty() ? "default" : active_printer_id_;
    if (data.contains("printers") && data["printers"].is_object()) {
        const auto printer = data["printers"].find(id);
        if (printer != data["printers"].end() && printer->is_object() &&
            printer->erase("preset") > 0) {
            cleared = true;
        }
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
    // Post-wizard, still allow narrow migrations for an already-provisioned printer:
    // filament_sensors, default_macros, role keys the stored config never held
    // (fans/*, heaters/*, temp_sensors/*, seeded only when absent or empty), and a
    // hardware/expected union. Without this, fixing a preset only helps fresh
    // installs: existing users stay broken even after an update.
    const bool wizard_done = get<bool>(df() + "wizard_completed", false);
    if (wizard_done) {
        // The merge addresses the printer both through df() pointers and as
        // /printers/<id>, so both have to be usable.
        if (active_printer_id_.empty() || !pointer_tokens(df()) ||
            (data.contains("printers") && !data["printers"].is_object())) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        std::string preset_relpath = std::string("presets/") + preset_name + ".json";
        std::string preset_path = helix::find_readable(preset_relpath);
        if (!hfs::exists(preset_path)) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        json preset_json = json::parse(tio::read_file(preset_path).value_or(""), nullptr, false);
        if (preset_json.is_discarded()) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        if (!preset_json.contains("printer") || !preset_json["printer"].is_object()) {
            spdlog::info("[Config] Wizard completed, skipping preset '{}' merge", preset_name);
            return false;
        }
        const auto& preset_printer = preset_json["printer"];
        bool changed = false;

        // Migration 1: filament_sensors. Two cases:
        //  A) filament_sensors.sensors is empty/missing → seed from preset
        //     (original installs whose old preset didn't write the block).
        //  B) Block exists but the preset has been updated to assign RUNOUT
        //     to sensors that the user's stored copy still has at "none" —
        //     stale role=none from a prior preset version. Upgrade those
        //     specific sensors. User-edited role=runout entries are never
        //     downgraded; role=none entries the preset also wants at none
        //     are left alone.
        if (preset_printer.contains("filament_sensors")) {
            const auto& preset_fs = preset_printer["filament_sensors"];
            json::json_pointer sensors_ptr(df() + "filament_sensors/sensors");

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
                changed = true;
            } else if (preset_fs.contains("sensors") && preset_fs["sensors"].is_array() &&
                       data.at(sensors_ptr).is_array()) {
                auto& user_sensors = data.at(sensors_ptr);
                for (const auto& preset_sensor : preset_fs["sensors"]) {
                    if (!preset_sensor.is_object())
                        continue;
                    std::string preset_klipper =
                        helix::json_util::safe_string(preset_sensor, "klipper_name");
                    std::string preset_role =
                        helix::json_util::safe_string(preset_sensor, "role", "none");
                    if (preset_klipper.empty() || preset_role == "none")
                        continue;
                    bool found = false;
                    for (auto& user_sensor : user_sensors) {
                        if (!user_sensor.is_object())
                            continue;
                        if (helix::json_util::safe_string(user_sensor, "klipper_name") !=
                            preset_klipper)
                            continue;
                        found = true;
                        std::string user_role =
                            helix::json_util::safe_string(user_sensor, "role", "none");
                        // Only upgrade when user has role=none — never overwrite an
                        // explicit user assignment (runout/toolhead/entry/z_probe).
                        if (user_role == "none") {
                            user_sensor["role"] = preset_role;
                            changed = true;
                            spdlog::info(
                                "[Config] Upgraded sensor '{}' role: none -> {} (preset '{}')",
                                preset_klipper, preset_role, preset_name);
                        }
                        break;
                    }
                    // Sensor in preset but missing entirely from user settings → append.
                    if (!found) {
                        user_sensors.push_back(preset_sensor);
                        changed = true;
                        spdlog::info("[Config] Added missing sensor '{}' from preset '{}'",
                                     preset_klipper, preset_name);
                    }
                }
            }
        }

        // Migration 2: default_macros. A stored macro is replaced with the
        // preset's CURRENT text only when it byte-matches an EARLIER release's
        // text for that (preset, key) pair (stale_default_macro_text()): a
        // user's own edit, or an already-migrated macro, never matches and is
        // left untouched. Idempotent: once migrated the stored text equals the
        // current text, not the stale one, so a second run finds nothing to do.
        if (preset_printer.contains("default_macros") &&
            preset_printer["default_macros"].is_object()) {
            auto stale_it = stale_default_macro_text().find(preset_name);
            json::json_pointer macros_ptr(df() + "default_macros");
            if (stale_it != stale_default_macro_text().end() && data.contains(macros_ptr) &&
                data.at(macros_ptr).is_object()) {
                auto& user_macros = data.at(macros_ptr);
                const auto& preset_macros = preset_printer["default_macros"];
                for (const auto& [macro_key, stale_text] : stale_it->second) {
                    if (!user_macros.contains(macro_key) || !user_macros[macro_key].is_string()) {
                        continue;
                    }
                    if (user_macros[macro_key].get<std::string>() != stale_text) {
                        continue;
                    }
                    if (!preset_macros.contains(macro_key) ||
                        !preset_macros[macro_key].is_string()) {
                        continue;
                    }
                    user_macros[macro_key] = preset_macros[macro_key].get<std::string>();
                    changed = true;
                    spdlog::info(
                        "[Config] Migrated default_macros.{} from preset '{}' (stale text)",
                        macro_key, preset_name);
                }
            }
        }

        // Migration 3: role keys. Seed the hardware mappings the preset defines onto a
        // machine provisioned before the preset landed, writing only absent-or-empty
        // stored values: a present value is the user's (or the auto-heal path's) call,
        // and a wrong-but-present one is the auto-heal path's job, not the preset's.
        // leds/strip is deliberately excluded — a preset LED name the machine lacks
        // raises "Configured LED strip not found", which notify_user reports ahead of
        // the toast this migration exists to silence.
        {
            auto& printer_node = data["printers"][active_printer_id_];
            if (!printer_node.is_object()) {
                printer_node = json::object();
            }
            for (const char* group : {"fans", "heaters", "temp_sensors"}) {
                if (!preset_printer.contains(group) || !preset_printer[group].is_object() ||
                    (printer_node.contains(group) && !printer_node[group].is_object())) {
                    continue;
                }
                for (const auto& [key, val] : preset_printer[group].items()) {
                    if (!val.is_string() || val.get<std::string>().empty()) {
                        continue;
                    }
                    const std::string rel = std::string(group) + "/" + key;
                    const json* stored = try_get_json(df() + rel);
                    const bool held = stored != nullptr && stored->is_string() &&
                                      !stored->get<std::string>().empty();
                    if (!held) {
                        printer_node[group][key] = val;
                        changed = true;
                        spdlog::info("[Config] Seeded '{}' from preset '{}'", rel, preset_name);
                    }
                }
            }
        }

        // Migration 4: hardware/expected union. The four AMS keywords are the only
        // expected entries that can raise an expected_missing warning ("AMS/MMU system
        // not detected"), so unioning one could invent that warning; every other name
        // only ever suppresses a false "new hardware" report.
        if (preset_printer.contains("hardware") && preset_printer["hardware"].is_object() &&
            preset_printer["hardware"].contains("expected") &&
            preset_printer["hardware"]["expected"].is_array()) {
            const char* why = nullptr;
            json* expected = node_for_write(data, df() + "hardware/expected", &why);
            if (expected == nullptr) {
                spdlog::warn("[Config] hardware/expected not merged from preset '{}': {}",
                             preset_name, why);
            } else {
                if (!expected->is_array()) {
                    *expected = json::array();
                }
                json& stored = *expected;
                for (const auto& entry : preset_printer["hardware"]["expected"]) {
                    if (!entry.is_string()) {
                        continue;
                    }
                    const std::string name = entry.get<std::string>();
                    if (name.empty() || name == "AFC" || name == "mmu" || name == "toolchanger" ||
                        name == "ace") {
                        continue;
                    }
                    if (std::find(stored.begin(), stored.end(), entry) == stored.end()) {
                        stored.push_back(entry);
                        changed = true;
                        spdlog::info("[Config] Added '{}' to hardware/expected from preset '{}'",
                                     name, preset_name);
                    }
                }
            }
        }

        if (changed) {
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
    if (!hfs::exists(preset_path)) {
        spdlog::warn("[Config] Preset file not found: {} (looked in writable + seed bundle)",
                     preset_path);
        return false;
    }

    // Load and parse preset JSON
    json preset_json = json::parse(tio::read_file(preset_path).value_or(""), nullptr, false);
    if (preset_json.is_discarded()) {
        spdlog::error("[Config] Failed to parse preset '{}': unreadable or not valid JSON",
                      preset_path);
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
        !active_printer_id_.empty() &&
        (!data.contains("printers") || data["printers"].is_object())) {
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
    // in the other. Seeding them is correct only when
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
    // /input/calibration/*) — which is distinct from the
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
        const json* wc = try_get_json(df() + "wizard_completed");
        if (wc != nullptr && wc->is_boolean()) {
            bool is_completed = wc->get<bool>();
            spdlog::trace("[Config] Per-printer wizard_completed = {}", is_completed);
            return !is_completed;
        }
    }

    // Fall back to root-level wizard_completed (backward compat)
    if (const json* wizard_completed = try_get_json("/wizard_completed")) {
        if (wizard_completed->is_boolean()) {
            bool is_completed = wizard_completed->get<bool>();
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

    // The restore chain reads these tiers whenever settings.json goes missing,
    // so a surviving backup would resurrect this pre-reset document on any
    // later loss of the file. No settings backup is written afterwards: init()
    // and save() both refuse to back up a wizard-incomplete document, which the
    // defaults are. The env backup comes back on the next start from the
    // unchanged helixscreen.env, which a factory reset does not touch.
    remove_backups(config_backup_search_paths());
    remove_backups(env_backup_search_paths());

    spdlog::info("[Config] Configuration reset to defaults. Wizard will run on next startup.");
}

MacroConfig Config::get_macro(const std::string& key, const MacroConfig& default_val) {
    const json* val = try_get_json(df() + "default_macros/" + key);
    if (val == nullptr) {
        spdlog::trace("[Config] Macro '{}' not found, using default", key);
        return default_val;
    }

    // Handle string format (backward compatibility): use as both label and gcode
    if (val->is_string()) {
        std::string macro = val->get<std::string>();
        spdlog::trace("[Config] Macro '{}' is string format: '{}'", key, macro);
        return {macro, macro};
    }

    // Handle object format: {label, gcode}. A field of the wrong type makes the
    // whole entry unusable rather than half of it.
    if (val->is_object()) {
        const auto label = val->find("label");
        const auto gcode = val->find("gcode");
        if ((label != val->end() && !label->is_string()) ||
            (gcode != val->end() && !gcode->is_string())) {
            spdlog::warn("[Config] Error reading macro '{}': label and gcode must be strings", key);
            return default_val;
        }
        MacroConfig result;
        result.label = label != val->end() ? label->get<std::string>() : default_val.label;
        result.gcode = gcode != val->end() ? gcode->get<std::string>() : default_val.gcode;
        spdlog::trace("[Config] Macro '{}': label='{}', gcode='{}'", key, result.label,
                      result.gcode);
        return result;
    }

    spdlog::warn("[Config] Macro '{}' has unexpected type, using default", key);
    return default_val;
}
