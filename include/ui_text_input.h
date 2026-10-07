// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

/**
 * @brief Keyboard mode hint for text inputs.
 *
 * Specifies which keyboard mode should be shown initially when the
 * textarea receives focus. The hint is stored on the textarea and
 * read by ui_keyboard_show() to set the initial keyboard mode.
 */
enum class KeyboardHint : uint8_t {
    TEXT = 0,    ///< Default text keyboard (lowercase letters)
    NUMERIC = 1, ///< Numeric keyboard (?123 mode with numbers/symbols)
};

/**
 * @brief Initialize the text_input custom widget for XML usage.
 *
 * Creates <text_input> which wraps lv_textarea with bind_text support.
 * This is a semantic widget that provides:
 * - Reactive data binding via bind_text attribute
 * - Keyboard hint for initial keyboard mode (keyboard_hint attribute)
 * - Optional clear button (Android-style, shown when text is present)
 * - Optional suffix: muted text such as a unit, inside the field's right end
 * - Responsive sizing with automatic vertical padding
 * - One-line mode by default for form inputs
 *
 * @par Usage in XML:
 * @code{.xml}
 * <text_input name="ip_input" bind_text="connection_ip" placeholder_text="127.0.0.1" width="100%"/>
 * <text_input name="port_input" bind_text="connection_port" keyboard_hint="numeric" width="100%"/>
 * <text_input name="search" show_clear_button="true" clear_callback="on_my_clear"
 * placeholder_text="Search..."/>
 * <text_input name="temp" bind_text="my_value" suffix="°C" style_text_align="right"/>
 * <text_input name="speed" bind_text="my_value" bind_suffix="my_unit_subject"/>
 * @endcode
 *
 * The bind_text attribute creates a reactive observer - when the subject
 * is updated via lv_subject_copy_string(), the textarea automatically updates.
 *
 * The keyboard_hint attribute specifies which keyboard mode to show initially:
 * - "text" (default): Standard lowercase letter keyboard
 * - "numeric": Numeric/symbol keyboard (?123 mode)
 *
 * The show_clear_button attribute adds an Android-style clear (X) icon inside
 * the right side of the input. It auto-shows when text is present and auto-hides
 * when empty. The optional clear_callback fires a registered XML event callback
 * after clearing the text.
 *
 * The suffix attribute (static text) or bind_suffix attribute (string subject)
 * shows muted text inside the right end of the field, after the typed text and
 * before the clear button. The field's right padding follows the suffix's
 * rendered width, so right-aligned text ends right beside it; an empty suffix
 * takes no space.
 *
 * @note All standard lv_textarea attributes are supported (placeholder_text, password_mode, etc.)
 */
void ui_text_input_init();

/**
 * @brief Get the keyboard hint for a textarea.
 *
 * Returns the keyboard hint that was set via the keyboard_hint XML attribute.
 * If no hint was set, returns KeyboardHint::TEXT.
 *
 * @param textarea The textarea to query
 * @return The keyboard hint for this textarea
 */
KeyboardHint ui_text_input_get_keyboard_hint(lv_obj_t* textarea);
