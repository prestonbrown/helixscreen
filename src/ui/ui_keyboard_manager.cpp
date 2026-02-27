// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_keyboard_manager.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_text_input.h"
#include "ui_utils.h"

#include "config.h"
#include "display_settings_manager.h"
#include "keyboard_layout_provider.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

using namespace helix;

// Macro for keyboard button control flags
#define LV_KB_BTN(width) static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | (width))

// Animation timing constants
static constexpr int32_t KEYBOARD_SLIDE_DURATION_MS = 200;
static constexpr int32_t KEYBOARD_EXIT_DURATION_MS = 150;

// ============================================================================
// STATIC DATA
// ============================================================================

// Alternative character mapping table (Gboard-style layout)
const KeyboardManager::AltCharMapping KeyboardManager::alt_char_map_[] = {
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
    // Middle row (A-L) -> symbols
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
    // Bottom row (Z-M) -> symbols
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
    {0, nullptr} // Sentinel
};

// Improved numeric keyboard with PERIOD
static const char* const kb_map_num_improved[] = {
    "1",   "2", "3", ICON_KEYBOARD_CLOSE, "\n",
    "4",   "5", "6", ICON_CHECK,          "\n",
    "7",   "8", "9", ICON_BACKSPACE,      "\n",
    "+/-", "0", ".", ICON_CHEVRON_LEFT,   ICON_CHEVRON_RIGHT,
    ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_num_improved[] = {
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(2),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED | 1),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED | 1)};

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

KeyboardManager& KeyboardManager::instance() {
    static KeyboardManager instance;
    return instance;
}

// ============================================================================
// HELPER METHODS
// ============================================================================

const char* KeyboardManager::find_alternatives(char base_char) const {
    for (size_t i = 0; alt_char_map_[i].alternatives != nullptr; i++) {
        if (alt_char_map_[i].base_char == base_char) {
            return alt_char_map_[i].alternatives;
        }
    }
    return nullptr;
}

bool KeyboardManager::point_in_area(const lv_area_t* area, const lv_point_t* point) const {
    return (point->x >= area->x1 && point->x <= area->x2 && point->y >= area->y1 &&
            point->y <= area->y2);
}

void KeyboardManager::overlay_cleanup() {
    helix::ui::safe_delete(overlay_);
}

void KeyboardManager::longpress_reset() {
    overlay_cleanup();
    alternatives_ = nullptr;
    pressed_char_ = 0;
    pressed_btn_id_ = 0;
}

void KeyboardManager::show_overlay(const lv_area_t* key_area, const char* alternatives) {
    if (!alternatives || !alternatives[0]) {
        spdlog::debug("[KeyboardManager] No alternatives to display");
        return;
    }

    overlay_cleanup();

    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[KeyboardManager] show_overlay: no active screen");
        return;
    }

    overlay_ = lv_obj_create(screen);
    if (!overlay_) {
        spdlog::error("[KeyboardManager] show_overlay: failed to create overlay");
        return;
    }

    size_t alt_count = strlen(alternatives);
    const int32_t char_width = 50;
    const int32_t char_height = 60;
    const int32_t padding = 8;
    int32_t overlay_width = (static_cast<int32_t>(alt_count) * char_width) + (padding * 2);
    int32_t overlay_height = char_height;

    lv_obj_set_size(overlay_, overlay_width, overlay_height);

    const char* card_bg_str =
        lv_xml_get_const(NULL, theme_manager_is_dark_mode() ? "card_bg_dark" : "card_bg_light");
    if (card_bg_str) {
        lv_obj_set_style_bg_color(overlay_, theme_manager_parse_hex_color(card_bg_str),
                                  LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay_, 2, LV_PART_MAIN);

    const char* border_color_str = lv_xml_get_const(NULL, "success");
    if (border_color_str) {
        lv_obj_set_style_border_color(overlay_, theme_manager_parse_hex_color(border_color_str),
                                      LV_PART_MAIN);
    }

    lv_obj_set_style_radius(overlay_, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(overlay_, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(overlay_, LV_OPA_30, LV_PART_MAIN);

    lv_obj_set_flex_flow(overlay_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(overlay_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(overlay_, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay_, padding, LV_PART_MAIN);

    const char* text_color_str =
        lv_xml_get_const(NULL, theme_manager_is_dark_mode() ? "text_dark" : "text_light");
    lv_color_t text_color = text_color_str ? theme_manager_parse_hex_color(text_color_str)
                                           : theme_manager_get_color("text");

    for (size_t i = 0; i < alt_count; i++) {
        lv_obj_t* label = lv_label_create(overlay_);
        char char_str[2] = {alternatives[i], '\0'};
        lv_label_set_text(label, char_str);
        lv_obj_set_style_text_font(label, &noto_sans_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_user_data(label, (void*)(uintptr_t)alternatives[i]);
    }

    int32_t key_center_x = (key_area->x1 + key_area->x2) / 2;
    int32_t overlay_x = key_center_x - (overlay_width / 2);
    int32_t overlay_y = key_area->y1 - overlay_height - 10;

    int32_t screen_width = lv_obj_get_width(screen);

    if (overlay_x < 0) {
        overlay_x = 0;
    } else if (overlay_x + overlay_width > screen_width) {
        overlay_x = screen_width - overlay_width;
    }

    if (overlay_y < 0) {
        overlay_y = key_area->y2 + 10;
    }

    lv_obj_set_pos(overlay_, overlay_x, overlay_y);
    lv_obj_move_foreground(overlay_);
    lv_obj_update_layout(overlay_);

    spdlog::info("[KeyboardManager] Showing overlay with {} alternatives at ({}, {})", alt_count,
                 overlay_x, overlay_y);
}

void KeyboardManager::apply_keyboard_mode() {
    if (keyboard_ == nullptr) {
        return;
    }

    spdlog::trace("[KeyboardManager] apply_keyboard_mode() called, mode={}", (int)mode_);

    keyboard_layout_mode_t layout_mode;
    const char* mode_name = "";

    switch (mode_) {
    case MODE_ALPHA_LC:
        layout_mode = KEYBOARD_LAYOUT_ALPHA_LC;
        mode_name = "alpha lowercase";
        break;
    case MODE_ALPHA_UC:
        layout_mode = KEYBOARD_LAYOUT_ALPHA_UC;
        mode_name = caps_lock_ ? "alpha uppercase (CAPS LOCK)" : "alpha uppercase (one-shot)";
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

    const char* const* map = keyboard_layout_get_map(layout_mode, caps_lock_);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(layout_mode);

    lv_buttonmatrix_set_map(keyboard_, map);
    lv_buttonmatrix_set_ctrl_map(keyboard_, ctrl_map);

    spdlog::debug("[KeyboardManager] Switched to {}", mode_name);
    lv_obj_invalidate(keyboard_);
}

// ============================================================================
// EVENT CALLBACKS
// ============================================================================

void KeyboardManager::textarea_focus_event_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("textarea_focus_event_cb");

    auto& mgr = KeyboardManager::instance();
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* textarea = lv_event_get_target_obj(e);

    if (code == LV_EVENT_FOCUSED) {
        spdlog::debug("[KeyboardManager] Textarea focused: {}", (void*)textarea);
        mgr.context_textarea_ = textarea;
        mgr.show(textarea);
    } else if (code == LV_EVENT_DEFOCUSED) {
        spdlog::debug("[KeyboardManager] Textarea defocused: {}", (void*)textarea);
        mgr.context_textarea_ = nullptr;
        mgr.hide();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void KeyboardManager::textarea_delete_event_cb(lv_event_t* e) {
    auto& mgr = KeyboardManager::instance();
    lv_obj_t* textarea = lv_event_get_target_obj(e);

    if (mgr.context_textarea_ == textarea) {
        spdlog::debug("[KeyboardManager] Textarea deleted while focused: {}", (void*)textarea);
        mgr.context_textarea_ = nullptr;
        mgr.hide();
    }
}

void KeyboardManager::longpress_event_handler(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("longpress_event_handler");

    auto& mgr = KeyboardManager::instance();
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* keyboard = lv_event_get_target_obj(e);

    if (keyboard == nullptr) {
        spdlog::warn("[KeyboardManager] longpress_event_handler: keyboard target is NULL");
        return;
    }

    spdlog::debug("[KeyboardManager] EVENT RECEIVED: code={}", (int)code);

    if (code == LV_EVENT_PRESSED) {
        uint32_t btn_id = lv_buttonmatrix_get_selected_button(keyboard);
        const char* btn_text = lv_buttonmatrix_get_button_text(keyboard, btn_id);

        if (btn_text && (strcmp(btn_text, "XYZ") == 0 || strcmp(btn_text, "?123") == 0)) {
            return;
        }

        bool is_non_printing =
            lv_buttonmatrix_has_button_ctrl(keyboard, btn_id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);

        if (is_non_printing) {
            return;
        }

        mgr.longpress_state_ = LP_PRESSED;
        mgr.pressed_btn_id_ = btn_id;
        mgr.pressed_char_ = (btn_text && btn_text[0]) ? btn_text[0] : 0;

        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &mgr.press_point_);
        }

        if (btn_text && btn_text[0] && !btn_text[1]) {
            mgr.alternatives_ = mgr.find_alternatives(btn_text[0]);
            if (mgr.alternatives_) {
                spdlog::debug("[KeyboardManager] PRESSED '{}' - has alternatives: '{}'",
                              btn_text[0], mgr.alternatives_);
            }
        }

    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (mgr.longpress_state_ == LP_PRESSED && mgr.alternatives_ != nullptr) {
            mgr.longpress_state_ = LP_LONG_DETECTED;

            lv_area_t btn_area;
            btn_area.x1 = mgr.press_point_.x - 25;
            btn_area.x2 = mgr.press_point_.x + 25;
            btn_area.y1 = mgr.press_point_.y - 25;
            btn_area.y2 = mgr.press_point_.y + 25;

            mgr.pressed_key_area_ = btn_area;
            mgr.show_overlay(&btn_area, mgr.alternatives_);

            // Auto-insert mode: insert the first alt char immediately on long-press
            if (mgr.auto_insert_alt_ && mgr.alternatives_[0] && mgr.context_textarea_ != nullptr) {
                char str[2] = {mgr.alternatives_[0], '\0'};
                lv_textarea_add_text(mgr.context_textarea_, str);
                mgr.longpress_state_ = LP_ALT_SELECTED;
                spdlog::info("[KeyboardManager] LONG_PRESSED '{}' - auto-inserted alt '{}'",
                             mgr.pressed_char_ ? mgr.pressed_char_ : '?', mgr.alternatives_[0]);
            } else {
                spdlog::info("[KeyboardManager] LONG_PRESSED detected for '{}' - overlay shown",
                             mgr.pressed_char_ ? mgr.pressed_char_ : '?');
            }
        }

    } else if (code == LV_EVENT_RELEASED) {
        spdlog::info("[KeyboardManager] RELEASED event - state={}, overlay={}, textarea={}",
                     (int)mgr.longpress_state_, (void*)mgr.overlay_, (void*)mgr.context_textarea_);

        if (mgr.longpress_state_ == LP_ALT_SELECTED) {
            // Auto-insert mode: alt char already inserted, just clean up
            spdlog::info("[KeyboardManager] Cleaning up overlay (alt already auto-inserted)");
            mgr.longpress_reset();
            mgr.longpress_state_ = LP_IDLE;

        } else if (mgr.longpress_state_ == LP_LONG_DETECTED && mgr.overlay_ != nullptr) {
            // Slide-to-select mode: check release position to pick a character
            lv_indev_t* indev = lv_indev_active();
            lv_point_t release_point;

            spdlog::info("[KeyboardManager] Long-press mode active, checking release position");

            if (indev) {
                lv_indev_get_point(indev, &release_point);
                spdlog::info("[KeyboardManager] Release point: ({}, {})", release_point.x,
                             release_point.y);

                lv_area_t overlay_area;
                lv_obj_get_coords(mgr.overlay_, &overlay_area);

                bool release_in_overlay = mgr.point_in_area(&overlay_area, &release_point);
                spdlog::info("[KeyboardManager] Release in overlay area: {}", release_in_overlay);

                uint32_t child_count = lv_obj_get_child_count(mgr.overlay_);
                char selected_char = 0;

                if (release_in_overlay && child_count > 0) {
                    int32_t min_dist = INT32_MAX;
                    for (uint32_t i = 0; i < child_count; i++) {
                        lv_obj_t* label = lv_obj_get_child(mgr.overlay_, static_cast<int32_t>(i));
                        lv_area_t label_area;
                        lv_obj_get_coords(label, &label_area);
                        char label_char = (char)(uintptr_t)lv_obj_get_user_data(label);

                        int32_t label_center_x = (label_area.x1 + label_area.x2) / 2;
                        int32_t dist = abs(release_point.x - label_center_x);

                        if (dist < min_dist) {
                            min_dist = dist;
                            selected_char = label_char;
                        }
                    }
                    spdlog::info("[KeyboardManager] Selected nearest label '{}' (dist={})",
                                 selected_char, min_dist);
                }

                if (selected_char != 0 && mgr.context_textarea_ != nullptr) {
                    char str[2] = {selected_char, '\0'};
                    lv_textarea_add_text(mgr.context_textarea_, str);
                    spdlog::info("[KeyboardManager] Inserted alternative character: '{}'",
                                 selected_char);
                } else if (mgr.point_in_area(&mgr.pressed_key_area_, &release_point)) {
                    spdlog::info("[KeyboardManager] Release in original key area");
                    if (mgr.pressed_char_ != 0 && mgr.context_textarea_ != nullptr) {
                        char str[2] = {mgr.pressed_char_, '\0'};
                        lv_textarea_add_text(mgr.context_textarea_, str);
                        spdlog::info("[KeyboardManager] Inserted primary character: '{}'",
                                     mgr.pressed_char_);
                    }
                } else {
                    spdlog::info("[KeyboardManager] Released outside - cancelled");
                }
            }

            spdlog::info("[KeyboardManager] Cleaning up overlay");
            mgr.longpress_reset();
            mgr.longpress_state_ = LP_IDLE;

        } else if (mgr.longpress_state_ == LP_PRESSED) {
            spdlog::debug("[KeyboardManager] Short press - normal input");
            mgr.longpress_reset();
            mgr.longpress_state_ = LP_IDLE;
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void KeyboardManager::keyboard_event_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("keyboard_event_cb");

    auto& mgr = KeyboardManager::instance();
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* keyboard = lv_event_get_target_obj(e);

    if (keyboard == nullptr) {
        spdlog::warn("[KeyboardManager] keyboard_event_cb: keyboard target is NULL");
        return;
    }

    if (code == LV_EVENT_READY) {
        spdlog::debug("[KeyboardManager] Enter pressed");
    } else if (code == LV_EVENT_CANCEL) {
        spdlog::debug("[KeyboardManager] Close pressed - hiding keyboard");
        mgr.hide();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (mgr.longpress_state_ == LP_LONG_DETECTED) {
            spdlog::debug("[KeyboardManager] Ignoring VALUE_CHANGED during long-press mode");
            return;
        }

        uint32_t btn_id = lv_buttonmatrix_get_selected_button(keyboard);
        const char* btn_text = lv_buttonmatrix_get_button_text(keyboard, btn_id);

        bool is_non_printing =
            lv_buttonmatrix_has_button_ctrl(keyboard, btn_id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);

        spdlog::trace(
            "[KeyboardManager] VALUE_CHANGED: btn_id={}, btn_text='{}', is_non_printing={}", btn_id,
            btn_text ? btn_text : "NULL", is_non_printing);

        if (is_non_printing) {
            if (btn_text && strcmp(btn_text, "?123") == 0) {
                mgr.mode_ = MODE_NUMBERS_SYMBOLS;
                mgr.shift_just_pressed_ = false;
                mgr.one_shot_shift_ = false;
                mgr.caps_lock_ = false;
                mgr.apply_keyboard_mode();
                spdlog::debug("[KeyboardManager] Mode switch: ?123 -> numbers/symbols");

            } else if (btn_text && strcmp(btn_text, "XYZ") == 0) {
                mgr.mode_ = MODE_ALPHA_LC;
                mgr.shift_just_pressed_ = false;
                mgr.one_shot_shift_ = false;
                mgr.caps_lock_ = false;
                mgr.apply_keyboard_mode();
                spdlog::debug("[KeyboardManager] Mode switch: AB -> alpha lowercase");

            } else if (btn_text && strcmp(btn_text, "#+=") == 0) {
                mgr.mode_ = MODE_ALT_SYMBOLS;
                mgr.apply_keyboard_mode();
                spdlog::debug("[KeyboardManager] Mode switch: #+= -> alternative symbols");

            } else if (btn_text && strcmp(btn_text, "123") == 0) {
                mgr.mode_ = MODE_NUMBERS_SYMBOLS;
                mgr.apply_keyboard_mode();
                spdlog::debug("[KeyboardManager] Mode switch: 123 -> numbers/symbols");

            } else if (btn_text && (strcmp(btn_text, ICON_KEYBOARD_SHIFT) == 0 ||
                                    strcmp(btn_text, ICON_KEYBOARD_CAPS) == 0)) {
                if (mgr.shift_just_pressed_ && !mgr.caps_lock_) {
                    mgr.caps_lock_ = true;
                    mgr.one_shot_shift_ = false;
                    mgr.shift_just_pressed_ = false;
                    mgr.mode_ = MODE_ALPHA_UC;
                    spdlog::debug("[KeyboardManager] Shift: Caps Lock ON");
                } else if (mgr.caps_lock_) {
                    mgr.caps_lock_ = false;
                    mgr.one_shot_shift_ = false;
                    mgr.shift_just_pressed_ = false;
                    mgr.mode_ = MODE_ALPHA_LC;
                    spdlog::debug("[KeyboardManager] Shift: Caps Lock OFF -> lowercase");
                } else {
                    mgr.one_shot_shift_ = true;
                    mgr.shift_just_pressed_ = true;
                    mgr.caps_lock_ = false;
                    mgr.mode_ = MODE_ALPHA_UC;
                    spdlog::debug("[KeyboardManager] Shift: One-shot uppercase");
                }
                mgr.apply_keyboard_mode();

            } else if (btn_text && strcmp(btn_text, ICON_KEYBOARD_RETURN) == 0) {
                if (mgr.context_textarea_) {
                    // Multiline textareas: insert newline, keep keyboard open
                    if (!lv_textarea_get_one_line(mgr.context_textarea_)) {
                        lv_textarea_add_char(mgr.context_textarea_, '\n');
                        spdlog::debug("[KeyboardManager] Enter: newline inserted (multiline)");
                        return;
                    }
                    // Single-line: fire READY so forms can handle Enter-to-next-field.
                    // Save current textarea — if a handler switches to another field via
                    // show(), context_textarea_ will change and we should NOT hide.
                    lv_obj_t* ta_before = mgr.context_textarea_;
                    lv_obj_send_event(ta_before, LV_EVENT_READY, nullptr);
                    if (mgr.context_textarea_ != ta_before) {
                        // Handler switched to a new textarea — keyboard already reassigned
                        spdlog::debug("[KeyboardManager] Enter: advanced to next field");
                        return;
                    }
                }
                mgr.hide();

            } else if (btn_text && strcmp(btn_text, ICON_KEYBOARD_CLOSE) == 0) {
                spdlog::debug("[KeyboardManager] Close button pressed");
                mgr.hide();

            } else if (btn_text && strcmp(btn_text, ICON_BACKSPACE) == 0) {
                if (mgr.context_textarea_) {
                    lv_textarea_delete_char(mgr.context_textarea_);
                }
                spdlog::debug("[KeyboardManager] Backspace");
            }
        } else {
            // Regular printing key — insert text into textarea
            if (mgr.context_textarea_ && btn_text) {
                if (strcmp(btn_text, keyboard_layout_get_spacebar_text()) == 0) {
                    lv_textarea_add_char(mgr.context_textarea_, ' ');
                    spdlog::debug("[KeyboardManager] Space");
                } else {
                    lv_textarea_add_text(mgr.context_textarea_, btn_text);
                }
            }

            mgr.shift_just_pressed_ = false;

            if (mgr.one_shot_shift_ && mgr.mode_ == MODE_ALPHA_UC) {
                mgr.one_shot_shift_ = false;
                mgr.mode_ = MODE_ALPHA_LC;
                mgr.apply_keyboard_mode();
                spdlog::debug("[KeyboardManager] One-shot shift: Reverting to lowercase");
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void KeyboardManager::keyboard_draw_alternative_chars(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("keyboard_draw_alternative_chars");

    auto& mgr = KeyboardManager::instance();
    lv_obj_t* keyboard = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);

    if (keyboard == nullptr)
        return;

    const char* const* map = lv_buttonmatrix_get_map(keyboard);
    if (!map)
        return;

    const char* gray_color_str = lv_xml_get_const(
        NULL, theme_manager_is_dark_mode() ? "text_muted_dark" : "text_muted_light");
    lv_color_t gray_color = gray_color_str ? theme_manager_parse_hex_color(gray_color_str)
                                           : theme_manager_get_color("text_muted");

    for (uint32_t i = 0; map[i][0] != '\0'; i++) {
        if (strcmp(map[i], "\n") == 0) {
            continue;
        }

        const char* btn_text = map[i];

        if (btn_text && btn_text[0] && !btn_text[1] && (unsigned char)btn_text[0] < 128) {
            const char* alternatives = mgr.find_alternatives(btn_text[0]);

            if (alternatives && alternatives[0]) {
                lv_area_t kb_coords;
                lv_obj_get_coords(keyboard, &kb_coords);

                lv_coord_t kb_width = lv_obj_get_width(keyboard);
                lv_coord_t kb_height = lv_obj_get_height(keyboard);
                lv_coord_t unit_width = kb_width / 40;
                lv_coord_t row_height = kb_height / 4;

                uint32_t row = 0;
                lv_coord_t cumulative_width = 0;

                for (uint32_t j = 0; j <= i; j++) {
                    if (strcmp(map[j], "\n") == 0) {
                        row++;
                        cumulative_width = 0;
                    } else if (j < i) {
                        const char* this_text = map[j];
                        lv_coord_t this_width;

                        if (strcmp(this_text, " ") == 0) {
                            this_width = 2 * unit_width;
                        } else if (strcmp(this_text, ICON_KEYBOARD_SHIFT) == 0 ||
                                   strcmp(this_text, ICON_KEYBOARD_CAPS) == 0 ||
                                   strcmp(this_text, ICON_BACKSPACE) == 0) {
                            this_width = 6 * unit_width;
                        } else {
                            this_width = 4 * unit_width;
                        }
                        cumulative_width += this_width;
                    }
                }

                lv_coord_t current_btn_width = 4 * unit_width;

                lv_coord_t btn_x = kb_coords.x1 + cumulative_width + current_btn_width - 10;
                lv_coord_t btn_y = kb_coords.y1 + static_cast<lv_coord_t>(row) * row_height + 6;

                lv_draw_label_dsc_t label_dsc;
                lv_draw_label_dsc_init(&label_dsc);
                label_dsc.font = &noto_sans_12;
                label_dsc.color = gray_color;
                label_dsc.opa = LV_OPA_60;

                char alt_str[2] = {alternatives[0], '\0'};
                label_dsc.text = alt_str;
                label_dsc.text_local = true;

                lv_area_t alt_area;
                alt_area.x1 = btn_x - 12;
                alt_area.y1 = btn_y;
                alt_area.x2 = btn_x;
                alt_area.y2 = btn_y + 14;

                lv_draw_label(layer, &label_dsc, &alt_area);
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// KEYBOARD MANAGER IMPLEMENTATION
// ============================================================================

void KeyboardManager::init(lv_obj_t* parent) {
    if (keyboard_ != nullptr) {
        spdlog::warn("[KeyboardManager] Already initialized, skipping");
        return;
    }

    spdlog::debug("[KeyboardManager] Initializing global keyboard");

    if (!keyboard_font_initialized_) {
        memcpy(&keyboard_font_, &noto_sans_20, sizeof(lv_font_t));
        keyboard_font_.fallback = &mdi_icons_24;
        keyboard_font_initialized_ = true;
        spdlog::debug("[KeyboardManager] Created font with MDI fallback");
    }

    keyboard_ = lv_keyboard_create(parent);

    // Remove LVGL's built-in keyboard handler — our custom handler manages all keys.
    // The default handler doesn't recognize our custom icon keys (ICON_BACKSPACE, etc.)
    // and inserts their UTF-8 bytes as text, causing corruption when cursor is mid-string.
    lv_obj_remove_event_cb(keyboard_, lv_keyboard_def_event_cb);

    lv_keyboard_set_mode(keyboard_, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(keyboard_, true);

    lv_keyboard_set_map(keyboard_, LV_KEYBOARD_MODE_NUMBER, kb_map_num_improved,
                        kb_ctrl_num_improved);

    spdlog::debug("[KeyboardManager] Using keyboard with long-press alternatives");
    mode_ = MODE_ALPHA_LC;
    apply_keyboard_mode();

    lv_color_t keyboard_bg = theme_manager_get_color("screen_bg");
    lv_color_t key_bg = theme_manager_get_color("card_bg");
    lv_color_t key_special_bg = theme_manager_get_color("overlay_bg");
    lv_color_t key_text = theme_manager_get_color("text");

    lv_obj_set_style_bg_color(keyboard_, keyboard_bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_bg_color(keyboard_, key_bg, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard_, 8, LV_PART_ITEMS);

    lv_obj_set_style_shadow_width(keyboard_, 2, LV_PART_ITEMS);
    lv_obj_set_style_shadow_opa(keyboard_, LV_OPA_30, LV_PART_ITEMS);
    lv_obj_set_style_shadow_offset_y(keyboard_, 1, LV_PART_ITEMS);
    lv_obj_set_style_shadow_color(keyboard_, lv_color_black(), LV_PART_ITEMS);

    lv_obj_set_style_bg_color(keyboard_, key_special_bg, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_set_style_text_font(keyboard_, &keyboard_font_, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard_, key_text, LV_PART_ITEMS);
    lv_obj_set_style_text_opa(keyboard_, LV_OPA_COVER, LV_PART_ITEMS);

    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(keyboard_, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_shadow_opa(keyboard_, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(keyboard_, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);

    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(keyboard_, keyboard_event_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard_, keyboard_event_cb, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(keyboard_, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_add_event_cb(keyboard_, longpress_event_handler, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(keyboard_, longpress_event_handler, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(keyboard_, longpress_event_handler, LV_EVENT_RELEASED, nullptr);

    lv_obj_add_event_cb(keyboard_, keyboard_draw_alternative_chars, LV_EVENT_DRAW_POST_END,
                        nullptr);

    initialized_ = true;
    spdlog::debug("[KeyboardManager] Initialization complete");
}

void KeyboardManager::register_textarea(lv_obj_t* textarea) {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    if (textarea == nullptr) {
        spdlog::error("[KeyboardManager] Cannot register NULL textarea");
        return;
    }

    spdlog::debug("[KeyboardManager] Registering textarea: {}", (void*)textarea);

    lv_obj_add_event_cb(textarea, textarea_focus_event_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(textarea, textarea_focus_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(textarea, textarea_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_group_t* default_group = lv_group_get_default();
    if (default_group) {
        lv_group_add_obj(default_group, textarea);
        spdlog::debug("[KeyboardManager] Added textarea to input group for physical keyboard");
    }
}

void KeyboardManager::register_textarea_ex(lv_obj_t* textarea, bool is_password) {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    if (textarea == nullptr) {
        spdlog::error("[KeyboardManager] Cannot register NULL textarea");
        return;
    }

    spdlog::debug("[KeyboardManager] Registering textarea: {} (password: {})", (void*)textarea,
                  is_password);

    register_textarea(textarea);
}

void KeyboardManager::show(lv_obj_t* textarea) {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    if (lv_obj_get_parent(keyboard_) == nullptr) {
        spdlog::debug("[KeyboardManager] Skipping show - keyboard is being cleaned up");
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        spdlog::debug("[KeyboardManager] Skipping show - no active screen");
        return;
    }

    spdlog::info("[KeyboardManager] Showing keyboard for textarea: {}", (void*)textarea);

    // Track which textarea receives key input from our custom handler
    context_textarea_ = textarea;

    // Cancel any in-progress hide animation
    lv_anim_delete(keyboard_, nullptr);

    KeyboardHint hint = ui_text_input_get_keyboard_hint(textarea);

    if (hint == KeyboardHint::NUMERIC) {
        mode_ = MODE_NUMBERS_SYMBOLS;
        spdlog::debug("[KeyboardManager] Using NUMERIC keyboard hint");
    } else {
        mode_ = MODE_ALPHA_LC;
    }
    apply_keyboard_mode();

    lv_keyboard_set_textarea(keyboard_, textarea);
    lv_obj_remove_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard_);
    lv_obj_update_layout(screen);

    // Animate keyboard sliding up from bottom
    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        int32_t keyboard_height = lv_obj_get_height(keyboard_);
        lv_obj_set_style_translate_y(keyboard_, keyboard_height, LV_PART_MAIN);

        lv_anim_t slide_anim;
        lv_anim_init(&slide_anim);
        lv_anim_set_var(&slide_anim, keyboard_);
        lv_anim_set_values(&slide_anim, keyboard_height, 0);
        lv_anim_set_time(&slide_anim, KEYBOARD_SLIDE_DURATION_MS);
        lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_start(&slide_anim);
    } else {
        lv_obj_set_style_translate_y(keyboard_, 0, LV_PART_MAIN);
    }

    if (!textarea) {
        return;
    }

    lv_area_t kb_coords, ta_coords;
    lv_obj_get_coords(keyboard_, &kb_coords);
    lv_obj_get_coords(textarea, &ta_coords);

    int32_t kb_top = kb_coords.y1;
    int32_t ta_bottom = ta_coords.y2;

    const int32_t padding = 20;
    int32_t desired_bottom = kb_top - padding;

    if (ta_bottom > desired_bottom) {
        int32_t shift_up = ta_bottom - desired_bottom;

        spdlog::debug("[KeyboardManager] Shifting screen UP by {} px", shift_up);

        uint32_t child_count = lv_obj_get_child_count(screen);
        bool animations_enabled = DisplaySettingsManager::instance().get_animations_enabled();

        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
            if (child == keyboard_)
                continue;

            int32_t current_y = lv_obj_get_y(child);
            int32_t target_y = current_y - shift_up;

            if (!animations_enabled) {
                lv_obj_set_y(child, target_y);
                continue;
            }

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, child);
            lv_anim_set_values(&a, current_y, target_y);
            lv_anim_set_time(&a, 200);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    }
}

void KeyboardManager::hide() {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    if (lv_obj_get_parent(keyboard_) == nullptr) {
        spdlog::debug("[KeyboardManager] Skipping hide - keyboard is being cleaned up");
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        spdlog::debug("[KeyboardManager] Skipping hide - no active screen");
        return;
    }

    spdlog::debug("[KeyboardManager] Hiding keyboard");

    // Cancel any in-progress show animation
    lv_anim_delete(keyboard_, nullptr);

    longpress_reset();
    longpress_state_ = LP_IDLE;

    lv_keyboard_set_textarea(keyboard_, nullptr);

    // Animate keyboard sliding down (or hide instantly if animations disabled)
    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        int32_t keyboard_height = lv_obj_get_height(keyboard_);

        lv_anim_t slide_anim;
        lv_anim_init(&slide_anim);
        lv_anim_set_var(&slide_anim, keyboard_);
        lv_anim_set_values(&slide_anim, 0, keyboard_height);
        lv_anim_set_time(&slide_anim, KEYBOARD_EXIT_DURATION_MS);
        lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
            lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
        });
        lv_anim_set_completed_cb(&slide_anim, [](lv_anim_t* anim) {
            lv_obj_t* kb = static_cast<lv_obj_t*>(anim->var);
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_translate_y(kb, 0, LV_PART_MAIN);
        });
        lv_anim_start(&slide_anim);
    } else {
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t child_count = lv_obj_get_child_count(screen);
    bool animations_enabled = DisplaySettingsManager::instance().get_animations_enabled();

    spdlog::debug("[KeyboardManager] Restoring screen children to y=0");

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
        if (child == keyboard_)
            continue;

        int32_t current_y = lv_obj_get_y(child);
        if (current_y != 0) {
            if (!animations_enabled) {
                lv_obj_set_y(child, 0);
                continue;
            }

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

bool KeyboardManager::is_visible() const {
    if (keyboard_ == nullptr) {
        return false;
    }
    return !lv_obj_has_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* KeyboardManager::get_instance() const {
    return keyboard_;
}

void KeyboardManager::set_mode(lv_keyboard_mode_t mode) {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    spdlog::debug("[KeyboardManager] Setting mode: {}", (int)mode);
    lv_keyboard_set_mode(keyboard_, mode);
}

void KeyboardManager::set_position(lv_align_t align, int32_t x_ofs, int32_t y_ofs) {
    if (keyboard_ == nullptr) {
        spdlog::error("[KeyboardManager] Not initialized - call init() first");
        return;
    }

    spdlog::debug("[KeyboardManager] Setting position: align={}, x={}, y={}", (int)align, x_ofs,
                  y_ofs);
    lv_obj_align(keyboard_, align, x_ofs, y_ofs);
}
