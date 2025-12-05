// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "printer_detector.h"

#include "ui_error_reporting.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include "hv/json.hpp"

using json = nlohmann::json;

// ============================================================================
// JSON Database Loader
// ============================================================================

namespace {
// Lazy-loaded printer database
struct PrinterDatabase {
    json data;
    bool loaded = false;

    bool load() {
        if (loaded)
            return true;

        try {
            std::ifstream file("config/printer_database.json");
            if (!file.is_open()) {
                NOTIFY_ERROR("Could not load printer database");
                LOG_ERROR_INTERNAL("[PrinterDetector] Failed to open config/printer_database.json");
                return false;
            }

            data = json::parse(file);
            loaded = true;
            spdlog::info("[PrinterDetector] Loaded printer database version {}",
                         data.value("version", "unknown"));
            return true;
        } catch (const std::exception& e) {
            NOTIFY_ERROR("Printer database format error");
            LOG_ERROR_INTERNAL("[PrinterDetector] Failed to parse printer database: {}", e.what());
            return false;
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

    // Check X range
    if (heuristic.contains("min_x")) {
        float min_x = heuristic["min_x"].get<float>();
        if (x_size < min_x)
            return false;
    }
    if (heuristic.contains("max_x")) {
        float max_x = heuristic["max_x"].get<float>();
        if (x_size > max_x)
            return false;
    }

    // Check Y range
    if (heuristic.contains("min_y")) {
        float min_y = heuristic["min_y"].get<float>();
        if (y_size < min_y)
            return false;
    }
    if (heuristic.contains("max_y")) {
        float max_y = heuristic["max_y"].get<float>();
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
// Execute a single heuristic and return confidence (0 = no match)
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

// Execute all heuristics for a printer and return best confidence + reason
PrinterDetectionResult execute_printer_heuristics(const json& printer,
                                                  const PrinterHardwareData& hardware) {
    std::string printer_id = printer.value("id", "");
    std::string printer_name = printer.value("name", "");

    if (!printer.contains("heuristics") || !printer["heuristics"].is_array()) {
        return {"", 0, ""};
    }

    PrinterDetectionResult best_result{"", 0, ""};

    // Try all heuristics for this printer
    for (const auto& heuristic : printer["heuristics"]) {
        int confidence = execute_heuristic(heuristic, hardware);
        if (confidence > best_result.confidence) {
            best_result.type_name = printer_name;
            best_result.confidence = confidence;
            best_result.reason = heuristic.value("reason", "");
        }
    }

    return best_result;
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
                spdlog::info("[PrinterDetector] Candidate: '{}' scored {}% via: {}",
                             result.type_name, result.confidence, result.reason);
            }

            if (result.confidence > best_match.confidence) {
                best_match = result;
            }
        }

        if (best_match.confidence > 0) {
            spdlog::info("[PrinterDetector] Detection complete: {} (confidence: {}, reason: {})",
                         best_match.type_name, best_match.confidence, best_match.reason);
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
