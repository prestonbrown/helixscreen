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

#include "ui_wizard_hotend_select.h"
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
static lv_subject_t hotend_heater_selected;
static lv_subject_t hotend_sensor_selected;

// Screen instance
static lv_obj_t* hotend_select_screen_root = nullptr;

// Dynamic options storage (for event callback mapping)
static std::vector<std::string> hotend_heater_items;
static std::vector<std::string> hotend_sensor_items;

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_hotend_heater_changed(lv_event_t* e);
static void on_hotend_sensor_changed(lv_event_t* e);

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_hotend_select_init_subjects() {
    spdlog::debug("[Wizard Hotend] Initializing subjects");

    // Load existing values from config if available
    Config* config = Config::get_instance();

    // Initialize hotend heater selection (default to extruder)
    int32_t heater_index = 0;
    if (config) {
        std::string heater = config->get<std::string>("/printer/hotend_heater", "extruder");
        if (heater == "None" || heater.empty()) {
            heater_index = 1;  // "None" option
        }
    }
    lv_subject_init_int(&hotend_heater_selected, heater_index);
    lv_xml_register_subject(nullptr, "hotend_heater_selected", &hotend_heater_selected);

    // Initialize hotend sensor selection (default to temperature_sensor extruder)
    int32_t sensor_index = 0;
    if (config) {
        std::string sensor = config->get<std::string>("/printer/hotend_sensor", "temperature_sensor extruder");
        if (sensor == "None" || sensor.empty()) {
            sensor_index = 1;  // "None" option
        }
    }
    lv_subject_init_int(&hotend_sensor_selected, sensor_index);
    lv_xml_register_subject(nullptr, "hotend_sensor_selected", &hotend_sensor_selected);

    // Always enable Next button for baseline implementation
    ui_wizard_set_button_enabled(true, true);

    spdlog::info("[Wizard Hotend] Subjects initialized - heater: {}, sensor: {}",
                 heater_index, sensor_index);
}

// ============================================================================
// Event Callbacks
// ============================================================================

static void on_hotend_heater_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Hotend] Heater selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&hotend_heater_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < hotend_heater_items.size()) {
        config->set("/printer/hotend_heater", hotend_heater_items[selected_index]);
        spdlog::debug("[Wizard Hotend] Saved hotend heater: {}", hotend_heater_items[selected_index]);
    }
}

static void on_hotend_sensor_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Hotend] Sensor selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&hotend_sensor_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < hotend_sensor_items.size()) {
        config->set("/printer/hotend_sensor", hotend_sensor_items[selected_index]);
        spdlog::debug("[Wizard Hotend] Saved hotend sensor: {}", hotend_sensor_items[selected_index]);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void ui_wizard_hotend_select_register_callbacks() {
    spdlog::debug("[Wizard Hotend] Registering callbacks");

    lv_xml_register_event_cb(nullptr, "on_hotend_heater_changed", on_hotend_heater_changed);
    lv_xml_register_event_cb(nullptr, "on_hotend_sensor_changed", on_hotend_sensor_changed);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* ui_wizard_hotend_select_create(lv_obj_t* parent) {
    spdlog::info("[Wizard Hotend] Creating hotend select screen");

    if (hotend_select_screen_root) {
        spdlog::warn("[Wizard Hotend] Screen already exists, destroying old instance");
        lv_obj_del(hotend_select_screen_root);
        hotend_select_screen_root = nullptr;
    }

    // Create screen from XML
    hotend_select_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_hotend_select", nullptr);
    if (!hotend_select_screen_root) {
        spdlog::error("[Wizard Hotend] Failed to create screen from XML");
        return nullptr;
    }

    // Get Moonraker client for hardware discovery
    MoonrakerClient* client = get_moonraker_client();

    // Build hotend heater options from discovered hardware
    hotend_heater_items.clear();
    std::string heater_options_str;

    if (client) {
        const auto& heaters = client->get_heaters();
        for (const auto& heater : heaters) {
            // Filter for extruder-related heaters
            if (heater.find("extruder") != std::string::npos) {
                hotend_heater_items.push_back(heater);
                if (!heater_options_str.empty()) heater_options_str += "\n";
                heater_options_str += heater;
            }
        }
    }

    // Always add "None" option
    hotend_heater_items.push_back("None");
    if (!heater_options_str.empty()) heater_options_str += "\n";
    heater_options_str += "None";

    // Build hotend sensor options from discovered hardware
    hotend_sensor_items.clear();
    std::string sensor_options_str;

    if (client) {
        const auto& sensors = client->get_sensors();
        // For hotend sensors, filter for extruder/hotend-related sensors
        for (const auto& sensor : sensors) {
            if (sensor.find("extruder") != std::string::npos ||
                sensor.find("hotend") != std::string::npos) {
                hotend_sensor_items.push_back(sensor);
                if (!sensor_options_str.empty()) sensor_options_str += "\n";
                sensor_options_str += sensor;
            }
        }
    }

    // Always add "None" option
    hotend_sensor_items.push_back("None");
    if (!sensor_options_str.empty()) sensor_options_str += "\n";
    sensor_options_str += "None";

    // Find and configure heater dropdown
    lv_obj_t* heater_dropdown = lv_obj_find_by_name(hotend_select_screen_root, "hotend_heater_dropdown");
    if (heater_dropdown) {
        lv_dropdown_set_options(heater_dropdown, heater_options_str.c_str());
        int index = lv_subject_get_int(&hotend_heater_selected);
        if (index >= static_cast<int>(hotend_heater_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(heater_dropdown, index);
        spdlog::debug("[Wizard Hotend] Configured heater dropdown with {} options, selected: {}",
                     hotend_heater_items.size(), index);
    }

    // Find and configure sensor dropdown
    lv_obj_t* sensor_dropdown = lv_obj_find_by_name(hotend_select_screen_root, "hotend_sensor_dropdown");
    if (sensor_dropdown) {
        lv_dropdown_set_options(sensor_dropdown, sensor_options_str.c_str());
        int index = lv_subject_get_int(&hotend_sensor_selected);
        if (index >= static_cast<int>(hotend_sensor_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(sensor_dropdown, index);
        spdlog::debug("[Wizard Hotend] Configured sensor dropdown with {} options, selected: {}",
                     hotend_sensor_items.size(), index);
    }

    spdlog::info("[Wizard Hotend] Screen created successfully");
    return hotend_select_screen_root;
}

// ============================================================================
// Cleanup
// ============================================================================

void ui_wizard_hotend_select_cleanup() {
    spdlog::debug("[Wizard Hotend] Cleaning up resources");

    if (hotend_select_screen_root) {
        lv_obj_del(hotend_select_screen_root);
        hotend_select_screen_root = nullptr;
    }
}

// ============================================================================
// Validation
// ============================================================================

bool ui_wizard_hotend_select_is_validated() {
    // Always return true for baseline implementation
    return true;
}