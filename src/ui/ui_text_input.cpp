// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_text_input.cpp
 * @brief Custom <text_input> XML widget with reactive data binding.
 *
 * Provides a semantic wrapper around lv_textarea that adds bind_text support,
 * similar to how lv_label has lv_label_bind_text(). LVGL's native textarea
 * doesn't support XML binding, so we implement it here using the observer pattern.
 *
 * Also supports keyboard_hint attribute, optional Android-style clear button, and
 * an optional muted suffix (a unit such as "°C") shown inside the field's right end.
 */

#include "ui_text_input.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_keyboard_manager.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_textarea_parser.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_observer_private.h" // For lv_observer_t internals
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>

/**
 * Magic value to identify text_input widgets and store keyboard hint.
 * The user_data is structured as: (MAGIC | hint_value)
 * This allows us to both identify text_input widgets and store their hint.
 */
static constexpr uintptr_t TEXT_INPUT_MAGIC = 0xBADC0DE0;
static constexpr uintptr_t TEXT_INPUT_MAGIC_MASK = 0xFFFFFFF0;
static constexpr uintptr_t TEXT_INPUT_HINT_MASK = 0x0000000F;

// Size of the clear button icon (matches mdi_icons_24 font)
static constexpr int32_t CLEAR_BTN_SIZE = 24;
// Right padding the clear button adds to the textarea for itself.
static constexpr int32_t CLEAR_BTN_EXTRA_PAD = CLEAR_BTN_SIZE + 4;

// ============================================================================
// Observer / Binding Callbacks
// ============================================================================

/**
 * Thread-local flag to prevent reentrancy during two-way binding updates.
 */
static thread_local bool g_updating_from_textarea = false;

/**
 * Observer callback - updates textarea when subject changes.
 */
static void textarea_text_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    if (subject->type == LV_SUBJECT_TYPE_STRING || subject->type == LV_SUBJECT_TYPE_POINTER) {
        // With max_length set, lv_textarea_set_text() adds one character at a
        // time and fires VALUE_CHANGED after each. Writing that partial text
        // back would overwrite the subject buffer it is still reading from.
        g_updating_from_textarea = true;
        lv_textarea_set_text(static_cast<lv_obj_t*>(observer->target),
                             static_cast<const char*>(subject->value.pointer));
        g_updating_from_textarea = false;
    }
}

/**
 * Event callback - updates subject when textarea text changes.
 */
static void textarea_value_changed_cb(lv_event_t* e) {
    if (g_updating_from_textarea) {
        return;
    }

    lv_obj_t* textarea = lv_event_get_target_obj(e);
    lv_subject_t* subject = static_cast<lv_subject_t*>(lv_event_get_user_data(e));

    if (subject == nullptr) {
        return;
    }

    if (subject->type != LV_SUBJECT_TYPE_STRING && subject->type != LV_SUBJECT_TYPE_POINTER) {
        return;
    }

    const char* new_text = lv_textarea_get_text(textarea);
    if (new_text == nullptr) {
        return;
    }

    const char* subject_text = lv_subject_get_string(subject);

    if (subject_text == nullptr || std::strcmp(new_text, subject_text) != 0) {
        g_updating_from_textarea = true;
        lv_subject_copy_string(subject, new_text);
        g_updating_from_textarea = false;
    }
}

// ============================================================================
// Clear Button Support
// ============================================================================

/**
 * Find the clear button icon child of a textarea.
 * The clear button is identified by having the name "text_input_clear_btn".
 */
static lv_obj_t* find_clear_btn(lv_obj_t* textarea) {
    return lv_obj_find_by_name(textarea, "text_input_clear_btn");
}

/**
 * Update clear button visibility based on textarea text content.
 */
static void update_clear_btn_visibility(lv_obj_t* textarea) {
    lv_obj_t* clear_btn = find_clear_btn(textarea);
    if (!clear_btn) {
        return;
    }

    const char* text = lv_textarea_get_text(textarea);
    bool has_text = text && text[0] != '\0';
    lv_obj_set_flag(clear_btn, LV_OBJ_FLAG_HIDDEN, !has_text);
}

/**
 * Value changed handler that toggles clear button visibility.
 */
static void clear_btn_value_changed_cb(lv_event_t* e) {
    lv_obj_t* textarea = lv_event_get_target_obj(e);
    update_clear_btn_visibility(textarea);
}

/**
 * Click handler for the clear button.
 * Clears the textarea text, hides the button, and optionally fires a callback.
 */
static void clear_btn_clicked_cb(lv_event_t* e) {
    lv_obj_t* clear_btn = lv_event_get_target_obj(e);
    lv_obj_t* textarea = lv_obj_get_parent(clear_btn);

    if (!textarea) {
        return;
    }

    // Clear the text
    lv_textarea_set_text(textarea, "");

    // Hide the clear button
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_HIDDEN);

    // Fire the optional clear callback directly (stored as user_data on the clear button).
    // This bypasses the normal value_changed debounce path, letting the panel
    // repopulate immediately after clearing.
    auto clear_cb = reinterpret_cast<lv_event_cb_t>(lv_obj_get_user_data(clear_btn));
    if (clear_cb) {
        clear_cb(e);
    }
}

/**
 * Create the clear button icon as a floating child of the textarea.
 *
 * Uses LV_OBJ_FLAG_FLOATING so it doesn't participate in scroll/layout,
 * and aligns to right-mid of the textarea.
 */
static lv_obj_t* create_clear_button(lv_obj_t* textarea) {
    lv_obj_t* btn = lv_label_create(textarea);
    lv_obj_set_name(btn, "text_input_clear_btn");

    // Set the close-circle icon glyph
    const char* glyph = helix::ui::icon::lookup_codepoint("close_circle");
    if (glyph) {
        lv_label_set_text(btn, glyph);
    }

    // Use 24px MDI icon font
    lv_obj_set_style_text_font(btn, &mdi_icons_24, 0);

    // Match the textarea's own text color so it looks natural
    lv_color_t text_color = lv_obj_get_style_text_color(textarea, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, text_color, 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_70, 0);

    // Make it floating so it doesn't scroll with text or participate in layout
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN); // Hidden by default (no text yet)

    // Add right padding to textarea so text doesn't overlap the button
    int32_t original_pad = lv_obj_get_style_pad_right(textarea, LV_PART_MAIN);
    int32_t extra_pad = CLEAR_BTN_EXTRA_PAD;
    lv_obj_set_style_pad_right(textarea, original_pad + extra_pad, 0);

    // Alignment is relative to the content area (inside padding).
    // Push the icon back into the padding zone so it sits near the widget's right edge.
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, extra_pad, 0);

    // Click handler
    lv_obj_add_event_cb(btn, clear_btn_clicked_cb, LV_EVENT_CLICKED, nullptr);

    spdlog::trace("[text_input] Created clear button");
    return btn;
}

// ============================================================================
// Suffix Support
// ============================================================================

/**
 * Lay out the suffix inside the textarea's right padding, between the text and
 * the clear button when there is one. Suffixes differ in width ("°C", "mm/s"),
 * so the padding follows the label's measured width; an empty suffix reserves
 * nothing.
 *
 * The suffix label's user_data holds the textarea's right padding as it was
 * before the suffix existed (clear-button reserve included).
 */
static void layout_suffix(lv_obj_t* suffix) {
    lv_obj_t* textarea = lv_obj_get_parent(suffix);
    if (!textarea) {
        return;
    }
    const auto base_pad =
        static_cast<int32_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(suffix)));
    const char* text = lv_label_get_text(suffix);
    const bool has_suffix = text && text[0] != '\0';
    const int32_t reserve =
        has_suffix ? lv_obj_get_width(suffix) + theme_manager_get_spacing("space_xs") : 0;

    if (lv_obj_get_style_pad_right(textarea, LV_PART_MAIN) != base_pad + reserve) {
        lv_obj_set_style_pad_right(textarea, base_pad + reserve, LV_PART_MAIN);
    }
    // Alignment is relative to the content area, so both float back out into the padding.
    lv_obj_align(suffix, LV_ALIGN_RIGHT_MID, reserve, 0);
    if (lv_obj_t* clear_btn = find_clear_btn(textarea)) {
        lv_obj_align(clear_btn, LV_ALIGN_RIGHT_MID, CLEAR_BTN_EXTRA_PAD + reserve, 0);
    }
}

/// The label's width is only known after a layout pass, which is also when it changes.
static void suffix_size_changed_cb(lv_event_t* e) {
    layout_suffix(lv_event_get_target_obj(e));
}

/**
 * Create the suffix label as a floating, muted child of the textarea.
 */
static lv_obj_t* create_suffix(lv_obj_t* textarea) {
    lv_obj_t* suffix = lv_label_create(textarea);
    lv_obj_set_name(suffix, "text_input_suffix");
    lv_label_set_text(suffix, "");
    lv_obj_set_style_text_font(suffix, theme_manager_get_font("font_body"), LV_PART_MAIN);
    lv_obj_add_style(suffix, ThemeManager::instance().get_style(StyleRole::TextMuted),
                     LV_PART_MAIN);
    lv_obj_add_flag(suffix, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(suffix, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(suffix, reinterpret_cast<void*>(static_cast<intptr_t>(
                                     lv_obj_get_style_pad_right(textarea, LV_PART_MAIN))));
    lv_obj_add_event_cb(suffix, suffix_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);
    return suffix;
}

// ============================================================================
// XML Widget Callbacks
// ============================================================================

/**
 * XML create callback for <text_input>.
 *
 * Creates a textarea with sensible defaults for form inputs:
 * - Responsive vertical padding from theme
 * - One-line mode enabled
 * - Default keyboard hint (TEXT)
 */
static void* ui_text_input_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* textarea = lv_textarea_create(parent);

    // Resolve text_font at creation so forced style refreshes (e.g. theme preview)
    // cannot catch inner label/placeholder/cursor children with a NULL font, which
    // crashes lv_font_get_line_height() during layout recalculation.
    lv_obj_set_style_text_font(textarea, theme_manager_get_font("font_body"), LV_PART_MAIN);

    // Apply responsive padding for consistent height across screen sizes
    const char* pad_ver = lv_xml_get_const(nullptr, "space_md");
    if (pad_ver) {
        int32_t padding = std::atoi(pad_ver);
        lv_obj_set_style_pad_ver(textarea, padding, 0);
    }

    // Note: Border and background styling is handled by the theme's apply_cb,
    // which adds the outlined InputBg style to all textareas.
    // We don't set inline styles here as they would override the theme.

    // One-line mode by default for form inputs
    lv_textarea_set_one_line(textarea, true);

    // Set default keyboard hint (TEXT) via user_data magic value
    lv_obj_set_user_data(
        textarea,
        reinterpret_cast<void*>(TEXT_INPUT_MAGIC | static_cast<uintptr_t>(KeyboardHint::TEXT)));

    // Auto-register with keyboard manager for software keyboard support.
    //
    // Deliberately the plain overload: this runs before lv_xml_textarea_apply()
    // has processed password_mode, so any flag passed here would be read too
    // early and always be false. KeyboardManager::is_password_context() instead
    // reads the textarea's live state at the moment it logs, which also tracks a
    // "show password" control toggling it later.
    KeyboardManager::instance().register_textarea(textarea);

    return textarea;
}

/**
 * XML apply callback for <text_input>.
 *
 * First applies standard textarea properties (via lv_xml_textarea_apply),
 * then handles our custom attributes:
 * - bind_text: reactive data binding
 * - keyboard_hint: initial keyboard mode hint
 * - show_clear_button: Android-style clear button
 * - clear_callback: optional XML event callback fired on clear
 */
static void ui_text_input_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* textarea = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));

    // Pre-scan for multiline BEFORE standard apply, so height/size attrs are applied
    // after one_line mode is already correct (prevents auto-sizing from overriding height)
    for (int i = 0; attrs[i]; i += 2) {
        if (lv_streq("multiline", attrs[i]) && lv_streq("true", attrs[i + 1])) {
            lv_textarea_set_one_line(textarea, false);
            break;
        }
    }

    // Apply standard textarea properties (handles height, width, etc.)
    lv_xml_textarea_apply(state, attrs);

    // Track clear button and suffix settings from attrs
    bool show_clear = false;
    const char* clear_callback_name = nullptr;
    const char* suffix_text = nullptr;
    lv_subject_t* suffix_subject = nullptr;

    // Then handle our custom attributes
    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (lv_streq("placeholder", name)) {
            lv_textarea_set_placeholder_text(textarea, value);
        } else if (lv_streq("max_length", name)) {
            lv_textarea_set_max_length(textarea, lv_xml_atoi(value));
        } else if (lv_streq("bind_text", name)) {
            lv_subject_t* subject = lv_xml_get_subject(&state->scope, value);
            if (subject == nullptr) {
                spdlog::warn("[text_input] Subject '{}' not found for bind_text", value);
                continue;
            }

            if (subject->type != LV_SUBJECT_TYPE_STRING &&
                subject->type != LV_SUBJECT_TYPE_POINTER) {
                spdlog::warn("[text_input] Subject '{}' has incompatible type {}", value,
                             static_cast<int>(subject->type));
                continue;
            }

            lv_subject_add_observer_obj(subject, textarea_text_observer_cb, textarea, nullptr);
            lv_obj_add_event_cb(textarea, textarea_value_changed_cb, LV_EVENT_VALUE_CHANGED,
                                subject);

            spdlog::trace("[text_input] Bound subject '{}' to textarea (two-way)", value);
        } else if (lv_streq("multiline", name)) {
            // Handled in pre-scan above
        } else if (lv_streq("keyboard_hint", name)) {
            KeyboardHint hint = KeyboardHint::TEXT;

            if (std::strcmp(value, "numeric") == 0) {
                hint = KeyboardHint::NUMERIC;
            } else if (std::strcmp(value, "text") == 0) {
                hint = KeyboardHint::TEXT;
            } else {
                spdlog::warn("[text_input] Unknown keyboard_hint '{}', using TEXT", value);
            }

            lv_obj_set_user_data(
                textarea, reinterpret_cast<void*>(TEXT_INPUT_MAGIC | static_cast<uintptr_t>(hint)));
        } else if (lv_streq("show_clear_button", name)) {
            show_clear = lv_streq("true", value);
        } else if (lv_streq("clear_callback", name)) {
            clear_callback_name = value;
        } else if (lv_streq("suffix", name)) {
            suffix_text = value;
        } else if (lv_streq("bind_suffix", name)) {
            suffix_subject = lv_xml_get_subject(&state->scope, value);
            if (suffix_subject == nullptr) {
                spdlog::warn("[text_input] Subject '{}' not found for bind_suffix", value);
            }
        }
    }

    // Create clear button after all attrs are processed (so padding is correct)
    if (show_clear) {
        lv_obj_t* clear_btn = create_clear_button(textarea);

        // If a clear_callback was specified, look up the registered XML event callback
        if (clear_callback_name) {
            lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, clear_callback_name);
            if (cb) {
                lv_obj_set_user_data(clear_btn, reinterpret_cast<void*>(cb));
            } else {
                spdlog::warn("[text_input] clear_callback '{}' not found", clear_callback_name);
            }
        }

        // Add value_changed handler to toggle clear button visibility
        lv_obj_add_event_cb(textarea, clear_btn_value_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // After the clear button, so the suffix's base padding includes its reserve.
    if (suffix_text || suffix_subject) {
        lv_obj_t* suffix = create_suffix(textarea);
        if (suffix_subject) {
            lv_label_bind_text(suffix, suffix_subject, nullptr);
        } else {
            lv_label_set_text(suffix, suffix_text);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_text_input_init() {
    lv_xml_register_widget("text_input", ui_text_input_create, ui_text_input_apply);
    spdlog::debug("[ui_text_input] Registered <text_input> widget");
}

KeyboardHint ui_text_input_get_keyboard_hint(lv_obj_t* textarea) {
    if (textarea == nullptr) {
        return KeyboardHint::TEXT;
    }

    auto user_data = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(textarea));

    if ((user_data & TEXT_INPUT_MAGIC_MASK) != TEXT_INPUT_MAGIC) {
        return KeyboardHint::TEXT;
    }

    return static_cast<KeyboardHint>(user_data & TEXT_INPUT_HINT_MASK);
}
