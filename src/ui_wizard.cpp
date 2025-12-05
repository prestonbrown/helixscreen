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

#include "ui_wizard.h"

#include "ui_error_reporting.h"
#include "ui_panel_home.h"
#include "ui_subject_registry.h"
#include "ui_theme.h"
#include "ui_wizard_connection.h"
#include "ui_wizard_fan_select.h"
#include "ui_wizard_heater_select.h"
#include "ui_wizard_led_select.h"
#include "ui_wizard_printer_identify.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_wifi.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstdio>

// Subject declarations (static/global scope required)
static lv_subject_t current_step;
static lv_subject_t total_steps;
static lv_subject_t wizard_title;
static lv_subject_t wizard_progress;
static lv_subject_t wizard_next_button_text;
static lv_subject_t wizard_subtitle;

// Non-static: accessible from ui_wizard_connection.cpp
lv_subject_t connection_test_passed; // Global: 0=connection not validated, 1=validated or N/A

// String buffers (must be persistent)
static char wizard_title_buffer[64];
static char wizard_progress_buffer[32];
static char wizard_next_button_text_buffer[16];
static char wizard_subtitle_buffer[128];

// Wizard container instance
static lv_obj_t* wizard_container = nullptr;

// Track current screen for proper cleanup
static int current_screen_step = 0;

// Forward declarations
static void on_back_clicked(lv_event_t* e);
static void on_next_clicked(lv_event_t* e);
static void ui_wizard_load_screen(int step);
static void ui_wizard_cleanup_current_screen();
static const char* get_step_title_from_xml(int step);
static const char* get_step_subtitle_from_xml(int step);

// ============================================================================
// Step Metadata (read from XML <consts>)
// ============================================================================

/**
 * Map step number to XML component name
 * Each component defines its own step_title in its <consts> block
 */
static const char* const STEP_COMPONENT_NAMES[] = {
    nullptr,                   // 0 (unused, 1-indexed)
    "wizard_wifi_setup",       // 1
    "wizard_connection",       // 2
    "wizard_printer_identify", // 3
    "wizard_heater_select",    // 4
    "wizard_fan_select",       // 5
    "wizard_led_select",       // 6
    "wizard_summary"           // 7
};
static constexpr int STEP_COMPONENT_COUNT = 7;

/**
 * Get step title from XML component's <consts> block
 *
 * Each wizard step XML file defines:
 *   <consts>
 *     <str name="step_title" value="WiFi Setup"/>
 *     <int name="step_order" value="1"/>
 *   </consts>
 *
 * This function reads step_title from the component's scope at runtime,
 * eliminating hardcoded title strings in C++.
 */
static const char* get_step_title_from_xml(int step) {
    if (step < 1 || step > STEP_COMPONENT_COUNT) {
        spdlog::warn("[Wizard] Invalid step {} for title lookup", step);
        return "Unknown Step";
    }

    const char* comp_name = STEP_COMPONENT_NAMES[step];
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        spdlog::warn("[Wizard] Component scope not found for '{}'", comp_name);
        return "Unknown Step";
    }

    const char* title = lv_xml_get_const(scope, "step_title");
    if (!title) {
        spdlog::warn("[Wizard] step_title not found in '{}' consts", comp_name);
        return "Unknown Step";
    }

    return title;
}

/**
 * Get step subtitle from XML component's <consts> block
 *
 * Subtitles provide contextual hints (e.g., "Skip if using Ethernet")
 * that appear below the title in the wizard header.
 */
static const char* get_step_subtitle_from_xml(int step) {
    if (step < 1 || step > STEP_COMPONENT_COUNT) {
        return "";
    }

    const char* comp_name = STEP_COMPONENT_NAMES[step];
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        return "";
    }

    const char* subtitle = lv_xml_get_const(scope, "step_subtitle");
    return subtitle ? subtitle : "";
}

void ui_wizard_init_subjects() {
    spdlog::debug("[Wizard] Initializing subjects");

    // Initialize subjects with defaults
    UI_SUBJECT_INIT_AND_REGISTER_INT(current_step, 1, "current_step");
    UI_SUBJECT_INIT_AND_REGISTER_INT(
        total_steps, 7,
        "total_steps"); // 7 steps: WiFi, Connection, Printer, Heater, Fan, LED, Summary

    UI_SUBJECT_INIT_AND_REGISTER_STRING(wizard_title, wizard_title_buffer, "Welcome",
                                        "wizard_title");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(wizard_progress, wizard_progress_buffer, "Step 1 of 7",
                                        "wizard_progress");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(wizard_next_button_text, wizard_next_button_text_buffer,
                                        "Next", "wizard_next_button_text");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(wizard_subtitle, wizard_subtitle_buffer, "",
                                        "wizard_subtitle");

    // Initialize connection_test_passed to 1 (enabled by default for all steps)
    // Step 2 (connection) will set it to 0 until test passes
    UI_SUBJECT_INIT_AND_REGISTER_INT(connection_test_passed, 1, "connection_test_passed");

    spdlog::debug("[0");
}

// Helper type for constant name/value pairs
struct WizardConstant {
    const char* name;
    const char* value;
};

// Helper: Register array of constants to a scope
static void register_constants_to_scope(lv_xml_component_scope_t* scope,
                                        const WizardConstant* constants) {
    if (!scope)
        return;
    for (int i = 0; constants[i].name != NULL; i++) {
        lv_xml_register_const(scope, constants[i].name, constants[i].value);
    }
}

void ui_wizard_container_register_responsive_constants() {
    spdlog::debug("[Wizard] Registering responsive constants to wizard_container scope");

    // 1. Detect screen size using custom breakpoints
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // 2. Determine responsive values based on breakpoint
    const char* header_height;
    const char* footer_height;
    const char* button_width;
    const char* header_font;
    const char* title_font;
    const char* wifi_card_height;
    const char* wifi_ethernet_height;
    const char* wifi_toggle_height;
    const char* network_icon_size;
    const char* size_label;

    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // ≤480: 480x320
        header_height = "32";
        footer_height = "72"; // header + 40
        button_width = "110";
        header_font = "montserrat_14";
        title_font = "montserrat_16";
        wifi_card_height = "80";
        wifi_ethernet_height = "70";
        wifi_toggle_height = "32";
        network_icon_size = "20";
        size_label = "SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        header_height = "42";
        footer_height = "82"; // header + 40
        button_width = "140";
        header_font = "montserrat_16";
        title_font = "montserrat_20";
        wifi_card_height = "120";
        wifi_ethernet_height = "100";
        wifi_toggle_height = "48";
        network_icon_size = "24";
        size_label = "MEDIUM";
    } else { // >800: 1024x600+
        header_height = "48";
        footer_height = "88"; // header + 40
        button_width = "160";
        header_font = "montserrat_20";
        title_font = lv_xml_get_const(NULL, "font_heading");
        wifi_card_height = "140";
        wifi_ethernet_height = "120";
        wifi_toggle_height = "64";
        network_icon_size = "32";
        size_label = "LARGE";
    }

    spdlog::debug("[Wizard] Screen size: {} (greater_res={}px)", size_label, greater_res);

    // 3. Read padding/gap from globals (unified space_* tokens)
    const char* padding_value = lv_xml_get_const(NULL, "space_lg");
    const char* gap_value = lv_xml_get_const(NULL, "space_md");

    // 4. Define all wizard constants in array
    WizardConstant constants[] = {
        // Layout dimensions
        {"wizard_padding", padding_value},
        {"wizard_gap", gap_value},
        {"wizard_header_height", header_height},
        {"wizard_footer_height", footer_height},
        {"wizard_button_width", button_width},
        // Typography
        {"wizard_header_font", header_font},
        {"wizard_title_font", title_font},
        // WiFi screen specific
        {"wifi_toggle_height", wifi_toggle_height},
        {"wifi_card_height", wifi_card_height},
        {"wifi_ethernet_height", wifi_ethernet_height},
        {"network_icon_size", network_icon_size},
        {NULL, NULL} // Sentinel
    };

    // 5. Register to wizard_container scope (parent)
    lv_xml_component_scope_t* parent_scope = lv_xml_component_get_scope("wizard_container");
    register_constants_to_scope(parent_scope, constants);

    // 6. Define child components that inherit these constants
    // Note: WiFi network list constants (list_item_padding, list_item_height, list_item_font)
    //       are registered separately by ui_wizard_wifi_register_responsive_constants()
    const char* children[] = {
        "wizard_wifi_setup",
        "wizard_connection",
        "wizard_printer_identify",
        "wizard_heater_select",
        "wizard_fan_select",
        "wizard_led_select",
        "wizard_summary",
        NULL // Sentinel
    };

    // 7. Propagate to all children
    int child_count = 0;
    for (int i = 0; children[i] != NULL; i++) {
        lv_xml_component_scope_t* child_scope = lv_xml_component_get_scope(children[i]);
        if (child_scope) {
            register_constants_to_scope(child_scope, constants);
            child_count++;
        }
    }

    spdlog::debug("[Wizard] Registered 11 constants to wizard_container and propagated to {} child "
                  "components (7 wizard screens)",
                  child_count);
    spdlog::debug("[Wizard] Values: padding={}, gap={}, header_h={}, footer_h={}, button_w={}",
                  padding_value, gap_value, header_height, footer_height, button_width);
}

void ui_wizard_register_event_callbacks() {
    spdlog::debug("[Wizard] Registering event callbacks");
    lv_xml_register_event_cb(nullptr, "on_back_clicked", on_back_clicked);
    lv_xml_register_event_cb(nullptr, "on_next_clicked", on_next_clicked);
}

lv_obj_t* ui_wizard_create(lv_obj_t* parent) {
    spdlog::debug("[Wizard] Creating wizard container");

    // Create wizard from XML (constants already registered)
    wizard_container = (lv_obj_t*)lv_xml_create(parent, "wizard_container", nullptr);

    if (!wizard_container) {
        spdlog::error("[Wizard] Failed to create wizard_container from XML");
        return nullptr;
    }

    // Background color applied automatically by LVGL theme (uses theme->color_card)
    // No explicit styling needed - theme patching in ui_theme.cpp handles this

    // Update layout to ensure SIZE_CONTENT calculates correctly
    lv_obj_update_layout(wizard_container);

    spdlog::debug("[Wizard] Wizard container created successfully");
    return wizard_container;
}

void ui_wizard_navigate_to_step(int step) {
    spdlog::debug("[Wizard] Navigating to step {}", step);

    // Clamp step to valid range
    int total = lv_subject_get_int(&total_steps);
    if (step < 1)
        step = 1;
    if (step > total)
        step = total;

    // Update current_step subject
    lv_subject_set_int(&current_step, step);

    // Update next button text based on step
    if (step == total) {
        lv_subject_copy_string(&wizard_next_button_text, "Finish");
    } else {
        lv_subject_copy_string(&wizard_next_button_text, "Next");
    }

    // Update progress text
    char progress_buf[32];
    snprintf(progress_buf, sizeof(progress_buf), "Step %d of %d", step, total);
    lv_subject_copy_string(&wizard_progress, progress_buf);

    // Load screen content
    ui_wizard_load_screen(step);

    // Force layout update on entire wizard after screen is loaded
    if (wizard_container) {
        lv_obj_update_layout(wizard_container);
    }

    spdlog::debug("[Wizard] Updated to step {}/{}, button: {}", step, total,
                  (step == total) ? "Finish" : "Next");
}

void ui_wizard_set_title(const char* title) {
    if (!title) {
        spdlog::warn("[Wizard] set_title called with nullptr, ignoring");
        return;
    }

    spdlog::debug("[Wizard] Setting title: {}", title);
    lv_subject_copy_string(&wizard_title, title);
}

// ============================================================================
// Screen Cleanup
// ============================================================================

/**
 * Cleanup the current wizard screen before navigating to a new one
 *
 * Calls the appropriate cleanup function based on current_screen_step.
 * This ensures resources are properly released and screen pointers are reset.
 */
static void ui_wizard_cleanup_current_screen() {
    if (current_screen_step == 0) {
        return; // No screen loaded yet
    }

    spdlog::debug("[Wizard] Cleaning up screen for step {}", current_screen_step);

    switch (current_screen_step) {
    case 1: // WiFi Setup
        get_wizard_wifi_step()->cleanup();
        break;
    case 2: // Moonraker Connection
        get_wizard_connection_step()->cleanup();
        break;
    case 3: // Printer Identification
        get_wizard_printer_identify_step()->cleanup();
        break;
    case 4: // Heater Select (combined bed + hotend)
        get_wizard_heater_select_step()->cleanup();
        break;
    case 5: // Fan Select
        get_wizard_fan_select_step()->cleanup();
        break;
    case 6: // LED Select
        get_wizard_led_select_step()->cleanup();
        break;
    case 7: // Summary
        get_wizard_summary_step()->cleanup();
        break;
    default:
        spdlog::warn("[Wizard] Unknown screen step {} during cleanup", current_screen_step);
        break;
    }
}

// ============================================================================
// Screen Loading
// ============================================================================

static void ui_wizard_load_screen(int step) {
    spdlog::debug("[Wizard] Loading screen for step {}", step);

    // Find wizard_content container
    lv_obj_t* content = lv_obj_find_by_name(wizard_container, "wizard_content");
    if (!content) {
        spdlog::error("[Wizard] wizard_content container not found");
        return;
    }

    // Cleanup previous screen resources BEFORE clearing widgets
    ui_wizard_cleanup_current_screen();

    // Clear existing content (widgets)
    lv_obj_clean(content);
    spdlog::debug("[Wizard] Cleared wizard_content container");

    // Set title and subtitle from XML metadata (no more hardcoded strings!)
    const char* title = get_step_title_from_xml(step);
    ui_wizard_set_title(title);
    const char* subtitle = get_step_subtitle_from_xml(step);
    lv_subject_copy_string(&wizard_subtitle, subtitle);

    // Create appropriate screen based on step
    // Note: Step-specific initialization remains in switch because each step
    // has unique logic (WiFi needs init_wifi_manager, etc.)
    switch (step) {
    case 1: // WiFi Setup
        spdlog::debug("[Wizard] Creating WiFi setup screen");
        get_wizard_wifi_step()->init_subjects();
        get_wizard_wifi_step()->register_callbacks();
        get_wizard_wifi_step()->create(content);
        lv_obj_update_layout(content);
        get_wizard_wifi_step()->init_wifi_manager();
        break;

    case 2: // Moonraker Connection
        spdlog::debug("[Wizard] Creating Moonraker connection screen");
        get_wizard_connection_step()->init_subjects();
        get_wizard_connection_step()->register_callbacks();
        get_wizard_connection_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 3: // Printer Identification
        spdlog::debug("[Wizard] Creating printer identification screen");
        get_wizard_printer_identify_step()->init_subjects();
        get_wizard_printer_identify_step()->register_callbacks();
        get_wizard_printer_identify_step()->create(content);
        lv_obj_update_layout(content);
        // Override subtitle with dynamic detection status
        lv_subject_copy_string(&wizard_subtitle,
                               get_wizard_printer_identify_step()->get_detection_status());
        break;

    case 4: // Heater Select (combined bed + hotend)
        spdlog::debug("[Wizard] Creating heater select screen");
        get_wizard_heater_select_step()->init_subjects();
        get_wizard_heater_select_step()->register_callbacks();
        get_wizard_heater_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 5: // Fan Select
        spdlog::debug("[Wizard] Creating fan select screen");
        get_wizard_fan_select_step()->init_subjects();
        get_wizard_fan_select_step()->register_callbacks();
        get_wizard_fan_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 6: // LED Select
        spdlog::debug("[Wizard] Creating LED select screen");
        get_wizard_led_select_step()->init_subjects();
        get_wizard_led_select_step()->register_callbacks();
        get_wizard_led_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 7: // Summary
        spdlog::debug("[Wizard] Creating summary screen");
        get_wizard_summary_step()->init_subjects();
        get_wizard_summary_step()->register_callbacks();
        get_wizard_summary_step()->create(content);
        lv_obj_update_layout(content);
        break;

    default:
        spdlog::warn("[Wizard] Invalid step {}, ignoring", step);
        break;
    }

    // Update current screen step tracking
    current_screen_step = step;
}

// ============================================================================
// Wizard Completion
// ============================================================================

void ui_wizard_complete() {
    spdlog::info("[Wizard] Completing wizard and transitioning to main UI");

    // 1. Mark wizard as completed in config
    Config* config = Config::get_instance();
    if (config) {
        spdlog::debug("[Wizard] Setting wizard_completed flag");
        config->set<bool>("/wizard_completed", true);
        if (!config->save()) {
            NOTIFY_ERROR("Failed to save setup completion");
        }
    } else {
        LOG_ERROR_INTERNAL("[Wizard] Failed to get config instance to mark wizard complete");
    }

    // 2. Cleanup current wizard screen
    ui_wizard_cleanup_current_screen();

    // 3. Delete wizard container (main UI is already created underneath)
    if (wizard_container) {
        spdlog::debug("[Wizard] Deleting wizard container");
        lv_obj_del(wizard_container);
        wizard_container = nullptr;
    }

    // 4. Connect to Moonraker using saved configuration
    if (!config) {
        config = Config::get_instance();
    }
    MoonrakerClient* client = get_moonraker_client();

    if (!config || !client) {
        spdlog::error("[Wizard] Failed to get config or moonraker client");
        return;
    }

    std::string moonraker_host = config->get<std::string>(WizardConfigPaths::MOONRAKER_HOST, "");
    int moonraker_port = config->get<int>(WizardConfigPaths::MOONRAKER_PORT, 7125);

    if (moonraker_host.empty()) {
        spdlog::warn("[Wizard] No Moonraker host configured, skipping connection");
        return;
    }

    // Build WebSocket URL
    std::string moonraker_url =
        "ws://" + moonraker_host + ":" + std::to_string(moonraker_port) + "/websocket";

    // Build HTTP base URL for file transfers (same host:port, http:// scheme)
    std::string http_base_url = "http://" + moonraker_host + ":" + std::to_string(moonraker_port);
    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        api->set_http_base_url(http_base_url);
    }

    // Check if already connected to the same URL - skip reconnection to avoid toast
    ConnectionState current_state = client->get_connection_state();
    const std::string& current_url = client->get_last_url();

    if (current_state == ConnectionState::CONNECTED && current_url == moonraker_url) {
        spdlog::info("[Wizard] Already connected to {} - skipping reconnection", moonraker_url);
    } else {
        // Connect to Moonraker
        spdlog::debug("[Wizard] Connecting to Moonraker at {}", moonraker_url);
        int connect_result = client->connect(
            moonraker_url.c_str(),
            []() {
                spdlog::info("✓ Connected to Moonraker");
                // Start auto-discovery (must be called AFTER connection is established)
                MoonrakerClient* client = get_moonraker_client();
                if (client) {
                    client->discover_printer(
                        []() { spdlog::info("✓ Printer auto-discovery complete"); });
                }
            },
            []() { spdlog::warn("✗ Disconnected from Moonraker"); });

        if (connect_result != 0) {
            spdlog::error("[Wizard] Failed to initiate Moonraker connection (code {})",
                          connect_result);
        }
    }

    // 5. Tell Home Panel to reload its config-derived state (printer image, LED visibility)
    get_global_home_panel().reload_from_config();

    spdlog::info("[Wizard] Wizard complete, transitioned to main UI");
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_back_clicked(lv_event_t* e) {
    (void)e;
    int current = lv_subject_get_int(&current_step);
    if (current > 1) {
        ui_wizard_navigate_to_step(current - 1);
    }
    spdlog::debug("[Wizard] Back button clicked, step: {}", current - 1);
}

static void on_next_clicked(lv_event_t* e) {
    (void)e;
    int current = lv_subject_get_int(&current_step);
    int total = lv_subject_get_int(&total_steps);

    if (current < total) {
        ui_wizard_navigate_to_step(current + 1);
        spdlog::debug("[Wizard] Next button clicked, step: {}", current + 1);
    } else {
        spdlog::info("[Wizard] Finish button clicked, completing wizard");
        ui_wizard_complete();
    }
}
