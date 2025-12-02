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

#include "ui_modal.h"

#include "ui_keyboard.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

/**
 * @brief Modal metadata stored alongside LVGL object
 */
struct ModalMetadata {
    lv_obj_t* modal_obj;        // The modal object itself
    ui_modal_config_t config;   // Original configuration
    std::string component_name; // XML component name
};

// Modal stack - topmost modal is at the back
static std::vector<ModalMetadata> g_modal_stack;

// =========================================================================
// MODAL DIALOG SUBJECTS
// =========================================================================

// Static subjects for modal_dialog.xml binding
static lv_subject_t g_dialog_severity;
static lv_subject_t g_dialog_show_cancel;
static lv_subject_t g_dialog_primary_text;
static lv_subject_t g_dialog_cancel_text;

// Default button labels (static storage for lv_subject_set_pointer)
static const char* g_default_primary_text = "OK";
static const char* g_default_cancel_text = "Cancel";
static bool g_subjects_initialized = false;

/**
 * @brief Find modal metadata by LVGL object pointer
 */
static ModalMetadata* find_modal_metadata(lv_obj_t* modal) {
    auto it = std::find_if(g_modal_stack.begin(), g_modal_stack.end(),
                           [modal](const ModalMetadata& meta) { return meta.modal_obj == modal; });
    return (it != g_modal_stack.end()) ? &(*it) : nullptr;
}

/**
 * @brief Backdrop click event handler
 *
 * Only closes the modal if the click target is the backdrop itself
 * (not a child widget). Only responds if this is the topmost modal.
 */
static void backdrop_click_event_cb(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* current = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only respond if click was directly on backdrop (not bubbled from child)
    if (target != current) {
        return;
    }

    // Only allow closing topmost modal
    if (!g_modal_stack.empty() && g_modal_stack.back().modal_obj == current) {
        spdlog::debug("[Modal] Backdrop clicked on topmost modal - closing");
        ui_modal_hide(current);
    }
}

/**
 * @brief ESC key event handler
 *
 * Closes the topmost modal when ESC is pressed.
 */
static void modal_key_event_cb(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_ESC && !g_modal_stack.empty()) {
        spdlog::debug("[Modal] ESC key pressed - closing topmost modal");
        ui_modal_hide(g_modal_stack.back().modal_obj);
    }
}

/**
 * @brief Determine automatic keyboard alignment based on modal position
 */
static void get_auto_keyboard_position(const ui_modal_position_t& modal_pos, lv_align_t* out_align,
                                       int32_t* out_x, int32_t* out_y) {
    // Default: bottom-center
    *out_align = LV_ALIGN_BOTTOM_MID;
    *out_x = 0;
    *out_y = 0;

    if (!modal_pos.use_alignment) {
        // Manual positioning - use heuristic based on x position
        // If modal is on right half of screen, put keyboard on left
        lv_obj_t* screen = lv_screen_active();
        int32_t screen_w = lv_obj_get_width(screen);

        if (modal_pos.x > screen_w / 2) {
            spdlog::debug("[Modal] Manual position on right side - keyboard on left");
            *out_align = LV_ALIGN_LEFT_MID;
            *out_x = 0;
            *out_y = 0;
        }
        return;
    }

    // Alignment-based positioning
    switch (modal_pos.alignment) {
    case LV_ALIGN_RIGHT_MID:
    case LV_ALIGN_TOP_RIGHT:
    case LV_ALIGN_BOTTOM_RIGHT:
        // Right-aligned modals get keyboard on left
        spdlog::debug("[Modal] Right-aligned modal - keyboard on left");
        *out_align = LV_ALIGN_LEFT_MID;
        *out_x = 0;
        *out_y = 0;
        break;

    case LV_ALIGN_LEFT_MID:
    case LV_ALIGN_TOP_LEFT:
    case LV_ALIGN_BOTTOM_LEFT:
        // Left-aligned modals get keyboard on right
        spdlog::debug("[Modal] Left-aligned modal - keyboard on right");
        *out_align = LV_ALIGN_RIGHT_MID;
        *out_x = 0;
        *out_y = 0;
        break;

    default:
        // Center/top/bottom modals get keyboard at bottom
        spdlog::debug("[Modal] Center/top/bottom modal - keyboard at bottom");
        *out_align = LV_ALIGN_BOTTOM_MID;
        *out_x = 0;
        *out_y = 0;
        break;
    }
}

/**
 * @brief Position keyboard based on modal configuration
 */
static void position_keyboard_for_modal(lv_obj_t* modal) {
    ModalMetadata* meta = find_modal_metadata(modal);
    if (!meta || !meta->config.keyboard) {
        return;
    }

    lv_align_t align;
    int32_t x, y;

    if (meta->config.keyboard->auto_position) {
        // Automatic positioning based on modal alignment
        get_auto_keyboard_position(meta->config.position, &align, &x, &y);
    } else {
        // Manual positioning from config
        align = meta->config.keyboard->alignment;
        x = meta->config.keyboard->x;
        y = meta->config.keyboard->y;
    }

    spdlog::debug("[Modal] Positioning keyboard: align={}, x={}, y={}", (int)align, x, y);
    ui_keyboard_set_position(align, x, y);
}

lv_obj_t* ui_modal_show(const char* component_name, const ui_modal_config_t* config,
                        const char** attrs) {
    if (!component_name || !config) {
        spdlog::error("[Modal] Invalid parameters: component_name={}, config={}",
                      (void*)component_name, (void*)config);
        return nullptr;
    }

    spdlog::info("[Modal] Showing modal: {}", component_name);

    // Create modal from XML
    lv_obj_t* modal =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), component_name, attrs));
    if (!modal) {
        spdlog::error("[Modal] Failed to create modal from XML: {}", component_name);
        return nullptr;
    }

    // Apply positioning
    if (config->position.use_alignment) {
        spdlog::debug("[Modal] Positioning with alignment: {}", (int)config->position.alignment);
        lv_obj_align(modal, config->position.alignment, 0, 0);
    } else {
        spdlog::debug("[Modal] Positioning at x={}, y={}", config->position.x, config->position.y);
        lv_obj_set_pos(modal, config->position.x, config->position.y);
    }

    // Set backdrop opacity (modal should have full-screen backdrop as root)
    lv_obj_set_style_bg_opa(modal, config->backdrop_opa, LV_PART_MAIN);

    // Add backdrop click handler
    lv_obj_add_event_cb(modal, backdrop_click_event_cb, LV_EVENT_CLICKED, nullptr);

    // Add ESC key handler
    lv_obj_add_event_cb(modal, modal_key_event_cb, LV_EVENT_KEY, nullptr);

    // Register on_close callback if provided
    if (config->on_close) {
        lv_obj_add_event_cb(modal, config->on_close, LV_EVENT_DELETE, nullptr);
        spdlog::debug("[Modal] Close callback registered for DELETE event");
    }

    // Bring to foreground (above all existing modals)
    lv_obj_move_foreground(modal);

    // Position keyboard if configured
    if (config->keyboard) {
        position_keyboard_for_modal(modal);
    }

    // Add to modal stack
    ModalMetadata meta;
    meta.modal_obj = modal;
    meta.config = *config;
    meta.component_name = component_name;

    // Deep copy keyboard config if present
    if (config->keyboard) {
        meta.config.keyboard = new ui_modal_keyboard_config_t(*config->keyboard);
    }

    g_modal_stack.push_back(meta);

    spdlog::info("[Modal] Modal shown successfully (stack depth: {})", g_modal_stack.size());

    return modal;
}

void ui_modal_hide(lv_obj_t* modal) {
    if (!modal) {
        spdlog::error("[Modal] Cannot hide NULL modal");
        return;
    }

    // Find modal in stack
    auto it = std::find_if(g_modal_stack.begin(), g_modal_stack.end(),
                           [modal](const ModalMetadata& meta) { return meta.modal_obj == modal; });

    if (it == g_modal_stack.end()) {
        spdlog::warn("[Modal] Modal not found in stack: {}", (void*)modal);
        // Still try to hide/delete it
        if (lv_obj_is_valid(modal)) {
            lv_obj_delete(modal);
        }
        return;
    }

    spdlog::info("[Modal] Hiding modal: {}", it->component_name);

    // Note: on_close callback is registered with LVGL event system
    // It will be automatically called when the modal is deleted

    // Clean up keyboard config if allocated
    if (it->config.keyboard) {
        delete it->config.keyboard;
        it->config.keyboard = nullptr;
    }

    // Remove from stack before deleting/hiding
    bool was_persistent = it->config.persistent;
    g_modal_stack.erase(it);

    // Hide or delete based on lifecycle policy
    if (was_persistent) {
        spdlog::debug("[Modal] Persistent modal - hiding");
        lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    } else {
        spdlog::debug("[Modal] Non-persistent modal - deleting");
        lv_obj_delete(modal);
    }

    spdlog::info("[Modal] Modal hidden (stack depth: {})", g_modal_stack.size());

    // If there are more modals, bring the new topmost to foreground
    if (!g_modal_stack.empty()) {
        lv_obj_move_foreground(g_modal_stack.back().modal_obj);
        spdlog::debug("[Modal] Brought previous modal to foreground");
    }
}

void ui_modal_hide_all() {
    spdlog::info("[Modal] Hiding all modals (count: {})", g_modal_stack.size());

    // Hide from top to bottom
    while (!g_modal_stack.empty()) {
        ui_modal_hide(g_modal_stack.back().modal_obj);
    }

    spdlog::info("[Modal] All modals hidden");
}

lv_obj_t* ui_modal_get_top() {
    if (g_modal_stack.empty()) {
        return nullptr;
    }
    return g_modal_stack.back().modal_obj;
}

bool ui_modal_is_visible() {
    return !g_modal_stack.empty();
}

void ui_modal_register_keyboard(lv_obj_t* modal, lv_obj_t* textarea) {
    if (!modal || !textarea) {
        spdlog::error("[Modal] Cannot register keyboard: modal={}, textarea={}", (void*)modal,
                      (void*)textarea);
        return;
    }

    // Position keyboard for this modal
    position_keyboard_for_modal(modal);

    // Check if this is a password textarea (via LVGL's password_mode property)
    bool is_password = lv_textarea_get_password_mode(textarea);

    // Register textarea with context-aware keyboard (auto-enables number row for passwords)
    if (is_password) {
        ui_keyboard_register_textarea_ex(textarea, true);
        spdlog::debug("[Modal] Registered PASSWORD textarea with keyboard");
    } else {
        ui_keyboard_register_textarea(textarea);
    }

    spdlog::debug("[Modal] Keyboard registered for modal: {}, textarea: {}", (void*)modal,
                  (void*)textarea);
}

// =========================================================================
// MODAL DIALOG SUBJECT IMPLEMENTATION
// =========================================================================

void ui_modal_init_subjects() {
    if (g_subjects_initialized) {
        spdlog::warn("[Modal] Subjects already initialized - skipping");
        return;
    }

    spdlog::info("[Modal] Initializing modal dialog subjects");

    // Initialize integer subjects
    lv_subject_init_int(&g_dialog_severity, UI_MODAL_SEVERITY_INFO);
    lv_subject_init_int(&g_dialog_show_cancel, 0);

    // Initialize string subjects (pointer type)
    lv_subject_init_pointer(&g_dialog_primary_text, (void*)g_default_primary_text);
    lv_subject_init_pointer(&g_dialog_cancel_text, (void*)g_default_cancel_text);

    // Register with LVGL XML system for binding (NULL scope = global subjects)
    lv_xml_register_subject(nullptr, "dialog_severity", &g_dialog_severity);
    lv_xml_register_subject(nullptr, "dialog_show_cancel", &g_dialog_show_cancel);
    lv_xml_register_subject(nullptr, "dialog_primary_text", &g_dialog_primary_text);
    lv_xml_register_subject(nullptr, "dialog_cancel_text", &g_dialog_cancel_text);

    g_subjects_initialized = true;
    spdlog::info("[Modal] Modal dialog subjects registered");
}

void ui_modal_configure(ui_modal_severity severity, bool show_cancel,
                        const char* primary_text, const char* cancel_text) {
    if (!g_subjects_initialized) {
        spdlog::error("[Modal] Cannot configure - subjects not initialized! Call ui_modal_init_subjects() first");
        return;
    }

    spdlog::debug("[Modal] Configuring dialog: severity={}, show_cancel={}, primary='{}', cancel='{}'",
                  (int)severity, show_cancel, primary_text ? primary_text : "(null)",
                  cancel_text ? cancel_text : "(null)");

    lv_subject_set_int(&g_dialog_severity, severity);
    lv_subject_set_int(&g_dialog_show_cancel, show_cancel ? 1 : 0);

    if (primary_text) {
        lv_subject_set_pointer(&g_dialog_primary_text, (void*)primary_text);
    }
    if (cancel_text) {
        lv_subject_set_pointer(&g_dialog_cancel_text, (void*)cancel_text);
    }
}

lv_subject_t* ui_modal_get_severity_subject() {
    return &g_dialog_severity;
}

lv_subject_t* ui_modal_get_show_cancel_subject() {
    return &g_dialog_show_cancel;
}

lv_subject_t* ui_modal_get_primary_text_subject() {
    return &g_dialog_primary_text;
}

lv_subject_t* ui_modal_get_cancel_text_subject() {
    return &g_dialog_cancel_text;
}
