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
#include "lvgl/lvgl.h"
#include <spdlog/spdlog.h>
#include <string>
#include <cstring>

// ============================================================================
// Static Data & Subjects
// ============================================================================

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

// Printer types (matches roller options in XML)
static const char* printer_types =
    "Anycubic i3 Mega\n"
    "Anycubic Kobra\n"
    "Anycubic Vyper\n"
    "Bambu Lab X1\n"
    "Bambu Lab P1P\n"
    "Creality CR-10\n"
    "Creality Ender 3\n"
    "Creality Ender 5\n"
    "Creality K1\n"
    "FlashForge Creator Pro\n"
    "FlashForge Dreamer\n"
    "LulzBot TAZ\n"
    "LulzBot Mini\n"
    "MakerBot Replicator\n"
    "Prusa i3 MK3\n"
    "Prusa i3 MK4\n"
    "Prusa Mini\n"
    "Prusa XL\n"
    "Qidi Tech X-Max\n"
    "Qidi Tech X-Plus\n"
    "Raise3D Pro2\n"
    "Raise3D E2\n"
    "Sovol SV01\n"
    "Sovol SV06\n"
    "Ultimaker 2+\n"
    "Ultimaker 3\n"
    "Ultimaker S3\n"
    "Voron 0.1\n"
    "Voron 2.4\n"
    "Voron Trident\n"
    "Voron Switchwire\n"
    "Custom/Other\n"
    "Unknown";

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_printer_name_changed(lv_event_t* e);
static void on_printer_type_changed(lv_event_t* e);

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_printer_identify_init_subjects() {
    spdlog::debug("[Wizard Printer] Initializing subjects");

    // Load existing values from config if available
    Config* config = Config::get_instance();
    std::string default_name = "";
    int default_type = 32; // Default to "Unknown"

    try {
        default_name = config->get<std::string>("/printer/name", "");
        default_type = config->get<int>("/printer/type_index", 32);
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

    lv_subject_init_string(&printer_detection_status, printer_detection_status_buffer, nullptr,
                          sizeof(printer_detection_status_buffer),
                          "Enter printer details or wait for auto-detection");

    // Register globally for XML binding
    lv_xml_register_subject(nullptr, "printer_name", &printer_name);
    lv_xml_register_subject(nullptr, "printer_type_selected", &printer_type_selected);
    lv_xml_register_subject(nullptr, "printer_detection_status", &printer_detection_status);

    // Reset validation state
    printer_identify_validated = false;

    spdlog::info("[Wizard Printer] Subjects initialized");
}

// ============================================================================
// Event Handlers
// ============================================================================

/**
 * @brief Handle printer name textarea changes
 */
static void on_printer_name_changed(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    const char* text = lv_textarea_get_text(ta);

    spdlog::debug("[Wizard Printer] Name changed: '{}'", text);

    // Update subject
    lv_subject_copy_string(&printer_name, text);

    // Check validation
    printer_identify_validated = (strlen(text) > 0);

    // Update status
    if (printer_identify_validated) {
        lv_subject_copy_string(&printer_detection_status, "✓ Printer name entered");
    } else {
        lv_subject_copy_string(&printer_detection_status,
                              "Enter printer details or wait for auto-detection");
    }

    // Save to config
    if (printer_identify_validated) {
        Config* config = Config::get_instance();
        try {
            config->set("/printer/name", std::string(text));
            config->save();
            spdlog::debug("[Wizard Printer] Saved printer name to config");
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
        lv_roller_set_options(roller, printer_types, LV_ROLLER_MODE_NORMAL);

        // Set to the saved selection
        int selected = lv_subject_get_int(&printer_type_selected);
        lv_roller_set_selected(roller, selected, LV_ANIM_OFF);

        // Attach change handler
        lv_obj_add_event_cb(roller, on_printer_type_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        spdlog::debug("[Wizard Printer] Roller configured with {} options", 33);
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

    spdlog::info("[Wizard Printer] Cleanup complete");
}

// ============================================================================
// Utility Functions
// ============================================================================

bool ui_wizard_printer_identify_is_validated() {
    return printer_identify_validated;
}