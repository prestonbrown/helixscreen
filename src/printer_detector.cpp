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
                spdlog::error("[PrinterDetector] Failed to open config/printer_database.json");
                return false;
            }

            data = json::parse(file);
            loaded = true;
            spdlog::info("[PrinterDetector] Loaded printer database version {}",
                         data.value("version", "unknown"));
            return true;
        } catch (const std::exception& e) {
            spdlog::error("[PrinterDetector] Failed to parse printer database: {}", e.what());
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
const std::vector<std::string>& get_field_data(const PrinterHardwareData& hardware,
                                               const std::string& field) {
    if (field == "sensors")
        return hardware.sensors;
    if (field == "fans")
        return hardware.fans;
    if (field == "heaters")
        return hardware.heaters;
    if (field == "leds")
        return hardware.leds;

    // For hostname, create temporary vector
    static std::vector<std::string> hostname_vec;
    hostname_vec = {hardware.hostname};
    return hostname_vec;
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

    const auto& field_data = get_field_data(hardware, field);

    if (type == "sensor_match" || type == "fan_match" || type == "hostname_match") {
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
    spdlog::debug("[PrinterDetector] Running detection with {} sensors, {} fans, hostname '{}'",
                  hardware.sensors.size(), hardware.fans.size(), hardware.hostname);

    // Load database if not already loaded
    if (!g_database.load()) {
        spdlog::error("[PrinterDetector] Cannot perform detection without database");
        return {"", 0, "Failed to load printer database"};
    }

    // Iterate through all printers in database and find best match
    PrinterDetectionResult best_match{"", 0, "No distinctive hardware detected"};

    if (!g_database.data.contains("printers") || !g_database.data["printers"].is_array()) {
        spdlog::error("[PrinterDetector] Invalid database format: missing 'printers' array");
        return {"", 0, "Invalid printer database format"};
    }

    for (const auto& printer : g_database.data["printers"]) {
        PrinterDetectionResult result = execute_printer_heuristics(printer, hardware);

        if (result.confidence > best_match.confidence) {
            best_match = result;
            spdlog::info("[PrinterDetector] New best match: {} (confidence: {})", result.type_name,
                         result.confidence);
        }
    }

    if (best_match.confidence > 0) {
        spdlog::info("[PrinterDetector] Detection complete: {} (confidence: {}, reason: {})",
                     best_match.type_name, best_match.confidence, best_match.reason);
    } else {
        spdlog::debug("[PrinterDetector] No distinctive fingerprints detected");
    }

    return best_match;
}
