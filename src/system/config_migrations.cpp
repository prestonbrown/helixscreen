// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_migrations.h"

#include "completion_alert_mode.h"
#include "config.h"
#include "config_testing.h"
#include "core/text_io.h"
#include "helix_fs.h"
#include "input_defaults.h"
#include "json_utils.h"
#include "platform_capabilities.h"
#include "screensaver_setting.h"

#if !defined(HELIX_SPLASH_ONLY) && !defined(HELIX_WATCHDOG)
#include "system/telemetry_manager.h"
#define CONFIG_RECORD_ERROR(...) TelemetryManager::instance().record_error(__VA_ARGS__)
#else
#define CONFIG_RECORD_ERROR(...) ((void)0)
#endif

#include <spdlog/spdlog.h>

#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace hfs = helix::fs;
namespace tio = helix::text_io;

using namespace helix;
using namespace helix::config_detail;

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

/// Move a single root /printer object under "printers/{slug}/", set
/// active_printer_id, and move root-level "filament" and "panel_widgets" under
/// that printer.
static void move_single_printer_under_printers(json& config) {
    // Skip if already has /printers (idempotent)
    if (config.contains("printers")) {
        return;
    }

    // Skip if no old /printer section exists
    if (!config.contains("printer") || !config["printer"].is_object()) {
        return;
    }

    json printer_data = config["printer"];

    // A printer with no LEDs gets an explicit empty selection. Config::init()
    // fills an absent leds block with a placeholder strip and selects it, which
    // would pre-select an LED the printer does not have.
    if (!printer_data.contains("leds")) {
        printer_data["leds"] = {{"selected", json::array()}};
    }

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
        spdlog::info("[Config] Versionless config: moved /display/printer_image to "
                     "/printers/{}/printer_image",
                     slug);
    }

    spdlog::info("[Config] Versionless config: restructured /printer to /printers/{}", slug);
}

/// Bring a versionless document into the shape the numbered chain starts from.
/// The shipped presets (assets/config/presets/*.json) and the tarball-default
/// settings.json seeded from them carry no config_version and describe their one
/// printer as a single root /printer object. Only version 0 runs this.
static void normalize_versionless_document(json& config) {
    move_single_printer_under_printers(config);

    // A single-printer install starts with the printer switcher hidden.
    if (config.contains("/printers/show_printer_switcher"_json_pointer)) {
        return;
    }
    int printer_count = 0;
    if (config.contains("printers") && config["printers"].is_object()) {
        for (const auto& [key, val] : config["printers"].items()) {
            if (val.is_object()) {
                printer_count++;
            }
        }
    }
    const char* why = nullptr;
    json* flag = printer_count <= 1
                     ? node_for_write(config, "/printers/show_printer_switcher", &why)
                     : nullptr;
    if (flag != nullptr) {
        *flag = false;
    }
}

/// Consolidate "power" widget into "power_device" with __all__ sentinel.
/// Scans all printers/panels/pages for widget entries with id=="power" and replaces
/// them with "power_device:N" (using the next available instance number).
static void migrate_v9_to_v10(json& config, const std::string& /*config_path*/) {
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
                    std::string id = helix::json_util::safe_string(widget, "id");
                    if (id.substr(0, 13) == "power_device:") {
                        const auto n = tio::parse_leading<int>(id.substr(13));
                        if (n && *n > max_instance)
                            max_instance = *n;
                    }
                }
            }

            // Migrate "power" → "power_device:N+1"
            for (auto& page : panel["pages"]) {
                if (!page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    if (helix::json_util::safe_string(widget, "id") == "power") {
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
static void migrate_v10_to_v11(json& config, const std::string& /*config_path*/) {
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
                const char* why = nullptr;
                json* dest = node_for_write(
                    config, std::string("/thermal/rates/") + heater + "/heat_rate", &why);
                if (dest == nullptr) {
                    spdlog::warn("[Config] Migration v11: heat_rate for '{}' not copied: {}",
                                 heater, why);
                    continue;
                }
                *dest = config["calibration"]["pid_history"][heater]["heat_rate"];
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
static void migrate_v11_to_v12(json& config, const std::string& /*config_path*/) {
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

/// True when the config's preset is `preset`, or any saved printer has type `printer_type`.
/// A migration freezes its detection rule at the version it repairs, so this reads the raw
/// config and never asks PrinterDetector.
static bool config_names_printer(const json& config, const char* preset, const char* printer_type) {
    if (helix::json_util::safe_string(config, "preset") == preset)
        return true;
    if (!config.contains("printers") || !config["printers"].is_object())
        return false;
    for (const auto& [printer_id, printer] : config["printers"].items()) {
        if (helix::json_util::safe_string(printer, "type") == printer_type)
            return true;
    }
    return false;
}

/// Turn the backlight off at sleep when the display block still carries the stale pair
/// (sleep_backlight_off=false with hardware_blank=0).
static void restore_backlight_off_sleep(json& config, const char* log_tag) {
    if (!config.contains("display") || !config["display"].is_object())
        return;

    auto& display = config["display"];
    if (!helix::json_util::safe_bool(display, "sleep_backlight_off", true) &&
        helix::json_util::safe_int(display, "hardware_blank", -1) == 0) {
        display["sleep_backlight_off"] = true;
        display["hardware_blank"] = 1;
        spdlog::info("[Config] {}: restored AD5X backlight-off sleep "
                     "(sleep_backlight_off=true, hardware_blank=1)",
                     log_tag);
    }
}

/// Repair stale AD5X display settings from the pre-#431 preset. The original AD5X
/// preset (#235) set sleep_backlight_off=false under the assumption that backlight
/// power-off prevented wake-on-touch. #431 corrected this after hardware verification.
/// Users whose wizard ran between those commits still have the stale combination
/// (backlight never turns off at sleep).
static void migrate_v12_to_v13(json& config, const std::string& /*config_path*/) {
    // VENDOR_OK: a migration freezes the printers it repairs
    if (config_names_printer(config, "ad5x", "FlashForge Adventurer 5X"))
        restore_backlight_off_sleep(config, "Migration v13");
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

    const std::string legacy_path =
        hfs::join_path(hfs::parent_path(config_path), "telemetry_config.json");
    if (!hfs::exists(legacy_path)) {
        return;
    }

    const json legacy = json::parse(tio::read_file(legacy_path).value_or(""), nullptr, false);
    if (legacy.is_discarded()) {
        spdlog::warn("[Config] Migration v14: failed to read legacy {}: unreadable or not "
                     "valid JSON (leaving file in place for retry)",
                     legacy_path);
        return;
    }
    if (!has_key && legacy.contains("enabled") && legacy["enabled"].is_boolean()) {
        bool legacy_enabled = legacy["enabled"].get<bool>();
        config["telemetry_enabled"] = legacy_enabled;
        spdlog::info("[Config] Migration v14: imported telemetry_enabled={} "
                     "from legacy {}",
                     legacy_enabled ? "true" : "false", legacy_path);
    }

    if (hfs::remove(legacy_path)) {
        spdlog::debug("[Config] Migration v14: removed legacy {}", legacy_path);
    }
}

/// v14→v15: Re-apply AD5X sleep preset for users whose printer-identify wizard
/// ran AFTER the v12→v13 migration. The wizard (pre-fix) force-wrote the stale
/// pre-#431 display config on every confirmation, undoing the v13 restore for
/// any AD5X user who ran it. The wizard block has been removed in the same
/// release that introduces this migration. Detection condition is identical to
/// v12→v13 so the fix is idempotent if the wizard is re-run on an older build.
static void migrate_v14_to_v15(json& config, const std::string& /*config_path*/) {
    // VENDOR_OK: a migration freezes the printers it repairs
    if (config_names_printer(config, "ad5x", "FlashForge Adventurer 5X"))
        restore_backlight_off_sleep(config, "Migration v15");
}

/// v15 → v16: Turn the screensaver off on BASIC/EMBEDDED tiers where Flying Toasters
/// causes Klipper print failures. Queues a one-time info modal via a transient flag
/// consumed by Application post-boot. Only affects type 1 (Flying Toasters); types 2/3
/// (Starfield, Pipes 3D) are left untouched because we have no evidence they cause
/// the same problem, and silently flipping an explicit user choice is worse than
/// leaving a slightly expensive screensaver running on their preferred setting.
static void migrate_v15_to_v16(json& config, const std::string& /*config_path*/) {
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

    int current_type = helix::json_util::safe_int(display, "screensaver_type", 0);
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
static void migrate_v16_to_v17(json& config, const std::string& /*config_path*/) {
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

/// Whether the active printer records a finished setup wizard. Runs after
/// normalize_versionless_document(), which copies an old root-level flag into
/// the printer it creates, so one place covers both shapes.
static bool records_finished_wizard(const json& config) {
    const auto printers = config.find("printers");
    if (printers == config.end() || !printers->is_object()) {
        return false;
    }
    const auto printer =
        printers->find(helix::json_util::safe_string(config, "active_printer_id", "default"));
    return printer != printers->end() &&
           helix::json_util::safe_bool(*printer, "wizard_completed", false);
}

/// v17 → v18: After the #943/#986 touch-scaling fix, the DRM/fbdev backends apply
/// evdev linear scaling to MT-only digitizers (e.g. Qidi Q2: 800x480 controller on a
/// 480x272 panel). Any affine calibration captured before the fix was computed in the
/// wrong (unscaled) coordinate space and would re-break touch on upgrade. We cannot
/// distinguish a stale capacitive affine from a legitimate resistive one in JSON alone
/// (large coefficients are valid for resistive panels), so set a one-shot
/// recheck_pending flag here; the display backend decides at boot — when it knows the
/// device's resistive/capacitive nature and live ABS range — whether to invalidate.
///
/// A versionless document whose wizard never completed is a shipped preset or an
/// installer seed: its calibration came from a known-good matrix solved for the
/// current scaling, and nobody has captured another, so it is left unflagged. A
/// versionless document with a finished wizard is a user config from before
/// config_version existed, and its wizard affine is exactly what this recheck is
/// for. The runner stamps config_version after the whole chain, so here it still
/// reads the document's original version.
static void migrate_v17_to_v18(json& config, const std::string& /*config_path*/) {
    if (helix::json_util::safe_int(config, "config_version", 0) == 0 &&
        !records_finished_wizard(config)) {
        return;
    }
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
static void migrate_v18_to_v19(json& config, const std::string& /*config_path*/) {
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
/// The /printer erase matters beyond tidiness: normalize_versionless_document() gates on
/// `config.contains("printer") && config["printer"].is_object()`, so a
/// resurrected /printer node would be split into a bogus printers/default entry
/// if that migration ever re-ran.
static void migrate_v19_to_v20(json& config, const std::string& /*config_path*/) {
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
    // already split out by normalize_versionless_document(). Erase it only when it truly holds
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
static constexpr const char* kScannerLeaves[] = {"usb_vendor_product", "usb_device_name",
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
            for (const char* leaf : kScannerLeaves) {
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
static void migrate_v20_to_v21(json& config, const std::string& /*config_path*/) {
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

/// Migration v21→v22: the two filament settings that were still read
/// per-printer but stored at the root.
///
/// Both are read as `df() + "filament/..."`, i.e. under /printers/<id>/, so a
/// value at the root was never consulted. On a real K2 Plus the root held
/// {"color_rgb": 16711680, "material": "PETG"} - a red PETG spool from months
/// earlier - while the live ASA-GF sat in the printer's own node. Harmless as
/// long as nothing reads it, and exactly the kind of thing that bites the day
/// something does.
///
/// fan_out_to_printers() is the right shape for both: a printer that already
/// holds a real value keeps it (so the live spool above survives untouched),
/// one that holds none inherits the root value rather than losing it, and the
/// root key is retired only once there was somewhere to put it.
static void migrate_v21_to_v22(json& config, const std::string& /*config_path*/) {
    const int spool_copies =
        fan_out_to_printers(config, "filament/external_spool", "filament/external_spool");
    if (spool_copies > 0) {
        spdlog::info("[Config] Migration v22: copied /filament/external_spool to {} printer(s)",
                     spool_copies);
    }

    const int cooldown_copies = fan_out_to_printers(config, "filament/cooldown_delay_seconds",
                                                    "filament/cooldown_delay_seconds");
    if (cooldown_copies > 0) {
        spdlog::info(
            "[Config] Migration v22: copied /filament/cooldown_delay_seconds to {} printer(s)",
            cooldown_copies);
    }

    // Drop the container only once it is genuinely empty - a key we have not
    // accounted for here is a key we must not delete.
    if (config.contains("filament") && config["filament"].is_object() &&
        config["filament"].empty()) {
        config.erase("filament");
    }
}

/// Migration v22→v23: the Macro Button widget's per-instance run gate changed
/// polarity and name.
///
/// It used to be "skip_param_prompt" (default false), which suppressed only the
/// parameter-entry modal — the Settings → Safety confirmation still fired, so a
/// user who asked for a one-tap macro button got a dialog anyway. It is now
/// "require_confirmation" (default true), governing both prompts, and its
/// off-state is what actually delivers the one-tap run.
///
/// The two keys are exact inverses, so the rewrite is lossless: a widget that
/// had skip_param_prompt:true becomes require_confirmation:false and keeps
/// running without a parameter prompt, now without the confirmation too.
/// Widgets that never carried the key keep the default and are left untouched,
/// which is why nothing is written when the key is absent.
///
/// The same fallback lives in favorite_macro_config_from_json() for configs this
/// migration cannot reach (preset assets under panel_widgets/<preset>/, an
/// imported widget config). This pass exists so the legacy key does not linger
/// on disk for users who never reopen the widget's config modal.
static void migrate_v22_to_v23(json& config, const std::string& /*config_path*/) {
    if (!config.contains("printers") || !config["printers"].is_object())
        return;

    int rewritten = 0;
    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object() || !printer.contains("panel_widgets") ||
            !printer["panel_widgets"].is_object())
            continue;
        for (auto& [panel_id, panel] : printer["panel_widgets"].items()) {
            if (!panel.is_object() || !panel.contains("pages") || !panel["pages"].is_array())
                continue;
            for (auto& page : panel["pages"]) {
                if (!page.is_object() || !page.contains("widgets") || !page["widgets"].is_array())
                    continue;
                for (auto& widget : page["widgets"]) {
                    if (!widget.is_object() || !widget.contains("config") ||
                        !widget["config"].is_object())
                        continue;
                    json& wc = widget["config"];
                    if (!wc.contains("skip_param_prompt"))
                        continue;
                    // A non-boolean legacy value was never honoured by the
                    // reader either; drop it rather than inventing a meaning.
                    if (wc["skip_param_prompt"].is_boolean() &&
                        !wc.contains("require_confirmation")) {
                        wc["require_confirmation"] = !wc["skip_param_prompt"].get<bool>();
                    }
                    wc.erase("skip_param_prompt");
                    ++rewritten;
                }
            }
        }
    }

    if (rewritten > 0) {
        spdlog::info("[Config] Migration v23: rewrote skip_param_prompt -> require_confirmation "
                     "on {} macro widget(s)",
                     rewritten);
    }
}

/// Migration v23->v24: mark every saved home layout as holding pre-square-cell units.
///
/// Runs at v24 rather than v22 because the released 1.0 line already spent v22
/// and v23 on the filament re-scope and the macro-button gate above; a config
/// stamped 23 by those builds has never seen this tag. Replaying it is a no-op
/// (see the three guards below), so the configs that did get it on the 1.1 line
/// are left alone.
///
/// Saved col/row/colspan/rowspan are counts of cells in a grid whose track
/// count and cell size both changed. This runs at config load — before
/// Application settles the screen size and long before LayoutManager::init() —
/// so there is no resolution HERE to rescale against, and none is stored in
/// settings.json either. That rules out converting the numbers now; it does not
/// rule out converting them at all. The first grid build knows the panel extent
/// and the measured content box, which is everything the conversion needs, so
/// the coordinates are left intact and tagged, and PanelWidgetManager ports them
/// once (see port_legacy_layout(), include/layout_port.h) and drops the tag.
///
/// `legacy_rows` carries what /ui/cached_grid/<panel>/rows held. The old grid
/// sized its row axis from the widgets in use rather than from a table, using
/// that cache as a floor for widgets whose hardware gate had not yet fired, so
/// it is the one part of the old grid the saved layout cannot re-derive alone.
/// It travels with the tag rather than being read back out of /ui, so the port
/// has a single source and the /ui key can go now.
///
/// Intent is read for entries with no coordinates: one that is disabled AND
/// holds real coordinates was removed with the trash button, which leaves the
/// position intact. One that is disabled at -1 was auto-disabled by the
/// placement engine or was never added, so the key is dropped and the
/// registry's default_enabled decides.
///
/// Iterates every printer profile, not just the active one — the shipped
/// config/settings.json already carries two.
static void migrate_v23_to_v24(json& config, const std::string& /*config_path*/) {
    // Harvest the per-panel row cache before dropping the node: nothing else
    // reads /ui/cached_grid any more, and the port wants it keyed to the panel.
    std::map<std::string, int> cached_rows;
    if (config.contains("ui") && config["ui"].is_object() && config["ui"].contains("cached_grid") &&
        config["ui"]["cached_grid"].is_object()) {
        for (const auto& [panel_id, node] : config["ui"]["cached_grid"].items()) {
            if (node.is_object() && node.contains("rows") && node["rows"].is_number_integer()) {
                cached_rows[panel_id] = node["rows"].get<int>();
            }
        }
        config["ui"].erase("cached_grid");
    }

    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }

    int tagged = 0;
    int profiles = 0;

    // An entry that never had coordinates and is switched off carries no intent
    // worth preserving; drop the key so the registry default decides. Entries
    // WITH coordinates are left exactly as they are for the port to read.
    auto clean_array = [](json& widgets) {
        if (!widgets.is_array()) {
            return;
        }
        for (auto& entry : widgets) {
            if (!entry.is_object()) {
                continue;
            }
            const bool enabled =
                entry.contains("enabled") && entry["enabled"].is_boolean() && entry["enabled"];
            const int col = (entry.contains("col") && entry["col"].is_number_integer())
                                ? entry["col"].get<int>()
                                : -1;
            if (!enabled && col < 0) {
                entry.erase("enabled");
            }
        }
    };

    // Replaying this migration must not change a document it has already run
    // on. That is not hypothetical: a rollback to a v22 build stamps the
    // version back down, and the next upgrade runs the chain again over a
    // layout that is already in track units. Re-tagging it makes the port read
    // tracks as cells and convert a second time. Three states have to be left
    // alone, each for its own reason.

    // Ported: `grid` and `parked_grids` are written by per-grid storage, which
    // is newer than this migration, so they cannot appear on a genuine v21
    // layout — PanelWidgetConfig::load() documents `grid` as absent on
    // everything written before it. Their presence means the coordinates
    // already count against a measured grid.
    auto already_ported = [](const json& panel) {
        return (panel.contains("grid") && panel["grid"].is_string() &&
                !panel["grid"].get<std::string>().empty()) ||
               (panel.contains("parked_grids") && panel["parked_grids"].is_object() &&
                !panel["parked_grids"].empty());
    };

    // Tagged but not yet ported: re-tagging would overwrite legacy_rows with 0,
    // because the /ui/cached_grid node it comes from was erased by the first
    // run. The tag that is already there carries the real value.
    auto already_tagged = [](const json& panel) {
        return panel.contains("layout_units") && panel["layout_units"].is_string() &&
               panel["layout_units"].get<std::string>() == "cells_v21";
    };

    // Nothing to port: the tag exists so port_legacy_layout() knows which
    // numbers are cells. A layout whose entries carry no coordinates has no
    // such numbers, so the tag would be cleared again having converted nothing.
    auto has_coordinates = [](const json& panel) {
        auto p = panel.find("pages");
        if (p == panel.end() || !p->is_array()) {
            return false;
        }
        for (const auto& page : *p) {
            auto w = page.is_object() ? page.find("widgets") : page.end();
            if (w == page.end() || !w->is_array()) {
                continue;
            }
            for (const auto& entry : *w) {
                if (entry.is_object() && entry.contains("col") &&
                    entry["col"].is_number_integer()) {
                    return true;
                }
            }
        }
        return false;
    };

    auto tag_panel = [&](const std::string& panel_id, json& panel) {
        if (already_ported(panel) || already_tagged(panel) || !has_coordinates(panel)) {
            return;
        }
        panel["layout_units"] = "cells_v21";
        auto it = cached_rows.find(panel_id);
        panel["legacy_rows"] = (it != cached_rows.end()) ? it->second : 0;
        ++tagged;
    };

    for (auto& printer : config["printers"]) {
        if (!printer.is_object() || !printer.contains("panel_widgets") ||
            !printer["panel_widgets"].is_object()) {
            continue;
        }
        ++profiles;
        for (auto&& [panel_id, panel] : printer["panel_widgets"].items()) {
            // Legacy flat array: lift it into the page shape as well. Left as an
            // array, PanelWidgetConfig::load() would find no entry with a grid
            // position, read the config as pre-grid and replace it wholesale
            // with the registry defaults, discarding every deliberate hide.
            if (panel.is_array()) {
                json widgets = panel;
                clean_array(widgets);
                panel = json{{"main_page_index", 0},
                             {"next_page_id", 1},
                             {"pages", json::array({json{{"id", "main"}, {"widgets", widgets}}})}};
                tag_panel(panel_id, panel);
                continue;
            }
            if (!panel.is_object() || !panel.contains("pages") || !panel["pages"].is_array()) {
                continue;
            }
            for (auto& page : panel["pages"]) {
                if (page.is_object() && page.contains("widgets")) {
                    clean_array(page["widgets"]);
                }
            }
            tag_panel(panel_id, panel);
        }
    }

    if (tagged > 0) {
        spdlog::info("[Config] Migration v24: tagged {} panel layout(s) across {} printer "
                     "profile(s) for porting to the square-cell grid",
                     tagged, profiles);
    }
}

/// Re-key the pre-print prediction history from phase ordinals to phase names.
///
/// An ordinal is a position in PrintStartPhase, not an identity: inserting a
/// phase renumbers everything after it and every stored number then names a
/// different phase. A name survives that, so this is the last migration the
/// history needs.
static void migrate_v24_to_v25(json& config, const std::string& /*config_path*/) {
    // PrintStartPhase as it was numbered when these documents were written,
    // before SOAKING took slot 4. Frozen — it describes stored data, not the
    // current enum, and must not be regenerated from it.
    static constexpr const char* kLegacyPhaseNames[] = {
        "IDLE",   "INITIALIZING", "HOMING",   "HEATING_BED", "HEATING_NOZZLE", "QGL",
        "Z_TILT", "BED_MESH",     "CLEANING", "PURGING",     "COMPLETE",
    };
    constexpr int kLegacyPhaseCount = static_cast<int>(std::size(kLegacyPhaseNames));

    if (!config.contains("print_start_history") ||
        !config["print_start_history"].contains("entries") ||
        !config["print_start_history"]["entries"].is_array()) {
        return;
    }

    int converted = 0;
    int dropped = 0;
    for (auto& entry : config["print_start_history"]["entries"]) {
        if (!entry.is_object() || !entry.contains("phases") || !entry["phases"].is_object()) {
            continue;
        }

        json renamed = json::object();
        bool changed = false;
        for (auto it = entry["phases"].begin(); it != entry["phases"].end(); ++it) {
            const std::string& key = it.key();
            // A key that is already a name stays put, so replaying the ladder
            // over a converted document is a no-op. A stamp rollback to an
            // older build and back does exactly that.
            if (key.empty() || key.size() > 3 ||
                key.find_first_not_of("0123456789") != std::string::npos) {
                renamed[key] = it.value();
                continue;
            }
            // At most three digits, so it always parses.
            const int ordinal = tio::parse_int<int>(key).value_or(-1);
            changed = true;
            if (ordinal < 0 || ordinal >= kLegacyPhaseCount) {
                ++dropped;
                continue;
            }
            renamed[kLegacyPhaseNames[ordinal]] = it.value();
        }

        if (changed) {
            entry["phases"] = std::move(renamed);
            ++converted;
        }
    }

    if (converted > 0 || dropped > 0) {
        spdlog::info("[Config] Migration v25: named the phase keys in {} predictor entries "
                     "({} unrecognised ordinals dropped)",
                     converted, dropped);
    }
}

/// Convert the persisted completion-alert mode from a boolean to the
/// Off/Notification/Alert int AudioSettingsManager has always read and written.
/// A fresh install's default before this migration stored the JSON boolean
/// `true`, and Config::get<int>() converts a JSON boolean via nlohmann's own
/// bool-to-arithmetic rule (true -> 1, false -> 0) rather than the intended
/// CompletionAlertMode::ALERT (2), so every install that never touched Print
/// Completion Alert fell back to Notification. AudioSettingsManager::
/// set_completion_alert_mode() always persists an int, so a boolean here can
/// only be the old default, never a user's actual choice.
static void migrate_v25_to_v26(json& config, const std::string& /*config_path*/) {
    if (!config.contains("completion_alert") || !config["completion_alert"].is_boolean()) {
        return;
    }
    const bool was_true = config["completion_alert"].get<bool>();
    config["completion_alert"] = was_true ? static_cast<int>(helix::CompletionAlertMode::ALERT)
                                          : static_cast<int>(helix::CompletionAlertMode::OFF);
    spdlog::info("[Config] Migration v26: completion_alert bool({}) -> int({})", was_true,
                 config["completion_alert"].get<int>());
}

/// Move a stored scroll_throw that is still the old shipped default onto the
/// platform default. ESP32 is the only platform whose default differs, so
/// elsewhere this leaves every document as it is.
static void migrate_v26_to_v27(json& config, const std::string& /*config_path*/) {
    if (!config.contains("input") || !config["input"].is_object()) {
        return;
    }
    json& input = config["input"];
    if (!input.contains("scroll_throw") || !input["scroll_throw"].is_number_integer()) {
        return;
    }
    const int stored = input["scroll_throw"].get<int>();
    const int migrated = migrated_scroll_throw(stored, helix::input_defaults::kScrollThrow);
    if (migrated != stored) {
        input["scroll_throw"] = migrated;
        spdlog::info("[Config] Migration v27: input.scroll_throw {} -> {}", stored, migrated);
    }
}

/// Rewrite an integer /display/screensaver_type as the stable name it stood for, straight from
/// the frozen legacy table and never through the live registry, so neither a reordered nor a
/// removed saver changes what an old number becomes. An out-of-range number becomes the default
/// saver. A string is already a name and is left as it is, as is any other JSON type, which the
/// reader treats as the default.
static void migrate_v27_to_v28(json& config, const std::string& /*config_path*/) {
    if (!config.contains("display") || !config["display"].is_object()) {
        return;
    }
    json& display = config["display"];
    if (!display.contains("screensaver_type") || !display["screensaver_type"].is_number_integer()) {
        return;
    }
    const json& stored = display["screensaver_type"];
    const char* name = helix::ui::legacy_screensaver_name(stored.get<long long>());
    if (name == nullptr) {
        name = helix::ui::screensaver_name(helix::ui::kDefaultScreensaverType);
    }
    spdlog::info("[Config] Migration v28: display.screensaver_type {} -> \"{}\"", stored.dump(),
                 name);
    display["screensaver_type"] = name;
}

/// Move each printer's ams/afc_unload_after_print to the backend-neutral
/// ams/unload_after_print. A printer that already holds the new key keeps it.
static void migrate_v28_to_v29(json& config, const std::string& /*config_path*/) {
    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }
    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object()) {
            continue;
        }
        if (migrate_config_keys(printer,
                                {{"/ams/afc_unload_after_print", "/ams/unload_after_print"}})) {
            spdlog::info("[Config] Migration v29: ams/afc_unload_after_print -> "
                         "ams/unload_after_print for printer '{}'",
                         printer_id);
        }
    }
}

/// Move each printer's lane eject distance and speed off the QIDI-named keys to
/// the backend-neutral ams/eject_distance and ams/eject_velocity. A printer that
/// already holds a new key keeps it.
static void migrate_v29_to_v30(json& config, const std::string& /*config_path*/) {
    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }
    for (auto& [printer_id, printer] : config["printers"].items()) {
        if (!printer.is_object()) {
            continue;
        }
        if (migrate_config_keys(printer, {{"/ams/qidi_eject_distance", "/ams/eject_distance"},
                                          {"/ams/qidi_eject_velocity", "/ams/eject_velocity"}})) {
            spdlog::info("[Config] Migration v30: ams/qidi_eject_* -> ams/eject_* for printer '{}'",
                         printer_id);
        }
    }
}

/// Move the install-wide /remote_screen/enabled and /remote_screen/control into the active
/// printer, the one they were last turned on for. Every other printer starts with sharing
/// off: a copy would list the screen among the webcams of printers nobody chose to share
/// it with. A printer that already holds a value keeps it.
static void migrate_v30_to_v31(json& config, const std::string& /*config_path*/) {
    if (!config.contains("remote_screen") || !config["remote_screen"].is_object()) {
        return;
    }
    const std::string active = find_active_printer_key(config);
    if (active.empty()) {
        return;
    }
    const std::string base = "/printers/" + active + "/remote_screen";
    if (migrate_config_keys(config, {{"/remote_screen/enabled", base + "/enabled"},
                                     {"/remote_screen/control", base + "/control"}})) {
        spdlog::info("[Config] Migration v31: screen sharing moved to printer '{}'", active);
    }
}

/// Whether a saved printer is an AD5X, by the preset it was set up from or its saved type.
/// A root preset marker speaks only for an active printer that names neither: the seeder
/// rewrites that marker on every update, so it can describe a different printer than the
/// one now active.
static bool printer_is_ad5x(const json& config, const std::string& printer_id,
                            const json& printer) {
    const std::string own_preset = helix::json_util::safe_string(printer, "preset");
    const std::string own_type = helix::json_util::safe_string(printer, "type");
    if (!own_preset.empty() || !own_type.empty()) {
        return own_preset == "ad5x" || own_type == "FlashForge Adventurer 5X";
    }
    return helix::json_util::safe_string(config, "preset") == "ad5x" &&
           find_active_printer_key(config) == printer_id;
}

/// Drop an AD5X's "filament_switch_sensor runout_sensor" entry. That name is zMod's client
/// variable, not a Klipper object, so Hardware Health reports it missing on every boot. A
/// printer with no head_switch_sensor entry gets the stale entry renamed to it instead, so
/// the role and switch the user chose carry over to the sensor the printer really has.
static void migrate_v31_to_v32(json& config, const std::string& /*config_path*/) {
    static constexpr const char* kStale = "filament_switch_sensor runout_sensor";
    static constexpr const char* kHead = "filament_switch_sensor head_switch_sensor";
    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }
    for (auto& [printer_id, printer] : config["printers"].items()) {
        // VENDOR_OK: a migration freezes the printers it repairs
        if (!printer.is_object() || !printer_is_ad5x(config, printer_id, printer)) {
            continue;
        }
        const json::json_pointer sensors_ptr("/filament_sensors/sensors");
        if (!printer.contains(sensors_ptr) || !printer.at(sensors_ptr).is_array()) {
            continue;
        }
        json& sensors = printer.at(sensors_ptr);
        bool has_head = false;
        for (const auto& s : sensors) {
            has_head = has_head || helix::json_util::safe_string(s, "klipper_name") == kHead;
        }
        for (auto it = sensors.begin(); it != sensors.end();) {
            if (helix::json_util::safe_string(*it, "klipper_name") != kStale) {
                ++it;
            } else if (!has_head) {
                (*it)["klipper_name"] = kHead;
                has_head = true;
                spdlog::info("[Config] Migration v32: renamed runout_sensor to "
                             "head_switch_sensor for printer '{}'",
                             printer_id);
                ++it;
            } else {
                it = sensors.erase(it);
                spdlog::info("[Config] Migration v32: dropped runout_sensor for printer '{}'",
                             printer_id);
            }
        }
    }
}

/// Which AD5M Pro firmware a saved printer was set up for, by its own preset or type.
/// Empty when it is not an AD5M Pro set up from a Forge-X or Z-Mod preset. A root preset
/// marker speaks only for an active printer that names neither, as in printer_is_ad5x().
static std::string ad5m_pro_firmware(const json& config, const std::string& printer_id,
                                     const json& printer) {
    std::string preset = helix::json_util::safe_string(printer, "preset");
    const std::string type = helix::json_util::safe_string(printer, "type");
    if (preset.empty() && type.empty() && find_active_printer_key(config) == printer_id) {
        preset = helix::json_util::safe_string(config, "preset");
    }
    if (preset == "ad5m_pro_forgex" ||
        (preset.empty() && type == "FlashForge Adventurer 5M Pro (ForgeX)")) {
        return "forgex";
    }
    if (preset == "ad5m_pro_zmod") {
        return "zmod";
    }
    return {};
}

/// Correct the AD5M Pro fan roles its Forge-X and Z-Mod presets once swapped. chamber_fan is
/// the side fan FlashForge's M106 P2 drives across the bed, so it is the aux fan, and
/// internal_fan is the enclosure's circulation fan. Z-Mod exposes neither internal_fan nor
/// external_fan to Klipper (AIR_CIRCULATION_* drives them), so there the chamber and exhaust
/// roles go. Only a role still holding the old preset's exact value changes: anything else
/// is a choice the user made.
static void migrate_v32_to_v33(json& config, const std::string& /*config_path*/) {
    static constexpr const char* kSideFan = "fan_generic chamber_fan";
    static constexpr const char* kCirculation = "fan_generic internal_fan";
    static constexpr const char* kExhaust = "fan_generic external_fan";
    if (!config.contains("printers") || !config["printers"].is_object()) {
        return;
    }
    for (auto& [printer_id, printer] : config["printers"].items()) {
        // VENDOR_OK: a migration freezes the printers it repairs
        const std::string firmware =
            printer.is_object() ? ad5m_pro_firmware(config, printer_id, printer) : "";
        if (firmware.empty() || !printer.contains("fans") || !printer["fans"].is_object()) {
            continue;
        }
        json& fans = printer["fans"];
        if (helix::json_util::safe_string(fans, "aux") != kCirculation) {
            continue;
        }
        const bool chamber_is_old_default =
            helix::json_util::safe_string(fans, "chamber") == kSideFan;
        if (firmware == "forgex") {
            if (!chamber_is_old_default) {
                continue;
            }
            fans["aux"] = kSideFan;
            fans["chamber"] = kCirculation;
        } else {
            fans["aux"] = kSideFan;
            if (chamber_is_old_default) {
                fans.erase("chamber");
            }
            if (helix::json_util::safe_string(fans, "exhaust") == kExhaust) {
                fans.erase("exhaust");
            }
        }
        spdlog::info("[Config] Migration v33: corrected AD5M Pro ({}) fan roles for printer '{}'",
                     firmware, printer_id);
    }
}

using MigrationFn = void (*)(json& config, const std::string& config_path);

/// The ladder, oldest first. A row runs when the document's stamp is below
/// its to_version; adding a migration is one function and one row here.
constexpr struct {
    int to_version;
    MigrationFn fn;
} kMigrations[] = {
    {10, migrate_v9_to_v10},  {11, migrate_v10_to_v11}, {12, migrate_v11_to_v12},
    {13, migrate_v12_to_v13}, {14, migrate_v13_to_v14}, {15, migrate_v14_to_v15},
    {16, migrate_v15_to_v16}, {17, migrate_v16_to_v17}, {18, migrate_v17_to_v18},
    {19, migrate_v18_to_v19}, {20, migrate_v19_to_v20}, {21, migrate_v20_to_v21},
    {22, migrate_v21_to_v22}, {23, migrate_v22_to_v23}, {24, migrate_v23_to_v24},
    {25, migrate_v24_to_v25}, {26, migrate_v25_to_v26}, {27, migrate_v26_to_v27},
    {28, migrate_v27_to_v28}, {29, migrate_v28_to_v29}, {30, migrate_v29_to_v30},
    {31, migrate_v30_to_v31}, {32, migrate_v31_to_v32}, {33, migrate_v32_to_v33},
};

static_assert(kMigrations[std::size(kMigrations) - 1].to_version == kCurrentConfigVersion,
              "the last migration must reach CURRENT_CONFIG_VERSION");

} // namespace

namespace helix::config_detail {

/// Run all versioned migrations in sequence from current version to kCurrentConfigVersion
void run_versioned_migrations(json& config, const std::string& config_path) {
    int version = 0;
    if (config.contains("config_version")) {
        const json& stamp = config["config_version"];
        // Running the chain from 0 would replay steps written for older shapes
        // over a current document, so an unreadable stamp
        // leaves the document unmigrated and unstamped for the user to fix.
        if (!stamp.is_number() && !stamp.is_boolean()) {
            spdlog::error("[Config] Migration failed, continuing with un-migrated config: "
                          "config_version is {}, not a number",
                          stamp.type_name());
            CONFIG_RECORD_ERROR(
                "migration", "config_migration_failed",
                fmt::format("migration error: config_version is {}", stamp.type_name()));
            return;
        }
        version = stamp.get<int>();
    }

    // A config written by a NEWER build than this one — reachable as soon as
    // update channels are user-switchable, since moving from the devel channel
    // back to stable installs an older binary over a newer config.
    //
    // Every row gates on `version < to_version`, so none of them would fire; the damage
    // is the unconditional stamp at the end, which would rewrite config_version
    // DOWN to ours. The newer build would then re-run migrations it had already
    // applied, against data already in the new shape. Leave the document alone
    // instead: unknown keys are read-through-default everywhere, and
    // Config::save() serializes the whole in-memory document, so a key nothing
    // in this build writes survives a round trip through it.
    //
    // That reaches only as far as the keys themselves. Any code that assigns a
    // whole subtree over its path discards what the node held before save()
    // ever sees it, so a subtree owner has to merge into the existing node —
    // PanelWidgetConfig::save() is the one that carries newer keys this way.
    if (version > kCurrentConfigVersion) {
        spdlog::warn("[Config] config_version {} was written by a newer build (this build "
                     "understands {}) — leaving the document unmigrated and unstamped",
                     version, kCurrentConfigVersion);
        return;
    }

    if (version == 0) {
        normalize_versionless_document(config);
    }
    for (const auto& m : kMigrations) {
        if (version < m.to_version) {
            m.fn(config, config_path);
        }
    }

    config["config_version"] = kCurrentConfigVersion;
}

int migrated_scroll_throw(int stored, int platform_default) {
    return stored == 25 ? platform_default : stored;
}

} // namespace helix::config_detail
