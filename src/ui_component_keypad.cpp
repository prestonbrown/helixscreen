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

#include "ui_component_keypad.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_theme.h"

#include "lvgl/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================================
// State management
// ============================================================================
static lv_obj_t* keypad_widget = nullptr;
static lv_obj_t* input_display_label = nullptr;
static lv_obj_t* title_label = nullptr;
static lv_obj_t* btn_decimal = nullptr;
static lv_obj_t* btn_minus = nullptr;

// Current input state
static char input_buffer[16] = "0"; // Current display string
static float current_value = 0.0f;
static ui_keypad_config_t current_config;

// Forward declarations
static void update_display();
static void append_digit(char digit);
static void handle_backspace();
static void handle_esc();
static void handle_ok();
static void wire_button_events();

// ============================================================================
// Initialization
// ============================================================================
void ui_keypad_init(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("Cannot init keypad: parent is null");
        return;
    }

    if (keypad_widget) {
        spdlog::error("Keypad already initialized");
        return;
    }

    // Create keypad from XML component (initially hidden)
    // Use default config - will be overridden on show()
    const char* attrs[] = {
        "initial_value", "0",     "min_value",      "0",     "max_value", "999", "unit_label", "",
        "allow_decimal", "false", "allow_negative", "false", NULL};

    keypad_widget = (lv_obj_t*)lv_xml_create(parent, "numeric_keypad_modal", attrs);

    if (!keypad_widget) {
        spdlog::error("Failed to create keypad from XML");
        return;
    }

    // Find widget references
    input_display_label = lv_obj_find_by_name(keypad_widget, "input_display");
    title_label = lv_obj_find_by_name(keypad_widget, "keypad_title");

    if (!input_display_label) {
        spdlog::error("Keypad Failed to find input display label");
        return;
    }

    if (!title_label) {
        spdlog::error("Keypad Failed to find title label");
        return;
    }

    // Note: btn_decimal and btn_minus removed from simplified layout
    btn_decimal = nullptr;
    btn_minus = nullptr;

    // Hide initially
    lv_obj_add_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);

    // Wire up button events
    wire_button_events();

    spdlog::debug("Numeric keypad initialized");
}

// ============================================================================
// Show/Hide
// ============================================================================
void ui_keypad_show(const ui_keypad_config_t* config) {
    if (!keypad_widget || !config) {
        spdlog::debug("Cannot show keypad: not initialized or invalid config");
        return;
    }

    // Store config
    current_config = *config;
    current_value = config->initial_value;

    // Format initial value into display
    if (config->allow_decimal) {
        snprintf(input_buffer, sizeof(input_buffer), "%.1f", current_value);
    } else {
        snprintf(input_buffer, sizeof(input_buffer), "%d", (int)current_value);
    }

    // Update title
    if (title_label && config->title_label) {
        lv_label_set_text(title_label, config->title_label);
    }

    // Update display
    update_display();

    // Note: Decimal and minus buttons removed from simplified layout
    // Integer-only input for temperature values

    // Show modal
    lv_obj_remove_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keypad_widget);

    spdlog::info("Keypad shown: initial=%.2f, min=%.2f, max=%.2f", config->initial_value,
                 config->min_value, config->max_value);
}

void ui_keypad_hide() {
    if (keypad_widget) {
        lv_obj_add_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_keypad_is_visible() {
    if (!keypad_widget)
        return false;
    return !lv_obj_has_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Input handling
// ============================================================================
static void update_display() {
    if (input_display_label) {
        lv_label_set_text(input_display_label, input_buffer);
        spdlog::debug("Display updated: '%s'", input_buffer);
    } else {
        spdlog::error("input_display_label is NULL!");
    }
}

static void append_digit(char digit) {
    size_t len = strlen(input_buffer);

    // Replace initial "0" with first digit
    if (len == 1 && input_buffer[0] == '0' && digit != '.') {
        input_buffer[0] = digit;
        input_buffer[1] = '\0';
        update_display();
        return;
    }

    // Max 3 digits (999 max)
    // Count digits only (ignore decimal point and minus sign)
    int digit_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (input_buffer[i] >= '0' && input_buffer[i] <= '9') {
            digit_count++;
        }
    }

    if (digit_count >= 3) {
        return; // Max digits reached
    }

    // Append digit
    if (len < sizeof(input_buffer) - 1) {
        input_buffer[len] = digit;
        input_buffer[len + 1] = '\0';
        update_display();
    }
}

static void handle_backspace() {
    size_t len = strlen(input_buffer);
    if (len > 0) {
        input_buffer[len - 1] = '\0';
    }

    // If empty, reset to "0"
    if (strlen(input_buffer) == 0 || strcmp(input_buffer, "-") == 0) {
        strcpy(input_buffer, "0");
    }

    update_display();
}

static void handle_esc() {
    // Cancel - hide without invoking callback
    ui_keypad_hide();
    spdlog::info("Keypad cancelled");
}

static void handle_ok() {
    // Parse current value
    float value = atof(input_buffer);

    // Clamp to min/max
    if (value < current_config.min_value) {
        value = current_config.min_value;
    }
    if (value > current_config.max_value) {
        value = current_config.max_value;
    }

    // Hide modal
    ui_keypad_hide();

    // Invoke callback
    if (current_config.callback) {
        current_config.callback(value, current_config.user_data);
        spdlog::info("Keypad confirmed: %.2f", value);
    }
}

// ============================================================================
// Event wiring
// ============================================================================
static void wire_button_events() {
    if (!keypad_widget)
        return;

    spdlog::debug("=== WIRING KEYPAD BUTTON EVENTS ===");

    // Number buttons (0-9) - using user_data to pass digit value
    const char* digit_names[] = {"btn_0", "btn_1", "btn_2", "btn_3", "btn_4",
                                 "btn_5", "btn_6", "btn_7", "btn_8", "btn_9"};
    for (int i = 0; i < 10; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(keypad_widget, digit_names[i]);
        if (btn) {
            spdlog::debug("Found button: {}", digit_names[i]);
            // Pass digit as user_data (store as pointer value)
            lv_obj_add_event_cb(
                btn,
                [](lv_event_t* e) {
                    ui_event_safe_call("keypad_digit_clicked", [&]() {
                        // Extract digit from user_data
                        intptr_t digit = (intptr_t)lv_event_get_user_data(e);
                        append_digit('0' + digit);
                    });
                },
                LV_EVENT_CLICKED, (void*)(intptr_t)i);
        } else {
            spdlog::error("BUTTON NOT FOUND: %s", digit_names[i]);
        }
    }

    // Backspace button
    lv_obj_t* btn_back = lv_obj_find_by_name(keypad_widget, "btn_backspace");
    if (btn_back) {
        lv_obj_add_event_cb(
            btn_back,
            [](lv_event_t* e) {
                ui_event_safe_call("keypad_backspace_clicked", [&]() {
                    (void)e;
                    handle_backspace();
                });
            },
            LV_EVENT_CLICKED, nullptr);
        spdlog::debug("Found backspace button");
    } else {
        spdlog::error("Backspace button NOT FOUND");
    }

    // OK button (now in header)
    lv_obj_t* btn_ok = lv_obj_find_by_name(keypad_widget, "btn_ok");
    if (btn_ok) {
        lv_obj_add_event_cb(
            btn_ok,
            [](lv_event_t* e) {
                ui_event_safe_call("keypad_ok_clicked", [&]() {
                    (void)e;
                    handle_ok();
                });
            },
            LV_EVENT_CLICKED, nullptr);
        spdlog::debug("Found OK button");
    } else {
        spdlog::error("OK button NOT FOUND");
    }

    // Back button (cancel like ESC)
    lv_obj_t* back_button = lv_obj_find_by_name(keypad_widget, "back_button");
    if (back_button) {
        lv_obj_add_event_cb(
            back_button,
            [](lv_event_t* e) {
                ui_event_safe_call("keypad_back_clicked", [&]() {
                    (void)e;
                    handle_esc();
                });
            },
            LV_EVENT_CLICKED, nullptr);
        spdlog::debug("Found back button");
    } else {
        spdlog::error("Back button NOT FOUND");
    }

    // Backdrop click to cancel
    lv_obj_add_event_cb(
        keypad_widget,
        [](lv_event_t* e) {
            ui_event_safe_call("keypad_backdrop_clicked", [&]() {
                lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
                lv_obj_t* current_target = (lv_obj_t*)lv_event_get_current_target(e);
                // Only handle if clicking the backdrop itself (not a child)
                if (target == current_target) {
                    handle_esc();
                }
            });
        },
        LV_EVENT_CLICKED, nullptr);
}
