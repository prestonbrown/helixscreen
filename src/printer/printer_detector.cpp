// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_detector.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "config.h"
#include "print_start_analyzer.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

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
            std::ifstream file("config/printer_database.json");
            if (!file.is_open()) {
                NOTIFY_ERROR("Could not load printer database");
                LOG_ERROR_INTERNAL("[PrinterDetector] Failed to open config/printer_database.json");
                return false;
            }

            data = json::parse(file);
            loaded_files.push_back("config/printer_database.json");
            spdlog::debug("[PrinterDetector] Loaded bundled printer database version {}",
                          data.value("version", "unknown"));
        } catch (const std::exception& e) {
            NOTIFY_ERROR("Printer database format error");
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
        loaded_files.clear();
        load_errors.clear();
        user_overrides = 0;
        user_additions = 0;
        data = json();
        load();
    }

  private:
    void merge_user_extensions() {
        const std::string extensions_dir = "config/printer_database.d";

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

            // Validate structure
            if (!extension_data.contains("printers") || !extension_data["printers"].is_array()) {
                load_errors.push_back(fmt::format("{}: missing 'printers' array", file_path));
                spdlog::warn("[PrinterDetector] {}", load_errors.back());
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

    auto field_data = get_field_data(hardware, field);

    if (type == "sensor_match" || type == "fan_match" || type == "hostname_match" ||
        type == "led_match") {
        // Simple pattern matching in specified field
        std::string pattern = heuristic.value("pattern", "");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Matched {} pattern '{}' (confidence: {})", type,
                          pattern, confidence);
            return confidence;
        }
    } else if (type == "hostname_exclude") {
        // If hostname matches this pattern, exclude this printer entirely
        std::string pattern = heuristic.value("pattern", "");
        if (has_pattern(field_data, pattern)) {
            spdlog::debug("[PrinterDetector] Excluded by {} pattern '{}'", type, pattern);
            return HEURISTIC_EXCLUDE;
        }
    } else if (type == "fan_combo") {
        // Multiple patterns must all be present
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
    } else if (type == "macro_match") {
        // Match against G-code macro names in printer_objects
        // G-code macros appear as "gcode_macro <NAME>" in the objects list
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
            matches.push_back({confidence, heuristic.value("reason", "")});
        }
    }

    if (matches.empty()) {
        return {"", 0, "", 0};
    }

    // Sort by confidence descending to get best match first
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    // Combined scoring: base + bonus for additional matches
    // 3 points per extra match, capped at 12 (4 extra matches worth)
    constexpr int BONUS_PER_EXTRA_MATCH = 3;
    constexpr int MAX_BONUS = 12;

    int base_confidence = matches[0].confidence;
    int extra_matches = static_cast<int>(matches.size()) - 1;
    int bonus = std::min(extra_matches * BONUS_PER_EXTRA_MATCH, MAX_BONUS);
    int combined = std::min(base_confidence + bonus, 100);

    // Format reason with match count if multiple matches
    std::string reason = matches[0].reason;
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

        // Iterate through all printers in database and find best match
        PrinterDetectionResult best_match{"", 0, "No distinctive hardware detected"};

        if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
            NOTIFY_ERROR("Printer database is corrupt");
            LOG_ERROR_INTERNAL(
                "[PrinterDetector] Invalid database format: missing 'printers' array");
            return {"", 0, "Invalid printer database format"};
        }

        for (const auto& printer : g_database.data["printers"]) {
            PrinterDetectionResult result = execute_printer_heuristics(printer, hardware);

            // Log all matches for debugging (not just best)
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

            // Tiebreakers: best_single_confidence first (more specific match wins),
            // then match_count (more supporting evidence)
            if (result.confidence > best_match.confidence ||
                (result.confidence == best_match.confidence &&
                 result.best_single_confidence > best_match.best_single_confidence) ||
                (result.confidence == best_match.confidence &&
                 result.best_single_confidence == best_match.best_single_confidence &&
                 result.match_count > best_match.match_count)) {
                best_match = result;
            }
        }

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

// ============================================================================
// Dynamic List Builder
// ============================================================================

namespace {

// Extract kinematics type from a printer's heuristics array
// Returns the pattern value from the first kinematics_match heuristic, or ""
std::string extract_kinematics(const json& printer) {
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

        // Apply kinematics filter
        std::string printer_kin = extract_kinematics(printer);
        if (!filter_lower.empty() && !printer_kin.empty() && printer_kin != filter_lower) {
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
// Print Start Capabilities Lookup
// ============================================================================

namespace {

/**
 * @brief Get set of valid capability keys from PrintStartOpCategory enum
 *
 * These keys must match what category_to_string() returns.
 */
const std::unordered_set<std::string>& get_valid_capability_keys() {
    static const std::unordered_set<std::string> keys = {
        helix::category_to_string(helix::PrintStartOpCategory::BED_MESH),
        helix::category_to_string(helix::PrintStartOpCategory::QGL),
        helix::category_to_string(helix::PrintStartOpCategory::Z_TILT),
        helix::category_to_string(helix::PrintStartOpCategory::NOZZLE_CLEAN),
        helix::category_to_string(helix::PrintStartOpCategory::PURGE_LINE),
        helix::category_to_string(helix::PrintStartOpCategory::SKEW_CORRECT),
        helix::category_to_string(helix::PrintStartOpCategory::CHAMBER_SOAK),
        // HOMING and UNKNOWN intentionally excluded - they shouldn't have capabilities
    };
    return keys;
}

/**
 * @brief Check if a capability key is recognized
 */
bool is_valid_capability_key(const std::string& key) {
    return get_valid_capability_keys().count(key) > 0;
}

} // namespace

PrintStartCapabilities
PrinterDetector::get_print_start_capabilities(const std::string& printer_name) {
    PrintStartCapabilities result;

    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::warn("[PrinterDetector] Cannot lookup capabilities without database");
        return result;
    }

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        return result;
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
            // Found matching printer - check for capabilities
            if (!printer.contains("print_start_capabilities")) {
                spdlog::debug("[PrinterDetector] Printer '{}' has no print_start_capabilities",
                              printer_name);
                return result;
            }

            const auto& caps = printer["print_start_capabilities"];
            result.macro_name = caps.value("macro_name", "");

            if (caps.contains("params") && caps["params"].is_object()) {
                for (const auto& [key, value] : caps["params"].items()) {
                    // Validate capability key
                    if (!is_valid_capability_key(key)) {
                        spdlog::warn("[PrinterDetector] Unknown capability key '{}' for printer "
                                     "'{}' - will be ignored during matching",
                                     key, printer_name);
                    }

                    PrintStartParamCapability param;
                    param.param = value.value("param", "");
                    param.skip_value = value.value("skip_value", "");
                    param.enable_value = value.value("enable_value", "");
                    param.default_value = value.value("default_value", "");
                    param.description = value.value("description", "");

                    // Validate required fields
                    if (param.param.empty()) {
                        spdlog::warn("[PrinterDetector] Capability '{}' for printer '{}' has empty "
                                     "'param' field - entry will be skipped",
                                     key, printer_name);
                        continue;
                    }

                    result.params[key] = param;
                }
            }

            spdlog::info("[PrinterDetector] Found {} capabilities for '{}' (macro: {})",
                         result.params.size(), printer_name, result.macro_name);
            return result;
        }
    }

    spdlog::debug("[PrinterDetector] No capabilities found for printer '{}'", printer_name);
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

    return detect(hw_data);
}

bool PrinterDetector::auto_detect_and_save(const helix::PrinterDiscovery& discovery,
                                           Config* config) {
    if (!config) {
        spdlog::warn("[PrinterDetector] auto_detect_and_save called with null config");
        return false;
    }

    // Check if printer type is already set
    std::string saved_type = config->get<std::string>(helix::wizard::PRINTER_TYPE, "");
    if (!saved_type.empty()) {
        spdlog::debug("[PrinterDetector] Printer type already set: '{}', skipping auto-detection",
                      saved_type);
        return false;
    }

    // Run detection
    PrinterDetectionResult result = auto_detect(discovery);

    if (result.confidence > 0) {
        spdlog::info("[PrinterDetector] Auto-detected printer: '{}' ({}% confidence, reason: {})",
                     result.type_name, result.confidence, result.reason);

        // Save to config
        config->set<std::string>(helix::wizard::PRINTER_TYPE, result.type_name);
        config->save();

        // Update PrinterState so home panel gets correct image and capabilities
        get_printer_state().set_printer_type_sync(result.type_name);

        return true;
    }

    spdlog::info("[PrinterDetector] No printer type detected from hardware fingerprints");
    return false;
}

bool PrinterDetector::is_voron_printer() {
    Config* config = Config::get_instance();
    if (!config) {
        return false;
    }

    std::string printer_type = config->get<std::string>(helix::wizard::PRINTER_TYPE, "");
    if (printer_type.empty()) {
        return false;
    }

    // Case-insensitive search for "voron"
    std::string lower_type = printer_type;
    for (auto& c : lower_type) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return lower_type.find("voron") != std::string::npos;
}
