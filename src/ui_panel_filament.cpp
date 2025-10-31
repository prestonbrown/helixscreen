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

#include "ui_panel_filament.h"
#include "ui_component_keypad.h"
#include "ui_nav.h"
#include "ui_utils.h"
#include "ui_theme.h"
#include <spdlog/spdlog.h>
#include <string.h>

// Temperature subjects (reactive data binding)
static lv_subject_t filament_temp_display_subject;
static lv_subject_t filament_status_subject;
static lv_subject_t filament_material_selected_subject;
static lv_subject_t filament_extrusion_allowed_subject;
static lv_subject_t filament_safety_warning_visible_subject;
static lv_subject_t filament_warning_temps_subject;

// Subject storage buffers
static char temp_display_buf[32];
static char status_buf[64];
static char warning_temps_buf[64];

// Current state
static int nozzle_current = 25;
static int nozzle_target = 0;
static int selected_material = -1;  // -1 = none, 0=PLA, 1=PETG, 2=ABS, 3=Custom
static const int MIN_EXTRUSION_TEMP = 170;

// Material temperature presets
static const int MATERIAL_TEMPS[] = {210, 240, 250, 200};  // PLA, PETG, ABS, Custom default

// Temperature limits (can be updated from Moonraker heater config)
static int nozzle_min_temp = 0;
static int nozzle_max_temp = 350;

// Panel widgets
static lv_obj_t* filament_panel = nullptr;
static lv_obj_t* parent_obj = nullptr;
static lv_obj_t* btn_load = nullptr;
static lv_obj_t* btn_unload = nullptr;
static lv_obj_t* btn_purge = nullptr;
static lv_obj_t* safety_warning = nullptr;
static lv_obj_t* spool_image = nullptr;

// Preset button widgets (for visual feedback)
static lv_obj_t* preset_buttons[4] = {nullptr, nullptr, nullptr, nullptr};

// Subjects initialized flag
static bool subjects_initialized = false;

// Forward declarations
static void update_temp_display();
static void update_status();
static void update_safety_state();
static void update_preset_buttons_visual();

void ui_panel_filament_init_subjects() {
    if (subjects_initialized) {
        spdlog::warn("[Filament] Subjects already initialized");
        return;
    }

    // Initialize subjects with default values
    snprintf(temp_display_buf, sizeof(temp_display_buf), "%d / %d°C", nozzle_current, nozzle_target);
    snprintf(status_buf, sizeof(status_buf), "Select material to begin");
    snprintf(warning_temps_buf, sizeof(warning_temps_buf), "Current: %d°C | Target: %d°C", nozzle_current, nozzle_target);

    lv_subject_init_string(&filament_temp_display_subject, temp_display_buf, nullptr, sizeof(temp_display_buf), temp_display_buf);
    lv_subject_init_string(&filament_status_subject, status_buf, nullptr, sizeof(status_buf), status_buf);
    lv_subject_init_int(&filament_material_selected_subject, -1);
    lv_subject_init_int(&filament_extrusion_allowed_subject, 0);  // false (cold at start)
    lv_subject_init_int(&filament_safety_warning_visible_subject, 1);  // true (cold at start)
    lv_subject_init_string(&filament_warning_temps_subject, warning_temps_buf, nullptr, sizeof(warning_temps_buf), warning_temps_buf);

    // Register subjects with XML system (global scope)
    lv_xml_register_subject(NULL, "filament_temp_display", &filament_temp_display_subject);
    lv_xml_register_subject(NULL, "filament_status", &filament_status_subject);
    lv_xml_register_subject(NULL, "filament_material_selected", &filament_material_selected_subject);
    lv_xml_register_subject(NULL, "filament_extrusion_allowed", &filament_extrusion_allowed_subject);
    lv_xml_register_subject(NULL, "filament_safety_warning_visible", &filament_safety_warning_visible_subject);
    lv_xml_register_subject(NULL, "filament_warning_temps", &filament_warning_temps_subject);

    subjects_initialized = true;

    spdlog::debug("[Filament] Subjects initialized: temp={}/{}°C, material={}",
           nozzle_current, nozzle_target, selected_material);
}

// Update temperature display text
static void update_temp_display() {
    snprintf(temp_display_buf, sizeof(temp_display_buf), "%d / %d°C", nozzle_current, nozzle_target);
    lv_subject_copy_string(&filament_temp_display_subject, temp_display_buf);
}

// Update status message
static void update_status() {
    const char* status_msg;

    if (nozzle_current >= MIN_EXTRUSION_TEMP) {
        // Hot enough
        if (nozzle_target > 0 && nozzle_current >= nozzle_target - 5 && nozzle_current <= nozzle_target + 5) {
            status_msg = "✓ Ready to load";
        } else {
            status_msg = "✓ Ready to load";
        }
    } else if (nozzle_target >= MIN_EXTRUSION_TEMP) {
        // Heating
        char heating_buf[64];
        snprintf(heating_buf, sizeof(heating_buf), "⚡ Heating to %d°C...", nozzle_target);
        status_msg = heating_buf;
    } else {
        // Cold
        status_msg = "❄ Select material to begin";
    }

    lv_subject_copy_string(&filament_status_subject, status_msg);
}

// Update warning card text
static void update_warning_text() {
    snprintf(warning_temps_buf, sizeof(warning_temps_buf), "Current: %d°C | Target: %d°C",
             nozzle_current, nozzle_target);
    lv_subject_copy_string(&filament_warning_temps_subject, warning_temps_buf);
}

// Update safety state (button enable/disable, warning visibility)
static void update_safety_state() {
    bool allowed = (nozzle_current >= MIN_EXTRUSION_TEMP);

    lv_subject_set_int(&filament_extrusion_allowed_subject, allowed ? 1 : 0);
    lv_subject_set_int(&filament_safety_warning_visible_subject, allowed ? 0 : 1);

    // Update button states (theme handles colors)
    if (btn_load) {
        if (allowed) {
            lv_obj_remove_state(btn_load, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(btn_load, LV_STATE_DISABLED);
        }
    }

    if (btn_unload) {
        if (allowed) {
            lv_obj_remove_state(btn_unload, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(btn_unload, LV_STATE_DISABLED);
        }
    }

    if (btn_purge) {
        if (allowed) {
            lv_obj_remove_state(btn_purge, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(btn_purge, LV_STATE_DISABLED);
        }
    }

    // Show/hide safety warning
    if (safety_warning) {
        if (allowed) {
            lv_obj_add_flag(safety_warning, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(safety_warning, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::debug("[Filament] Safety state updated: allowed={} (temp={}°C)", allowed, nozzle_current);
}

// Update visual feedback for preset buttons
static void update_preset_buttons_visual() {
    for (int i = 0; i < 4; i++) {
        if (preset_buttons[i]) {
            if (i == selected_material) {
                // Selected state - theme handles colors
                lv_obj_add_state(preset_buttons[i], LV_STATE_CHECKED);
            } else {
                // Unselected state - theme handles colors
                lv_obj_remove_state(preset_buttons[i], LV_STATE_CHECKED);
            }
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

// Event handler: Material preset buttons
static void preset_button_cb(lv_event_t* e) {
    (void)lv_event_get_target(e);  // Unused - we only need user_data
    int material_id = (int)(uintptr_t)lv_event_get_user_data(e);

    selected_material = material_id;
    nozzle_target = MATERIAL_TEMPS[material_id];

    lv_subject_set_int(&filament_material_selected_subject, selected_material);
    update_preset_buttons_visual();
    update_temp_display();
    update_status();

    spdlog::info("[Filament] Material selected: {} (target={}°C)", material_id, nozzle_target);

    // TODO: Send command to printer to set temperature
}

// Custom temperature keypad callback
static void custom_temp_confirmed_cb(float value, void* user_data) {
    (void)user_data;

    spdlog::info("[Filament] Custom temperature confirmed: {}°C", static_cast<int>(value));

    selected_material = 3;  // Custom
    nozzle_target = (int)value;

    lv_subject_set_int(&filament_material_selected_subject, selected_material);
    update_preset_buttons_visual();
    update_temp_display();
    update_status();

    // TODO: Send command to printer to set temperature
}

// Event handler: Custom preset button (opens keypad)
static void preset_custom_button_cb(lv_event_t* e) {
    (void)e;

    spdlog::debug("[Filament] Opening custom temperature keypad");

    ui_keypad_config_t config = {
        .initial_value = (float)(nozzle_target > 0 ? nozzle_target : 200),
        .min_value = 0.0f,
        .max_value = (float)nozzle_max_temp,
        .title_label = "Custom Temperature",
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = custom_temp_confirmed_cb,
        .user_data = nullptr
    };

    ui_keypad_show(&config);
}

// Event handler: Load filament button
static void load_button_cb(lv_event_t* e) {
    (void)e;

    if (nozzle_current < MIN_EXTRUSION_TEMP) {
        spdlog::warn("[Filament] Load blocked: nozzle too cold ({}°C < {}°C)",
                     nozzle_current, MIN_EXTRUSION_TEMP);
        return;
    }

    spdlog::info("[Filament] Loading filament");
    // TODO: Send LOAD_FILAMENT macro to printer
}

// Event handler: Unload filament button
static void unload_button_cb(lv_event_t* e) {
    (void)e;

    if (nozzle_current < MIN_EXTRUSION_TEMP) {
        spdlog::warn("[Filament] Unload blocked: nozzle too cold ({}°C < {}°C)",
                     nozzle_current, MIN_EXTRUSION_TEMP);
        return;
    }

    spdlog::info("[Filament] Unloading filament");
    // TODO: Send UNLOAD_FILAMENT macro to printer
}

// Event handler: Purge button
static void purge_button_cb(lv_event_t* e) {
    (void)e;

    if (nozzle_current < MIN_EXTRUSION_TEMP) {
        spdlog::warn("[Filament] Purge blocked: nozzle too cold ({}°C < {}°C)",
                     nozzle_current, MIN_EXTRUSION_TEMP);
        return;
    }

    spdlog::info("[Filament] Purging 10mm");
    // TODO: Send extrude command to printer (M83 \n G1 E10 F300)
}

// ============================================================================
// PUBLIC API
// ============================================================================

lv_obj_t* ui_panel_filament_create(lv_obj_t* parent) {
    if (!subjects_initialized) {
        spdlog::error("[Filament] Call ui_panel_filament_init_subjects() first!");
        return nullptr;
    }

    filament_panel = (lv_obj_t*)lv_xml_create(parent, "filament_panel", nullptr);
    if (!filament_panel) {
        spdlog::error("[Filament] Failed to create filament_panel from XML");
        return nullptr;
    }

    spdlog::debug("[Filament] Panel created from XML");
    return filament_panel;
}

void ui_panel_filament_setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    filament_panel = panel;
    parent_obj = parent_screen;

    spdlog::debug("[Filament] Setting up panel event handlers");

    // Find and setup preset buttons
    const char* preset_names[] = {"preset_pla", "preset_petg", "preset_abs", "preset_custom"};
    for (int i = 0; i < 4; i++) {
        preset_buttons[i] = lv_obj_find_by_name(panel, preset_names[i]);
        if (preset_buttons[i]) {
            if (i < 3) {
                // Standard presets (PLA, PETG, ABS)
                lv_obj_add_event_cb(preset_buttons[i], preset_button_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
            } else {
                // Custom preset (opens keypad)
                lv_obj_add_event_cb(preset_buttons[i], preset_custom_button_cb, LV_EVENT_CLICKED, nullptr);
            }
        }
    }
    spdlog::debug("[Filament] Preset buttons configured (4)");

    // Find and setup action buttons
    btn_load = lv_obj_find_by_name(panel, "btn_load");
    if (btn_load) {
        lv_obj_add_event_cb(btn_load, load_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Filament] Load button configured");
    }

    btn_unload = lv_obj_find_by_name(panel, "btn_unload");
    if (btn_unload) {
        lv_obj_add_event_cb(btn_unload, unload_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Filament] Unload button configured");
    }

    btn_purge = lv_obj_find_by_name(panel, "btn_purge");
    if (btn_purge) {
        lv_obj_add_event_cb(btn_purge, purge_button_cb, LV_EVENT_CLICKED, nullptr);
        spdlog::debug("[Filament] Purge button configured");
    }

    // Find safety warning card
    safety_warning = lv_obj_find_by_name(panel, "safety_warning");

    // Find spool image widget
    spool_image = lv_obj_find_by_name(panel, "spool_image");

    // Initialize visual state
    update_preset_buttons_visual();
    update_temp_display();
    update_status();
    update_warning_text();
    update_safety_state();

    spdlog::info("[Filament] Panel setup complete");
}

void ui_panel_filament_set_temp(int current, int target) {
    // Validate temperature ranges
    if (current < nozzle_min_temp || current > nozzle_max_temp) {
        spdlog::warn("[Filament] Invalid current temperature {}°C (valid: {}-{}°C), clamping",
                     current, nozzle_min_temp, nozzle_max_temp);
        current = (current < nozzle_min_temp) ? nozzle_min_temp : nozzle_max_temp;
    }
    if (target < nozzle_min_temp || target > nozzle_max_temp) {
        spdlog::warn("[Filament] Invalid target temperature {}°C (valid: {}-{}°C), clamping",
                     target, nozzle_min_temp, nozzle_max_temp);
        target = (target < nozzle_min_temp) ? nozzle_min_temp : nozzle_max_temp;
    }

    nozzle_current = current;
    nozzle_target = target;

    update_temp_display();
    update_status();
    update_warning_text();
    update_safety_state();
}

void ui_panel_filament_get_temp(int* current, int* target) {
    if (current) *current = nozzle_current;
    if (target) *target = nozzle_target;
}

void ui_panel_filament_set_material(int material_id) {
    if (material_id < 0 || material_id > 3) {
        spdlog::error("[Filament] Invalid material ID {} (valid: 0-3)", material_id);
        return;
    }

    selected_material = material_id;
    nozzle_target = MATERIAL_TEMPS[material_id];

    lv_subject_set_int(&filament_material_selected_subject, selected_material);
    update_preset_buttons_visual();
    update_temp_display();
    update_status();

    spdlog::info("[Filament] Material set: {} (target={}°C)", material_id, nozzle_target);
}

int ui_panel_filament_get_material() {
    return selected_material;
}

bool ui_panel_filament_is_extrusion_allowed() {
    return (nozzle_current >= MIN_EXTRUSION_TEMP);
}

void ui_panel_filament_set_limits(int min_temp, int max_temp) {
    nozzle_min_temp = min_temp;
    nozzle_max_temp = max_temp;
    spdlog::info("[Filament] Nozzle temperature limits updated: {}-{}°C", min_temp, max_temp);
}
