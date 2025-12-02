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

#pragma once

#include "lvgl.h"

/**
 * @brief Unified modal dialog system for HelixScreen
 *
 * Provides a consistent API for creating and managing modal dialogs with support for:
 * - Modal stacking (multiple modals layered on top of each other)
 * - Flexible positioning (alignment presets or manual x/y coordinates)
 * - Automatic keyboard positioning based on modal location
 * - Configurable lifecycle (persistent vs. create-on-demand)
 * - Backdrop click-to-dismiss and ESC key handling
 *
 * Usage:
 * 1. Configure modal with ui_modal_config_t
 * 2. Call ui_modal_show() with XML component name
 * 3. Modal system handles backdrop, z-order, keyboard positioning
 * 4. Call ui_modal_hide() or use backdrop/ESC to dismiss
 *
 * Example:
 *   ui_modal_config_t config = {
 *       .position = {.use_alignment = true, .alignment = LV_ALIGN_CENTER},
 *       .backdrop_opa = 180,
 *       .keyboard = nullptr,
 *       .persistent = true
 *   };
 *   lv_obj_t* modal = ui_modal_show("confirmation_dialog", &config, attrs);
 */

/**
 * @brief Modal positioning configuration
 *
 * Supports either alignment presets (center, right_mid, etc.) or manual
 * x/y coordinates for precise positioning.
 */
typedef struct {
    bool use_alignment;   /**< true = use alignment, false = use x/y */
    lv_align_t alignment; /**< Alignment preset (if use_alignment=true) */
    int32_t x;            /**< Manual x position (if use_alignment=false) */
    int32_t y;            /**< Manual y position (if use_alignment=false) */
} ui_modal_position_t;

/**
 * @brief Keyboard positioning configuration
 *
 * By default, keyboard position is automatically determined based on modal
 * alignment (e.g., left side for right-aligned modals). Manual override
 * available when needed.
 */
typedef struct {
    bool auto_position;   /**< true = auto based on modal, false = manual */
    lv_align_t alignment; /**< Manual alignment (if auto_position=false) */
    int32_t x;            /**< Manual x offset (if auto_position=false) */
    int32_t y;            /**< Manual y offset (if auto_position=false) */
} ui_modal_keyboard_config_t;

/**
 * @brief Complete modal configuration
 */
typedef struct {
    ui_modal_position_t position;         /**< Modal positioning */
    uint8_t backdrop_opa;                 /**< Backdrop opacity (0-255) */
    ui_modal_keyboard_config_t* keyboard; /**< Keyboard config (NULL = no keyboard) */
    bool persistent;                      /**< true = persistent, false = create-on-demand */
    lv_event_cb_t on_close;               /**< Optional close callback */
} ui_modal_config_t;

/**
 * @brief Show a modal dialog
 *
 * Creates and displays a modal with the specified configuration. The modal
 * is automatically added to the modal stack and layered on top of any
 * existing modals.
 *
 * @param component_name XML component name (e.g., "confirmation_dialog")
 * @param config Modal configuration
 * @param attrs Optional XML attributes (NULL-terminated array, can be NULL)
 * @return Pointer to the created modal object, or NULL on error
 */
lv_obj_t* ui_modal_show(const char* component_name, const ui_modal_config_t* config,
                        const char** attrs);

/**
 * @brief Hide a specific modal
 *
 * Removes the modal from the stack. If the modal is persistent, it is hidden
 * (LV_OBJ_FLAG_HIDDEN). If non-persistent, it is deleted.
 *
 * @param modal The modal object to hide
 */
void ui_modal_hide(lv_obj_t* modal);

/**
 * @brief Hide all modals
 *
 * Closes all modals in the stack, from top to bottom.
 */
void ui_modal_hide_all();

/**
 * @brief Get the topmost modal
 *
 * @return Pointer to the topmost modal, or NULL if no modals are visible
 */
lv_obj_t* ui_modal_get_top();

/**
 * @brief Check if any modals are currently visible
 *
 * @return true if at least one modal is visible, false otherwise
 */
bool ui_modal_is_visible();

/**
 * @brief Register a textarea with automatic keyboard positioning
 *
 * When the textarea receives focus, the keyboard will be shown and positioned
 * automatically based on the modal's alignment (if keyboard config has
 * auto_position=true).
 *
 * @param modal The modal containing the textarea
 * @param textarea The textarea widget
 */
void ui_modal_register_keyboard(lv_obj_t* modal, lv_obj_t* textarea);

// =========================================================================
// MODAL DIALOG SUBJECTS (for modal_dialog.xml binding)
// =========================================================================

/**
 * @brief Severity levels for modal dialogs
 *
 * Controls which icon is displayed in modal_dialog:
 * - INFO (0): Blue info icon - informational messages
 * - WARNING (1): Orange warning icon - caution messages
 * - ERROR (2): Red error icon - error messages
 */
enum ui_modal_severity {
    UI_MODAL_SEVERITY_INFO = 0,
    UI_MODAL_SEVERITY_WARNING = 1,
    UI_MODAL_SEVERITY_ERROR = 2,
};

/**
 * @brief Initialize modal dialog subjects
 *
 * Creates and registers LVGL subjects used by modal_dialog.xml:
 * - dialog_severity (int): Controls icon display (0=info, 1=warning, 2=error)
 * - dialog_show_cancel (int): Shows cancel button when 1
 * - dialog_primary_text (string): Primary button label
 * - dialog_cancel_text (string): Cancel button label
 *
 * Call ONCE during app startup, before creating any modal_dialog components.
 */
void ui_modal_init_subjects();

/**
 * @brief Configure modal dialog before showing
 *
 * Sets all subject values atomically. Call BEFORE lv_xml_create("modal_dialog", ...).
 *
 * @param severity Dialog severity (UI_MODAL_SEVERITY_INFO/WARNING/ERROR)
 * @param show_cancel Whether to show cancel button
 * @param primary_text Primary button label (e.g., "OK", "Delete")
 * @param cancel_text Cancel button label (e.g., "Cancel", "No")
 */
void ui_modal_configure(ui_modal_severity severity, bool show_cancel,
                        const char* primary_text, const char* cancel_text);

/**
 * @brief Get dialog_severity subject for direct access
 * @return Pointer to lv_subject_t
 */
lv_subject_t* ui_modal_get_severity_subject();

/**
 * @brief Get dialog_show_cancel subject for direct access
 * @return Pointer to lv_subject_t
 */
lv_subject_t* ui_modal_get_show_cancel_subject();

/**
 * @brief Get dialog_primary_text subject for direct access
 * @return Pointer to lv_subject_t
 */
lv_subject_t* ui_modal_get_primary_text_subject();

/**
 * @brief Get dialog_cancel_text subject for direct access
 * @return Pointer to lv_subject_t
 */
lv_subject_t* ui_modal_get_cancel_text_subject();
