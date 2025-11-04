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

#include "ui_wizard_fan_select.h"
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
static lv_subject_t hotend_fan_selected;
static lv_subject_t part_fan_selected;

// Screen instance
static lv_obj_t* fan_select_screen_root = nullptr;

// Dynamic options storage (for event callback mapping)
static std::vector<std::string> hotend_fan_items;
static std::vector<std::string> part_fan_items;

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_hotend_fan_changed(lv_event_t* e);
static void on_part_fan_changed(lv_event_t* e);

// ============================================================================
// Subject Initialization
// ============================================================================

void ui_wizard_fan_select_init_subjects() {
    spdlog::debug("[Wizard Fan] Initializing subjects");

    // Load existing values from config if available
    Config* config = Config::get_instance();

    // Initialize hotend fan selection (default to first option)
    int32_t hotend_index = 0;
    if (config) {
        std::string fan = config->get<std::string>("/printer/hotend_fan", "heater_fan hotend_fan");
        if (fan == "None" || fan.empty()) {
            hotend_index = 1;  // "None" option
        }
    }
    lv_subject_init_int(&hotend_fan_selected, hotend_index);
    lv_xml_register_subject(nullptr, "hotend_fan_selected", &hotend_fan_selected);

    // Initialize part fan selection (default to first option)
    int32_t part_index = 0;
    if (config) {
        std::string fan = config->get<std::string>("/printer/part_fan", "fan");
        if (fan == "fan_generic part_fan") {
            part_index = 1;
        } else if (fan == "None" || fan.empty()) {
            part_index = 2;  // "None" option
        }
    }
    lv_subject_init_int(&part_fan_selected, part_index);
    lv_xml_register_subject(nullptr, "part_fan_selected", &part_fan_selected);

    spdlog::info("[Wizard Fan] Subjects initialized - hotend: {}, part: {}",
                 hotend_index, part_index);
}

// ============================================================================
// Event Callbacks
// ============================================================================

static void on_hotend_fan_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Fan] Hotend fan selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&hotend_fan_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < hotend_fan_items.size()) {
        config->set("/printer/hotend_fan", hotend_fan_items[selected_index]);
        spdlog::debug("[Wizard Fan] Saved hotend fan: {}", hotend_fan_items[selected_index]);
    }
}

static void on_part_fan_changed(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    uint16_t selected_index = lv_dropdown_get_selected(dropdown);

    spdlog::debug("[Wizard Fan] Part fan selection changed to index: {}", selected_index);

    // Update subject
    lv_subject_set_int(&part_fan_selected, selected_index);

    // Save to config
    Config* config = Config::get_instance();
    if (config && selected_index < part_fan_items.size()) {
        config->set("/printer/part_fan", part_fan_items[selected_index]);
        spdlog::debug("[Wizard Fan] Saved part fan: {}", part_fan_items[selected_index]);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void ui_wizard_fan_select_register_callbacks() {
    spdlog::debug("[Wizard Fan] Registering callbacks");

    lv_xml_register_event_cb(nullptr, "on_hotend_fan_changed", on_hotend_fan_changed);
    lv_xml_register_event_cb(nullptr, "on_part_fan_changed", on_part_fan_changed);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* ui_wizard_fan_select_create(lv_obj_t* parent) {
    spdlog::info("[Wizard Fan] Creating fan select screen");

    if (fan_select_screen_root) {
        spdlog::warn("[Wizard Fan] Screen already exists, destroying old instance");
        lv_obj_del(fan_select_screen_root);
        fan_select_screen_root = nullptr;
    }

    // Create screen from XML
    fan_select_screen_root = (lv_obj_t*)lv_xml_create(parent, "wizard_fan_select", nullptr);
    if (!fan_select_screen_root) {
        spdlog::error("[Wizard Fan] Failed to create screen from XML");
        return nullptr;
    }

    // Get Moonraker client for hardware discovery
    MoonrakerClient* client = get_moonraker_client();

    // Build hotend fan options from discovered hardware
    hotend_fan_items.clear();
    std::string hotend_options_str;

    if (client) {
        const auto& fans = client->get_fans();
        for (const auto& fan : fans) {
            // Filter for hotend/heater fans
            if (fan.find("heater_fan") != std::string::npos ||
                fan.find("hotend_fan") != std::string::npos) {
                hotend_fan_items.push_back(fan);
                if (!hotend_options_str.empty()) hotend_options_str += "\n";
                hotend_options_str += fan;
            }
        }
    }

    // Always add "None" option
    hotend_fan_items.push_back("None");
    if (!hotend_options_str.empty()) hotend_options_str += "\n";
    hotend_options_str += "None";

    // Build part cooling fan options from discovered hardware
    part_fan_items.clear();
    std::string part_options_str;

    if (client) {
        const auto& fans = client->get_fans();
        for (const auto& fan : fans) {
            // Filter for part cooling fans (has "fan" but NOT "heater_fan")
            if (fan.find("fan") != std::string::npos &&
                fan.find("heater_fan") == std::string::npos &&
                fan.find("hotend_fan") == std::string::npos) {
                part_fan_items.push_back(fan);
                if (!part_options_str.empty()) part_options_str += "\n";
                part_options_str += fan;
            }
        }
    }

    // Always add "None" option
    part_fan_items.push_back("None");
    if (!part_options_str.empty()) part_options_str += "\n";
    part_options_str += "None";

    // Find and configure hotend fan dropdown
    lv_obj_t* hotend_dropdown = lv_obj_find_by_name(fan_select_screen_root, "hotend_fan_dropdown");
    if (hotend_dropdown) {
        lv_dropdown_set_options(hotend_dropdown, hotend_options_str.c_str());
        int index = lv_subject_get_int(&hotend_fan_selected);
        if (index >= static_cast<int>(hotend_fan_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(hotend_dropdown, index);
        spdlog::debug("[Wizard Fan] Configured hotend fan dropdown with {} options, selected: {}",
                     hotend_fan_items.size(), index);
    }

    // Find and configure part fan dropdown
    lv_obj_t* part_dropdown = lv_obj_find_by_name(fan_select_screen_root, "part_cooling_fan_dropdown");
    if (part_dropdown) {
        lv_dropdown_set_options(part_dropdown, part_options_str.c_str());
        int index = lv_subject_get_int(&part_fan_selected);
        if (index >= static_cast<int>(part_fan_items.size())) {
            index = 0;  // Reset to first option if out of range
        }
        lv_dropdown_set_selected(part_dropdown, index);
        spdlog::debug("[Wizard Fan] Configured part fan dropdown with {} options, selected: {}",
                     part_fan_items.size(), index);
    }

    spdlog::info("[Wizard Fan] Screen created successfully");
    return fan_select_screen_root;
}

// ============================================================================
// Cleanup
// ============================================================================

void ui_wizard_fan_select_cleanup() {
    spdlog::debug("[Wizard Fan] Cleaning up resources");

    if (fan_select_screen_root) {
        lv_obj_del(fan_select_screen_root);
        fan_select_screen_root = nullptr;
    }
}

// ============================================================================
// Validation
// ============================================================================

bool ui_wizard_fan_select_is_validated() {
    // Always return true for baseline implementation
    return true;
}