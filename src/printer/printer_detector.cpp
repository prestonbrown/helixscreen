// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_detector.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "config.h"
#include "data_root_resolver.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_start_analyzer.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "probe_preparation.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

#ifdef __GLIBC__
#include <malloc.h> // malloc_trim() — see PrinterDatabase::compact()
#endif

// C++17 filesystem - use std::filesystem if available, fall back to experimental
#if __cplusplus >= 201703L && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

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

    bool load() {
        if (loaded)
            return true;

        // Phase 1: Load bundled database
        try {
            const std::string db_path = helix::find_readable("printer_database.json");
            std::ifstream file(db_path);
            if (!file.is_open()) {
                NOTIFY_ERROR(lv_tr("Could not load printer database"));
                LOG_ERROR_INTERNAL("[PrinterDetector] Failed to open {}", db_path);
                return false;
            }

            data = json::parse(file);
            loaded_files.push_back(db_path);
            // safe_string, not .value(): a null "version" would throw
            // type_error.302 from inside this log statement, and the catch below
            // returns false — taking the ENTIRE printer database out for the
            // process lifetime (detection fails, the roller collapses to
            // Custom/Other) over a field used only for a debug line.
            spdlog::debug("[PrinterDetector] Loaded bundled printer database version {}",
                          helix::json_util::safe_string(data, "version", "unknown"));
        } catch (const std::exception& e) {
            NOTIFY_ERROR(lv_tr("Printer database format error"));
            LOG_ERROR_INTERNAL("[PrinterDetector] Failed to parse printer database: {}", e.what());
            return false;
        }

        // Phase 2: Merge user extensions from config/printer_database.d/
        merge_user_extensions();

        loaded = true;
        return true;
    }

    void reload() {
        loaded = false;
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
        if (data.contains("printers") && data["printers"].is_array()) {
            for (auto& printer : data["printers"]) {
                // Extract kinematics before stripping heuristics (needed for filtered lists)
                if (!printer.contains("_kinematics") && printer.contains("heuristics") &&
                    printer["heuristics"].is_array()) {
                    for (const auto& h : printer["heuristics"]) {
                        if (h.value("type", "") == "kinematics_match") {
                            printer["_kinematics"] = h.value("pattern", "");
                            break;
                        }
                    }
                }
                printer.erase("heuristics");
            }
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
        if (!fs::exists(extensions_dir) || !fs::is_directory(extensions_dir)) {
            spdlog::debug("[PrinterDetector] No user extensions directory at {}", extensions_dir);
            return;
        }

        // Build index of bundled printers by ID for fast lookup
        std::map<std::string, size_t> bundled_index;
        if (data.contains("printers") && data["printers"].is_array()) {
            for (size_t i = 0; i < data["printers"].size(); ++i) {
                std::string id = data["printers"][i].value("id", "");
                if (!id.empty()) {
                    bundled_index[id] = i;
                }
            }
        }

        // Scan for JSON files in extensions directory
        std::vector<std::string> extension_files;
        try {
            for (const auto& entry : fs::directory_iterator(extensions_dir)) {
                if (entry.path().extension() == ".json") {
                    extension_files.push_back(entry.path().string());
                }
            }
        } catch (const std::exception& e) {
            load_errors.push_back(fmt::format("Failed to scan {}: {}", extensions_dir, e.what()));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
            return;
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
        try {
            std::ifstream file(file_path);
            if (!file.is_open()) {
                load_errors.push_back(fmt::format("Could not open {}", file_path));
                spdlog::warn("[PrinterDetector] {}", load_errors.back());
                return;
            }

            json extension_data = json::parse(file);
            loaded_files.push_back(file_path);

            // Console filter sets merge into the shared table before the printer
            // entries that reference them, so an extension can define its own set
            // or replace a bundled one by name. Same override rule as printers:
            // last writer wins, and files are processed in sorted order.
            bool merged_sets = false;
            if (extension_data.contains("console_filter_sets") &&
                extension_data["console_filter_sets"].is_object()) {
                if (!data.contains("console_filter_sets") ||
                    !data["console_filter_sets"].is_object()) {
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
                std::string id = printer.value("id", "");
                if (id.empty()) {
                    load_errors.push_back(fmt::format("{}: printer missing 'id' field", file_path));
                    spdlog::warn("[PrinterDetector] {}", load_errors.back());
                    continue;
                }

                // Check if printer is disabled
                bool enabled = printer.value("enabled", true);

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
                        std::string name = printer.value("name", "");
                        if (name.empty()) {
                            load_errors.push_back(fmt::format(
                                "{}: printer '{}' missing 'name' field", file_path, id));
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

        } catch (const json::parse_error& e) {
            load_errors.push_back(fmt::format("{}: JSON parse error: {}", file_path, e.what()));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
        } catch (const std::exception& e) {
            load_errors.push_back(fmt::format("{}: {}", file_path, e.what()));
            spdlog::warn("[PrinterDetector] {}", load_errors.back());
        }
    }
};

PrinterDatabase g_database;
} // namespace

// ============================================================================
// Helper Functions
// ============================================================================

namespace {
// Case-insensitive substring search
bool has_pattern(const std::vector<std::string>& objects, const std::string& pattern) {
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return std::any_of(objects.begin(), objects.end(), [&pattern_lower](const std::string& obj) {
        std::string obj_lower = obj;
        std::transform(obj_lower.begin(), obj_lower.end(), obj_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return obj_lower.find(pattern_lower) != std::string::npos;
    });
}

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
        std::string stepper_lower = stepper;
        std::transform(stepper_lower.begin(), stepper_lower.end(), stepper_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

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
bool is_corroborating_only(const std::string& type) {
    return type == "build_volume_range";
}

// Check if build volume is within specified range
bool check_build_volume_range(const BuildVolume& volume, const json& heuristic) {
    // Get the dimensions we need to check
    float x_size = volume.x_max - volume.x_min;
    float y_size = volume.y_max - volume.y_min;

    // If no volume data, can't match
    if (x_size <= 0 || y_size <= 0) {
        return false;
    }

    // Check X range (value() provides type-safe default if key is wrong type)
    if (heuristic.contains("min_x")) {
        float min_x = heuristic.value("min_x", 0.0f);
        if (x_size < min_x)
            return false;
    }
    if (heuristic.contains("max_x")) {
        float max_x = heuristic.value("max_x", 0.0f);
        if (x_size > max_x)
            return false;
    }

    // Check Y range
    if (heuristic.contains("min_y")) {
        float min_y = heuristic.value("min_y", 0.0f);
        if (y_size < min_y)
            return false;
    }
    if (heuristic.contains("max_y")) {
        float max_y = heuristic.value("max_y", 0.0f);
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
constexpr int HEURISTIC_EXCLUDE = -1;

// Execute a single heuristic and return confidence (0 = no match, -1 = exclude)
int execute_heuristic(const json& heuristic, const PrinterHardwareData& hardware) {
    std::string type = heuristic.value("type", "");
    std::string field = heuristic.value("field", "");
    int confidence = heuristic.value("confidence", 0);

    if (type == "sensor_match" || type == "fan_match" || type == "hostname_match" ||
        type == "led_match") {
        // Simple pattern matching in specified field
        auto field_data = get_field_data(hardware, field);
        std::string pattern = heuristic.value("pattern", "");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Matched {} pattern '{}' (confidence: {})", type,
                          pattern, confidence);
            return confidence;
        }
    } else if (type == "hostname_exclude") {
        // If hostname matches this pattern, exclude this printer entirely
        auto field_data = get_field_data(hardware, field);
        std::string pattern = heuristic.value("pattern", "");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Excluded by {} pattern '{}'", type, pattern);
            return HEURISTIC_EXCLUDE;
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
        std::string pattern = heuristic.value("pattern", "");
        if (!hardware.kinematics.empty()) {
            std::string kinematics_lower = hardware.kinematics;
            std::transform(kinematics_lower.begin(), kinematics_lower.end(),
                           kinematics_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string pattern_lower = pattern;
            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

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
        std::string pattern = heuristic.value("pattern", "");
        if (!hardware.kinematics.empty()) {
            std::string kinematics_lower = hardware.kinematics;
            std::transform(kinematics_lower.begin(), kinematics_lower.end(),
                           kinematics_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string pattern_lower = pattern;
            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (kinematics_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Excluded by kinematics '{}'", pattern);
                return HEURISTIC_EXCLUDE;
            }
        }
    } else if (type == "object_exists") {
        // Check if a Klipper object exists in the printer_objects list
        std::string pattern = heuristic.value("pattern", "");
        if (has_pattern(hardware.printer_objects, pattern)) {
            spdlog::debug("[PrinterDetector] Found object '{}' (confidence: {})", pattern,
                          confidence);
            return confidence;
        }
    } else if (type == "stepper_count") {
        // Count Z steppers and match against pattern (z_count_1, z_count_2, z_count_3, z_count_4)
        std::string pattern = heuristic.value("pattern", "");
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
        std::string pattern = heuristic.value("pattern", "");
        if (!hardware.mcu.empty()) {
            std::string mcu_lower = hardware.mcu;
            std::transform(mcu_lower.begin(), mcu_lower.end(), mcu_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string pattern_lower = pattern;
            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (mcu_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Matched MCU '{}' (confidence: {})", pattern,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "tool_count") {
        // Count extruder objects from heaters (matching "extruder" prefix, excluding
        // "extruder_stepper")
        std::string pattern = heuristic.value("pattern", "");
        int extruder_count = 0;
        for (const auto& heater : hardware.heaters) {
            if (heater.rfind("extruder", 0) == 0 && heater.rfind("extruder_stepper", 0) != 0) {
                extruder_count++;
            }
        }

        // Parse expected count from pattern (tool_count_N)
        if (pattern.rfind("tool_count_", 0) == 0) {
            int expected_count = 0;
            try {
                expected_count = std::stoi(pattern.substr(11));
            } catch (...) {
                spdlog::warn("[PrinterDetector] Invalid tool_count pattern: {}", pattern);
                return 0;
            }
            if (extruder_count == expected_count) {
                spdlog::debug("[PrinterDetector] Matched {} extruders (confidence: {})",
                              extruder_count, confidence);
                return confidence;
            }
        }
    } else if (type == "cpu_arch_match") {
        // Case-insensitive substring match of cpu_arch against pattern
        std::string pattern = heuristic.value("pattern", "");
        if (!hardware.cpu_arch.empty()) {
            std::string cpu_lower = hardware.cpu_arch;
            std::transform(cpu_lower.begin(), cpu_lower.end(), cpu_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string pattern_lower = pattern;
            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (cpu_lower.find(pattern_lower) != std::string::npos) {
                spdlog::debug("[PrinterDetector] Matched CPU arch '{}' (confidence: {})", pattern,
                              confidence);
                return confidence;
            }
        }
    } else if (type == "board_match") {
        // Match against board names found in temperature_sensor objects
        // Board names appear as "temperature_sensor <BOARD_NAME>" in the objects list
        std::string pattern = heuristic.value("pattern", "");
        std::string pattern_lower = pattern;
        std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        for (const auto& obj : hardware.printer_objects) {
            if (obj.rfind("temperature_sensor ", 0) == 0 ||
                obj.rfind("temperature_host ", 0) == 0) {
                std::string sensor_name = obj.substr(obj.find(' ') + 1);
                std::string sensor_lower = sensor_name;
                std::transform(sensor_lower.begin(), sensor_lower.end(), sensor_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

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
        std::string pattern = heuristic.value("pattern", "");
        std::string pattern_lower = pattern;
        std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        for (const auto& obj : hardware.printer_objects) {
            // Check if object is a G-code macro
            if (obj.rfind("gcode_macro ", 0) == 0) {
                // Extract macro name (everything after "gcode_macro ")
                std::string macro_name = obj.substr(12);
                std::string macro_lower = macro_name;
                std::transform(macro_lower.begin(), macro_lower.end(), macro_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (macro_lower.find(pattern_lower) != std::string::npos) {
                    if (type == "macro_exclude") {
                        spdlog::debug("[PrinterDetector] Excluded by macro '{}' ", macro_name);
                        return HEURISTIC_EXCLUDE;
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
    std::string printer_id = printer.value("id", "");
    std::string printer_name = printer.value("name", "");

    if (!printer.contains("heuristics") || !printer["heuristics"].is_array()) {
        return {"", 0, "", 0};
    }

    // Collect ALL matching heuristics
    struct HeuristicMatch {
        int confidence;
        std::string reason;
        bool corroborating; // may support a match, may never establish one
    };
    std::vector<HeuristicMatch> matches;

    for (const auto& heuristic : printer["heuristics"]) {
        int confidence = execute_heuristic(heuristic, hardware);
        if (confidence == HEURISTIC_EXCLUDE) {
            spdlog::debug("[PrinterDetector] {} excluded by heuristic: {}", printer_name,
                          heuristic.value("reason", ""));
            return {"", 0, "", 0};
        }
        if (confidence > 0) {
            matches.push_back({confidence, heuristic.value("reason", ""),
                               is_corroborating_only(heuristic.value("type", ""))});
        }
    }

    if (matches.empty()) {
        return {"", 0, "", 0};
    }

    // Sort by confidence descending to get best match first
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    // A build volume is shared by dozens of printers - at 215-235mm up to 15
    // database windows cover the same point. It can support an identification
    // made on other evidence, but it can never make one on its own, so the base
    // score must come from a heuristic that actually names this printer. With
    // nothing but volume matching, the printer scores nothing at all.
    auto identifying = std::find_if(matches.begin(), matches.end(),
                                    [](const auto& m) { return !m.corroborating; });
    if (identifying == matches.end()) {
        spdlog::debug("[PrinterDetector] {} matched only corroborating evidence "
                      "(build volume) - not identifying, scoring 0",
                      printer_name);
        return {"", 0, "", 0};
    }

    // Combined scoring: base + bonus for additional matches
    // 3 points per extra match, capped at 12 (4 extra matches worth)
    constexpr int BONUS_PER_EXTRA_MATCH = 3;
    constexpr int MAX_BONUS = 12;

    int base_confidence = identifying->confidence;
    int extra_matches = static_cast<int>(matches.size()) - 1;
    int bonus = std::min(extra_matches * BONUS_PER_EXTRA_MATCH, MAX_BONUS);
    int combined = std::min(base_confidence + bonus, 100);

    // Format reason with match count if multiple matches
    std::string reason = identifying->reason;
    if (matches.size() > 1) {
        reason += fmt::format(" (+{} more)", matches.size() - 1);
    }

    spdlog::debug("[PrinterDetector] {} scored {}% (base {} + bonus {} from {} matches)",
                  printer_name, combined, base_confidence, bonus, matches.size());

    return {printer_name, combined, reason, static_cast<int>(matches.size()), base_confidence};
}
} // namespace

// ============================================================================
// Main Detection Entry Point
// ============================================================================

PrinterDetectionResult PrinterDetector::detect(const PrinterHardwareData& hardware) {
    try {
        // Verbose debug output for troubleshooting detection issues
        spdlog::info("[PrinterDetector] Running detection with {} sensors, {} fans, hostname '{}'",
                     hardware.sensors.size(), hardware.fans.size(), hardware.hostname);
        spdlog::info("[PrinterDetector]   printer_objects: {}, steppers: {}, kinematics: '{}'",
                     hardware.printer_objects.size(), hardware.steppers.size(),
                     hardware.kinematics);

        // Load database if not already loaded
        if (!g_database.load()) {
            LOG_ERROR_INTERNAL("[PrinterDetector] Cannot perform detection without database");
            return {"", 0, "Failed to load printer database"};
        }

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

        // Iterate through all printers in database and find best match
        PrinterDetectionResult best_match{"", 0, "No distinctive hardware detected"};
        PrinterDetectionResult runner_up{"", 0, ""};

        if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
            NOTIFY_ERROR(lv_tr("Printer database is corrupt"));
            LOG_ERROR_INTERNAL(
                "[PrinterDetector] Invalid database format: missing 'printers' array");
            return {"", 0, "Invalid printer database format"};
        }

        for (const auto& printer : g_database.data["printers"]) {
            PrinterDetectionResult result = execute_printer_heuristics(printer, hardware);

            if (result.confidence > 0) {
                spdlog::info("[PrinterDetector] Candidate: '{}' scored {}% ({} matches, best={}%) "
                             "via: {}",
                             result.type_name, result.confidence, result.match_count,
                             result.best_single_confidence, result.reason);
            }

            // Non-printer addons (show_in_list: false) can't win detection
            // They're scored and logged for diagnostics, but excluded from the winner
            if (!printer.value("show_in_list", true)) {
                if (result.confidence > 0) {
                    spdlog::info("[PrinterDetector]   [excluded from winner - not a real printer]");
                }
                continue;
            }

            auto beats = [](const PrinterDetectionResult& a, const PrinterDetectionResult& b) {
                return a.confidence > b.confidence ||
                       (a.confidence == b.confidence &&
                        a.best_single_confidence > b.best_single_confidence) ||
                       (a.confidence == b.confidence &&
                        a.best_single_confidence == b.best_single_confidence &&
                        a.match_count > b.match_count);
            };

            if (beats(result, best_match)) {
                runner_up = best_match; // demote previous winner
                best_match = result;
                if (printer.contains("preset") && printer["preset"].is_string()) {
                    best_match.preset = printer["preset"].get<std::string>();
                } else {
                    best_match.preset.clear();
                }
            } else if (result.confidence > 0 && beats(result, runner_up)) {
                runner_up = result;
            }
        }

        best_match.runner_up_type_name = runner_up.type_name;
        best_match.runner_up_confidence = runner_up.confidence;

        if (best_match.confidence > 0) {
            spdlog::info("[PrinterDetector] Detection complete: {} (confidence: {}%, {} matches, "
                         "reason: {})",
                         best_match.type_name, best_match.confidence, best_match.match_count,
                         best_match.reason);
        } else {
            spdlog::debug("[PrinterDetector] No distinctive fingerprints detected");
        }

        return best_match;
    } catch (const std::exception& e) {
        spdlog::error("[PrinterDetector] Exception during detection: {}", e.what());
        return {"", 0, std::string("Detection error: ") + e.what()};
    }
}

// ============================================================================
// Image Lookup Functions
// ============================================================================

std::string PrinterDetector::get_image_for_printer(const std::string& printer_name) {
    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup image without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    // Case-insensitive search by printer name
    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower == name_lower) {
            std::string image = printer.value("image", "");
            if (!image.empty()) {
                spdlog::debug("[PrinterDetector] Found image '{}' for printer '{}'", image,
                              printer_name);
            }
            return image;
        }
    }

    spdlog::debug("[PrinterDetector] No image found for printer '{}'", printer_name);
    return "";
}

std::string PrinterDetector::get_image_for_printer_id(const std::string& printer_id) {
    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup image without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    // Case-insensitive search by printer ID
    std::string id_lower = printer_id;
    std::transform(id_lower.begin(), id_lower.end(), id_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_id = printer.value("id", "");
        std::string db_id_lower = db_id;
        std::transform(db_id_lower.begin(), db_id_lower.end(), db_id_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_id_lower == id_lower) {
            std::string image = printer.value("image", "");
            if (!image.empty()) {
                spdlog::debug("[PrinterDetector] Found image '{}' for printer ID '{}'", image,
                              printer_id);
            }
            return image;
        }
    }

    spdlog::debug("[PrinterDetector] No image found for printer ID '{}'", printer_id);
    return "";
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
    if (printer_name.empty()) {
        return patterns;
    }
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup filter patterns without database");
        return patterns;
    }
    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return patterns;
    }

    const json* sets = nullptr;
    if (g_database.data.contains("console_filter_sets") &&
        g_database.data["console_filter_sets"].is_object()) {
        sets = &g_database.data["console_filter_sets"];
    }

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const std::string needle = lower(printer_name);

    for (const auto& printer : g_database.data["printers"]) {
        const std::string db_name = lower(printer.value("name", ""));
        const std::string db_id = lower(printer.value("id", ""));
        if (db_name != needle && db_id != needle) {
            continue;
        }

        // Named sets first — these are the shared vocabulary, so a fix to one
        // shape reaches every printer that references it. `console_filter_patterns`
        // then adds anything specific enough to this model that a set would be
        // overkill, and is what pre-set databases still carry.
        if (printer.contains("console_filters") && printer["console_filters"].is_array()) {
            for (const auto& name : printer["console_filters"]) {
                if (name.is_string()) {
                    collect_filter_set(sets, name.get<std::string>(), patterns);
                } else {
                    spdlog::warn("[PrinterDetector] Non-string console_filters entry for '{}', "
                                 "skipping",
                                 printer.value("id", "?"));
                }
            }
        }

        if (printer.contains("console_filter_patterns") &&
            printer["console_filter_patterns"].is_array()) {
            for (const auto& spec : printer["console_filter_patterns"]) {
                if (spec.is_string()) {
                    append_unique_spec(patterns, spec.get<std::string>());
                } else {
                    spdlog::warn("[PrinterDetector] Non-string console_filter_patterns entry "
                                 "for '{}', skipping",
                                 printer.value("id", "?"));
                }
            }
        }
        return patterns;
    }
    return patterns;
}

std::string PrinterDetector::get_name_for_preset(const std::string& preset_name) {
    if (preset_name.empty()) {
        return "";
    }

    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup preset without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    std::string preset_lower = preset_name;
    std::transform(preset_lower.begin(), preset_lower.end(), preset_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_preset = printer.value("preset", "");
        std::string db_preset_lower = db_preset;
        std::transform(db_preset_lower.begin(), db_preset_lower.end(), db_preset_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_preset_lower == preset_lower) {
            return printer.value("name", "");
        }
    }

    return "";
}

std::string PrinterDetector::get_preset_for_name(const std::string& printer_name) {
    if (printer_name.empty()) {
        return "";
    }

    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup name without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    for (const auto& printer : g_database.data["printers"]) {
        if (printer.value("name", "") == printer_name) {
            return printer.value("preset", "");
        }
    }

    return "";
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
    static constexpr const char* FORGE_X_SUFFIX = "_forgex";
    constexpr size_t FORGE_X_LEN = 7; // strlen("_forgex")
    bool preset_is_forgex =
        preset.size() >= FORGE_X_LEN &&
        preset.compare(preset.size() - FORGE_X_LEN, FORGE_X_LEN, FORGE_X_SUFFIX) == 0;
    bool is_zmod = !preset_is_forgex && std::find(objects.begin(), objects.end(),
                                                  "fan_generic fanM106") != objects.end();

    std::string applied = preset;
    if (is_zmod) {
        std::string zmod_preset = preset + "_zmod";
        spdlog::info("[PrinterDetector] ZMOD firmware detected (fan_generic fanM106), "
                     "trying preset '{}'",
                     zmod_preset);
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
    if (printer.contains("_kinematics") && printer["_kinematics"].is_string()) {
        std::string pattern = printer["_kinematics"].get<std::string>();
        std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return pattern;
    }

    if (!printer.contains("heuristics") || !printer["heuristics"].is_array()) {
        return "";
    }
    for (const auto& h : printer["heuristics"]) {
        if (h.value("type", "") == "kinematics_match") {
            std::string pattern = h.value("pattern", "");
            std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return pattern;
        }
    }
    return "";
}

// Cached list data - built once and reused
struct ListCache {
    std::string options;            // Newline-separated string for lv_roller_set_options()
    std::vector<std::string> names; // Vector of names for index lookups
    bool built = false;

    void reset() {
        options.clear();
        names.clear();
        built = false;
    }

    void build() {
        if (built)
            return;

        // Load database if not already loaded
        if (!g_database.load()) {
            spdlog::warn("[PrinterDetector] Cannot build list without database");
            // Fallback to just Custom/Other and Unknown
            names = {"Custom/Other", "Unknown"};
            options = "Custom/Other\nUnknown";
            built = true;
            return;
        }

        if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
            names = {"Custom/Other", "Unknown"};
            options = "Custom/Other\nUnknown";
            built = true;
            return;
        }

        // Collect all printer names that should appear in list
        for (const auto& printer : g_database.data["printers"]) {
            // Check enabled flag (defaults to true if missing) - allows user to hide bundled
            bool enabled = printer.value("enabled", true);
            if (!enabled) {
                continue;
            }

            // Check show_in_list flag (defaults to true if missing)
            bool show = printer.value("show_in_list", true);
            if (!show) {
                continue;
            }

            std::string name = printer.value("name", "");
            if (!name.empty()) {
                names.push_back(name);
            }
        }

        // Sort alphabetically for consistent ordering
        std::sort(names.begin(), names.end());

        // Always append Custom/Other and Unknown at the end
        names.push_back("Custom/Other");
        names.push_back("Unknown");

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

    if (!g_database.load()) {
        g_filtered_list_cache.names = {"Custom/Other", "Unknown"};
        g_filtered_list_cache.options = "Custom/Other\nUnknown";
        g_filtered_list_cache.built = true;
        return;
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        g_filtered_list_cache.names = {"Custom/Other", "Unknown"};
        g_filtered_list_cache.options = "Custom/Other\nUnknown";
        g_filtered_list_cache.built = true;
        return;
    }

    std::string filter_lower = kinematics_filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        if (!printer.value("enabled", true))
            continue;
        if (!printer.value("show_in_list", true))
            continue;

        std::string name = printer.value("name", "");
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

        g_filtered_list_cache.names.push_back(name);
    }

    std::sort(g_filtered_list_cache.names.begin(), g_filtered_list_cache.names.end());
    g_filtered_list_cache.names.push_back("Custom/Other");
    g_filtered_list_cache.names.push_back("Unknown");

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
    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (size_t i = 0; i < g_list_cache.names.size(); ++i) {
        std::string cached_lower = g_list_cache.names[i];
        std::transform(cached_lower.begin(), cached_lower.end(), cached_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

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

int PrinterDetector::find_list_index(const std::string& printer_name,
                                     const std::string& kinematics) {
    if (kinematics.empty())
        return find_list_index(printer_name);
    build_filtered_list(kinematics);

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (size_t i = 0; i < g_filtered_list_cache.names.size(); ++i) {
        std::string cached_lower = g_filtered_list_cache.names[i];
        std::transform(cached_lower.begin(), cached_lower.end(), cached_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
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

    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup pre_print_options without database");
        return result;
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return result;
    }

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower != name_lower) {
            continue;
        }

        if (!printer.contains("pre_print_options")) {
            spdlog::debug("[PrinterDetector] Printer '{}' has no pre_print_options", printer_name);
            return result;
        }

        result = parse_pre_print_option_set(printer["pre_print_options"]);
        spdlog::info("[PrinterDetector] Found {} pre-print options for '{}' (macro: {})",
                     result.options.size(), printer_name, result.macro_name);
        return result;
    }

    spdlog::debug("[PrinterDetector] No pre_print_options found for printer '{}'", printer_name);
    return result;
}

// ============================================================================
// Z-Offset Calibration Strategy Lookup
// ============================================================================

std::string PrinterDetector::get_z_offset_calibration_strategy(const std::string& printer_name) {
    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::warn(
            "[PrinterDetector] Cannot lookup z_offset_calibration_strategy without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    // Case-insensitive search by printer name
    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower == name_lower) {
            std::string strategy = printer.value("z_offset_calibration_strategy", "");
            if (!strategy.empty()) {
                spdlog::debug(
                    "[PrinterDetector] Found z_offset_calibration_strategy '{}' for printer '{}'",
                    strategy, printer_name);
            }
            return strategy;
        }
    }

    spdlog::debug("[PrinterDetector] No z_offset_calibration_strategy found for printer '{}'",
                  printer_name);
    return "";
}

// ============================================================================
// Probe Type Lookup
// ============================================================================

std::string PrinterDetector::get_probe_type(const std::string& printer_name) {
    if (!g_database.load()) {
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower == name_lower) {
            std::string probe_type = printer.value("probe_type", "");
            if (!probe_type.empty()) {
                spdlog::debug("[PrinterDetector] Found probe_type '{}' for printer '{}'",
                              probe_type, printer_name);
            }
            return probe_type;
        }
    }

    return "";
}

std::string PrinterDetector::get_bed_mesh_calibrate_gcode(const std::string& printer_name) {
    if (!g_database.load()) {
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower != name_lower) {
            continue;
        }

        if (!printer.contains("calibration") || !printer["calibration"].is_object()) {
            return "";
        }
        const auto& cal = printer["calibration"];
        std::string gcode = cal.value("bed_mesh_gcode", "");
        if (!gcode.empty()) {
            spdlog::debug("[PrinterDetector] Found bed_mesh_gcode override for printer '{}'",
                          printer_name);
        }
        return gcode;
    }

    return "";
}

// ============================================================================
// Print Start Profile Lookup
// ============================================================================

std::string PrinterDetector::get_print_start_profile(const std::string& printer_name) {
    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup print_start_profile without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    // Case-insensitive search by printer name
    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower == name_lower) {
            std::string profile = printer.value("print_start_profile", "");
            if (!profile.empty()) {
                spdlog::debug("[PrinterDetector] Found print_start_profile '{}' for printer '{}'",
                              profile, printer_name);
            }
            return profile;
        }
    }

    spdlog::debug("[PrinterDetector] No print_start_profile found for printer '{}'", printer_name);
    return "";
}

// ============================================================================
// Pre-print Phase Defaults
// ============================================================================

std::map<int, int>
PrinterDetector::get_print_start_default_phases(const std::string& printer_name) {
    std::map<int, int> result;
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup print_start_default_phases without database");
        return result;
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return result;
    }

    // Phase name → enum int. Keep in sync with PrintStartPhase in printer_state.h.
    // HEATING_* are excluded intentionally — ThermalRateModel handles heating
    // time separately, and predictor entries drop those phases on save.
    static const std::map<std::string, int> PHASE_NAMES = {
        {"HOMING", static_cast<int>(helix::PrintStartPhase::HOMING)},
        {"QGL", static_cast<int>(helix::PrintStartPhase::QGL)},
        {"Z_TILT", static_cast<int>(helix::PrintStartPhase::Z_TILT)},
        {"BED_MESH", static_cast<int>(helix::PrintStartPhase::BED_MESH)},
        {"CLEANING", static_cast<int>(helix::PrintStartPhase::CLEANING)},
        {"PURGING", static_cast<int>(helix::PrintStartPhase::PURGING)},
    };

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (db_name_lower != name_lower) {
            continue;
        }
        if (!printer.contains("print_start_default_phases") ||
            !printer["print_start_default_phases"].is_object()) {
            return result;
        }
        const auto& phases = printer["print_start_default_phases"];
        for (auto it = phases.begin(); it != phases.end(); ++it) {
            auto found = PHASE_NAMES.find(it.key());
            if (found == PHASE_NAMES.end()) {
                spdlog::warn(
                    "[PrinterDetector] Unknown print_start_default_phase '{}' for printer '{}'",
                    it.key(), printer_name);
                continue;
            }
            if (!it.value().is_number_integer()) {
                spdlog::warn("[PrinterDetector] Non-integer duration for phase '{}' on '{}'",
                             it.key(), printer_name);
                continue;
            }
            result[found->second] = it.value().get<int>();
        }
        spdlog::debug("[PrinterDetector] print_start_default_phases for '{}': {} entries",
                      printer_name, result.size());
        return result;
    }

    return result;
}

// ============================================================================
// Toolhead Style Lookup
// ============================================================================

std::string PrinterDetector::get_toolhead_style(const std::string& printer_name) {
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup toolhead_style without database");
        return "";
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (db_name_lower == name_lower) {
            std::string style = printer.value("toolhead_style", "");
            if (!style.empty()) {
                spdlog::debug("[PrinterDetector] Found toolhead_style '{}' for printer '{}'", style,
                              printer_name);
            }
            return style;
        }
    }

    spdlog::debug("[PrinterDetector] No toolhead_style found for printer '{}'", printer_name);
    return "";
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
    // Ensure database is loaded
    g_database.load();

    LoadStatus status;
    status.loaded = g_database.loaded;
    status.total_printers = 0;
    status.user_overrides = g_database.user_overrides;
    status.user_additions = g_database.user_additions;
    status.loaded_files = g_database.loaded_files;
    status.load_errors = g_database.load_errors;

    // Count enabled printers
    if (g_database.data.contains("printers") && g_database.data["printers"].is_array()) {
        for (const auto& printer : g_database.data["printers"]) {
            if (printer.value("enabled", true)) {
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
            config->apply_preset_file(saved_preset);
        }
        // Still compact if database was loaded (e.g., by list building)
        compact_database();
        return false;
    }

    // Run detection
    PrinterDetectionResult result = auto_detect(discovery);

    if (!meets_autosave_threshold(result)) {
        // Ambiguous. Persist nothing: PRINTER_TYPE stays empty so the wizard's
        // identify step offers its Custom/Other default for the user to correct,
        // and a later reconnect with a fuller discovery snapshot gets another
        // chance instead of being locked out by a non-empty saved type.
        spdlog::info("[PrinterDetector] Detection below auto-save bar (best '{}' at {}%, "
                     "runner-up '{}' at {}%, need >={}%) - leaving printer type unset "
                     "for the user to choose",
                     result.type_name, result.confidence, result.runner_up_type_name,
                     result.runner_up_confidence, AUTOSAVE_MIN_CONFIDENCE);
        // Deliberately NOT compacting: compact_database() strips the heuristics,
        // and without them a later attempt could never match anything. Holding
        // them is the cost of staying open to a better answer.
        return false;
    }

    spdlog::info("[PrinterDetector] Auto-detected printer: '{}' ({}% confidence, reason: {})",
                 result.type_name, result.confidence, result.reason);

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
    if (!config) {
        return false;
    }

    std::string printer_type =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (printer_type.empty()) {
        return false;
    }

    std::string lower_type = printer_type;
    for (auto& c : lower_type) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::string lower_needle = needle;
    for (auto& c : lower_needle) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return lower_type.find(lower_needle) != std::string::npos;
}

bool PrinterDetector::is_voron_printer() {
    return printer_type_contains("voron");
}

bool PrinterDetector::is_pfa_printer() {
    return printer_type_contains("pfa");
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

std::string PrinterDetector::screws_tilt_direction_override() {
    Config* config = Config::get_instance();
    if (!config) {
        return "";
    }
    std::string printer_name =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    if (printer_name.empty()) {
        return "";
    }

    if (!g_database.load()) {
        return "";
    }
    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return "";
    }

    std::string name_lower = printer_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& printer : g_database.data["printers"]) {
        std::string db_name = printer.value("name", "");
        std::string db_name_lower = db_name;
        std::transform(db_name_lower.begin(), db_name_lower.end(), db_name_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (db_name_lower == name_lower) {
            std::string value = printer.value("screws_tilt_direction", "");
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (value == "cw" || value == "ccw") {
                return value;
            }
            return "";
        }
    }
    return "";
}

bool PrinterDetector::meets_autosave_threshold(const PrinterDetectionResult& result) {
    if (result.type_name.empty() || !result.detected())
        return false;
    return result.confidence >= AUTOSAVE_MIN_CONFIDENCE;
}

const char* PrinterDetector::mismatch_decision_name(MismatchDecision decision) {
    switch (decision) {
    case MismatchDecision::Warn:
        return "warn";
    case MismatchDecision::NoDetection:
        return "no detection";
    case MismatchDecision::ConfidenceTooLow:
        return "confidence below threshold";
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
    if (printer_name.empty()) {
        return printer_name;
    }

    if (!g_database.load() || !g_database.data.contains("printers") ||
        !g_database.data["printers"].is_array()) {
        // No database is not an error here — without one there is nothing to
        // rename against, and the caller's own name is the best answer.
        return printer_name;
    }

    const auto& printers = g_database.data["printers"];

    // Current names win outright. Checked in a separate pass so an entry that
    // still publishes a name another entry lists as a former one cannot be
    // rewritten out from under itself.
    for (const auto& printer : printers) {
        if (printer.value("name", "") == printer_name) {
            return printer_name;
        }
    }

    for (const auto& printer : printers) {
        auto aliases = printer.find("aliases");
        if (aliases == printer.end() || !aliases->is_array()) {
            continue;
        }
        for (const auto& alias : *aliases) {
            if (alias.is_string() && alias.get<std::string>() == printer_name) {
                std::string current = printer.value("name", "");
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
                                        const std::string& flag_value) {
    // Emptiness is checked before confidence so a no-detection pass is
    // distinguishable in the log from a real candidate that scored short.
    if (detected_type.empty())
        return MismatchDecision::NoDetection;
    if (detected_confidence < MISMATCH_MIN_CONFIDENCE)
        return MismatchDecision::ConfidenceTooLow;
    if (detected_type == saved_type)
        return MismatchDecision::MatchesSavedType;
    if (saved_type.empty() || saved_type == "Custom/Other" || saved_type == "Unknown")
        return MismatchDecision::SavedTypeNotSpecific;
    if (flag_value == saved_type)
        return MismatchDecision::AlreadyDismissed;
    return MismatchDecision::Warn;
}

bool PrinterDetector::should_warn_type_mismatch(const std::string& saved_type,
                                                const std::string& detected_type,
                                                int detected_confidence,
                                                const std::string& flag_value) {
    return classify_type_mismatch(saved_type, detected_type, detected_confidence, flag_value) ==
           MismatchDecision::Warn;
}
