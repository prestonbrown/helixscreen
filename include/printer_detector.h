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

#pragma once

#include <string>
#include <vector>

/**
 * @brief Printer auto-detection result with confidence and reasoning
 */
struct PrinterDetectionResult {
    std::string type_name; ///< Printer type name (e.g., "FlashForge AD5M Pro", "Voron 2.4")
    int confidence;        ///< Confidence score 0-100 (≥70 = high confidence, <70 = low confidence)
    std::string reason;    ///< Human-readable detection reasoning

    /**
     * @brief Check if detection succeeded
     * @return true if confidence > 0, false otherwise
     */
    bool detected() const {
        return confidence > 0;
    }
};

/**
 * @brief Build volume dimensions from bed_mesh configuration
 */
struct BuildVolume {
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    float z_max = 0.0f; ///< Maximum Z height (if available)
};

/**
 * @brief Printer hardware discovery data
 *
 * Aggregates hardware information from Moonraker for detection analysis.
 */
struct PrinterHardwareData {
    std::vector<std::string> heaters{};         ///< Controllable heaters (extruders, bed, etc.)
    std::vector<std::string> sensors{};         ///< Read-only temperature sensors
    std::vector<std::string> fans{};            ///< All fan types
    std::vector<std::string> leds{};            ///< LED outputs
    std::string hostname{};                     ///< Printer hostname from printer.info
    std::vector<std::string> printer_objects{}; ///< Full list of Klipper objects from objects/list
    std::vector<std::string> steppers{}; ///< Stepper motor names (stepper_x, stepper_z, etc.)
    std::string kinematics{};            ///< Kinematics type (corexy, cartesian, delta, etc.)
    std::string mcu{};                   ///< Primary MCU chip type (e.g., "stm32h723xx", "rp2040")
    std::vector<std::string> mcu_list{}; ///< All MCU chips (primary + secondary, CAN toolheads)
    BuildVolume build_volume{};          ///< Build volume dimensions from bed_mesh
};

/**
 * @brief Printer auto-detection using hardware fingerprints
 *
 * Data-driven printer detection system that loads heuristics from JSON database.
 * Analyzes hardware discovery data to identify printer models based on
 * distinctive patterns found in real printers (FlashForge AD5M Pro, Voron V2, etc.).
 *
 * This class is completely independent of UI code and printer type lists.
 * It returns printer type names as strings, which the caller can map to their
 * own data structures (e.g., UI dropdowns, config values).
 *
 * Detection heuristics are defined in config/printer_database.json, allowing
 * new printer types to be added without recompilation.
 *
 * **Contract**: Returned type_name strings should match printer names in
 * PrinterTypes::PRINTER_TYPES_ROLLER for UI integration, but the detector
 * doesn't depend on that list and can be tested independently.
 */
class PrinterDetector {
  public:
    /**
     * @brief Detect printer type from hardware data
     *
     * Loads heuristics from config/printer_database.json and executes pattern matching
     * rules to identify printer model. Supports multiple heuristic types:
     * - sensor_match: Pattern matching on sensors array
     * - fan_match: Pattern matching on fans array
     * - hostname_match: Pattern matching on printer hostname
     * - fan_combo: Multiple fan patterns must all be present
     *
     * Returns the printer with highest confidence match, or empty result if
     * no distinctive fingerprints detected.
     *
     * @param hardware Hardware discovery data from Moonraker
     * @return Detection result with type name, confidence, and reasoning
     */
    static PrinterDetectionResult detect(const PrinterHardwareData& hardware);

    /**
     * @brief Get image filename for a printer type
     *
     * Looks up the image field from the printer database JSON.
     * Returns just the filename (e.g., "voron-24r2.png"), not the full path.
     *
     * @param printer_name Printer name (e.g., "Voron 2.4", "FlashForge Adventurer 5M")
     * @return Image filename if found, empty string if not found
     */
    static std::string get_image_for_printer(const std::string& printer_name);

    /**
     * @brief Get image filename for a printer by ID
     *
     * Looks up the image field from the printer database JSON using the printer ID.
     * Returns just the filename (e.g., "voron-24r2.png"), not the full path.
     *
     * @param printer_id Printer ID (e.g., "voron_2_4", "flashforge_adventurer_5m")
     * @return Image filename if found, empty string if not found
     */
    static std::string get_image_for_printer_id(const std::string& printer_id);
};
