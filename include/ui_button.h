// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

/**
 * @file ui_button.h
 * @brief Semantic button widget with variant styles, icons, and auto-contrast
 *
 * Provides a <ui_button> XML widget with:
 * - Semantic variants: primary, secondary, danger, success, tertiary, warning, ghost
 * - Optional icon support with auto-contrast (light icon on dark bg, dark on light)
 * - Auto-contrast text: automatically picks light/dark text based on bg luminance
 * - Reactive styling: updates automatically when theme changes
 *
 * Usage in XML:
 *   <ui_button variant="primary" text="Save"/>
 *   <ui_button variant="danger" text="Delete"/>
 *   <ui_button variant="secondary" text="Cancel"/>
 *   <ui_button variant="ghost" text="Skip"/>
 *   <ui_button icon="heat_wave" text="Dryer"/>
 *   <ui_button icon="settings" text=""/>
 *   <ui_button icon="cog" icon_position="right" text="Settings"/>
 *   <ui_button bind_icon="light_icon_subject" text="Light"/>
 *
 * Attributes:
 * - variant: Button style variant (default: "primary")
 *   - primary: ThemeManager::get_style(StyleRole::ButtonPrimary) (primary/accent color bg)
 *   - secondary: ThemeManager::get_style(StyleRole::ButtonSecondary) (surface color bg)
 *   - danger: ThemeManager::get_style(StyleRole::ButtonDanger) (danger/red color bg)
 *   - success: ThemeManager::get_style(StyleRole::ButtonSuccess) (success/green color bg)
 *   - tertiary: ThemeManager::get_style(StyleRole::ButtonTertiary) (tertiary color bg)
 *   - warning: ThemeManager::get_style(StyleRole::ButtonWarning) (warning/amber color bg)
 *   - ghost: ThemeManager::get_style(StyleRole::ButtonGhost) (transparent bg)
 *
 * - text: Button label text (optional, can be empty for icon-only buttons)
 *
 * - bind_text: Subject name for dynamic text binding (string subject)
 *
 * - icon: Icon name from MDI font (optional, e.g., "settings", "heat_wave", "cog")
 *   Icon uses auto-contrast just like text.
 *
 * - bind_icon: Subject name for dynamic icon binding (string subject containing icon name)
 *   The subject value should be an icon name (e.g., "light", "light_off", "play", "pause").
 *   When the subject changes, the icon updates automatically.
 *
 * - icon_position: Where to place icon relative to text (default: "left")
 *   - "left": Icon before text (horizontal flex)
 *   - "right": Icon after text (horizontal flex)
 *   - "top": Icon stacked above text (vertical flex)
 *   - "bottom": Icon stacked below text (vertical flex)
 *   Applies to icons from the static icon= attribute and to icons created later
 *   by a bind_icon subject alike.
 *
 * Layout:
 * - Icon + text: Horizontal flex layout with small gap between
 * - Icon only: Centered icon
 * - Text only: Centered text (original behavior)
 *
 * Auto-contrast is computed using luminance formula:
 *   L = (299*R + 587*G + 114*B) / 1000
 * If L < 128 (dark bg): light text/icon (ThemeManager text contrast helpers)
 * If L >= 128 (light bg): dark text/icon (ThemeManager text contrast helpers)
 */

/**
 * @brief Initialize the ui_button custom widget
 *
 * Registers the <ui_button> XML widget with LVGL's XML parser.
 * Must be called after lv_xml_init() and after theme is initialized.
 */
void ui_button_init();

/**
 * @brief Drop the memoized icon_font_sm lookup so the next button re-reads it
 *
 * icon_font_sm is responsive, and this widget resolves it once for the whole
 * process. Called from theme_manager_register_responsive_fonts() so a
 * breakpoint change reaches button icons too (#1210); see
 * ui_icon_invalidate_font_cache() for the same problem in <icon>.
 */
void ui_button_invalidate_icon_font_cache();

/**
 * @brief Set the text of a ui_button's label
 * @param btn The ui_button widget
 * @param text New label text
 *
 * Safe to call on any lv_obj — returns silently if not a ui_button
 * or if the button has no label (icon-only).
 */
void ui_button_set_text(lv_obj_t* btn, const char* text);

/**
 * @brief Change the icon of an existing ui_button at runtime
 *
 * Looks up the icon name via ui_icon::lookup_codepoint and updates the
 * icon label text. Safe to call on any lv_obj — returns silently if not
 * a ui_button or if the button has no icon.
 */
void ui_button_set_icon(lv_obj_t* btn, const char* icon_name);

/**
 * @brief Show or hide the internal label of a ui_button
 *
 * Toggles LV_OBJ_FLAG_HIDDEN on the button's label widget, leaving the
 * icon untouched. The label's text and translation tag are preserved, so
 * restoring visibility requires no re-bind. Safe to call on any lv_obj —
 * returns silently if not a ui_button or if the button has no label.
 */
void ui_button_set_label_hidden(lv_obj_t* btn, bool hidden);

/**
 * @brief Get the internal icon glyph label of a ui_button
 *
 * Returns the lv_label that renders the button's MDI icon glyph, or nullptr if
 * the button has no icon (text-only) or is not a ui_button. Intended for tests
 * and for callers that need to inspect/animate the icon slot.
 */
lv_obj_t* ui_button_get_icon(lv_obj_t* btn);

/**
 * @brief Get the op-state spinner arc of a ui_button, if one exists
 *
 * A ui_button created with bind_op_state lazily creates an animated arc spinner
 * in its icon slot the first time it enters the busy state. Returns that arc, or
 * nullptr if the button has no op-state binding / has never gone busy / is not a
 * ui_button. Intended for tests.
 */
lv_obj_t* ui_button_get_op_spinner(lv_obj_t* btn);
