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

#include "ui_wizard_printer_identify.h"
#include "ui_wizard.h"
#include "ui_keyboard.h"
#include "app_globals.h"
#include "config.h"
#include "printer_types.h"
#include "printer_detector.h"
#include "moonraker_client.h"
#include "lvgl/lvgl.h"
#include <spdlog/spdlog.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <sstream>

// ============================================================================
// Static Data & Subjects
// ============================================================================

// Extern declaration for global connection_test_passed subject (defined in ui_wizard.cpp)
extern lv_subject_t connection_test_passed;

// Subject declarations (module scope)
static lv_subject_t printer_name;
static lv_subject_t printer_type_selected;
static lv_subject_t printer_detection_status;

// String buffers (must be persistent)
static char printer_name_buffer[128];
static char printer_detection_status_buffer[256];

// Screen instance
static lv_obj_t* printer_identify_screen_root = nullptr;

// Validation state
static bool printer_identify_validated = false;

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_printer_name_changed(lv_event_t* e);
static void on_printer_type_changed(lv_event_t* e);

// ============================================================================
// Auto-Detection Infrastructure (Placeholder for Phase 3)
// ============================================================================

/**
 * @brief Printer auto-detection hint (confidence + reasoning)
 *
 * Future integration point for printer auto-detection heuristics.
 * Phase 3 will query MoonrakerClient for discovered hardware and
 * use pattern matching to suggest printer type.
 */
struct PrinterDetectionHint {
    int type_index;      // Index into PrinterTypes::PRINTER_TYPES_ROLLER
    int confidence;      // 0-100 (≥70 = auto-select, <70 = suggest)
    std::string reason;  // Human-readable detection reasoning
};

/**
 * @brief Find index of printer name in PRINTER_TYPES_ROLLER
 * @param printer_name Printer type name to search for
 * @return Index in roller (0-32), or DEFAULT_PRINTER_TYPE_INDEX if not found
 */
static int find_printer_type_index(const std::string& printer_name) {
    std::istringstream stream(PrinterTypes::PRINTER_TYPES_ROLLER);
    std::string line;
    int index = 0;

    while (std::getline(stream, line)) {
        if (line == printer_name) {
            return index;
        }
        index++;
    }

    return PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX;  // "Unknown"
}

/**
 * @brief Detect printer type from hardware discovery data
 *
 * Integrates with PrinterDetector to analyze discovered hardware and suggest
 * printer type based on fingerprinting heuristics (sensors, fans, hostname).
 *
 * @return Detection hint with confidence and reasoning
 */
static PrinterDetectionHint detect_printer_type() {
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::debug("[Wizard Printer] No MoonrakerClient available for auto-detection");
        return {
            PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX,
            0,
            "No printer connection available"
        };
    }

    // Build hardware data from MoonrakerClient discovery
    PrinterHardwareData hardware;
    hardware.heaters = client->get_heaters();
    hardware.sensors = client->get_sensors();
    hardware.fans = client->get_fans();
    hardware.leds = client->get_leds();
    hardware.hostname = "";  // TODO: Add hostname getter to MoonrakerClient

    // Run detection engine
    PrinterDetectionResult result = PrinterDetector::detect(hardware);

    if (result.confidence == 0) {
        // No match found
        return {
            PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX,
            0,
            result.reason
        };
    }

    // Map detected type_name to roller index
    int type_index = find_printer_type_index(result.type_name);

    if (type_index == PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX && result.confidence > 0) {
        // Detected a printer but it's not in our roller list
        spdlog::warn("[Wizard Printer] Detected '{}' ({}% confident) but not found in PRINTER_TYPES_ROLLER",
                     result.type_name, result.confidence);
        return {
            PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX,
            result.confidence,
            "Detected: " + result.type_name + " (" + result.reason + ") - not in dropdown list"
        };
    }

    spdlog::info("[Wizard Printer] Auto-detected: {} (confidence: {}, reason: {})",
                 result.type_name, result.confidence, result.reason);

    return {
        type_index,
        result.confidence,
        result.reason
    };
}

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_printer_identify_init_subjects() {
    spdlog::debug("[Wizard Printer] Initializing subjects");

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_name = "";
    int default_type = PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX;

    try {
        default_name = config->get<std::string>("/printer/name", "");
        default_type = config->get<int>("/printer/type_index", PrinterTypes::DEFAULT_PRINTER_TYPE_INDEX);
        spdlog::debug("[Wizard Printer] Loaded from config: name='{}', type_index={}",
                      default_name, default_type);
    } catch (const std::exception& e) {
        spdlog::debug("[Wizard Printer] No existing config, using defaults");
    }

    // Initialize with values from config or defaults
    strncpy(printer_name_buffer, default_name.c_str(), sizeof(printer_name_buffer) - 1);
    printer_name_buffer[sizeof(printer_name_buffer) - 1] = '\0';

    lv_subject_init_string(&printer_name, printer_name_buffer, nullptr,
                          sizeof(printer_name_buffer), printer_name_buffer);

    lv_subject_init_int(&printer_type_selected, default_type);

    // Run auto-detection (placeholder for Phase 3)
    PrinterDetectionHint hint = detect_printer_type();
    if (hint.confidence > 0) {
        spdlog::info("[Wizard Printer] Auto-detection: {} (confidence: {}%)", hint.reason, hint.confidence);
    } else {
        spdlog::debug("[Wizard Printer] Auto-detection: {}", hint.reason);
    }

    // Initialize status message
    lv_subject_init_string(&printer_detection_status, printer_detection_status_buffer, nullptr,
                          sizeof(printer_detection_status_buffer),
                          hint.confidence >= 70 ? hint.reason.c_str() :
                          "Enter printer details or wait for auto-detection");

    // Register globally for XML binding
    lv_xml_register_subject(nullptr, "printer_name", &printer_name);
    lv_xml_register_subject(nullptr, "printer_type_selected", &printer_type_selected);
    lv_xml_register_subject(nullptr, "printer_detection_status", &printer_detection_status);

    // Initialize validation state and connection_test_passed based on loaded name
    printer_identify_validated = (default_name.length() > 0);

    // Control Next button reactively: enable if name exists, disable if empty
    int button_state = printer_identify_validated ? 1 : 0;
    lv_subject_set_int(&connection_test_passed, button_state);

    spdlog::info("[Wizard Printer] Subjects initialized (validation: {}, button_state: {})",
                 printer_identify_validated ? "valid" : "invalid", button_state);
}

// ============================================================================
// Event Handlers
// ============================================================================

/**
 * @brief Handle printer name textarea changes with enhanced validation
 *
 * Validates input, trims whitespace, updates reactive button control.
 */
static void on_printer_name_changed(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    const char* text = lv_textarea_get_text(ta);

    // Trim leading/trailing whitespace for validation
    std::string trimmed(text);
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);

    // Log if trimming made a difference
    if (trimmed != text) {
        spdlog::debug("[Wizard Printer] Name changed (trimmed): '{}' -> '{}'", text, trimmed);
    } else {
        spdlog::debug("[Wizard Printer] Name changed: '{}'", text);
    }

    // Update subject with raw text (let user keep their spaces if they want)
    lv_subject_copy_string(&printer_name, text);

    // Validate trimmed length and check max size
    const size_t max_length = sizeof(printer_name_buffer) - 1;  // 127
    bool length_valid = (trimmed.length() > 0 && trimmed.length() <= max_length);

    // Update validation state
    printer_identify_validated = length_valid;

    // Update connection_test_passed reactively (controls Next button)
    lv_subject_set_int(&connection_test_passed, printer_identify_validated ? 1 : 0);

    // Update status message
    if (printer_identify_validated) {
        lv_subject_copy_string(&printer_detection_status, "✓ Printer name entered");
    } else if (trimmed.length() > max_length) {
        lv_subject_copy_string(&printer_detection_status, "⚠ Name too long (max 127 characters)");
    } else {
        lv_subject_copy_string(&printer_detection_status,
                              "Enter printer details or wait for auto-detection");
    }

    // Save to config if valid
    if (printer_identify_validated) {
        Config* config = Config::get_instance();
        try {
            config->set("/printer/name", trimmed);  // Save trimmed version
            config->save();
            spdlog::info("[Wizard Printer] Saved printer name to config: '{}'", trimmed);
        } catch (const std::exception& e) {
            spdlog::error("[Wizard Printer] Failed to save config: {}", e.what());
        }
    }
}

/**
 * @brief Handle printer type roller changes
 */
static void on_printer_type_changed(lv_event_t* e) {
    lv_obj_t* roller = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected = lv_roller_get_selected(roller);

    spdlog::debug("[Wizard Printer] Type changed: index {}", selected);

    // Update subject
    lv_subject_set_int(&printer_type_selected, selected);

    // Save to config
    Config* config = Config::get_instance();
    try {
        config->set("/printer/type_index", (int)selected);

        // Also save the type name for reference
        char buf[64];
        lv_roller_get_selected_str(roller, buf, sizeof(buf));
        config->set("/printer/type", std::string(buf));

        config->save();
        spdlog::debug("[Wizard Printer] Saved printer type to config: {}", buf);
    } catch (const std::exception& e) {
        spdlog::error("[Wizard Printer] Failed to save config: {}", e.what());
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void ui_wizard_printer_identify_register_callbacks() {
    spdlog::debug("[Wizard Printer] Registering event callbacks");

    // Register callbacks with lv_xml system
    lv_xml_register_event_cb(nullptr, "on_printer_name_changed", on_printer_name_changed);
    lv_xml_register_event_cb(nullptr, "on_printer_type_changed", on_printer_type_changed);

    spdlog::info("[Wizard Printer] Event callbacks registered");
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* ui_wizard_printer_identify_create(lv_obj_t* parent) {
    spdlog::debug("[Wizard Printer] Creating printer identification screen");

    if (!parent) {
        spdlog::error("[Wizard Printer] Cannot create: null parent");
        return nullptr;
    }

    // Create from XML
    printer_identify_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_printer_identify", nullptr);

    if (!printer_identify_screen_root) {
        spdlog::error("[Wizard Printer] Failed to create from XML");
        return nullptr;
    }

    // Find and set up the roller with printer types
    lv_obj_t* roller = lv_obj_find_by_name(printer_identify_screen_root, "printer_type_roller");
    if (roller) {
        lv_roller_set_options(roller, PrinterTypes::PRINTER_TYPES_ROLLER, LV_ROLLER_MODE_NORMAL);

        // Set to the saved selection
        int selected = lv_subject_get_int(&printer_type_selected);
        lv_roller_set_selected(roller, selected, LV_ANIM_OFF);

        // Attach change handler
        lv_obj_add_event_cb(roller, on_printer_type_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        spdlog::debug("[Wizard Printer] Roller configured with {} options", PrinterTypes::PRINTER_TYPE_COUNT);
    } else {
        spdlog::warn("[Wizard Printer] Roller not found in XML");
    }

    // Find and set up the name textarea
    lv_obj_t* name_ta = lv_obj_find_by_name(printer_identify_screen_root, "printer_name_input");
    if (name_ta) {
        lv_obj_add_event_cb(name_ta, on_printer_name_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        ui_keyboard_register_textarea(name_ta);
        spdlog::debug("[Wizard Printer] Name textarea configured with keyboard");
    }

    // Update layout
    lv_obj_update_layout(printer_identify_screen_root);

    spdlog::info("[Wizard Printer] Screen created successfully");
    return printer_identify_screen_root;
}

// ============================================================================
// Cleanup
// ============================================================================

void ui_wizard_printer_identify_cleanup() {
    spdlog::debug("[Wizard Printer] Cleaning up printer identification screen");

    // Reset UI references
    printer_identify_screen_root = nullptr;

    // Reset connection_test_passed to enabled (1) for other wizard steps
    lv_subject_set_int(&connection_test_passed, 1);

    spdlog::info("[Wizard Printer] Cleanup complete");
}

// ============================================================================
// Utility Functions
// ============================================================================

bool ui_wizard_printer_identify_is_validated() {
    return printer_identify_validated;
}