// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_component_keypad.cpp
 * @brief Numeric keypad overlay with reactive Subject-Observer pattern
 *
 * Uses standard overlay navigation (NavigationManager push_overlay/go_back) and reactive
 * bindings for the display. The XML binds to the keypad_display subject,
 * so updating the subject automatically updates the UI.
 */

#include "ui_component_keypad.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "keypad_input.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================================
// Reactive State (Subject for XML binding)
// ============================================================================
static lv_subject_t keypad_display_subject;
static char keypad_display_buf[16] = "";
static bool subjects_initialized = false;
static SubjectManager subjects_;

// Widget reference (for showing/hiding via nav system)
static lv_obj_t* keypad_widget = nullptr;
// Parent captured at init; the widget tree itself is built on first show.
static lv_obj_t* keypad_parent = nullptr;

// Current config and input state
static ui_keypad_config_t current_config;
static helix::ui::KeypadInput input;

// ============================================================================
// Forward declarations
// ============================================================================
static void update_display();
static void handle_confirm();
static void wire_button_events();

// ============================================================================
// Subject Initialization (call BEFORE XML creation)
// ============================================================================
void ui_keypad_init_subjects() {
    if (subjects_initialized) {
        return;
    }

    // Initialize display subject for reactive binding (starts empty)
    UI_MANAGED_SUBJECT_STRING(keypad_display_subject, keypad_display_buf, "", "keypad_display",
                              subjects_);

    subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy("KeypadSubjects", []() {
        keypad_widget = nullptr;
        keypad_parent = nullptr;
        ui_keypad_deinit_subjects();
    });

    spdlog::debug("[Keypad] Subjects initialized");
}

void ui_keypad_deinit_subjects() {
    if (!subjects_initialized) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized = false;
    spdlog::debug("[Keypad] Subjects deinitialized");
}

// ============================================================================
// Widget Initialization (call AFTER XML creation)
// ============================================================================
void ui_keypad_init(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[Keypad] Cannot init keypad: parent is null");
        return;
    }

    if (keypad_widget) {
        spdlog::warn("[Keypad] Already initialized");
        return;
    }

    // Subjects MUST stay eager: XML elsewhere binds "keypad_display" by name at
    // parse time, so the subject has to exist before any component referencing
    // it is built. Only the widget tree is deferred.
    ui_keypad_init_subjects();

    keypad_parent = parent;
    spdlog::debug("[Keypad] Numeric keypad registered (tree deferred to first show)");
}

// Build the keypad tree on demand. Plenty of sessions never touch a numeric
// field, and this is a ~15-button subtree whose XML the engine also retains.
static bool ensure_keypad_built() {
    if (keypad_widget) {
        return true;
    }
    if (!keypad_parent) {
        spdlog::error("[Keypad] Cannot build keypad: never initialized with a parent");
        return false;
    }

    keypad_widget = (lv_obj_t*)lv_xml_create(keypad_parent, "numeric_keypad_panel", nullptr);
    if (!keypad_widget) {
        spdlog::error("[Keypad] Failed to create keypad from XML");
        return false;
    }

    wire_button_events();
    spdlog::debug("[Keypad] Numeric keypad built on first use");
    return true;
}

// ============================================================================
// Public API
// ============================================================================
void ui_keypad_show(const ui_keypad_config_t* config) {
    if (!config) {
        spdlog::error("[Keypad] Cannot show keypad: invalid config");
        return;
    }
    if (!ensure_keypad_built()) {
        return;
    }

    // Store config
    current_config = *config;

    // Start with empty display (user enters fresh value)
    input.clear();

    // Update display via subject (reactive binding updates XML automatically)
    update_display();

    // Update unit label (set dynamically since XML prop is only evaluated at creation)
    lv_obj_t* unit_label = lv_obj_find_by_name(keypad_widget, "input_unit");
    if (unit_label) {
        lv_label_set_text(unit_label, config->unit_label ? config->unit_label : "");
    }

    // Register with nullptr lifecycle — keypad is function-based, not class-based
    NavigationManager::instance().register_overlay_instance(keypad_widget, nullptr);

    // Show via overlay navigation, but keep previous panel visible (transparent overlay)
    NavigationManager::instance().push_overlay(keypad_widget, false /* hide_previous */);

    spdlog::info("[Keypad] Showing (initial={:.1f}, range={:.0f}-{:.0f})", config->initial_value,
                 config->min_value, config->max_value);
}

void ui_keypad_hide() {
    if (keypad_widget && ui_keypad_is_visible()) {
        NavigationManager::instance().go_back();
    }
}

bool ui_keypad_is_visible() {
    if (!keypad_widget)
        return false;
    return !lv_obj_has_flag(keypad_widget, LV_OBJ_FLAG_HIDDEN);
}

lv_subject_t* ui_keypad_get_display_subject() {
    return &keypad_display_subject;
}

// ============================================================================
// Input Logic
// ============================================================================
static void update_display() {
    lv_subject_copy_string(&keypad_display_subject, input.buf);
}

static void handle_confirm() {
    float value = input.value();

    // Validate range - show error if out of bounds
    if (value < current_config.min_value || value > current_config.max_value) {
        NOTIFY_ERROR(lv_tr("Value must be between {:.0f} and {:.0f}"), current_config.min_value,
                     current_config.max_value);
        return; // Don't close keypad, let user correct the value
    }

    // Invoke callback before hiding — hiding pops the overlay stack which
    // invalidates lifetime tokens on the parent overlay, so the callback
    // must run while the parent is still active.
    if (current_config.callback) {
        current_config.callback(value, current_config.user_data);
        spdlog::info("[Keypad] Confirmed value={:.1f}", value);
    }

    ui_keypad_hide();
}

// ============================================================================
// Event Wiring
// ============================================================================
static void wire_button_events() {
    if (!keypad_widget)
        return;

    // Number buttons 0-9
    const char* btn_names[] = {"btn_0", "btn_1", "btn_2", "btn_3", "btn_4",
                               "btn_5", "btn_6", "btn_7", "btn_8", "btn_9"};

    for (int i = 0; i < 10; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(keypad_widget, btn_names[i]);
        if (btn) {
            lv_obj_add_event_cb(
                btn,
                [](lv_event_t* e) {
                    helix::ui::event_safe_call("keypad_digit", [e]() {
                        int digit = (int)(intptr_t)lv_event_get_user_data(e);
                        if (input.append_digit(digit))
                            update_display();
                    });
                },
                LV_EVENT_CLICKED, (void*)(intptr_t)i);
        }
    }

    // Dot button
    lv_obj_t* btn_dot = lv_obj_find_by_name(keypad_widget, "btn_dot");
    if (btn_dot) {
        lv_obj_add_event_cb(
            btn_dot,
            [](lv_event_t*) {
                helix::ui::event_safe_call("keypad_dot", []() {
                    if (input.append_dot())
                        update_display();
                });
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // Backspace button
    lv_obj_t* btn_back = lv_obj_find_by_name(keypad_widget, "btn_backspace");
    if (btn_back) {
        lv_obj_add_event_cb(
            btn_back,
            [](lv_event_t*) {
                helix::ui::event_safe_call("keypad_backspace", []() {
                    if (input.backspace())
                        update_display();
                });
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // NOTE: Back button is handled by header_bar's default on_header_back_clicked callback
    // Do NOT add a second handler here - it would cause double navigation!

    // Action button (OK in header_bar) → confirm
    lv_obj_t* ok_btn = lv_obj_find_by_name(keypad_widget, "action_button");
    if (ok_btn) {
        lv_obj_add_event_cb(
            ok_btn,
            [](lv_event_t*) {
                helix::ui::event_safe_call("keypad_confirm", []() { handle_confirm(); });
            },
            LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[Keypad] Events wired");
}
