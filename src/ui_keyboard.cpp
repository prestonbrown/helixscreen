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
#include <spdlog/spdlog.h>

// Global keyboard instance
static lv_obj_t* g_keyboard = NULL;

/**
 * @brief Textarea focus event callback - handles auto show/hide
 */
static void textarea_focus_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* textarea = lv_event_get_target_obj(e);

    if (code == LV_EVENT_FOCUSED) {
        spdlog::debug("[Keyboard] Textarea focused: {}", (void*)textarea);
        ui_keyboard_show(textarea);
    }
    else if (code == LV_EVENT_DEFOCUSED) {
        spdlog::debug("[Keyboard] Textarea defocused: {}", (void*)textarea);
        ui_keyboard_hide();
    }
}

/**
 * @brief Keyboard event callback - handles ready/cancel events
 */
static void keyboard_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        spdlog::info("[Keyboard] OK pressed - input confirmed");
        // User pressed OK/Enter - hide keyboard
        ui_keyboard_hide();
    }
    else if (code == LV_EVENT_CANCEL) {
        spdlog::info("[Keyboard] Cancel pressed");
        // User pressed close button - hide keyboard
        ui_keyboard_hide();
    }
}

void ui_keyboard_init(lv_obj_t* parent)
{
    if (g_keyboard != NULL) {
        spdlog::warn("[Keyboard] Already initialized, skipping");
        return;
    }

    spdlog::info("[Keyboard] Initializing global keyboard");

    // Create keyboard at bottom of screen
    g_keyboard = lv_keyboard_create(parent);

    // Set initial mode (lowercase text)
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Enable pop-overs (iOS/Android-style key feedback)
    lv_keyboard_set_popovers(g_keyboard, true);

    // Apply styling - theme handles colors, set opacity for transparency
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_80, LV_PART_MAIN);  // 80% opacity = 20% transparent
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(g_keyboard, 8, LV_PART_ITEMS);  // Rounded key corners

    // Position at bottom-middle (default)
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Start hidden
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Add event handler for ready/cancel events
    lv_obj_add_event_cb(g_keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    spdlog::info("[Keyboard] Initialization complete");
}

void ui_keyboard_register_textarea(lv_obj_t* textarea)
{
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    if (textarea == NULL) {
        spdlog::error("[Keyboard] Cannot register NULL textarea");
        return;
    }

    spdlog::debug("[Keyboard] Registering textarea: {}", (void*)textarea);

    // Add event handler to catch focus/defocus events
    lv_obj_add_event_cb(textarea, textarea_focus_event_cb, LV_EVENT_ALL, NULL);
}

void ui_keyboard_show(lv_obj_t* textarea)
{
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Showing keyboard for textarea: {}", (void*)textarea);

    // Assign textarea to keyboard (NULL is valid - clears assignment)
    lv_keyboard_set_textarea(g_keyboard, textarea);

    // Show keyboard
    lv_obj_remove_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Auto-scroll textarea to be visible above keyboard
    if (textarea) {
        // Force layout update to get accurate keyboard position
        lv_obj_update_layout(lv_screen_active());

        // Get keyboard height and position
        int32_t kb_y = lv_obj_get_y(g_keyboard);
        int32_t textarea_y = lv_obj_get_y(textarea);
        int32_t textarea_h = lv_obj_get_height(textarea);

        // Calculate if textarea is obscured by keyboard
        // (textarea bottom is below keyboard top)
        if (textarea_y + textarea_h > kb_y) {
            spdlog::debug("[Keyboard] Textarea obscured by keyboard - scrolling into view");

            // Scroll with smooth animation
            lv_obj_scroll_to_view_recursive(textarea, LV_ANIM_ON);
        } else {
            spdlog::debug("[Keyboard] Textarea already visible above keyboard");
        }
    }
}

void ui_keyboard_hide()
{
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Hiding keyboard");

    // Get current textarea before clearing (for scroll-back)
    lv_obj_t* textarea = lv_keyboard_get_textarea(g_keyboard);

    // Clear textarea assignment
    lv_keyboard_set_textarea(g_keyboard, NULL);

    // Hide keyboard
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Scroll textarea back with smooth animation (if it was scrolled)
    if (textarea) {
        lv_obj_scroll_to_view_recursive(textarea, LV_ANIM_ON);
        spdlog::debug("[Keyboard] Scrolled textarea back after hide");
    }
}

bool ui_keyboard_is_visible()
{
    if (g_keyboard == NULL) {
        return false;
    }

    return !lv_obj_has_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* ui_keyboard_get_instance()
{
    return g_keyboard;
}

void ui_keyboard_set_mode(lv_keyboard_mode_t mode)
{
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Setting mode: {}", (int)mode);
    lv_keyboard_set_mode(g_keyboard, mode);
}

void ui_keyboard_set_position(lv_align_t align, int32_t x_ofs, int32_t y_ofs)
{
    if (g_keyboard == NULL) {
        spdlog::error("[Keyboard] Not initialized - call ui_keyboard_init() first");
        return;
    }

    spdlog::debug("[Keyboard] Setting position: align={}, x={}, y={}",
                  (int)align, x_ofs, y_ofs);
    lv_obj_align(g_keyboard, align, x_ofs, y_ofs);
}
