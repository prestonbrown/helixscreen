// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

/**
 * @file ui_split_button.h
 * @brief Split button widget — primary action button with dropdown selector
 *
 * Provides a <ui_split_button> XML widget with:
 * - Primary click zone (main button) + dropdown arrow zone
 * - Semantic variants matching ui_button (primary, secondary, danger, etc.)
 * - Optional icon on main button
 * - Dropdown selection updates button label via text_format
 * - Auto-contrast text/icons based on background luminance
 *
 * Usage in XML:
 *   <ui_split_button text="Preheat" options="PLA&#10;PETG&#10;ABS">
 *     <event_cb trigger="clicked" callback="on_preheat"/>
 *     <event_cb trigger="value_changed" callback="on_material_changed"/>
 *   </ui_split_button>
 *
 *   <ui_split_button variant="primary" icon="heat_wave"
 *                    text_format="Preheat %s" options="PLA&#10;PETG&#10;ABS"
 *                    selected="0" show_selection="true">
 *     <event_cb trigger="clicked" callback="on_preheat"/>
 *     <event_cb trigger="value_changed" callback="on_material_changed"/>
 *   </ui_split_button>
 *
 * Attributes:
 * - variant: Button style variant (default: "primary") — same as ui_button
 * - text: Button label text (static or overridden by show_selection)
 * - text_format: Format string with %s replaced by selected option name
 * - icon: Optional MDI icon name (e.g., "heat_wave")
 * - options: Newline-separated dropdown options (use &#10; in XML)
 * - selected: Initially selected option index (default: 0)
 * - show_selection: Update button text with selected option (default: "true")
 *
 * Events:
 * - clicked: Main button area tapped (primary action)
 * - value_changed: Dropdown selection changed
 */

/**
 * @brief Initialize the ui_split_button custom widget
 *
 * Registers the <ui_split_button> XML widget with LVGL's XML parser.
 * Must be called after lv_xml_init() and after theme is initialized.
 */
void ui_split_button_init();

/**
 * @brief Drop the memoized icon_font_sm lookup so the next split button re-reads it
 *
 * Same process-lifetime cache as ui_button; see
 * ui_button_invalidate_icon_font_cache().
 */
void ui_split_button_invalidate_icon_font_cache();

/**
 * @brief Set the dropdown options
 * @param sb The ui_split_button widget
 * @param options Newline-separated option strings
 */
void ui_split_button_set_options(lv_obj_t* sb, const char* options);

/**
 * @brief Set the selected dropdown index
 * @param sb The ui_split_button widget
 * @param index Option index to select
 */
void ui_split_button_set_selected(lv_obj_t* sb, uint32_t index);

/**
 * @brief Get the currently selected dropdown index
 * @param sb The ui_split_button widget
 * @return Currently selected index
 */
uint32_t ui_split_button_get_selected(lv_obj_t* sb);

/**
 * @brief Set the button label text directly
 * @param sb The ui_split_button widget
 * @param text New label text
 */
void ui_split_button_set_text(lv_obj_t* sb, const char* text);
