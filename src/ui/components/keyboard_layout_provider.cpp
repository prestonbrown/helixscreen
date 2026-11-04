// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboard_layout_provider.h"

#include "ui_fonts.h"

#include "lvgl.h"

#include <array>
#include <cstring>

/**
 * @file keyboard_layout_provider.cpp
 * @brief Keyboard layout data provider for on-screen keyboard
 *
 * Provides layout maps and control maps for different keyboard modes:
 * - Lowercase alphabet (Gboard-style, no number row)
 * - Uppercase alphabet (caps lock and one-shot modes)
 * - Numbers and symbols (?123 mode)
 * - Alternative symbols (#+= mode)
 *
 * This module extracts layout data from ui_keyboard.cpp for better
 * modularity and testability. Layout changes can be made here without
 * recompiling the entire keyboard event handling logic.
 */

//=============================================================================
// KEYBOARD LAYOUT CONSTANTS
//=============================================================================

// Macro for keyboard buttons with popover support (C++ requires explicit cast)
#define LV_KEYBOARD_CTRL_BUTTON_FLAGS                                                              \
    (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |                            \
     LV_BUTTONMATRIX_CTRL_CHECKED)

// Double space for spacebar (appears mostly blank but is unique/detectable)
#define SPACEBAR_TEXT "  "

//=============================================================================
// LAYOUT MAPS
//=============================================================================

// Lowercase alphabet (Gboard-style: no number row)
static const char* const kb_map_alpha_lc[] = {
    // Row 1: q-p (10 letters) - numbers 1-0 on long-press
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    // Row 2: spacer + a-l (9 letters) + spacer
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    // Row 3: [SHIFT] z-m [BACKSPACE] - shift on left, backspace on right (above Enter)
    ICON_KEYBOARD_SHIFT, "z", "x", "c", "v", "b", "n", "m", ICON_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", ICON_KEYBOARD_CLOSE, ",", SPACEBAR_TEXT, ".", ICON_KEYBOARD_RETURN, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_alpha_lc[] = {
    // Row 1: q-p (equal width) - NO_REPEAT to prevent key repeat
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 2: disabled spacer + a-l + disabled spacer (width 2 each spacer)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_DISABLED | 2),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_DISABLED | 2),
    // Row 3: Shift (wide) + z-m (regular) + Backspace (wide) - mark Shift/Backspace as CUSTOM_1
    // (non-printing)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // Shift
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // Backspace
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER (2 + 3 + 2 + 12 + 2 + 3 = 24) - mark
    // mode/control buttons as CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 2), // ?123
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3), // Close
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Comma
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        12), // SPACEBAR - NO CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Period
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3) // Enter
};

// Uppercase alphabet (caps lock mode - uses caps lock symbol, no number row)
static const char* const kb_map_alpha_uc[] = {
    // Row 1: Q-P (10 letters, uppercase) - numbers 1-0 on long-press
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    // Row 2: [SPACER] A-L (9 letters, uppercase) [SPACER]
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    // Row 3: [SHIFT] Z-M [BACKSPACE] - caps lock symbol to indicate caps lock
    ICON_KEYBOARD_CAPS, "Z", "X", "C", "V", "B", "N", "M", ICON_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", ICON_KEYBOARD_CLOSE, ",", SPACEBAR_TEXT, ".", ICON_KEYBOARD_RETURN, ""};

// Uppercase alphabet (one-shot mode - uses shift symbol, no number row)
static const char* const kb_map_alpha_uc_oneshot[] = {
    // Row 1: Q-P (10 letters, uppercase) - numbers 1-0 on long-press
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    // Row 2: [SPACER] A-L (9 letters, uppercase) [SPACER]
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    // Row 3: [SHIFT] Z-M [BACKSPACE] - shift symbol for one-shot (visually distinct)
    ICON_KEYBOARD_SHIFT, "Z", "X", "C", "V", "B", "N", "M", ICON_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", ICON_KEYBOARD_CLOSE, ",", SPACEBAR_TEXT, ".", ICON_KEYBOARD_RETURN, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_alpha_uc[] = {
    // Row 1: Q-P (equal width) - NO_REPEAT to prevent key repeat
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 2: disabled spacer + A-L + disabled spacer (2 + 36 + 2 = 40)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_DISABLED | 2),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_DISABLED | 2),
    // Row 3: Shift (wide) + Z-M (regular) + Backspace (wide) - mark Shift/Backspace as CUSTOM_1
    // (non-printing)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // Shift (active)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // Backspace
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER (2 + 3 + 2 + 12 + 2 + 3 = 24) - mark
    // mode/control buttons as CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 2), // ?123
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3), // Close
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Comma
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        12), // SPACEBAR - NO CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Period
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3) // Enter
};

// Numbers and symbols layout (4 rows, matching alpha keyboard structure)
// Provides numbers 1-0 on row 1, common symbols on row 2, punctuation + mode switch on row 3
static const char* const kb_map_numbers_symbols[] = {
    // Row 1: Numbers 1-0 (10 keys)
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    // Row 2: Common symbols (10 keys)
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "*", "\n",
    // Row 3: #+= + punctuation + Backspace (matches alpha row 3 structure)
    "#+=", ".", ",", "?", "!", "\"", ICON_BACKSPACE, "\n",
    // Row 4: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "XYZ", ICON_KEYBOARD_CLOSE, ",", SPACEBAR_TEXT, ".", ICON_KEYBOARD_RETURN, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_numbers_symbols[] = {
    // Row 1: Numbers 1-0 (10 keys, equal width 4)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 2: Common symbols (10 keys, equal width 4)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 3: #+= (wide) + 5 punctuation + Backspace (wide)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // #+=
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // .
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ,
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ?
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // !
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // "
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // Backspace
    // Row 4: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 2), // XYZ
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3), // Close
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Comma
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        12), // SPACEBAR - NO CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Period
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3) // Enter
};

// Alternative symbols layout (#+= mode, 4 rows)
// Provides additional ASCII symbols and extended Unicode characters
static const char* const kb_map_alt_symbols[] = {
    // Row 1: Brackets & math (10 keys)
    "[", "]", "{", "}", "#", "%", "^", "+", "=", "_", "\n",
    // Row 2: Misc ASCII + bullet/ellipsis (10 keys)
    "\\", "|", "`", "~", "<", ">", "'", ";", "\xe2\x80\xa2", "\xe2\x80\xa6", "\n", // • …
    // Row 3: 123 + Extended symbols + Backspace (10 keys)
    // UTF-8 encoding: © = \xc2\xa9, ® = \xc2\xae, ™ = \xe2\x84\xa2, € = \xe2\x82\xac,
    //                 £ = \xc2\xa3, ¥ = \xc2\xa5, ° = \xc2\xb0, ± = \xc2\xb1
    "123", "\xc2\xa9", "\xc2\xae", "\xe2\x84\xa2", "\xe2\x82\xac", "\xc2\xa3", "\xc2\xa5",
    "\xc2\xb0", "\xc2\xb1", ICON_BACKSPACE, "\n",
    // Row 4: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "XYZ", ICON_KEYBOARD_CLOSE, ",", SPACEBAR_TEXT, ".", ICON_KEYBOARD_RETURN, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_alt_symbols[] = {
    // Row 1: Brackets & math (10 keys, equal width 4)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 2: Misc ASCII + bullet/ellipsis (10 keys, equal width 4)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4),
    // Row 3: 123 + 8 extended symbols + Backspace (all width 4)
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 4), // 123
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ©
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ®
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ™
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // €
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // £
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ¥
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // °
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 4), // ±
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 4), // Backspace
    // Row 4: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 2), // XYZ
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3), // Close
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Comma
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        12), // SPACEBAR - NO CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER |
                                        LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), // Period
    static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 3) // Enter
};

//=============================================================================
// PUBLIC API IMPLEMENTATION
//=============================================================================

const char* const* keyboard_layout_get_map(keyboard_layout_mode_t mode, bool caps_lock_active) {
    switch (mode) {
    case KEYBOARD_LAYOUT_ALPHA_LC:
        return kb_map_alpha_lc;
    case KEYBOARD_LAYOUT_ALPHA_UC:
        // Choose between caps lock (eject symbol) or one-shot (upload symbol)
        return caps_lock_active ? kb_map_alpha_uc : kb_map_alpha_uc_oneshot;
    case KEYBOARD_LAYOUT_NUMBERS_SYMBOLS:
        return kb_map_numbers_symbols;
    case KEYBOARD_LAYOUT_ALT_SYMBOLS:
        return kb_map_alt_symbols;
    default:
        return kb_map_alpha_lc; // Fallback to lowercase
    }
}

const lv_buttonmatrix_ctrl_t* keyboard_layout_get_ctrl_map(keyboard_layout_mode_t mode) {
    switch (mode) {
    case KEYBOARD_LAYOUT_ALPHA_LC:
        return kb_ctrl_alpha_lc;
    case KEYBOARD_LAYOUT_ALPHA_UC:
        // Both caps lock and one-shot use the same control map
        return kb_ctrl_alpha_uc;
    case KEYBOARD_LAYOUT_NUMBERS_SYMBOLS:
        return kb_ctrl_numbers_symbols;
    case KEYBOARD_LAYOUT_ALT_SYMBOLS:
        return kb_ctrl_alt_symbols;
    default:
        return kb_ctrl_alpha_lc; // Fallback to lowercase
    }
}

// NAMESPACE_OK: one of the global keyboard_layout_* functions
void keyboard_layout_apply(lv_obj_t* keyboard, keyboard_layout_mode_t mode, bool caps_lock_active) {
    const char* const* map = keyboard_layout_get_map(mode, caps_lock_active);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(mode);
    lv_buttonmatrix_set_map(keyboard, map);
    if (lv_keyboard_get_popovers(keyboard)) {
        lv_buttonmatrix_set_ctrl_map(keyboard, ctrl_map);
        return;
    }
    // The buttonmatrix copies the control map, so a local array is enough.
    std::array<lv_buttonmatrix_ctrl_t, 64> stripped{};
    size_t count = 0;
    for (size_t i = 0; map[i][0] != '\0'; ++i) {
        if (std::strcmp(map[i], "\n") != 0) {
            ++count;
        }
    }
    if (count > stripped.size()) {
        lv_buttonmatrix_set_ctrl_map(keyboard, ctrl_map);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        lv_buttonmatrix_ctrl_t ctrl = ctrl_map[i];
        if (ctrl & LV_BUTTONMATRIX_CTRL_POPOVER) {
            ctrl = static_cast<lv_buttonmatrix_ctrl_t>((ctrl & ~LV_BUTTONMATRIX_CTRL_POPOVER) |
                                                       LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
        }
        stripped[i] = ctrl;
    }
    lv_buttonmatrix_set_ctrl_map(keyboard, stripped.data());
}

const char* keyboard_layout_get_spacebar_text() {
    return SPACEBAR_TEXT;
}
