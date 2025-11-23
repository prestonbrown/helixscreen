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

#include "ui_keyboard.h"

#include "keyboard_layout_provider.h"
#include "ui_event_safety.h"
#include "ui_theme.h"

#include "config.h"

#include <spdlog/spdlog.h>

#include <cstring>

// Macro for keyboard button control flags
#define LV_KB_BTN(width) static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | (width))

// Keyboard mode enum
enum KeyboardMode {
    MODE_ALPHA_LC,        // Lowercase alphabet
    MODE_ALPHA_UC,        // Uppercase alphabet
    MODE_NUMBERS_SYMBOLS, // Numbers and symbols (?123)
    MODE_ALT_SYMBOLS      // Alternative symbols (#+= mode)
};

// Global keyboard instance
static lv_obj_t* g_keyboard = NULL;
static lv_obj_t* g_context_textarea = NULL; // Currently focused textarea

// Keyboard state
static KeyboardMode g_mode = MODE_ALPHA_LC;

//=============================================================================
// LONG-PRESS ALTERNATIVE CHARACTER SYSTEM
//=============================================================================

// Alternative character mapping (iOS-style)
// Maps base character to alternative character(s)
struct AltCharMapping {
    char base_char;
    const char* alternatives; // NULL-terminated string of alternatives
};

// Alternative character mapping table (Gboard-style layout)
// Numbers appear as long-press alternatives on top row letters
static const AltCharMapping g_alt_char_map[] = {
    // Top row (Q-P) -> numbers 1-0
    {'Q', "1"},
    {'q', "1"},
    {'W', "2"},
    {'w', "2"},
    {'E', "3"},
    {'e', "3"},
    {'R', "4"},
    {'r', "4"},
    {'T', "5"},
    {'t', "5"},
    {'Y', "6"},
    {'y', "6"},
    {'U', "7"},
    {'u', "7"},
    {'I', "8"},
    {'i', "8"},
    {'O', "9"},
    {'o', "9"},
    {'P', "0"},
    {'p', "0"},
    // Middle row (A-L) -> symbols (from screenshot: a@, s#, d$, f_, g&, h-, j+, k(, l))
    {'A', "@"},
    {'a', "@"},
    {'S', "#"},
    {'s', "#"},
    {'D', "$"},
    {'d', "$"},
    {'F', "_"},
    {'f', "_"},
    {'G', "&"},
    {'g', "&"},
    {'H', "-"},
    {'h', "-"},
    {'J', "+"},
    {'j', "+"},
    {'K', "("},
    {'k', "("},
    {'L', ")"},
    {'l', ")"},
    // Bottom row (Z-M) -> symbols (from screenshot: z*, x", c', v:, b;, n!, m?)
    {'Z', "*"},
    {'z', "*"},
    {'X', "\""},
    {'x', "\""},
    {'C', "'"},
    {'c', "'"},
    {'V', ":"},
    {'v', ":"},
    {'B', ";"},
    {'b', ";"},
    {'N', "!"},
    {'n', "!"},
    {'M', "?"},
    {'m', "?"},
    {0, NULL} // Sentinel
};

// Long-press state machine
enum LongPressState { LP_IDLE, LP_PRESSED, LP_LONG_DETECTED, LP_ALT_SELECTED };

// Long-press state tracking
static LongPressState g_longpress_state = LP_IDLE;
static lv_obj_t* g_overlay = NULL;        // Alternative character overlay widget
static uint32_t g_pressed_btn_id = 0;     // Currently pressed button ID
static const char* g_pressed_char = NULL; // Currently pressed character
static const char* g_alternatives = NULL; // Alternative chars for pressed key
static lv_point_t g_press_point;          // Initial press coordinates

// Shift key behavior tracking (iOS-style)
static bool g_shift_just_pressed =
    false; // True if shift was just pressed (for detecting consecutive press)
static bool g_one_shot_shift = false; // Single-press: one uppercase letter then revert
static bool g_caps_lock = false;      // Two consecutive presses: stay uppercase

// Forward declarations for helper functions (not event callbacks)
static void overlay_cleanup();
static void apply_keyboard_mode();

//=============================================================================
// NOTE: Keyboard layout data has been extracted to keyboard_layout_provider.cpp
// See keyboard_layout_provider.h for API
//=============================================================================

// Improved numeric keyboard with PERIOD (critical for IPs and decimals)
static const char* const kb_map_num_improved[] = {
    "1", "2", "3", LV_SYMBOL_KEYBOARD,  "\n", "4",   "5", "6", LV_SYMBOL_OK,   "\n",
    "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n", "+/-", "0", ".", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT,
    ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_num_improved[] = {
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    static_cast<lv_buttonmatrix_ctrl_t>(2),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    LV_KB_BTN(1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED | 1)};

/**
 * @brief Textarea focus event callback - handles auto show/hide
 */
static void textarea_focus_event_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("textarea_focus_event_cb");

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* textarea = lv_event_get_target_obj(e);

    if (code == LV_EVENT_FOCUSED) {
        spdlog::debug("[Keyboard] Textarea focused: {}", (void*)textarea);
        g_context_textarea = textarea;
        ui_keyboard_show(textarea);
    } else if (code == LV_EVENT_DEFOCUSED) {
        spdlog::debug("[Keyboard] Textarea defocused: {}", (void*)textarea);
        g_context_textarea = NULL;
        ui_keyboard_hide();
    }

    LVGL_SAFE_EVENT_CB_END();
}

/**
 * @brief Check if a point is within an area
 * @param area Area to check
 * @param point Point to test
 * @return true if point is inside area, false otherwise
 */
static bool point_in_area(const lv_area_t* area, const lv_point_t* point) {
    return (point->x >= area->x1 && point->x <= area->x2 && point->y >= area->y1 &&
            point->y <= area->y2);
}

/**
 * @brief Find alternative characters for a given base character
 * @param base_char Character to look up (e.g., 'a', 'Q', '1')
 * @return Alternative characters string, or NULL if no alternatives exist
 */
static const char* find_alternatives(char base_char) {
    for (size_t i = 0; g_alt_char_map[i].alternatives != NULL; i++) {
        if (g_alt_char_map[i].base_char == base_char) {
            return g_alt_char_map[i].alternatives;
        }
    }
    return NULL;
}

/**
 * @brief Clean up overlay widget and reset state
 */
static void overlay_cleanup() {
    if (g_overlay != NULL) {
        lv_obj_delete(g_overlay);
        g_overlay = NULL;
    }
    g_alternatives = NULL;
    g_pressed_char = NULL;
    g_pressed_btn_id = 0;
}

/**
 * @brief Create and show the alternative character overlay
 * @param key_area Coordinates of the pressed key button
 * @param alternatives String of alternative characters to display
 */
static void show_overlay(const lv_area_t* key_area, const char* alternatives) {
    if (!alternatives || !alternatives[0]) {
        spdlog::debug("[LongPress] No alternatives to display");
        return;
    }

    // Clean up any existing overlay
    overlay_cleanup();

    // Create overlay container (positioned above the key)
    g_overlay = lv_obj_create(lv_screen_active());

    // Calculate overlay size based on number of alternatives
    size_t alt_count = strlen(alternatives);
    const int32_t char_width = 50;  // Width per character
    const int32_t char_height = 60; // Height of overlay
    const int32_t padding = 8;
    int32_t overlay_width = (alt_count * char_width) + (padding * 2);
    int32_t overlay_height = char_height;

    lv_obj_set_size(g_overlay, overlay_width, overlay_height);

    // Style the overlay using theme colors
    const char* card_bg_str =
        lv_xml_get_const(NULL, ui_theme_is_dark_mode() ? "card_bg_dark" : "card_bg_light");
    if (card_bg_str) {
        lv_obj_set_style_bg_color(g_overlay, ui_theme_parse_color(card_bg_str), LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 2, LV_PART_MAIN);

    const char* border_color_str = lv_xml_get_const(NULL, "secondary_color");
    if (border_color_str) {
        lv_obj_set_style_border_color(g_overlay, ui_theme_parse_color(border_color_str),
                                      LV_PART_MAIN);
    }

    lv_obj_set_style_radius(g_overlay, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(g_overlay, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(g_overlay, LV_OPA_30, LV_PART_MAIN);

    // Use flexbox for laying out alternative characters
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(g_overlay, LV_FLEX_ALIGN_SPACE_EVENLY, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(g_overlay, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, padding, LV_PART_MAIN);

    // Create labels for each alternative character
    const char* text_color_str = lv_xml_get_const(
        NULL, ui_theme_is_dark_mode() ? "text_primary_dark" : "text_primary_light");
    lv_color_t text_color =
        text_color_str ? ui_theme_parse_color(text_color_str) : lv_color_hex(0x000000);

    for (size_t i = 0; i < alt_count; i++) {
        lv_obj_t* label = lv_label_create(g_overlay);
        char char_str[2] = {alternatives[i], '\0'};
        lv_label_set_text(label, char_str);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);

        // Store the character in user_data for hit detection
        lv_obj_set_user_data(label, (void*)(uintptr_t)alternatives[i]);
    }

    // Position overlay above the pressed key (centered horizontally)
    int32_t key_center_x = (key_area->x1 + key_area->x2) / 2;
    int32_t overlay_x = key_center_x - (overlay_width / 2);
    int32_t overlay_y = key_area->y1 - overlay_height - 10; // 10px gap above key

    // Handle screen edge cases
    lv_obj_t* screen = lv_screen_active();
    int32_t screen_width = lv_obj_get_width(screen);

    // Clamp X position to screen bounds
    if (overlay_x < 0) {
        overlay_x = 0;
    } else if (overlay_x + overlay_width > screen_width) {
        overlay_x = screen_width - overlay_width;
    }

    // If overlay would go off top of screen, show below key instead
    if (overlay_y < 0) {
        overlay_y = key_area->y2 + 10;
    }

    lv_obj_set_pos(g_overlay, overlay_x, overlay_y);

    // Move overlay to foreground to ensure it appears above everything (keyboard, modals, etc.)
    lv_obj_move_foreground(g_overlay);

    spdlog::info("[LongPress] Showing overlay with {} alternatives at ({}, {})", alt_count,
                 overlay_x, overlay_y);
}

/**
 * @brief Long-press event handler for keyboard
 * Intercepts PRESSED, LONG_PRESSED, and RELEASED events to manage overlay
 */
static void longpress_event_handler(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("longpress_event_handler");

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* keyboard = lv_event_get_target_obj(e);

    if (code == LV_EVENT_PRESSED) {
        // Get pressed button info
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(keyboard);
        const char* btn_text = lv_buttonmatrix_get_button_text(keyboard, btn_id);

        // Don't process longpress for mode switch buttons
        if (btn_text && (strcmp(btn_text, "XYZ") == 0 || strcmp(btn_text, "?123") == 0)) {
            return;
        }

        // Check if this is a non-printing button
        bool is_non_printing =
            lv_buttonmatrix_has_button_ctrl(keyboard, btn_id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);

        if (is_non_printing) {
            return; // Don't process longpress for non-printing buttons
        }

        // Track initial press (only for printing characters)
        g_longpress_state = LP_PRESSED;

        g_pressed_btn_id = btn_id;
        g_pressed_char = btn_text;

        // Get press coordinates
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &g_press_point);
        }

        // Look up alternatives for this character (single char only)
        if (btn_text && btn_text[0] && !btn_text[1]) {
            g_alternatives = find_alternatives(btn_text[0]);
            if (g_alternatives) {
                spdlog::debug("[LongPress] PRESSED '{}' - has alternatives: '{}'", btn_text[0],
                              g_alternatives);
            }
        }

    } else if (code == LV_EVENT_LONG_PRESSED) {
        // Long press detected - show overlay if alternatives exist
        if (g_longpress_state == LP_PRESSED && g_alternatives != NULL) {
            g_longpress_state = LP_LONG_DETECTED;

            // Get keyboard coordinates (use press point to approximate button location)
            lv_area_t kb_area;
            lv_obj_get_coords(keyboard, &kb_area);

            // Create approximate button area based on press point
            // For overlay positioning, we just need rough location above the key
            lv_area_t btn_area;
            btn_area.x1 = g_press_point.x - 25; // Approximate button bounds
            btn_area.x2 = g_press_point.x + 25;
            btn_area.y1 = g_press_point.y - 25;
            btn_area.y2 = g_press_point.y + 25;

            // Show overlay
            show_overlay(&btn_area, g_alternatives);

            spdlog::info("[LongPress] LONG_PRESSED detected for '{}' - overlay shown",
                         g_pressed_char ? g_pressed_char : "?");
        }

    } else if (code == LV_EVENT_RELEASED) {
        // Handle release
        if (g_longpress_state == LP_LONG_DETECTED && g_overlay != NULL) {
            // Long-press was active - check if user selected an alternative
            lv_indev_t* indev = lv_indev_active();
            lv_point_t release_point;

            if (indev) {
                lv_indev_get_point(indev, &release_point);

                // Hit test against overlay labels
                uint32_t child_count = lv_obj_get_child_count(g_overlay);
                char selected_char = 0;

                for (uint32_t i = 0; i < child_count; i++) {
                    lv_obj_t* label = lv_obj_get_child(g_overlay, i);
                    lv_area_t label_area;
                    lv_obj_get_coords(label, &label_area);

                    if (point_in_area(&label_area, &release_point)) {
                        // User released on this alternative character
                        selected_char = (char)(uintptr_t)lv_obj_get_user_data(label);
                        break;
                    }
                }

                if (selected_char != 0 && g_context_textarea != NULL) {
                    // Insert the alternative character
                    char str[2] = {selected_char, '\0'};
                    lv_textarea_add_text(g_context_textarea, str);
                    spdlog::info("[LongPress] Inserted alternative character: '{}'", selected_char);
                } else {
                    spdlog::debug("[LongPress] Released outside overlay - no character inserted");
                }
            }

            // Clean up overlay
            overlay_cleanup();
            g_longpress_state = LP_IDLE;

        } else if (g_longpress_state == LP_PRESSED) {
            // Normal short press - let LVGL's keyboard handle it normally
            spdlog::debug("[LongPress] Short press - normal input");
            g_longpress_state = LP_IDLE;
            overlay_cleanup();
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

/**
 * @brief Apply keyboard mode to the keyboard widget
 * Updates the keyboard map based on current mode
 */
static void apply_keyboard_mode() {
    if (g_keyboard == NULL) {
        return;
    }

    spdlog::trace("[Keyboard] apply_keyboard_mode() called, mode={}", (int)g_mode);

    // Map internal mode to layout provider mode
    keyboard_layout_mode_t layout_mode;
    const char* mode_name = "";

    switch (g_mode) {
    case MODE_ALPHA_LC:
        layout_mode = KEYBOARD_LAYOUT_ALPHA_LC;
        mode_name = "alpha lowercase";
        break;
    case MODE_ALPHA_UC:
        layout_mode = KEYBOARD_LAYOUT_ALPHA_UC;
        mode_name = g_caps_lock ? "alpha uppercase (CAPS LOCK)" : "alpha uppercase (one-shot)";
        break;
    case MODE_NUMBERS_SYMBOLS:
        layout_mode = KEYBOARD_LAYOUT_NUMBERS_SYMBOLS;
        mode_name = "numbers/symbols";
        break;
    case MODE_ALT_SYMBOLS:
        layout_mode = KEYBOARD_LAYOUT_ALT_SYMBOLS;
        mode_name = "alternative symbols (#+= mode)";
        break;
    default:
        layout_mode = KEYBOARD_LAYOUT_ALPHA_LC;
        mode_name = "alpha lowercase (fallback)";
        break;
    }

    // Get layout and control maps from provider
    const char* const* map = keyboard_layout_get_map(layout_mode, g_caps_lock);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(layout_mode);

    // Apply maps to buttonmatrix
    lv_buttonmatrix_set_map(g_keyboard, map);
    lv_buttonmatrix_set_ctrl_map(g_keyboard, ctrl_map);

    spdlog::debug("[Keyboard] Switched to {}", mode_name);

    // Force keyboard to redraw with new layout
    lv_obj_invalidate(g_keyboard);
}

/**
 * @brief Keyboard event callback - handles ready/cancel events and mode switching
 *
 * CRITICAL: Non-printing buttons (mode buttons, shift, backspace, enter, spacebar) are marked
 * with LV_BUTTONMATRIX_CTRL_CUSTOM_1 flag. We stop the event to prevent LVGL's default text
 * insertion.
 */
static void keyboard_event_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("keyboard_event_cb");

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* keyboard = lv_event_get_target_obj(e);

    if (code == LV_EVENT_READY) {
        spdlog::info("[Keyboard] OK pressed - input confirmed");
        ui_keyboard_hide();
    } else if (code == LV_EVENT_CANCEL) {
        spdlog::info("[Keyboard] Cancel pressed");
        ui_keyboard_hide();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        // Get button info
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(keyboard);
        const char* btn_text = lv_buttonmatrix_get_button_text(keyboard, btn_id);

        // Check if this is a non-printing button
        bool is_non_printing =
            lv_buttonmatrix_has_button_ctrl(keyboard, btn_id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);

        spdlog::trace("[Keyboard] VALUE_CHANGED: btn_id={}, btn_text='{}', is_non_printing={}",
                      btn_id, btn_text ? btn_text : "NULL", is_non_printing);

        if (is_non_printing) {
            // LVGL's default handler has already inserted the button text
            // We need to delete it and handle the button action instead
            if (g_context_textarea && btn_text) {
                // Delete the characters that LVGL just inserted
                size_t len = strlen(btn_text);
                spdlog::info("[Keyboard] Deleting {} bytes of '{}' from textarea", len, btn_text);
                for (size_t i = 0; i < len; i++) {
                    lv_textarea_delete_char(g_context_textarea);
                }
                spdlog::info("[Keyboard] Deleted {} chars, textarea now: '{}'", len,
                             lv_textarea_get_text(g_context_textarea));
            }

            // Now handle the button action
            if (btn_text && strcmp(btn_text, "?123") == 0) {
                g_mode = MODE_NUMBERS_SYMBOLS;
                g_shift_just_pressed = false;
                g_one_shot_shift = false;
                g_caps_lock = false;
                apply_keyboard_mode();
                spdlog::debug("[Keyboard] Mode switch: ?123 -> numbers/symbols");

            } else if (btn_text && strcmp(btn_text, "XYZ") == 0) {
                g_mode = MODE_ALPHA_LC;
                g_shift_just_pressed = false;
                g_one_shot_shift = false;
                g_caps_lock = false;
                apply_keyboard_mode();
                spdlog::debug("[Keyboard] Mode switch: AB -> alpha lowercase");

            } else if (btn_text && strcmp(btn_text, "#+=") == 0) {
                g_mode = MODE_ALT_SYMBOLS;
                apply_keyboard_mode();
                spdlog::debug("[Keyboard] Mode switch: #+= -> alternative symbols");

            } else if (btn_text && strcmp(btn_text, "123") == 0) {
                g_mode = MODE_NUMBERS_SYMBOLS;
                apply_keyboard_mode();
                spdlog::debug("[Keyboard] Mode switch: 123 -> numbers/symbols");

            } else if (btn_text && (strcmp(btn_text, LV_SYMBOL_UP) == 0 ||
                                    strcmp(btn_text, LV_SYMBOL_EJECT) == 0 ||
                                    strcmp(btn_text, LV_SYMBOL_UPLOAD) == 0)) {
                // Shift key pressed
                if (g_shift_just_pressed && !g_caps_lock) {
                    g_caps_lock = true;
                    g_one_shot_shift = false;
                    g_shift_just_pressed = false;
                    g_mode = MODE_ALPHA_UC;
                    spdlog::debug("[Keyboard] Shift: Caps Lock ON");
                } else if (g_caps_lock) {
                    g_caps_lock = false;
                    g_one_shot_shift = false;
                    g_shift_just_pressed = false;
                    g_mode = MODE_ALPHA_LC;
                    spdlog::debug("[Keyboard] Shift: Caps Lock OFF -> lowercase");
                } else {
                    g_one_shot_shift = true;
                    g_shift_just_pressed = true;
                    g_caps_lock = false;
                    g_mode = MODE_ALPHA_UC;
                    spdlog::debug("[Keyboard] Shift: One-shot uppercase");
                }
                apply_keyboard_mode();

            } else if (btn_text && strcmp(btn_text, LV_SYMBOL_NEW_LINE) == 0) {
                // Enter key - LVGL already handled it, just close keyboard
                ui_keyboard_hide();

            } else if (btn_text && strcmp(btn_text, LV_SYMBOL_KEYBOARD) == 0) {
                // Close keyboard button
                spdlog::debug("[Keyboard] Close button pressed");
                ui_keyboard_hide();

            } else if (btn_text && strcmp(btn_text, LV_SYMBOL_BACKSPACE) == 0) {
                // Backspace - delete one more character (LVGL inserted the symbol, we deleted it,
                // now delete user char)
                if (g_context_textarea) {
                    lv_textarea_delete_char(g_context_textarea);
                }
                spdlog::debug("[Keyboard] Backspace");
            }
        } else {
            // Regular printing key - LVGL has already inserted the text
            // Special case: convert double-space to actual single space character
            if (btn_text && strcmp(btn_text, keyboard_layout_get_spacebar_text()) == 0 && g_context_textarea) {
                // Delete the double-space that LVGL inserted (2 characters)
                lv_textarea_delete_char(g_context_textarea);
                lv_textarea_delete_char(g_context_textarea);
                // Insert actual single space character
                lv_textarea_add_char(g_context_textarea, ' ');
                spdlog::debug("[Keyboard] Converted double-space to single space");
            }

            // Reset shift consecutive press flag
            g_shift_just_pressed = false;

            // If one-shot shift is active, revert to lowercase
            if (g_one_shot_shift && g_mode == MODE_ALPHA_UC) {
                g_one_shot_shift = false;
                g_mode = MODE_ALPHA_LC;
                apply_keyboard_mode();
                spdlog::debug("[Keyboard] One-shot shift: Reverting to lowercase after character");
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

/**
 * @brief Custom draw event handler to display alternative characters on keys
 * Draws small gray text in upper-right corner showing long-press alternatives
 * Uses LV_EVENT_DRAW_POST_END to render after main button drawing
 */
static void keyboard_draw_alternative_chars(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("keyboard_draw_alternative_chars");

    lv_obj_t* keyboard = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    // Get keyboard map to iterate through buttons
    const char* const* map = lv_buttonmatrix_get_map(keyboard);
    if (!map)
        return;

    // Get theme-appropriate gray color for alternative text
    const char* gray_color_str = lv_xml_get_const(
        NULL, ui_theme_is_dark_mode() ? "text_secondary_dark" : "text_secondary_light");
    lv_color_t gray_color =
        gray_color_str ? ui_theme_parse_color(gray_color_str) : lv_color_hex(0x888888);

    // Iterate through all buttons and draw alternative characters
    for (uint32_t i = 0; map[i][0] != '\0'; i++) {
        // Skip newline markers
        if (strcmp(map[i], "\n") == 0) {
            continue;
        }

        const char* btn_text = map[i];

        // Only process single-character ASCII buttons (skip multi-byte UTF-8 symbols, multi-char
        // buttons) Check: first byte exists, second byte is null (single char), and first byte is
        // ASCII (< 128)
        if (btn_text && btn_text[0] && !btn_text[1] && (unsigned char)btn_text[0] < 128) {
            // Look up alternatives for this character
            const char* alternatives = find_alternatives(btn_text[0]);

            if (alternatives && alternatives[0]) {
                // Get button coordinates
                lv_area_t kb_coords;
                lv_obj_get_coords(keyboard, &kb_coords);

                // Calculate button position by summing up actual button widths
                // Total width units: 40 (10 buttons × 4 units each on row 1)
                lv_coord_t kb_width = lv_obj_get_width(keyboard);
                lv_coord_t kb_height = lv_obj_get_height(keyboard);
                lv_coord_t unit_width = kb_width / 40; // Each unit is 1/40th of keyboard width
                lv_coord_t row_height = kb_height / 4; // 4 rows total

                // Count which row and calculate cumulative width to this button
                uint32_t row = 0;
                lv_coord_t cumulative_width = 0;

                for (uint32_t j = 0; j <= i; j++) {
                    if (strcmp(map[j], "\n") == 0) {
                        row++;
                        cumulative_width = 0;
                    } else if (j < i) {
                        // Add width of this button to cumulative
                        // We need to determine the button width from the control array
                        // For now, approximate: spacers=2, shift/backspace=6, regular=4
                        const char* this_text = map[j];
                        lv_coord_t this_width;

                        if (strcmp(this_text, " ") == 0) {
                            this_width = 2 * unit_width; // Spacer
                        } else if (strcmp(this_text, LV_SYMBOL_UP) == 0 ||
                                   strcmp(this_text, LV_SYMBOL_BACKSPACE) == 0) {
                            this_width = 6 * unit_width; // Shift/Backspace
                        } else {
                            this_width = 4 * unit_width; // Regular button
                        }
                        cumulative_width += this_width;
                    }
                }

                // Current button width (always 4 units for letter keys)
                lv_coord_t current_btn_width = 4 * unit_width;

                // Calculate button top-right corner
                lv_coord_t btn_x = kb_coords.x1 + cumulative_width + current_btn_width - 10;
                lv_coord_t btn_y = kb_coords.y1 + row * row_height + 6;

                // Create draw label descriptor
                lv_draw_label_dsc_t label_dsc;
                lv_draw_label_dsc_init(&label_dsc);
                label_dsc.font = &lv_font_montserrat_12;
                label_dsc.color = gray_color;
                label_dsc.opa = LV_OPA_60;

                // Create text with first alternative
                char alt_str[2] = {alternatives[0], '\0'};
                label_dsc.text = alt_str;
                label_dsc.text_local = true;

                // Create area for label
                lv_area_t alt_area;
                alt_area.x1 = btn_x - 12;
                alt_area.y1 = btn_y;
                alt_area.x2 = btn_x;
                alt_area.y2 = btn_y + 14;

                // Draw the alternative character
                lv_draw_label(layer, &label_dsc, &alt_area);
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void ui_keyboard_init(lv_obj_t* parent) {
    if (g_keyboard != NULL) {
        spdlog::warn("[Keyboard] Already initialized, skipping");
        return;
    }

    spdlog::info("[Keyboard] Initializing global keyboard");

    // Create keyboard at bottom of screen
    g_keyboard = lv_keyboard_create(parent);

    // Set initial mode (lowercase text)
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Enable pop-overs (key feedback on press)
    lv_keyboard_set_popovers(g_keyboard, true);

    // Apply improved numeric keyboard layout (adds period key for IPs/decimals)
    lv_keyboard_set_map(g_keyboard, LV_KEYBOARD_MODE_NUMBER, kb_map_num_improved,
                        kb_ctrl_num_improved);

    // Apply keyboard layouts
    spdlog::info("[Keyboard] Using keyboard with long-press alternatives");
    g_mode = MODE_ALPHA_LC;
    apply_keyboard_mode();

    // Apply styling - theme handles colors, set opacity for solid background
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_MAIN); // Fully opaque background
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(g_keyboard, 8, LV_PART_ITEMS); // Rounded key corners

    // Ensure text is visible on all buttons (set this BEFORE disabled styling)
    lv_obj_set_style_text_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_keyboard, lv_color_white(), LV_PART_ITEMS);

    // Make disabled buttons (spacers) invisible - set AFTER general styling
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(g_keyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_shadow_opa(g_keyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(g_keyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);

    // Position at bottom-middle (default)
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Start hidden
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Add our custom event handlers BEFORE LVGL's default handler
    // We handle PRESSED first to intercept non-printing buttons before VALUE_CHANGED fires
    // For VALUE_CHANGED, our handler runs after LVGL's, so we can clean up if needed
    lv_obj_add_event_cb(g_keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(g_keyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Add long-press event handlers for alternative character system
    lv_obj_add_event_cb(g_keyboard, longpress_event_handler, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_keyboard, longpress_event_handler, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(g_keyboard, longpress_event_handler, LV_EVENT_RELEASED, NULL);

    // Add custom draw handler to display alternative characters on keys
    lv_obj_add_event_cb(g_keyboard, keyboard_draw_alternative_chars, LV_EVENT_DRAW_POST_END, NULL);

    spdlog::info(
        "[Keyboard] Initialization complete (with long-press alternatives and visual hints)");
}

void ui_keyboard_register_textarea(lv_obj_t* textarea) {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    if (textarea == NULL) {
        spdlog::error("[Keyboard] Cannot register NULL textarea");
        return;
    }

    spdlog::debug("[Keyboard] Registering textarea: {}", (void*)textarea);

    // Add event handler to catch focus/defocus events (not ALL events to avoid cleanup issues)
    lv_obj_add_event_cb(textarea, textarea_focus_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(textarea, textarea_focus_event_cb, LV_EVENT_DEFOCUSED, NULL);

    // Add textarea to default input group for physical keyboard input
    lv_group_t* default_group = lv_group_get_default();
    if (default_group) {
        lv_group_add_obj(default_group, textarea);
        spdlog::debug("[Keyboard] Added textarea to input group for physical keyboard");
    }
}

void ui_keyboard_show(lv_obj_t* textarea) {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    // Safety: if keyboard's parent is NULL, we're in cleanup - bail out
    if (lv_obj_get_parent(g_keyboard) == NULL) {
        spdlog::debug("[Keyboard] Skipping show - keyboard is being cleaned up");
        return;
    }

    // Safety: check if screen is valid before layout operations
    // Note: Root screens have NULL parents by design, so only check for NULL screen
    lv_obj_t* screen = lv_screen_active();
    if (screen == NULL) {
        spdlog::debug("[Keyboard] Skipping show - no active screen");
        return;
    }

    spdlog::debug("[Keyboard] Showing keyboard for textarea: {}", (void*)textarea);

    // Reset keyboard to lowercase mode on each show
    g_mode = MODE_ALPHA_LC;
    apply_keyboard_mode();

    // Assign textarea to keyboard (standard LVGL API)
    lv_keyboard_set_textarea(g_keyboard, textarea);

    // Show keyboard
    lv_obj_remove_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Move keyboard to foreground to ensure it appears above modals
    lv_obj_move_foreground(g_keyboard);

    // Force layout update to get accurate positions
    lv_obj_update_layout(screen);

    if (!textarea) {
        return;
    }

    // Get absolute coordinates
    lv_area_t kb_coords, ta_coords;
    lv_obj_get_coords(g_keyboard, &kb_coords);
    lv_obj_get_coords(textarea, &ta_coords);

    int32_t kb_top = kb_coords.y1;
    int32_t ta_bottom = ta_coords.y2;

    // Add padding above textarea (20px breathing room)
    const int32_t padding = 20;
    int32_t desired_bottom = kb_top - padding;

    // Calculate if we need to shift the screen up
    if (ta_bottom > desired_bottom) {
        int32_t shift_up = ta_bottom - desired_bottom;

        spdlog::debug("[Keyboard] Shifting screen UP by {} px (ta_bottom={}, kb_top={})", shift_up,
                      ta_bottom, kb_top);

        // Move all screen children (except keyboard) up with animation
        lv_obj_t* screen = lv_screen_active();
        uint32_t child_count = lv_obj_get_child_count(screen);

        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (child == g_keyboard)
                continue;

            int32_t current_y = lv_obj_get_y(child);
            int32_t target_y = current_y - shift_up;

            // Animate the Y position change (200ms, fast and smooth)
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, child);
            lv_anim_set_values(&a, current_y, target_y);
            lv_anim_set_time(&a, 200);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    } else {
        spdlog::debug("[Keyboard] Textarea already visible (ta_bottom={}, kb_top={})", ta_bottom,
                      kb_top);
    }
}

void ui_keyboard_hide() {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    // Safety: if keyboard's parent is NULL, we're in cleanup - bail out
    if (lv_obj_get_parent(g_keyboard) == NULL) {
        spdlog::debug("[Keyboard] Skipping hide - keyboard is being cleaned up");
        return;
    }

    // Safety: check if screen is valid before layout operations
    // Note: Root screens have NULL parents by design, so only check for NULL screen
    lv_obj_t* screen = lv_screen_active();
    if (screen == NULL) {
        spdlog::debug("[Keyboard] Skipping hide - no active screen");
        return;
    }

    spdlog::debug("[Keyboard] Hiding keyboard");

    // Clean up any active long-press overlay
    overlay_cleanup();
    g_longpress_state = LP_IDLE;

    // Clear keyboard assignment
    lv_keyboard_set_textarea(g_keyboard, NULL);

    // Hide keyboard
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Move all screen children (except keyboard) back to y=0 with animation
    uint32_t child_count = lv_obj_get_child_count(screen);

    spdlog::debug("[Keyboard] Restoring screen children to y=0");

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, i);
        if (child == g_keyboard)
            continue;

        int32_t current_y = lv_obj_get_y(child);
        if (current_y != 0) {
            // Animate back to y=0 (200ms, fast and smooth)
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, child);
            lv_anim_set_values(&a, current_y, 0);
            lv_anim_set_time(&a, 200);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
            lv_anim_start(&a);
        }
    }
}

bool ui_keyboard_is_visible() {
    if (g_keyboard == NULL) {
        return false;
    }

    return !lv_obj_has_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* ui_keyboard_get_instance() {
    return g_keyboard;
}

void ui_keyboard_set_mode(lv_keyboard_mode_t mode) {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Setting mode: {}", (int)mode);
    lv_keyboard_set_mode(g_keyboard, mode);
}

void ui_keyboard_set_position(lv_align_t align, int32_t x_ofs, int32_t y_ofs) {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Setting position: align={}, x={}, y={}", (int)align, x_ofs, y_ofs);
    lv_obj_align(g_keyboard, align, x_ofs, y_ofs);
}

void ui_keyboard_register_textarea_ex(lv_obj_t* textarea, bool is_password) {
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    if (textarea == NULL) {
        spdlog::error("[Keyboard] Cannot register NULL textarea");
        return;
    }

    spdlog::debug("[Keyboard] Registering textarea: {} (password: {})", (void*)textarea,
                  is_password);

    // Use standard registration which adds focus/defocus handlers
    ui_keyboard_register_textarea(textarea);
}
