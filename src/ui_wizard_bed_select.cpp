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

#include "ui_wizard_bed_select.h"
#include "ui_wizard.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_client.h"
#include "lvgl/lvgl.h"
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <cstring>

// ============================================================================
// Static Data & Subjects
// ============================================================================

// Subject declarations (module scope)
static lv_subject_t bed_heater_selected;
static lv_subject_t bed_sensor_selected;

// Screen instance
static lv_obj_t* bed_select_screen_root = nullptr;

// Dynamic options storage (for event callback mapping)
static std::vector<std::string> bed_heater_items;
static std::vector<std::string> bed_sensor_items;

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_bed_heater_changed(lv_event_t* e);
static void on_bed_sensor_changed(lv_event_t* e);

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_bed_select_init_subjects() {
    spdlog::debug("[Wizard Bed] Initializing subjects");

    // Load existing values from config if available
    Config* config = Config::get_instance();

    // Initialize bed heater selection (default to first option)
    int32_t heater_index = 0;
    if (config) {
        std::string heater = config->get<std::string>("/printer/bed_heater", "heater_bed");
        if (heater == "None" || heater.empty()) {
            heater_index = 1;  // "None" option
        }
    }
    lv_subject_init_int(&bed_heater_selected, heater_index);
    lv_xml_register_subject(nullptr, "bed_heater_selected", &bed_heater_selected);

    // Initialize bed sensor selection (default to second option)
    int32_t sensor_index = 1;  // Default to "temperature_sensor bed"
    if (config) {
        std::string sensor = config->get<std::string>("/printer/bed_sensor", "temperature_sensor bed");
        if (sensor == "temperature_sensor extruder") {
            sensor_index = 0;
        } else if (sensor == "None" || sensor.empty()) {
            sensor_index = 2;  // "None" option
        }
    }
    lv_subject_init_int(&bed_sensor_selected, sensor_index);
    lv_xml_register_subject(nullptr, "bed_sensor_selected", &bed_sensor_selected);

    // Always enable Next button for baseline implementation
    ui_wizard_set_button_enabled(true, true);

    spdlog::info("[Wizard Bed] Subjects initialized - heater: {}, sensor: {}",
                 heater_index, sensor_index);
}

// ============================================================================
// Event Callbacks
// ============================================================================

static void on_bed_heater_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Bed] Heater selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&bed_heater_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < bed_heater_items.size()) {
        config->set("/printer/bed_heater", bed_heater_items[selected_index]);
        spdlog::debug("[Wizard Bed] Saved bed heater: {}", bed_heater_items[selected_index]);
    }
}

static void on_bed_sensor_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Bed] Sensor selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&bed_sensor_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < bed_sensor_items.size()) {
        config->set("/printer/bed_sensor", bed_sensor_items[selected_index]);
        spdlog::debug("[Wizard Bed] Saved bed sensor: {}", bed_sensor_items[selected_index]);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void ui_wizard_bed_select_register_callbacks() {
    spdlog::debug("[Wizard Bed] Registering callbacks");

    lv_xml_register_event_cb(nullptr, "on_bed_heater_changed", on_bed_heater_changed);
    lv_xml_register_event_cb(nullptr, "on_bed_sensor_changed", on_bed_sensor_changed);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* ui_wizard_bed_select_create(lv_obj_t* parent) {
    spdlog::info("[Wizard Bed] Creating bed select screen");

    if (bed_select_screen_root) {
        spdlog::warn("[Wizard Bed] Screen already exists, destroying old instance");
        lv_obj_del(bed_select_screen_root);
        bed_select_screen_root = nullptr;
    }

    // Create screen from XML
    bed_select_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_bed_select", nullptr);
    if (!bed_select_screen_root) {
        spdlog::error("[Wizard Bed] Failed to create screen from XML");
        return nullptr;
    }

    // Get Moonraker client for hardware discovery
    MoonrakerClient* client = get_moonraker_client();

    // Build bed heater options from discovered hardware
    bed_heater_items.clear();
    std::string heater_options_str;

    if (client) {
        const auto& heaters = client->get_heaters();
        for (const auto& heater : heaters) {
            // Filter for bed-related heaters
            if (heater.find("bed") != std::string::npos) {
                bed_heater_items.push_back(heater);
                if (!heater_options_str.empty()) heater_options_str += "\n";
                heater_options_str += heater;
            }
        }
    }

    // Always add "None" option
    bed_heater_items.push_back("None");
    if (!heater_options_str.empty()) heater_options_str += "\n";
    heater_options_str += "None";

    // Build bed sensor options from discovered hardware
    bed_sensor_items.clear();
    std::string sensor_options_str;

    if (client) {
        const auto& sensors = client->get_sensors();
        // For bed sensors, include all sensors (user can choose chamber, bed, etc.)
        for (const auto& sensor : sensors) {
            bed_sensor_items.push_back(sensor);
            if (!sensor_options_str.empty()) sensor_options_str += "\n";
            sensor_options_str += sensor;
        }
    }

    // Always add "None" option
    bed_sensor_items.push_back("None");
    if (!sensor_options_str.empty()) sensor_options_str += "\n";
    sensor_options_str += "None";

    // Find and configure heater dropdown
    lv_obj_t* heater_dropdown = lv_obj_find_by_name(bed_select_screen_root, "bed_heater_dropdown");
    if (heater_dropdown) {
        lv_dropdown_set_options(heater_dropdown, heater_options_str.c_str());
        int index = lv_subject_get_int(&bed_heater_selected);
        if (index >= static_cast<int>(bed_heater_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(heater_dropdown, index);
        spdlog::debug("[Wizard Bed] Configured heater dropdown with {} options, selected: {}",
                     bed_heater_items.size(), index);
    }

    // Find and configure sensor dropdown
    lv_obj_t* sensor_dropdown = lv_obj_find_by_name(bed_select_screen_root, "bed_sensor_dropdown");
    if (sensor_dropdown) {
        lv_dropdown_set_options(sensor_dropdown, sensor_options_str.c_str());
        int index = lv_subject_get_int(&bed_sensor_selected);
        if (index >= static_cast<int>(bed_sensor_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(sensor_dropdown, index);
        spdlog::debug("[Wizard Bed] Configured sensor dropdown with {} options, selected: {}",
                     bed_sensor_items.size(), index);
    }

    spdlog::info("[Wizard Bed] Screen created successfully");
    return bed_select_screen_root;
}

// ============================================================================
// Cleanup
// ============================================================================

void ui_wizard_bed_select_cleanup() {
    spdlog::debug("[Wizard Bed] Cleaning up resources");

    if (bed_select_screen_root) {
        lv_obj_del(bed_select_screen_root);
        bed_select_screen_root = nullptr;
    }
}

// ============================================================================
// Validation
// ============================================================================

bool ui_wizard_bed_select_is_validated() {
    // Always return true for baseline implementation
    return true;
}