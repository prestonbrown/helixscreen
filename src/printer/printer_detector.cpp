// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_detector.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "config.h"
#include "data_root_resolver.h"
#include "filament_sensor_manager.h"
#include "helix_fs.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_start_analyzer.h"
#include "printer_discovery.h"
#include "printer_image_variants.h"
#include "printer_state.h"
#include "probe_preparation.h"
#include "probe_sensor_manager.h"
#include "text_io.h"
#include "thermal_rate_model.h"
#include "width_sensor_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <vector>

#ifdef __GLIBC__
#include <malloc.h> // malloc_trim() — see PrinterDatabase::compact()
#endif

#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace hfs = helix::fs;

float helix::printable_axis_span(float min, float max) {
    if (min < 0.0f && -min * 2.0f < max) {
        return max;
    }
    return max - min;
}

float BuildVolume::printable_x() const {
    return declared_bed_x > 0.0f ? declared_bed_x : printable_axis_span(x_min, x_max);
}

float BuildVolume::printable_y() const {
    return declared_bed_y > 0.0f ? declared_bed_y : printable_axis_span(y_min, y_max);
}

// ============================================================================
// JSON Database Loader with User Extensions Support
// ============================================================================

namespace {

/**
 * @brief Extensible printer database with user override support
 *
 * Loads printer definitions from:
 * 1. Bundled database: config/printer_database.json
 * 2. User extensions: config/printer_database.d/ *.json (higher priority)
 *
 * User definitions can:
 * - Add new printers (unique ID)
 * - Override bundled printers (same ID replaces bundled)
 * - Disable bundled printers ("enabled": false)
 */
struct PrinterDatabase {
    json data;
    bool loaded = false;
    std::vector<std::string> loaded_files;
    std::vector<std::string> load_errors;
    int user_overrides = 0;
    int user_additions = 0;
    // The file, and its mtime, that last failed to load. Every lookup calls
    // load(), so retrying an unchanged broken file would toast once per lookup.
    std::optional<std::pair<std::string, std::optional<std::int64_t>>> failed_file;

    bool load() {
        if (loaded)
            return true;

        // Phase 1: Load bundled database
        const std::string db_path = helix::find_readable("printer_database.json");
        const auto db_mtime = hfs::mtime_ns(db_path);
        if (failed_file && failed_file->first == db_path && failed_file->second == db_mtime) {
            return false;
        }
        const auto db_text = helix::text_io::read_file(db_path);
        if (!db_text) {
            failed_file.emplace(db_path, db_mtime);
            NOTIFY_ERROR(lv_tr("Could not load printer database"));
            LOG_ERROR_INTERNAL("[PrinterDetector] Failed to open {}", db_path);
            return false;
        }

        data = json::parse(*db_text, nullptr, /*allow_exceptions=*/false);
        // Every reader, and the extension merge, works on an object holding a
        // "printers" array; this file is user-editable, so any other shape is
        // rejected here rather than met by a throwing accessor later.
        const json* printers = helix::json_util::find_member(data, "printers");
        if (printers == nullptr || !printers->is_array()) {
            data = json();
            failed_file.emplace(db_path, db_mtime);
            NOTIFY_ERROR(lv_tr("Printer database format error"));
            LOG_ERROR_INTERNAL(
                "[PrinterDetector] Printer database is not an object with a 'printers' array: {}",
                db_path);
            return false;
        }
        failed_file.reset();
        loaded_files.push_back(db_path);
        // safe_string, not .value(): a null "version" must not cost the entire
        // printer database (detection fails, the roller collapses to
        // Custom/Other) over a field used only for a debug line.
        spdlog::debug("[PrinterDetector] Loaded bundled printer database version {}",
                      helix::json_util::safe_string(data, "version", "unknown"));

        // Phase 2: Merge user extensions from config/printer_database.d/
        merge_user_extensions();

        loaded = true;
        return true;
    }

    void reload() {
        loaded = false;
        failed_file.reset();
        compacted = false;
        loaded_files.clear();
        load_errors.clear();
        user_overrides = 0;
        user_additions = 0;
        data = json();
        load();
    }

    // Strip heuristics arrays after detection to reclaim memory.
    // Heuristics are the bulk of the database and only needed during detect().
    // Preserves kinematics info needed for filtered list building.
    void compact() {
        if (compacted || !loaded)
            return;
        for (auto& printer : data["printers"]) {
            // erase() throws on a non-object, and a stray entry is the user's typo.
            if (!printer.is_object()) {
                continue;
            }
            // Extract kinematics before stripping heuristics (needed for filtered lists)
            if (!printer.contains("_kinematics") && printer.contains("heuristics") &&
                printer["heuristics"].is_array()) {
                for (const auto& h : printer["heuristics"]) {
                    if (helix::json_util::safe_string(h, "type") == "kinematics_match") {
                        printer["_kinematics"] = helix::json_util::safe_string(h, "pattern");
                        break;
                    }
                }
            }
            printer.erase("heuristics");
        }
        compacted = true;

#ifdef __GLIBC__
        // Erasing the heuristics only hands those nodes back to glibc's free
        // lists — they are small scattered chunks that never reach the top of
        // the arena, so brk() never moves and RSS is completely unchanged.
        // Measured in-app: the free alone gives back 0 kB every time, and the
        // trim is what actually returns pages to the OS. Without this line,
        // compaction reclaims nothing at all.
        //
        // This runs once per boot, right after detection, so the cost of
        // walking the arena is paid at the point where the boot transients are
        // also free — which is why the trim gives back more than the
        // heuristics themselves.
        malloc_trim(0);
#endif

        spdlog::debug("[PrinterDetector] Database compacted (heuristics stripped)");
    }

    bool compacted = false;

  private:
    void merge_user_extensions() {
        const std::string extensions_dir = helix::writable_path("printer_database.d");

        // Check if extensions directory exists
        if (!hfs::exists(extensions_dir) || !hfs::is_directory(extensions_dir)) {
            spdlog::debug("[PrinterDetector] No user extensions directory at {}", extensions_dir);
            return;
        }

        // Build index of bundled printers by ID for fast lookup
        std::map<std::string, size_t> bundled_index;
        for (size_t i = 0; i < data["printers"].size(); ++i) {
            std::string id = helix::json_util::safe_string(data["printers"][i], "id");
            if (!id.empty()) {
                bundled_index[id] = i;
            }
        }

        // Scan for JSON files in extensions directory
        std::vector<std::string> extension_files;
        auto entries = hfs::list_dir(extensions_dir);
        if (!entries) {
            load_errors.push_back(
                fmt::format("Failed to scan {}: {}", extensions_dir, std::strerror(errno)));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
            return;
        }
        for (const auto& e : *entries) {
            if (hfs::extension(e.path) == ".json") {
                extension_files.push_back(e.path);
            }
        }

        // Sort for consistent ordering
        std::sort(extension_files.begin(), extension_files.end());

        // Process each extension file
        for (const auto& file_path : extension_files) {
            merge_extension_file(file_path, bundled_index);
        }

        if (user_overrides > 0 || user_additions > 0) {
            spdlog::info("[PrinterDetector] User extensions: {} overrides, {} additions",
                         user_overrides, user_additions);
        }
    }

    void merge_extension_file(const std::string& file_path,
                              std::map<std::string, size_t>& bundled_index) {
        const auto extension_text = helix::text_io::read_file(file_path);
        if (!extension_text) {
            load_errors.push_back(fmt::format("Could not open {}", file_path));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
            return;
        }

        json extension_data = json::parse(*extension_text, nullptr, /*allow_exceptions=*/false);
        if (extension_data.is_discarded()) {
            load_errors.push_back(fmt::format("{}: JSON parse error", file_path));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
            return;
        }
        loaded_files.push_back(file_path);

        // Console filter sets merge into the shared table before the printer
        // entries that reference them, so an extension can define its own set
        // or replace a bundled one by name. Same override rule as printers:
        // last writer wins, and files are processed in sorted order.
        bool merged_sets = false;
        if (extension_data.contains("console_filter_sets") &&
            extension_data["console_filter_sets"].is_object()) {
            if (!data.contains("console_filter_sets") || !data["console_filter_sets"].is_object()) {
                data["console_filter_sets"] = json::object();
            }
            auto& incoming = extension_data["console_filter_sets"];
            for (auto it = incoming.begin(); it != incoming.end(); ++it) {
                data["console_filter_sets"][it.key()] = it.value();
                merged_sets = true;
                spdlog::debug("[PrinterDetector] User console filter set '{}'", it.key());
            }
        }

        // Validate structure. A file that only contributes filter sets is
        // legitimate, so only complain when it carried nothing at all.
        if (!extension_data.contains("printers") || !extension_data["printers"].is_array()) {
            if (!merged_sets) {
                load_errors.push_back(fmt::format("{}: missing 'printers' array", file_path));
                spdlog::warn("[PrinterDetector] {}", load_errors.back());
            }
            return;
        }

        // Process each printer in the extension
        for (const auto& printer : extension_data["printers"]) {
            std::string id = helix::json_util::safe_string(printer, "id");
            if (id.empty()) {
                load_errors.push_back(fmt::format("{}: printer missing 'id' field", file_path));
                spdlog::warn("[PrinterDetector] {}", load_errors.back());
                continue;
            }

            // Check if printer is disabled
            bool enabled = helix::json_util::safe_bool(printer, "enabled", true);

            // Check if this overrides a bundled printer
            auto it = bundled_index.find(id);
            if (it != bundled_index.end()) {
                // Override bundled printer
                if (!enabled) {
                    // Mark as disabled (will be filtered out in list)
                    data["printers"][it->second]["enabled"] = false;
                    spdlog::debug("[PrinterDetector] Disabled bundled printer '{}'", id);
                } else {
                    // Replace bundled definition
                    data["printers"][it->second] = printer;
                    spdlog::debug("[PrinterDetector] User override for '{}'", id);
                }
                user_overrides++;
            } else {
                // Add new printer
                if (enabled) {
                    // Validate required fields for new printers
                    std::string name = helix::json_util::safe_string(printer, "name");
                    if (name.empty()) {
                        load_errors.push_back(
                            fmt::format("{}: printer '{}' missing 'name' field", file_path, id));
                        spdlog::warn("[PrinterDetector] {}", load_errors.back());
                        continue;
                    }

                    data["printers"].push_back(printer);
                    bundled_index[id] = data["printers"].size() - 1;
                    spdlog::debug("[PrinterDetector] Added user printer '{}'", name);
                    user_additions++;
                }
            }
        }

        spdlog::debug("[PrinterDetector] Processed extension file: {}", file_path);
    }
};

PrinterDatabase g_database;

/// The database's "printers" array, or nullptr when the database cannot load.
const json* database_printers() {
    return g_database.load() ? &g_database.data["printers"] : nullptr;
}
} // namespace

// ============================================================================
// Helper Functions
// ============================================================================

namespace helix {
// Declared in printer_discovery.h — shared with probe_preparation rules.
// Case-insensitive substring search. A leading '^' pins the pattern to the
// start of a name and a trailing '$' to its end, so "^box$" names exactly the
// Klipper object "box" and not a "gcode_macro BOX_UNLOAD" that contains it.
bool has_pattern(const std::vector<std::string>& objects, const std::string& pattern) {
    std::string pattern_lower = helix::text_io::to_lower(pattern);

    const bool at_start = !pattern_lower.empty() && pattern_lower.front() == '^';
    const bool at_end = pattern_lower.size() > (at_start ? 1u : 0u) && pattern_lower.back() == '$';
    const std::string core = pattern_lower.substr(
        at_start ? 1 : 0, pattern_lower.size() - (at_start ? 1 : 0) - (at_end ? 1 : 0));

    if (core.empty()) {
        return false; // a bare anchor names nothing
    }

    return std::any_of(objects.begin(), objects.end(), [&](const std::string& obj) {
        std::string obj_lower = helix::text_io::to_lower(obj);
        if (at_start && at_end)
            return obj_lower == core;
        if (at_start)
            return obj_lower.rfind(core, 0) == 0;
        if (at_end)
            return obj_lower.size() >= core.size() &&
                   obj_lower.compare(obj_lower.size() - core.size(), core.size(), core) == 0;
        return obj_lower.find(core) != std::string::npos;
    });
}
} // namespace helix

namespace {

// Check if all patterns in array are present
bool has_all_patterns(const std::vector<std::string>& objects, const json& patterns) {
    for (const auto& pattern : patterns) {
        if (!pattern.is_string())
            continue;
        if (!has_pattern(objects, pattern.get<std::string>())) {
            return false;
        }
    }
    return true;
}

// Get field data from hardware based on field name
// Returns a vector by value for string fields, reference for vector fields
std::vector<std::string> get_field_data(const PrinterHardwareData& hardware,
                                        const std::string& field) {
    if (field == "sensors")
        return hardware.sensors;
    if (field == "fans")
        return hardware.fans;
    if (field == "heaters")
        return hardware.heaters;
    if (field == "leds")
        return hardware.leds;
    if (field == "printer_objects")
        return hardware.printer_objects;
    if (field == "steppers")
        return hardware.steppers;
    if (field == "hostname")
        return {hardware.hostname};
    if (field == "kinematics")
        return {hardware.kinematics};
    if (field == "mcu")
        return {hardware.mcu};
    if (field == "cpu_arch")
        return {hardware.cpu_arch};

    // Unknown field - return empty vector
    return {};
}

// Count Z steppers in the steppers list
int count_z_steppers(const std::vector<std::string>& steppers) {
    int count = 0;
    for (const auto& stepper : steppers) {
        std::string stepper_lower = helix::text_io::to_lower(stepper);

        // Match stepper_z, stepper_z1, stepper_z2, stepper_z3 patterns
        if (stepper_lower.find("stepper_z") == 0) {
            count++;
        }
    }
    return count;
}

// Heuristic types that corroborate an identification but can never establish
// one. Build volume is shared by dozens of printers - it narrows a field, it
// does not name a machine - so a printer whose ONLY evidence is its bed size
// is not identified at all.
// Corroborating evidence can support an identification made on other grounds
// but can never make one by itself.
//
// Build volumes are corroborating by default because dozens of printers share a
// band. An entry whose volume really is distinctive opts back in with
// "identifying": true -- a 120mm cube has no neighbours the way 220-250mm does.
//
// A heuristic naming something shared across vendors -- a chamber thermistor, an
// MCU part number -- opts out with "corroborating": true. Those describe a class
// of printer, not a model, and at identifying strength they outscore every real
// signal a simpler machine has (prestonbrown/helixscreen#1489).
//
// A volume can also carry a third role: separation. Within a family the entry's
// other heuristics identify, a bed size that names the model opts in with
// "separator": true. It stays corroborating - never the base - but its weight
// joins the uncapped score, so the sibling whose window the bed missed loses by
// a real margin instead of a heuristic-count tiebreak. The same opt-in applies
// to hardware: an MCU part number or a mainboard name can separate vendors
// whose class evidence otherwise ties.
bool is_corroborating_only(const json& heuristic) {
    if (helix::json_util::safe_bool(heuristic, "corroborating", false)) {
        return true;
    }
    if (helix::json_util::safe_string(heuristic, "type") == "build_volume_range") {
        return !helix::json_util::safe_bool(heuristic, "identifying", false);
    }
    return false;
}

// Check if build volume is within specified range. A window measures stepper
// travel unless the heuristic sets "measure": "declared_bed", which reads the
// bed size the firmware's config declares instead: travel also covers
// overtravel to a purge or nozzle-clean position, which is not the bed. A
// firmware that declares no bed matches no declared-bed window; nothing falls
// back to travel.
bool check_build_volume_range(const BuildVolume& volume, const json& heuristic) {
    const std::string measure = helix::json_util::safe_string(heuristic, "measure");
    float x_size = 0.0f;
    float y_size = 0.0f;
    if (measure.empty()) {
        x_size = volume.x_max - volume.x_min;
        y_size = volume.y_max - volume.y_min;
    } else if (measure == "declared_bed") {
        x_size = volume.declared_bed_x;
        y_size = volume.declared_bed_y;
    } else {
        spdlog::warn("[PrinterDetector] build_volume_range measure '{}' is unknown, not matching",
                     measure);
        return false;
    }

    // If no volume data, can't match
    if (x_size <= 0 || y_size <= 0) {
        return false;
    }

    // Check X range (safe_float falls back to the default on a wrong-typed key)
    if (heuristic.contains("min_x")) {
        float min_x = helix::json_util::safe_float(heuristic, "min_x", 0.0f);
        if (x_size < min_x)
            return false;
    }
    if (heuristic.contains("max_x")) {
        float max_x = helix::json_util::safe_float(heuristic, "max_x", 0.0f);
        if (x_size > max_x)
            return false;
    }

    // Check Y range
    if (heuristic.contains("min_y")) {
        float min_y = helix::json_util::safe_float(heuristic, "min_y", 0.0f);
        if (y_size < min_y)
            return false;
    }
    if (heuristic.contains("max_y")) {
        float max_y = helix::json_util::safe_float(heuristic, "max_y", 0.0f);
        if (y_size > max_y)
            return false;
    }

    return true;
}
} // namespace

// ============================================================================
// Heuristic Execution Engine
// ============================================================================

namespace {
// Special return value: -1 means "exclude this printer entirely"
constexpr int kHeuristicExclude = -1;

// Execute a single heuristic and return confidence (0 = no match, -1 = exclude)
int execute_heuristic(const json& heuristic, const PrinterHardwareData& hardware) {
    std::string type = helix::json_util::safe_string(heuristic, "type");
    std::string field = helix::json_util::safe_string(heuristic, "field");
    int confidence = helix::json_util::safe_int(heuristic, "confidence");

    if (type == "sensor_match" || type == "fan_match" || type == "hostname_match" ||
        type == "led_match") {
        // Simple pattern matching in specified field
        auto field_data = get_field_data(hardware, field);
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Matched {} pattern '{}' (confidence: {})", type,
                          pattern, confidence);
            return confidence;
        }
    } else if (type == "led_required" || type == "sensor_required" || type == "fan_required" ||
               type == "object_required") {
        // Absence as evidence: the entry's machine always carries this pattern,
        // so a snapshot that lacks it is some other machine. Only a reported list
        // can be missing something - an unreported one says nothing about the
        // hardware, and firing on it would score a real match as the wrong
        // model while discovery is still filling in.
        if (!hardware.objects_reported) {
            return 0;
        }
        auto field_data = get_field_data(hardware, field);
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (!has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Excluded by {}: '{}' absent from {}", type, pattern,
                          field);
            return kHeuristicExclude;
        }
    } else if (type == "led_exclude" || type == "sensor_exclude" || type == "fan_exclude" ||
               type == "object_exclude") {
        // Presence as evidence against: the entry's machine never carries this
        // pattern, so a snapshot that has it is some other machine. The
        // counterpart of *_required for the sibling that lacks the hardware.
        auto field_data = get_field_data(hardware, field);
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Excluded by {}: '{}' present in {}", type, pattern,
                          field);
            return kHeuristicExclude;
        }
    } else if (type == "hostname_exclude") {
        // If hostname matches this pattern, exclude this printer entirely
        auto field_data = get_field_data(hardware, field);
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Excluded by {} pattern '{}'", type, pattern);
            return kHeuristicExclude;
        }
    } else if (type == "fan_combo") {
        // Multiple patterns must all be present
        auto field_data = get_field_data(hardware, field);
        if (heuristic.contains("patterns") && heuristic["patterns"].is_array()) {
            if (has_all_patterns(field_data, heuristic["patterns"])) {
                spdlog::debug("[PrinterDetector] Matched fan combo (confidence: {})", confidence);
                return confidence;
            }
        }
    } else if (type == "kinematics_match") {
        // Match against printer kinematics type (corexy, cartesian, delta, etc.)
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (!hardware.kinematics.empty()) {
            std::string kinematics_lower = helix::text_io::to_lower(hardware.kinematics);
            std::string pattern_lower = helix::text_io::to_lower(pattern);

            if (kinematics_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Matched kinematics '{}' (confidence: {})", pattern,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "kinematics_exclude") {
        // If the printer's kinematics matches this pattern, exclude it entirely.
        // Encodes a hard hardware rule — e.g. every Qidi machine is corexy, so a
        // cartesian printer is never a Qidi regardless of build volume. Only fires
        // when kinematics is known; an empty/unreported value never excludes.
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (!hardware.kinematics.empty()) {
            std::string kinematics_lower = helix::text_io::to_lower(hardware.kinematics);
            std::string pattern_lower = helix::text_io::to_lower(pattern);
            if (kinematics_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Excluded by kinematics '{}'", pattern);
                return kHeuristicExclude;
            }
        }
    } else if (type == "object_exists") {
        // Check if a Klipper object exists in the printer_objects list
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (has_pattern(hardware.printer_objects, pattern)) {
            spdlog::debug("[PrinterDetector] Found object '{}' (confidence: {})", pattern,
                          confidence);
            return confidence;
        }
    } else if (type == "stepper_count") {
        // Count Z steppers and match against pattern (z_count_1, z_count_2, z_count_3, z_count_4)
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        int z_count = count_z_steppers(hardware.steppers);

        // Also check for delta steppers (stepper_a, stepper_b, stepper_c)
        if (pattern == "stepper_a") {
            // Delta printer detection via stepper naming
            if (has_pattern(hardware.steppers, "stepper_a")) {
                spdlog::debug("[PrinterDetector] Found delta stepper pattern (confidence: {})",
                              confidence);
                return confidence;
            }
        } else {
            // Parse expected count from pattern (z_count_N)
            int expected_count = 0;
            if (pattern == "z_count_1")
                expected_count = 1;
            else if (pattern == "z_count_2")
                expected_count = 2;
            else if (pattern == "z_count_3")
                expected_count = 3;
            else if (pattern == "z_count_4")
                expected_count = 4;

            if (expected_count > 0 && z_count == expected_count) {
                spdlog::debug("[PrinterDetector] Matched {} Z steppers (confidence: {})", z_count,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "build_volume_range") {
        // Check if build volume is within specified range
        if (check_build_volume_range(hardware.build_volume, heuristic)) {
            spdlog::debug("[PrinterDetector] Matched build volume range (confidence: {})",
                          confidence);
            return confidence;
        }
    } else if (type == "mcu_match") {
        // Match against MCU chip type
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (!hardware.mcu.empty()) {
            std::string mcu_lower = helix::text_io::to_lower(hardware.mcu);
            std::string pattern_lower = helix::text_io::to_lower(pattern);

            if (mcu_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Matched MCU '{}' (confidence: {})", pattern,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "tool_count") {
        // Count extruders with the strict naming grammar: a tool count is
        // exactly what it exists for, so an `extruder`-prefixed heater that is
        // not a numbered extruder (extruder_mixing, extruder_stepper, a
        // four-digit index) must not inflate it. Discovery's looser prefix
        // match is for heater enumeration and stays loose.
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        const int extruder_count = static_cast<int>(helix::count_extruder_names(hardware.heaters));

        // Parse expected count from pattern (tool_count_N)
        if (pattern.rfind("tool_count_", 0) == 0) {
            const auto parsed_count = helix::text_io::parse_leading<int>(pattern.substr(11));
            if (!parsed_count) {
                spdlog::warn("[PrinterDetector] Invalid tool_count pattern: {}", pattern);
                return 0;
            }
            const int expected_count = *parsed_count;
            if (extruder_count == expected_count) {
                spdlog::debug("[PrinterDetector] Matched {} extruders (confidence: {})",
                              extruder_count, confidence);
                return confidence;
            }
        }
    } else if (type == "cpu_arch_match") {
        // Case-insensitive substring match of cpu_arch against pattern
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        if (!hardware.cpu_arch.empty()) {
            std::string cpu_lower = helix::text_io::to_lower(hardware.cpu_arch);
            std::string pattern_lower = helix::text_io::to_lower(pattern);

            if (cpu_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Matched CPU arch '{}' (confidence: {})", pattern,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "board_match") {
        // Match against board names found in temperature_sensor objects
        // Board names appear as "temperature_sensor <BOARD_NAME>" in the objects list
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        std::string pattern_lower = helix::text_io::to_lower(pattern);

        for (const auto& obj : hardware.printer_objects) {
            if (obj.rfind("temperature_sensor ", 0) == 0 ||
                obj.rfind("temperature_host ", 0) == 0) {
                std::string sensor_name = obj.substr(obj.find(' ') + 1);
                std::string sensor_lower = helix::text_io::to_lower(sensor_name);

                if (sensor_lower.find(pattern_lower) != std::string::npos) {
                    spdlog::debug("[PrinterDetector] Matched board '{}' in sensor '{}' "
                                  "(confidence: {})",
                                  pattern, sensor_name, confidence);
                    return confidence;
                }
            }
        }
    } else if (type == "macro_match" || type == "macro_exclude") {
        // Match against G-code macro names in printer_objects
        // G-code macros appear as "gcode_macro <NAME>" in the objects list
        // macro_exclude: if the macro IS present, exclude this printer entirely
        std::string pattern = helix::json_util::safe_string(heuristic, "pattern");
        std::string pattern_lower = helix::text_io::to_lower(pattern);

        for (const auto& obj : hardware.printer_objects) {
            // Check if object is a G-code macro
            if (obj.rfind("gcode_macro ", 0) == 0) {
                // Extract macro name (everything after "gcode_macro ")
                std::string macro_name = obj.substr(12);
                std::string macro_lower = helix::text_io::to_lower(macro_name);

                if (macro_lower.find(pattern_lower) != std::string::npos) {
                    if (type == "macro_exclude") {
                        spdlog::debug("[PrinterDetector] Excluded by macro '{}' ", macro_name);
                        return kHeuristicExclude;
                    }
                    spdlog::debug("[PrinterDetector] Matched macro '{}' (confidence: {})",
                                  macro_name, confidence);
                    return confidence;
                }
            }
        }
    } else {
        spdlog::warn("[PrinterDetector] Unknown heuristic type: {}", type);
    }

    return 0; // No match
}

// Execute all heuristics for a printer and return combined confidence + reason
PrinterDetectionResult execute_printer_heuristics(const json& printer,
                                                  const PrinterHardwareData& hardware) {
    std::string printer_id = helix::json_util::safe_string(printer, "id");
    std::string printer_name = helix::json_util::safe_string(printer, "name");

    if (!printer.contains("heuristics") || !printer["heuristics"].is_array()) {
        return {"", 0, "", 0};
    }

    // Collect ALL matching heuristics
    struct HeuristicMatch {
        int confidence;
        std::string reason;
        bool corroborating; // may support a match, may never establish one
        bool volume_based;  // came from a build_volume_range window
        bool separator;     // opted-in window naming the model within its family
    };
    std::vector<HeuristicMatch> matches;
    bool declares_kinematics = false;
    bool kinematics_matched = false;

    for (const auto& heuristic : printer["heuristics"]) {
        const bool is_kinematics =
            helix::json_util::safe_string(heuristic, "type") == "kinematics_match";
        declares_kinematics = declares_kinematics || is_kinematics;

        int confidence = execute_heuristic(heuristic, hardware);
        if (confidence == kHeuristicExclude) {
            spdlog::debug("[PrinterDetector] {} excluded by heuristic: {}", printer_name,
                          helix::json_util::safe_string(heuristic, "reason"));
            return {"", 0, "", 0};
        }
        if (confidence > 0) {
            kinematics_matched = kinematics_matched || is_kinematics;
            matches.push_back(
                {confidence, helix::json_util::safe_string(heuristic, "reason"),
                 is_corroborating_only(heuristic),
                 helix::json_util::safe_string(heuristic, "type") == "build_volume_range",
                 helix::json_util::safe_bool(heuristic, "separator", false)});
        }
    }

    if (matches.empty()) {
        return {"", 0, "", 0};
    }

    // Sort by confidence descending to get best match first
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    // The base score must come from a heuristic that actually names this printer,
    // never from corroborating evidence alone: a build volume is shared by dozens
    // of models (at 215-235mm up to 15 database windows cover the same point), and
    // a chamber sensor or MCU part number is shared across whole vendors. With
    // nothing but corroborating matches, the printer scores nothing at all.
    auto identifying = std::find_if(matches.begin(), matches.end(),
                                    [](const auto& m) { return !m.corroborating; });
    if (identifying == matches.end()) {
        spdlog::debug("[PrinterDetector] {} matched only corroborating evidence "
                      "(build volume, chamber sensor, MCU) - not identifying, scoring 0",
                      printer_name);
        return {"", 0, "", 0};
    }

    // An opted-in volume may lead, but never on its own and never on the wrong
    // motion system. A bed size is not an identification: a bare 120mm cartesian
    // rig shares the Voron 0's window and is not a Voron 0. Requiring both another
    // match and the entry's own kinematics keeps the opt-in from reintroducing the
    // identify-on-size-alone failure that made a 500mm Geralkom auto-save as a
    // Sovol.
    if (identifying->volume_based &&
        (matches.size() < 2 || (declares_kinematics && !kinematics_matched))) {
        spdlog::debug("[PrinterDetector] {} led on an identifying volume without "
                      "matching kinematics or any second match - a bed size alone is "
                      "not an identification, scoring 0",
                      printer_name);
        return {"", 0, "", 0};
    }

    // Combined scoring: base + bonus for additional matches
    // 3 points per extra match, capped at 12 (4 extra matches worth)
    constexpr int kBonusPerExtraMatch = 3;
    constexpr int kMaxBonus = 12;

    int base_confidence = identifying->confidence;
    // The bonus stacks identifying evidence only. Corroborating evidence
    // supports an identification through match_count and opted-in separation;
    // letting it raise the bonus would separate look-alikes by how much
    // corroboration one entry happens to author, which is the match-count
    // tiebreak in another form.
    const int identifying_matches = static_cast<int>(std::count_if(
        matches.begin(), matches.end(), [](const auto& m) { return !m.corroborating; }));
    const int bonus = std::min((identifying_matches - 1) * kBonusPerExtraMatch, kMaxBonus);
    int combined = std::min(base_confidence + bonus, 100);

    // A separator volume names the model inside a family the entry's other
    // evidence has already identified. Its weight lands in the uncapped score -
    // where margin() is measured - so the sibling whose window the bed missed
    // loses by a real lead instead of a heuristic-count tiebreak. It never
    // raises the published score, and it fires only when the non-volume base
    // clears the autosave bar: below that bar a bed size must not tip any
    // scale, or a sparse rig sharing the window collects a sibling's name it
    // did not earn (prestonbrown/helixscreen#1606). A window leading as
    // identifying evidence already carries its weight and adds nothing here.
    int separation = 0;
    if (!identifying->volume_based && base_confidence >= PrinterDetector::kAutosaveMinConfidence) {
        for (const auto& m : matches) {
            if (m.separator) {
                separation = m.confidence;
                break;
            }
        }
    }

    // Format reason with match count if multiple matches
    std::string reason = identifying->reason;
    if (matches.size() > 1) {
        reason += fmt::format(" (+{} more)", matches.size() - 1);
    }

    spdlog::debug("[PrinterDetector] {} scored {}% (base {} + bonus {} from {} identifying "
                  "matches, separation {})",
                  printer_name, combined, base_confidence, bonus, identifying_matches, separation);

    PrinterDetectionResult result{printer_name, combined, reason, static_cast<int>(matches.size()),
                                  base_confidence};
    result.uncapped_confidence = base_confidence + bonus + separation;
    return result;
}
} // namespace

// ============================================================================
// Main Detection Entry Point
// ============================================================================

PrinterDetectionResult PrinterDetector::detect(const PrinterHardwareData& hardware) {
    // Verbose debug output for troubleshooting detection issues
    spdlog::info("[PrinterDetector] Running detection with {} sensors, {} fans, hostname '{}'",
                 hardware.sensors.size(), hardware.fans.size(), hardware.hostname);
    spdlog::info("[PrinterDetector]   printer_objects: {}, steppers: {}, kinematics: '{}'",
                 hardware.printer_objects.size(), hardware.steppers.size(), hardware.kinematics);

    // A prior detection compacts the in-memory database to reclaim memory,
    // stripping every printer's heuristics array. Detection needs those
    // heuristics, so restore them from disk before matching. This is the path
    // hit when a second printer is added in the same process session: the first
    // printer's detection (or startup auto-detect) already compacted the shared
    // database, which would otherwise leave detect() with nothing to match
    // against and misclassify the new printer as "Unknown".
    if (g_database.compacted) {
        g_database.reload();
    }
    const json* printers = database_printers();
    if (printers == nullptr) {
        LOG_ERROR_INTERNAL("[PrinterDetector] Cannot perform detection without database");
        return {"", 0, "Failed to load printer database"};
    }

    PrinterDetectionResult best_match{"", 0, "No distinctive hardware detected"};

    // Candidates are separated by the machine they name. The image is the
    // physical printer the user owns; the preset is a configuration applied
    // to it, and a firmware variant of one printer is not a second answer to
    // "which printer is this". So two entries picturing the same machine
    // are interchangeable here, however their presets differ.
    struct ScoredCandidate {
        PrinterDetectionResult result;
        std::string preset;
        std::string image;

        bool same_outcome_as(const ScoredCandidate& other) const {
            return image == other.image;
        }
    };
    std::vector<ScoredCandidate> candidates;

    for (const auto& printer : *printers) {
        PrinterDetectionResult result = execute_printer_heuristics(printer, hardware);

        if (result.confidence > 0) {
            spdlog::debug("[PrinterDetector] Candidate: '{}' scored {}% ({} matches, best={}%) "
                          "via: {}",
                          result.type_name, result.confidence, result.match_count,
                          result.best_single_confidence, result.reason);
        }

        // Non-printer addons (show_in_list: false) can't win detection
        // They're scored and logged for diagnostics, but excluded from the winner
        if (!helix::json_util::safe_bool(printer, "show_in_list", true)) {
            if (result.confidence > 0) {
                spdlog::debug("[PrinterDetector]   [excluded from winner - not a real printer]");
            }
            continue;
        }

        if (result.confidence <= 0) {
            continue;
        }

        candidates.push_back({std::move(result), helix::json_util::safe_string(printer, "preset"),
                              helix::json_util::safe_string(printer, "image")});
    }

    // Equal on every tiebreaker leaves database order deciding, so the sort
    // has to be stable to keep answering the way the loop that fed it did.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const ScoredCandidate& a, const ScoredCandidate& b) {
                         return std::tie(b.result.confidence, b.result.best_single_confidence,
                                         b.result.match_count) <
                                std::tie(a.result.confidence, a.result.best_single_confidence,
                                         a.result.match_count);
                     });

    if (!candidates.empty()) {
        const ScoredCandidate& winner = candidates.front();
        best_match = winner.result;
        best_match.preset = winner.preset;

        for (const auto& candidate : candidates) {
            if (candidate.result.confidence != best_match.confidence) {
                break; // sorted by confidence: the tied run ends here
            }
            ++best_match.tied_count;
        }

        for (auto it = candidates.begin() + 1; it != candidates.end(); ++it) {
            if (!it->same_outcome_as(winner)) {
                best_match.runner_up_type_name = it->result.type_name;
                best_match.runner_up_confidence = it->result.confidence;
                best_match.runner_up_uncapped_confidence = it->result.uncapped_confidence;
                break;
            }
        }

        // The machines a person has to choose between: every outcome the
        // winner does not lead by the margin floor, one name per machine.
        // Listed past the winner only when the result is ambiguous, so the
        // list never disagrees with margin().
        best_match.contenders.push_back(winner.result.type_name);
        if (best_match.ambiguous()) {
            std::vector<const ScoredCandidate*> listed{&winner};
            for (auto it = candidates.begin() + 1; it != candidates.end(); ++it) {
                const bool separated =
                    best_match.lead_over(it->result.confidence, it->result.uncapped_confidence) >=
                    kDetectMinMargin;
                const bool same_machine =
                    std::any_of(listed.begin(), listed.end(),
                                [&](const ScoredCandidate* c) { return it->same_outcome_as(*c); });
                if (!separated && !same_machine) {
                    listed.push_back(&*it);
                    best_match.contenders.push_back(it->result.type_name);
                }
            }
        }
    }

    if (best_match.confidence > 0) {
        spdlog::info("[PrinterDetector] Detection complete: {} (confidence: {}%, {} matches, "
                     "margin: {} over '{}', {} tied, reason: {})",
                     best_match.type_name, best_match.confidence, best_match.match_count,
                     best_match.margin(), best_match.runner_up_type_name, best_match.tied_count,
                     best_match.reason);
    } else {
        spdlog::debug("[PrinterDetector] No distinctive fingerprints detected");
    }

    return best_match;
}

namespace {
/// The database entry named `printer_name` (case-insensitive), or nullptr.
/// `match_id` also accepts the entry's "id" in place of its name.
const json* find_printer_entry(const std::string& printer_name, bool match_id = false) {
    if (printer_name.empty()) {
        return nullptr;
    }
    const json* printers = database_printers();
    if (printers == nullptr) {
        spdlog::warn("[PrinterDetector] Cannot look up '{}' without the printer database",
                     printer_name);
        return nullptr;
    }
    const std::string wanted = helix::text_io::to_lower(printer_name);
    for (const auto& printer : *printers) {
        if (helix::text_io::to_lower(helix::json_util::safe_string(printer, "name")) == wanted ||
            (match_id &&
             helix::text_io::to_lower(helix::json_util::safe_string(printer, "id")) == wanted)) {
            return &printer;
        }
    }
    return nullptr;
}

/// Member `field` of the entry named `printer_name`, or nullptr when either is absent.
const json* printer_member(const std::string& printer_name, const char* field) {
    const json* printer = find_printer_entry(printer_name);
    return printer ? helix::json_util::find_member(*printer, field) : nullptr;
}

/// A boolean field of the entry named `printer_name`, @p fallback when either is absent.
bool printer_flag(const std::string& printer_name, const char* field, bool fallback = false) {
    const json* printer = find_printer_entry(printer_name);
    return printer ? helix::json_util::safe_bool(*printer, field, fallback) : fallback;
}

/// One string field of the entry named `printer_name`, or "" when either is absent.
std::string printer_field(const std::string& printer_name, const char* field) {
    const json* printer = find_printer_entry(printer_name);
    std::string value = printer ? helix::json_util::safe_string(*printer, field) : std::string();
    spdlog::debug("[PrinterDetector] {} for '{}': '{}'", field, printer_name, value);
    return value;
}
} // namespace

// ============================================================================
// Image Lookup Functions
// ============================================================================

std::string PrinterDetector::get_image_for_printer(const std::string& printer_name) {
    return get_image_for_printer(printer_name, helix::printer_images::HardwareFacts{});
}

std::string
PrinterDetector::get_image_for_printer(const std::string& printer_name,
                                       const helix::printer_images::HardwareFacts& facts) {
    const json* printer = find_printer_entry(printer_name);
    std::string image = printer ? helix::printer_images::select_image(*printer, facts) : "";
    spdlog::debug("[PrinterDetector] image for '{}' ({} filament system(s), enclosure top {}, "
                  "chamber heater {}): '{}'",
                  printer_name, facts.filament_systems.size(), facts.enclosure_top,
                  facts.has_chamber_heater, image);
    return image;
}

namespace {

/// Append `spec` unless it is already present. Sets are allowed to overlap, and a
/// duplicate pattern is pure per-line matching cost with no change in behaviour.
void append_unique_spec(std::vector<std::string>& out, std::string spec) {
    if (std::find(out.begin(), out.end(), spec) == out.end()) {
        out.push_back(std::move(spec));
    }
}

/// Copy the pattern strings out of one `console_filter_sets` entry into `out`.
/// A missing or malformed set is a warning, not a failure: the database is
/// user-editable via printer_database.d/, so a typo must not take the console
/// down with it.
void collect_filter_set(const json* sets, const std::string& set_name,
                        std::vector<std::string>& out) {
    if (!sets) {
        spdlog::warn("[PrinterDetector] Console filter set '{}' requested but the database "
                     "defines no 'console_filter_sets' table",
                     set_name);
        return;
    }
    const auto it = sets->find(set_name);
    if (it == sets->end()) {
        spdlog::warn("[PrinterDetector] Unknown console filter set '{}', ignoring", set_name);
        return;
    }
    if (!it->is_object() || !it->contains("patterns") || !(*it)["patterns"].is_array()) {
        spdlog::warn("[PrinterDetector] Console filter set '{}' has no 'patterns' array, ignoring",
                     set_name);
        return;
    }
    for (const auto& spec : (*it)["patterns"]) {
        if (spec.is_string()) {
            append_unique_spec(out, spec.get<std::string>());
        } else {
            spdlog::warn("[PrinterDetector] Non-string pattern in console filter set '{}', "
                         "skipping",
                         set_name);
        }
    }
}

} // namespace

std::vector<std::string>
PrinterDetector::get_console_filter_patterns(const std::string& printer_name) {
    std::vector<std::string> patterns;
    const json* printer = find_printer_entry(printer_name, /*match_id=*/true);
    if (printer == nullptr) {
        return patterns;
    }
    const json* sets = helix::json_util::find_member(g_database.data, "console_filter_sets");
    if (sets != nullptr && !sets->is_object()) {
        sets = nullptr;
    }

    // Named sets first — these are the shared vocabulary, so a fix to one
    // shape reaches every printer that references it. `console_filter_patterns`
    // then adds anything specific enough to this model that a set would be
    // overkill, and is what pre-set databases still carry.
    const json* names = helix::json_util::find_member(*printer, "console_filters");
    if (names != nullptr && names->is_array()) {
        for (const auto& name : *names) {
            if (name.is_string()) {
                collect_filter_set(sets, name.get<std::string>(), patterns);
            } else {
                spdlog::warn("[PrinterDetector] Non-string console_filters entry for '{}', "
                             "skipping",
                             helix::json_util::safe_string(*printer, "id", "?"));
            }
        }
    }

    const json* specs = helix::json_util::find_member(*printer, "console_filter_patterns");
    if (specs != nullptr && specs->is_array()) {
        for (const auto& spec : *specs) {
            if (spec.is_string()) {
                append_unique_spec(patterns, spec.get<std::string>());
            } else {
                spdlog::warn("[PrinterDetector] Non-string console_filter_patterns entry "
                             "for '{}', skipping",
                             helix::json_util::safe_string(*printer, "id", "?"));
            }
        }
    }
    return patterns;
}

std::string PrinterDetector::get_name_for_preset(const std::string& preset_name) {
    const json* printers = preset_name.empty() ? nullptr : database_printers();
    if (printers == nullptr) {
        return "";
    }

    std::string preset_lower = helix::text_io::to_lower(preset_name);

    // Entries sharing a preset are one family. The entry marked
    // "preset_default" names the family when nothing narrower is known; a
    // family without one answers with its first entry.
    std::string first_match;
    for (const auto& printer : *printers) {
        std::string db_preset = helix::json_util::safe_string(printer, "preset");
        std::string db_preset_lower = helix::text_io::to_lower(db_preset);

        if (db_preset_lower == preset_lower) {
            if (helix::json_util::safe_bool(printer, "preset_default", false)) {
                return helix::json_util::safe_string(printer, "name");
            }
            if (first_match.empty()) {
                first_match = helix::json_util::safe_string(printer, "name");
            }
        }
    }

    return first_match;
}

std::string PrinterDetector::get_preset_for_name(const std::string& printer_name) {
    return printer_field(printer_name, "preset");
}

std::string PrinterDetector::apply_preset_with_variants(helix::Config* config,
                                                        const std::string& preset,
                                                        const helix::PrinterDiscovery& discovery) {
    if (!config || preset.empty()) {
        return "";
    }

    // Firmware-variant detection: ZMOD renames standard Klipper objects
    // (e.g. "fan" → "fan_generic fanM106"). Probe for the variant signature
    // and prefer the "_zmod" preset when present.
    //
    // ForgeX is a DIFFERENT firmware modification — mutually exclusive with ZMOD.
    // ForgeX happens to use the same `fan_generic fanM106` / `heater_fan heat_fan`
    // names for its part/hotend fans, so the fanM106 marker alone is NOT a
    // ZMOD discriminator. If the printer DB resolved a `_forgex` preset, the
    // detector already locked onto ForgeX (typically via SUPPORT_FORGE_X macro
    // or the chamber-LED heuristic) — never try a `_zmod` variant on top of
    // that, it would either miss-load or compound rename errors.
    auto& objects = discovery.printer_objects();
    // Match the variant suffix exactly (preset name ends with `_forgex`) rather
    // than substring — a future preset whose name happens to contain "_forgex"
    // for unrelated reasons should not silently disable ZMOD detection.
    static constexpr const char* kForgeXSuffix = "_forgex";
    constexpr size_t kForgeXLen = 7; // strlen("_forgex")
    bool preset_is_forgex =
        preset.size() >= kForgeXLen &&
        preset.compare(preset.size() - kForgeXLen, kForgeXLen, kForgeXSuffix) == 0;
    auto has_object = [&objects](const char* name) {
        return std::find(objects.begin(), objects.end(), name) != objects.end();
    };
    // Stock Creator 5 / 5 Pro firmware names its part fan fanM106 too, so on
    // those presets only Z-Mod's own `zmod` object identifies it.
    bool preset_is_creator5 = preset.rfind("creator5", 0) == 0;
    const char* zmod_signal = has_object("zmod") ? "zmod"
                              : (!preset_is_creator5 && has_object("fan_generic fanM106"))
                                  ? "fan_generic fanM106"
                                  : nullptr;
    bool is_zmod = !preset_is_forgex && zmod_signal != nullptr;

    std::string applied = preset;
    if (is_zmod) {
        std::string zmod_preset = preset + "_zmod";
        spdlog::info("[PrinterDetector] ZMOD firmware detected ({}), trying preset '{}'",
                     zmod_signal, zmod_preset);
        if (config->apply_preset_file(zmod_preset)) {
            applied = zmod_preset;
        } else {
            spdlog::info("[PrinterDetector] No ZMOD preset variant, using '{}'", preset);
            if (!config->apply_preset_file(preset)) {
                return "";
            }
        }
    } else {
        if (!config->apply_preset_file(preset)) {
            return "";
        }
    }

    config->set_preset(applied);
    helix::reload_sensor_roles_after_preset();
    return applied;
}

// ============================================================================
// Dynamic List Builder
// ============================================================================

namespace {

// Extract kinematics type from a printer's heuristics array or compacted _kinematics field
// Returns the pattern value from the first kinematics_match heuristic, or ""
std::string extract_kinematics(const json& printer) {
    // Check compacted field first (available after compact())
    const std::string compacted = helix::json_util::safe_string(printer, "_kinematics");
    if (!compacted.empty()) {
        return helix::text_io::to_lower(compacted);
    }

    const json* heuristics = helix::json_util::find_member(printer, "heuristics");
    if (heuristics == nullptr || !heuristics->is_array()) {
        return "";
    }
    for (const auto& h : *heuristics) {
        if (helix::json_util::safe_string(h, "type") == "kinematics_match") {
            std::string pattern =
                helix::text_io::to_lower(helix::json_util::safe_string(h, "pattern"));
            return pattern;
        }
    }
    return "";
}

// Cached list data - built once and reused
struct ListCache {
    std::string options;                   // Newline-separated string for lv_roller_set_options()
    std::vector<std::string> names;        // Vector of names for index lookups
    std::vector<PrinterListEntry> entries; // names + manufacturers, index-aligned with names
    bool built = false;

    void reset() {
        options.clear();
        names.clear();
        entries.clear();
        built = false;
    }

    void build() {
        if (built)
            return;

        const json* printers = database_printers();
        if (printers == nullptr) {
            spdlog::warn("[PrinterDetector] Cannot build list without database");
            // Fallback to just Custom/Other and Unknown
            names = {"Custom/Other", "Unknown"};
            entries = {{"Custom/Other", ""}, {"Unknown", ""}};
            options = "Custom/Other\nUnknown";
            built = true;
            return;
        }

        // Collect all printers that should appear in list
        for (const auto& printer : *printers) {
            // Check enabled flag (defaults to true if missing) - allows user to hide bundled
            bool enabled = helix::json_util::safe_bool(printer, "enabled", true);
            if (!enabled) {
                continue;
            }

            // Check show_in_list flag (defaults to true if missing)
            bool show = helix::json_util::safe_bool(printer, "show_in_list", true);
            if (!show) {
                continue;
            }

            std::string name = helix::json_util::safe_string(printer, "name");
            if (!name.empty()) {
                entries.push_back({name, helix::json_util::safe_string(printer, "manufacturer")});
            }
        }

        // Sort alphabetically for consistent ordering, then derive names from
        // entries so the two vectors can never disagree.
        std::sort(
            entries.begin(), entries.end(),
            [](const PrinterListEntry& a, const PrinterListEntry& b) { return a.name < b.name; });
        for (const auto& entry : entries) {
            names.push_back(entry.name);
        }

        // Always append Custom/Other and Unknown at the end
        names.push_back("Custom/Other");
        names.push_back("Unknown");
        entries.push_back({"Custom/Other", ""});
        entries.push_back({"Unknown", ""});

        // Build newline-separated string for list display
        for (size_t i = 0; i < names.size(); ++i) {
            options += names[i];
            if (i < names.size() - 1) {
                options += "\n";
            }
        }

        spdlog::info("[PrinterDetector] Built list with {} printer types", names.size());
        built = true;
    }
};

ListCache g_list_cache;

ListCache g_filtered_list_cache;
std::string g_filtered_kinematics; // The kinematics filter currently applied

void build_filtered_list(const std::string& kinematics_filter) {
    if (g_filtered_list_cache.built && g_filtered_kinematics == kinematics_filter) {
        return; // Already built with same filter
    }

    g_filtered_list_cache.reset();
    g_filtered_kinematics = kinematics_filter;

    const json* printers = database_printers();
    if (printers == nullptr) {
        g_filtered_list_cache.names = {"Custom/Other", "Unknown"};
        g_filtered_list_cache.entries = {{"Custom/Other", ""}, {"Unknown", ""}};
        g_filtered_list_cache.options = "Custom/Other\nUnknown";
        g_filtered_list_cache.built = true;
        return;
    }

    std::string filter_lower = helix::text_io::to_lower(kinematics_filter);

    for (const auto& printer : *printers) {
        if (!helix::json_util::safe_bool(printer, "enabled", true))
            continue;
        if (!helix::json_util::safe_bool(printer, "show_in_list", true))
            continue;

        std::string name = helix::json_util::safe_string(printer, "name");
        if (name.empty())
            continue;

        // Apply kinematics filter (contains-match to handle variants like
        // limited_corexy, hybrid_corexz, generic_cartesian, etc.)
        std::string printer_kin = extract_kinematics(printer);
        if (!filter_lower.empty() && !printer_kin.empty() &&
            filter_lower.find(printer_kin) == std::string::npos) {
            continue; // Kinematics doesn't match filter, skip
        }
        // Printers with no kinematics heuristic are always included

        g_filtered_list_cache.entries.push_back(
            {name, helix::json_util::safe_string(printer, "manufacturer")});
    }

    std::sort(g_filtered_list_cache.entries.begin(), g_filtered_list_cache.entries.end(),
              [](const PrinterListEntry& a, const PrinterListEntry& b) { return a.name < b.name; });
    for (const auto& entry : g_filtered_list_cache.entries) {
        g_filtered_list_cache.names.push_back(entry.name);
    }
    g_filtered_list_cache.names.push_back("Custom/Other");
    g_filtered_list_cache.names.push_back("Unknown");
    g_filtered_list_cache.entries.push_back({"Custom/Other", ""});
    g_filtered_list_cache.entries.push_back({"Unknown", ""});

    for (size_t i = 0; i < g_filtered_list_cache.names.size(); ++i) {
        g_filtered_list_cache.options += g_filtered_list_cache.names[i];
        if (i < g_filtered_list_cache.names.size() - 1) {
            g_filtered_list_cache.options += "\n";
        }
    }

    spdlog::info("[PrinterDetector] Built filtered list ({}) with {} printer types",
                 kinematics_filter, g_filtered_list_cache.names.size());
    g_filtered_list_cache.built = true;
}

} // namespace

const std::string& PrinterDetector::get_list_options() {
    g_list_cache.build();
    return g_list_cache.options;
}

const std::vector<std::string>& PrinterDetector::get_list_names() {
    g_list_cache.build();
    return g_list_cache.names;
}

int PrinterDetector::find_list_index(const std::string& printer_name) {
    g_list_cache.build();

    // Case-insensitive search
    std::string name_lower = helix::text_io::to_lower(printer_name);

    for (size_t i = 0; i < g_list_cache.names.size(); ++i) {
        std::string cached_lower = helix::text_io::to_lower(g_list_cache.names[i]);

        if (cached_lower == name_lower) {
            return static_cast<int>(i);
        }
    }

    // Return Unknown index if not found
    return get_unknown_list_index();
}

std::string PrinterDetector::get_list_name_at(int index) {
    g_list_cache.build();

    if (index < 0 || static_cast<size_t>(index) >= g_list_cache.names.size()) {
        return "Unknown";
    }

    return g_list_cache.names[static_cast<size_t>(index)];
}

int PrinterDetector::get_unknown_list_index() {
    g_list_cache.build();

    // Unknown is always the last entry
    if (g_list_cache.names.empty()) {
        return 0;
    }
    return static_cast<int>(g_list_cache.names.size() - 1);
}

// ============================================================================
// Kinematics-Filtered List API
// ============================================================================

const std::string& PrinterDetector::get_list_options(const std::string& kinematics) {
    if (kinematics.empty())
        return get_list_options();
    build_filtered_list(kinematics);
    return g_filtered_list_cache.options;
}

const std::vector<std::string>& PrinterDetector::get_list_names(const std::string& kinematics) {
    if (kinematics.empty())
        return get_list_names();
    build_filtered_list(kinematics);
    return g_filtered_list_cache.names;
}

const std::vector<PrinterListEntry>&
PrinterDetector::get_list_entries(const std::string& kinematics) {
    if (kinematics.empty()) {
        g_list_cache.build();
        return g_list_cache.entries;
    }
    build_filtered_list(kinematics);
    return g_filtered_list_cache.entries;
}

int PrinterDetector::find_list_index(const std::string& printer_name,
                                     const std::string& kinematics) {
    if (kinematics.empty())
        return find_list_index(printer_name);
    build_filtered_list(kinematics);

    std::string name_lower = helix::text_io::to_lower(printer_name);

    for (size_t i = 0; i < g_filtered_list_cache.names.size(); ++i) {
        std::string cached_lower = helix::text_io::to_lower(g_filtered_list_cache.names[i]);
        if (cached_lower == name_lower) {
            return static_cast<int>(i);
        }
    }

    // Return Unknown index in filtered list
    return get_unknown_list_index(kinematics);
}

std::string PrinterDetector::get_list_name_at(int index, const std::string& kinematics) {
    if (kinematics.empty())
        return get_list_name_at(index);
    build_filtered_list(kinematics);

    if (index < 0 || static_cast<size_t>(index) >= g_filtered_list_cache.names.size()) {
        return "Unknown";
    }
    return g_filtered_list_cache.names[static_cast<size_t>(index)];
}

int PrinterDetector::get_unknown_list_index(const std::string& kinematics) {
    if (kinematics.empty())
        return get_unknown_list_index();
    build_filtered_list(kinematics);

    if (g_filtered_list_cache.names.empty())
        return 0;
    return static_cast<int>(g_filtered_list_cache.names.size() - 1);
}

// ============================================================================
// Pre-Print Option Set Lookup (printer-agnostic option framework)
// ============================================================================

PrePrintOptionSet PrinterDetector::get_pre_print_option_set(const std::string& printer_name) {
    PrePrintOptionSet result;
    const json* options = printer_member(printer_name, "pre_print_options");
    if (options == nullptr) {
        spdlog::debug("[PrinterDetector] No pre_print_options for printer '{}'", printer_name);
        return result;
    }
    result = parse_pre_print_option_set(*options);
    spdlog::info("[PrinterDetector] Found {} pre-print options for '{}' (macro: {})",
                 result.options.size(), printer_name, result.macro_name);
    return result;
}

// ============================================================================
// Z-Offset Calibration Strategy Lookup
// ============================================================================

std::string PrinterDetector::get_z_offset_calibration_strategy(const std::string& printer_name) {
    return printer_field(printer_name, "z_offset_calibration_strategy");
}

bool PrinterDetector::hide_manual_z_calibration(const std::string& printer_name) {
    return printer_flag(printer_name, "hide_manual_z_calibration");
}

bool PrinterDetector::z_offset_persists(const std::string& printer_name) {
    return printer_flag(printer_name, "z_offset_persists", true);
}

bool PrinterDetector::tool_change_resets_z_offset(const std::string& printer_name) {
    return printer_flag(printer_name, "tool_change_resets_z_offset");
}

// ============================================================================
// Probe Type Lookup
// ============================================================================

std::string PrinterDetector::get_probe_type(const std::string& printer_name) {
    return printer_field(printer_name, "probe_type");
}

namespace {

/// `calibration.<key>` as a string, empty when the entry, its calibration object
/// or the key is absent.
std::string calibration_string(const std::string& printer_name, const char* key) {
    const json* calibration = printer_member(printer_name, "calibration");
    return calibration ? helix::json_util::safe_string(*calibration, key) : "";
}

/// `calibration.<key>` is literally true.
bool calibration_flag(const std::string& printer_name, const char* key) {
    const json* calibration = printer_member(printer_name, "calibration");
    const json* flag = calibration ? helix::json_util::find_member(*calibration, key) : nullptr;
    return flag != nullptr && flag->is_boolean() && flag->get<bool>();
}

} // namespace

std::string PrinterDetector::get_bed_mesh_calibrate_gcode(const std::string& printer_name) {
    return calibration_string(printer_name, "bed_mesh_gcode");
}

std::string PrinterDetector::get_screws_tilt_gcode(const std::string& printer_name) {
    return calibration_string(printer_name, "screws_tilt_gcode");
}

bool PrinterDetector::get_screws_tilt_self_prepares(const std::string& printer_name) {
    return calibration_flag(printer_name, "screws_tilt_self_prepares");
}

// ============================================================================
// Pre-print lookups: profile, default phases, heating rates
// ============================================================================

bool PrinterDetector::get_bed_mesh_self_prepares(const std::string& printer_name) {
    return calibration_flag(printer_name, "bed_mesh_self_prepares");
}

std::string PrinterDetector::get_print_start_profile(const std::string& printer_name) {
    return printer_field(printer_name, "print_start_profile");
}

std::map<int, int>
PrinterDetector::get_print_start_default_phases(const std::string& printer_name) {
    std::map<int, int> result;
    const json* phases = printer_member(printer_name, "print_start_default_phases");
    if (phases == nullptr || !phases->is_object()) {
        return result;
    }
    for (auto it = phases->begin(); it != phases->end(); ++it) {
        // Only phases the prediction history keeps a duration for: a default
        // for anything else would be a number nothing consults.
        const auto phase = helix::print_start_phase_from_name(it.key());
        if (!phase || !helix::print_start_phase_stores_duration(*phase)) {
            spdlog::warn(
                "[PrinterDetector] Unknown print_start_default_phase '{}' for printer '{}'",
                it.key(), printer_name);
            continue;
        }
        if (!it.value().is_number_integer()) {
            spdlog::warn("[PrinterDetector] Non-integer duration for phase '{}' on '{}'", it.key(),
                         printer_name);
            continue;
        }
        result[static_cast<int>(*phase)] = it.value().get<int>();
    }
    spdlog::debug("[PrinterDetector] print_start_default_phases for '{}': {} entries", printer_name,
                  result.size());
    return result;
}

std::map<std::string, float> PrinterDetector::get_thermal_rates(const std::string& printer_name) {
    std::map<std::string, float> rates;
    const json* configured = printer_member(printer_name, "thermal_rates");
    if (configured == nullptr || !configured->is_object()) {
        return rates;
    }
    const auto& persisted = ThermalRateManager::kPersistedHeaters;
    for (const auto& [heater, rate] : configured->items()) {
        if (std::find(persisted.begin(), persisted.end(), heater) == persisted.end()) {
            spdlog::warn("[PrinterDetector] Ignoring thermal rate for '{}' on '{}': not a heater "
                         "the rate model keeps (extruder, heater_bed)",
                         heater, printer_name);
            continue;
        }
        if (!rate.is_number() || rate.get<float>() <= 0.0f) {
            spdlog::warn("[PrinterDetector] Ignoring thermal rate '{}' for '{}': not a positive "
                         "number",
                         heater, printer_name);
            continue;
        }
        rates[heater] = rate.get<float>();
    }
    return rates;
}

// ============================================================================
// Toolhead Style Lookup
// ============================================================================

std::string PrinterDetector::get_toolhead_style(const std::string& printer_name) {
    return printer_field(printer_name, "toolhead_style");
}

bool PrinterDetector::is_enclosed(const std::string& printer_name) {
    return printer_flag(printer_name, "enclosed");
}

helix::calibration::BeltTensionerKind
PrinterDetector::get_belt_tensioner(const std::string& printer_name) {
    using helix::calibration::BeltTensionerKind;
    const std::string kind = printer_field(printer_name, "belt_tensioner");
    if (kind == "screw")
        return BeltTensionerKind::SCREW;
    if (kind == "spring")
        return BeltTensionerKind::SPRING;
    return BeltTensionerKind::UNKNOWN;
}

bool PrinterDetector::klipper_hides_dot_dirs(const std::string& printer_name) {
    return printer_flag(printer_name, "klipper_hides_dot_dirs");
}

std::optional<helix::PlrDbBlock> PrinterDetector::get_plr_block(const std::string& printer_name) {
    const json* printer = find_printer_entry(printer_name);
    return printer ? helix::plr_parse_db_block(*printer) : std::nullopt;
}

// ============================================================================
// Memory Management
// ============================================================================

void PrinterDetector::compact_database() {
    g_database.compact();
}

// ============================================================================
// Reload and Status Functions
// ============================================================================

void PrinterDetector::reload() {
    spdlog::info("[PrinterDetector] Reloading printer database and extensions");
    g_list_cache.reset();
    g_filtered_list_cache.reset();
    g_filtered_kinematics.clear();
    g_database.reload();
}

PrinterDetector::LoadStatus PrinterDetector::get_load_status() {
    const json* printers = database_printers();

    LoadStatus status;
    status.loaded = g_database.loaded;
    status.total_printers = 0;
    status.user_overrides = g_database.user_overrides;
    status.user_additions = g_database.user_additions;
    status.loaded_files = g_database.loaded_files;
    status.load_errors = g_database.load_errors;

    // Count enabled printers
    if (printers != nullptr) {
        for (const auto& printer : *printers) {
            if (helix::json_util::safe_bool(printer, "enabled", true)) {
                status.total_printers++;
            }
        }
    }

    return status;
}

// ============================================================================
// Auto-Detection on Startup
// ============================================================================

PrinterDetectionResult PrinterDetector::auto_detect(const helix::PrinterDiscovery& discovery) {
    // Build PrinterHardwareData from discovery
    PrinterHardwareData hw_data;
    hw_data.heaters = discovery.heaters();
    hw_data.sensors = discovery.sensors();
    hw_data.fans = discovery.fans();
    hw_data.leds = discovery.leds();
    hw_data.hostname = discovery.hostname();
    hw_data.steppers = discovery.steppers();
    hw_data.printer_objects = discovery.printer_objects();
    hw_data.kinematics = discovery.kinematics();
    hw_data.build_volume = discovery.build_volume();
    hw_data.mcu = discovery.mcu();
    hw_data.mcu_list = discovery.mcu_list();
    hw_data.cpu_arch = discovery.cpu_arch();
    hw_data.objects_reported = discovery.objects_reported();

    return detect(hw_data);
}

std::string PrinterDetector::apply_type_choice(Config* config, const std::string& type_name,
                                               const helix::PrinterDiscovery& discovery) {
    if (!config || type_name.empty()) {
        spdlog::warn("[PrinterDetector] apply_type_choice called with no config or empty type");
        return {};
    }

    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, type_name);

    std::string applied;
    const std::string preset = get_preset_for_name(type_name);
    if (!preset.empty()) {
        applied = apply_preset_with_variants(config, preset, discovery);
        if (!applied.empty()) {
            spdlog::info("[PrinterDetector] Applied preset '{}' for user-chosen printer '{}'",
                         applied, type_name);
        }
    }

    config->save();
    get_printer_state().set_printer_type_sync(type_name);
    spdlog::info("[PrinterDetector] Printer type set by user: '{}'", type_name);
    return applied;
}

namespace {
/// Whether @p candidate names the same printer family as the installed @p family
/// preset: the family itself, or one of its variants (`ad5m` covers `ad5m_pro`,
/// `ad5m_pro_forgex`). The separator is required so that `ad5x` is not read as a
/// variant of `ad5m`.
bool preset_in_family(const std::string& candidate, const std::string& family) {
    if (candidate.empty() || family.empty()) {
        return false;
    }
    if (candidate == family) {
        return true;
    }
    return candidate.size() > family.size() && candidate.compare(0, family.size(), family) == 0 &&
           candidate[family.size()] == '_';
}
} // namespace

bool PrinterDetector::auto_detect_and_save(const helix::PrinterDiscovery& discovery,
                                           Config* config) {
    if (!config) {
        spdlog::warn("[PrinterDetector] auto_detect_and_save called with null config");
        return false;
    }

    // Check if printer type is already set
    std::string saved_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (!saved_type.empty()) {
        spdlog::debug("[PrinterDetector] Printer type already set: '{}', skipping auto-detection",
                      saved_type);
        // Even when the printer type is already saved, run the post-wizard
        // preset migration path. apply_preset_file checks wizard_completed and
        // only writes filament_sensors when the user's block is empty/missing —
        // a no-op for fully-configured installs, a one-shot fix for users whose
        // older preset didn't seed the block (Snapmaker U1 shipped pre-2026-05
        // with role=none on e1/e2/e3; tonight's preset fix only helped fresh
        // installs without this hook).
        std::string saved_preset = get_preset_for_name(saved_type);
        if (!saved_preset.empty()) {
            if (config->apply_preset_file(saved_preset)) {
                helix::reload_sensor_roles_after_preset();
            }
        }
        // Apply the saved type to PrinterState for the same reason the
        // detected-type path does: it is what resolves the pre-print option
        // set, the z-offset calibration strategy, the purge-line capability and
        // the probe-type override. Skipping it leaves every one of those at its
        // default, and the only other startup caller is PrinterImageWidget —
        // so a user who takes that tile off the home screen silently gets a
        // different calibration surface from one who keeps it.
        get_printer_state().set_printer_type_sync(saved_type);
        // Still compact if database was loaded (e.g., by list building)
        compact_database();
        return false;
    }

    // Run detection
    PrinterDetectionResult result = auto_detect(discovery);

    if (!meets_autosave_threshold(result)) {
        spdlog::info("[PrinterDetector] Detection below auto-save bar (best '{}' at {}%, "
                     "runner-up '{}' at {}%, margin {}, {} tied; need >={}% and margin >={})",
                     result.type_name, result.confidence, result.runner_up_type_name,
                     result.runner_up_confidence, result.margin(), result.tied_count,
                     kAutosaveMinConfidence, kDetectMinMargin);

        // Handing an ambiguous field back to the user assumes a step that asks.
        // A platform package seeds its preset into settings.json, and the wizard
        // skips every hardware step on a preset install - so on those machines
        // nobody is ever asked, the type stays empty for good, and a known
        // printer wears the generic image.
        //
        // The package named the machine family at install time, so a weak win
        // inside that family is still an answer: the winner is the variant
        // carrying the most corroboration. A result whose winner is outside the
        // family, or that names no machine at all, gets the family's default
        // name. A tie with at most one of the family's machines in it resolves
        // the same way, entries picturing the same machine counting as one: to
        // the winner when the winner is in the family, otherwise to the
        // family's default name, even when the family machine in the tie is a
        // different one. A tie between two of the family's own machines saves
        // nothing: any name would be a guess between them, so the Printer
        // Manager's model row is where the choice is made
        // (prestonbrown/helixscreen#1606).
        const std::string installed_preset = config->get_preset();
        const auto family_machines = std::count_if(
            result.contenders.begin(), result.contenders.end(), [&](const std::string& name) {
                return preset_in_family(get_preset_for_name(name), installed_preset);
            });
        std::string resolved;
        if (!installed_preset.empty() && family_machines < 2) {
            resolved = (result.detected() && preset_in_family(result.preset, installed_preset))
                           ? result.type_name
                           : get_name_for_preset(installed_preset);
        }

        if (resolved.empty()) {
            // PRINTER_TYPE stays empty so the wizard's identify step offers its
            // Custom/Other default for the user to correct, and a later reconnect
            // with a fuller discovery snapshot gets another chance instead of
            // being locked out by a non-empty saved type.
            //
            // Deliberately NOT compacting: compact_database() strips the heuristics,
            // and without them a later attempt could never match anything. Holding
            // them is the cost of staying open to a better answer.
            spdlog::info("[PrinterDetector] Leaving printer type unset for the user to choose");
            return false;
        }

        spdlog::info("[PrinterDetector] Preset '{}' install has no step that asks - resolving "
                     "printer type to '{}'",
                     installed_preset, resolved);
        result.type_name = resolved;
        result.preset = get_preset_for_name(resolved);
    } else {
        spdlog::info("[PrinterDetector] Auto-detected printer: '{}' ({}% confidence, reason: {})",
                     result.type_name, result.confidence, result.reason);
    }

    // Save to config
    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, result.type_name);
    if (!result.preset.empty()) {
        std::string applied = apply_preset_with_variants(config, result.preset, discovery);
        if (!applied.empty()) {
            spdlog::info("[PrinterDetector] Applied preset '{}' for printer '{}'", applied,
                         result.type_name);
        }
    }
    config->save();

    // Update PrinterState so home panel gets correct image and capabilities
    get_printer_state().set_printer_type_sync(result.type_name);

    // Strip heuristics to reclaim memory — the type is settled now.
    compact_database();

    return true;
}

/// Case-insensitive check whether the configured printer type contains @p needle.
static bool printer_type_contains(const std::string& needle) {
    Config* config = Config::get_instance();

    std::string printer_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (printer_type.empty()) {
        return false;
    }

    return helix::text_io::to_lower(printer_type).find(helix::text_io::to_lower(needle)) !=
           std::string::npos;
}

bool PrinterDetector::is_creality_k1() {
    return printer_type_contains("creality") && printer_type_contains("k1");
}

bool PrinterDetector::is_creality_k2() {
    return printer_type_contains("creality") && printer_type_contains("k2");
}

bool PrinterDetector::is_creality_hi() {
    return printer_type_contains("creality") && printer_type_contains("hi");
}

namespace helix::probe_prep {

/// Declared in probe_preparation.h. Lives here because g_database is file-local.
/// Returned by value: reload() would invalidate any reference into the database,
/// and the rule list is a handful of entries, not the 98-printer heuristics table.
nlohmann::json database_rules() {
    if (!g_database.load()) {
        return nlohmann::json::array();
    }
    const auto it = g_database.data.find("probe_preparation");
    if (it == g_database.data.end() || !it->is_array()) {
        return nlohmann::json::array();
    }
    return *it;
}

} // namespace helix::probe_prep

bool PrinterDetector::meets_autosave_threshold(const PrinterDetectionResult& result) {
    if (result.type_name.empty() || !result.detected())
        return false;
    return result.confidence >= kAutosaveMinConfidence && !result.ambiguous();
}

const char* PrinterDetector::mismatch_decision_name(MismatchDecision decision) {
    switch (decision) {
    case MismatchDecision::Warn:
        return "warn";
    case MismatchDecision::NoDetection:
        return "no detection";
    case MismatchDecision::ConfidenceTooLow:
        return "confidence below threshold";
    case MismatchDecision::Ambiguous:
        return "tied with a rival model";
    case MismatchDecision::MatchesSavedType:
        return "detected type matches saved type";
    case MismatchDecision::SavedTypeNotSpecific:
        return "saved type is unset or not specific";
    case MismatchDecision::AlreadyDismissed:
        return "already answered for this saved type";
    }
    return "unknown";
}

std::string PrinterDetector::canonical_type_name(const std::string& printer_name) {
    const json* printers = printer_name.empty() ? nullptr : database_printers();
    if (printers == nullptr) {
        // No database is not an error here — without one there is nothing to
        // rename against, and the caller's own name is the best answer.
        return printer_name;
    }

    // Current names win outright. Checked in a separate pass so an entry that
    // still publishes a name another entry lists as a former one cannot be
    // rewritten out from under itself.
    for (const auto& printer : *printers) {
        if (helix::json_util::safe_string(printer, "name") == printer_name) {
            return printer_name;
        }
    }

    for (const auto& printer : *printers) {
        const json* aliases = helix::json_util::find_member(printer, "aliases");
        if (aliases == nullptr || !aliases->is_array()) {
            continue;
        }
        for (const auto& alias : *aliases) {
            if (alias.is_string() && alias.get<std::string>() == printer_name) {
                std::string current = helix::json_util::safe_string(printer, "name");
                if (current.empty()) {
                    continue;
                }
                spdlog::info("[PrinterDetector] Printer type '{}' was renamed to '{}' — "
                             "resolving to the current name",
                             printer_name, current);
                return current;
            }
        }
    }

    return printer_name;
}

PrinterDetector::MismatchDecision
PrinterDetector::classify_type_mismatch(const std::string& saved_type,
                                        const std::string& detected_type, int detected_confidence,
                                        const std::string& flag_value, int detected_margin) {
    // Emptiness is checked before confidence so a no-detection pass is
    // distinguishable in the log from a real candidate that scored short.
    if (detected_type.empty())
        return MismatchDecision::NoDetection;
    if (detected_confidence < kMismatchMinConfidence)
        return MismatchDecision::ConfidenceTooLow;
    // Ordered after the confidence test and before everything else: "scored 92
    // and so did four other models" is a different diagnosis from "scored 68",
    // and a debug bundle has to be able to tell them apart.
    if (detected_margin < kDetectMinMargin)
        return MismatchDecision::Ambiguous;
    if (detected_type == saved_type)
        return MismatchDecision::MatchesSavedType;
    if (saved_type.empty() || saved_type == "Custom/Other" || saved_type == "Unknown")
        return MismatchDecision::SavedTypeNotSpecific;
    if (flag_value == saved_type)
        return MismatchDecision::AlreadyDismissed;
    return MismatchDecision::Warn;
}

PrinterDetector::MismatchDecision
PrinterDetector::classify_type_mismatch(const std::string& saved_type,
                                        const PrinterDetectionResult& detected,
                                        const std::string& flag_value) {
    if (!detected.detected()) {
        return MismatchDecision::NoDetection;
    }
    return classify_type_mismatch(saved_type, detected.type_name, detected.confidence, flag_value,
                                  detected.margin());
}

PrinterDetector::TypeRelationResult
PrinterDetector::relate_types(const std::vector<VariantEntry>& entries,
                              const std::string& saved_type, const std::string& detected_type) {
    auto by_name = [&](const std::string& name) -> const VariantEntry* {
        for (const auto& entry : entries) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    };
    // The base entry's name for a usable variant, "" for a base entry or a
    // variant whose link does not resolve or that names nothing to show.
    auto base_of = [&](const VariantEntry* entry) -> std::string {
        if (entry == nullptr || entry->variant_of.empty() || entry->variant_label.empty()) {
            return {};
        }
        for (const auto& candidate : entries) {
            if (candidate.id == entry->variant_of && candidate.variant_of.empty()) {
                return candidate.name;
            }
        }
        return {};
    };

    TypeRelationResult result;
    const VariantEntry* saved = by_name(saved_type);
    const VariantEntry* detected = by_name(detected_type);
    if (saved == nullptr || detected == nullptr || saved == detected) {
        return result;
    }
    const std::string saved_base = base_of(saved);
    const std::string detected_base = base_of(detected);

    if (saved_base.empty() && detected_base == saved->name) {
        result.relation = TypeRelation::DetectedVariant;
        result.base = saved->name;
    } else if (detected_base.empty() && saved_base == detected->name) {
        result.relation = TypeRelation::SavedVariant;
        result.base = detected->name;
    } else if (!saved_base.empty() && saved_base == detected_base) {
        result.relation = TypeRelation::SiblingVariants;
        result.base = saved_base;
    } else {
        return result;
    }
    if (!saved_base.empty()) {
        result.saved_label = saved->variant_label;
    }
    if (!detected_base.empty()) {
        result.detected_label = detected->variant_label;
    }
    return result;
}

PrinterDetector::TypeRelationResult
PrinterDetector::relate_types(const std::string& saved_type, const std::string& detected_type) {
    std::vector<VariantEntry> entries;
    if (const json* printers = database_printers()) {
        entries.reserve(printers->size());
        for (const auto& printer : *printers) {
            entries.push_back({helix::json_util::safe_string(printer, "id"),
                               helix::json_util::safe_string(printer, "name"),
                               helix::json_util::safe_string(printer, "variant_of"),
                               helix::json_util::safe_string(printer, "variant_label")});
        }
    }
    return relate_types(entries, saved_type, detected_type);
}
