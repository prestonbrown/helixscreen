// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_component_keypad.h
 * @brief Numeric keypad overlay with reactive Subject-Observer pattern
 *
 * Uses standard overlay navigation and reactive bindings. The display
 * is bound to the keypad_display subject in XML.
 *
 * Initialization order:
 *   1. ui_keypad_init_subjects() - before XML creation
 *   2. Register XML components
 *   3. ui_keypad_init(parent) - creates widget, wires events
 */

#pragma once

#include "keypad_input.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>

/**
 * @brief Callback for keypad value confirmation
 * @param value Confirmed value (clamped to min/max)
 * @param user_data User-provided context pointer
 */
typedef void (*ui_keypad_callback_t)(float value, void* user_data);

/**
 * @brief Keypad configuration
 */
struct ui_keypad_config_t {
    float initial_value;           ///< Starting display value
    float min_value;               ///< Minimum allowed value
    float max_value;               ///< Maximum allowed value
    const char* title_label;       ///< Header title (e.g., "Nozzle Temp")
    const char* unit_label;        ///< Unit suffix (e.g., "°C")
    bool allow_decimal;            ///< Allow decimal point input
    bool allow_negative;           ///< Allow negative values
    ui_keypad_callback_t callback; ///< Called on OK confirmation
    void* user_data;               ///< Passed to callback
    /// Digits accepted after the decimal point, 1 to 2; 0 caps only the total digit count.
    int max_fraction_digits = 0;
    /// Called on OK with an in-range value, before `callback`. An empty result accepts
    /// it; anything else is shown as an error and the keypad stays open.
    std::function<std::string(float)> validate;
    /// Called with the accepted value once the keypad has closed, so it may show the
    /// keypad again.
    std::function<void(float)> on_confirm;
    /// Called once when the keypad closes without a confirmed value.
    std::function<void()> on_cancel;
    /// Ipv4 types a dotted quad: the dot key shows, the sign key does not, and the
    /// numeric range, `callback`, `validate` and `on_confirm` are unused.
    helix::ui::KeypadInput::Mode mode = helix::ui::KeypadInput::Mode::Number;
    /// Ipv4 mode: what the display starts with, kept only if it types as an address.
    std::string initial_text;
    /// Ipv4 mode: called with the typed text once the keypad has closed.
    std::function<void(const std::string&)> on_confirm_text;
};

/**
 * @brief Initialize keypad subjects for reactive binding
 *
 * MUST be called BEFORE XML creation so bindings can connect.
 * Safe to call multiple times (idempotent).
 */
void ui_keypad_init_subjects();

/**
 * @brief Deinitialize keypad subjects
 *
 * Disconnects observers before shutdown. Called by StaticPanelRegistry.
 */
void ui_keypad_deinit_subjects();

/**
 * @brief Initialize keypad widget
 *
 * Creates the keypad from XML and wires button events.
 * Call AFTER XML component registration.
 *
 * @param parent Parent widget (usually screen)
 */
void ui_keypad_init(lv_obj_t* parent);

/**
 * @brief Show keypad overlay
 *
 * Uses NavigationManager::push_overlay() for standard overlay behavior.
 *
 * @param config Keypad configuration with initial value and callback
 * @return false when the keypad could not be built or the config is null
 */
bool ui_keypad_show(const ui_keypad_config_t* config);

/**
 * @brief Hide keypad overlay (cancel without callback)
 *
 * Uses NavigationManager::go_back() for standard overlay dismissal.
 */
void ui_keypad_hide();

/**
 * @brief Check if keypad is visible
 * @return true if overlay is showing
 */
bool ui_keypad_is_visible();

/**
 * @brief Get the display subject for external binding
 *
 * Useful if other components need to observe keypad input.
 *
 * @return Pointer to keypad_display subject
 */
lv_subject_t* ui_keypad_get_display_subject();
