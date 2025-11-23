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

#include "keyboard_layout_provider.h"
#include "lvgl.h"

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
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", LV_SYMBOL_KEYBOARD, ",", SPACEBAR_TEXT, ".", LV_SYMBOL_NEW_LINE, ""};

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

// Uppercase alphabet (caps lock mode - uses eject symbol, no number row)
static const char* const kb_map_alpha_uc[] = {
    // Row 1: Q-P (10 letters, uppercase) - numbers 1-0 on long-press
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    // Row 2: [SPACER] A-L (9 letters, uppercase) [SPACER]
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    // Row 3: [SHIFT] Z-M [BACKSPACE] - eject symbol to indicate caps lock
    LV_SYMBOL_EJECT, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", LV_SYMBOL_KEYBOARD, ",", SPACEBAR_TEXT, ".", LV_SYMBOL_NEW_LINE, ""};

// Uppercase alphabet (one-shot mode - uses filled/distinct arrow symbol, no number row)
static const char* const kb_map_alpha_uc_oneshot[] = {
    // Row 1: Q-P (10 letters, uppercase) - numbers 1-0 on long-press
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    // Row 2: [SPACER] A-L (9 letters, uppercase) [SPACER]
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    // Row 3: [SHIFT] Z-M [BACKSPACE] - upload symbol for one-shot (visually distinct)
    LV_SYMBOL_UPLOAD, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    // Row 4: ?123 + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "?123", LV_SYMBOL_KEYBOARD, ",", SPACEBAR_TEXT, ".", LV_SYMBOL_NEW_LINE, ""};

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

// Numbers and symbols layout
// Provides common punctuation and symbols with [ABC] button to return to alpha mode
static const char* const kb_map_numbers_symbols[] = {
    // Row 1: Special characters and numbers
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    // Row 2: More symbols
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    // Row 3: [SPACER] Additional punctuation [SPACER]
    " ", ".", ",", "?", "!", "'", "\"", "+", "=", "_", " ", "\n",
    // Row 4: [#+=] + brackets/symbols + [BACKSPACE] (8 buttons like alpha row 4)
    "#+=", "[", "]", "{", "}", "|", "\\", LV_SYMBOL_BACKSPACE, "\n",
    // Row 5: XYZ (back to alpha) + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    // Using "XYZ" to avoid LVGL's "ABC"/"abc" mode checks
    "XYZ", LV_SYMBOL_KEYBOARD, ",", SPACEBAR_TEXT, ".", LV_SYMBOL_NEW_LINE, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_numbers_symbols[] = {
    // Row 1: Special chars and numbers (equal width) - NO_REPEAT
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
    // Row 2: More symbols (equal width) - NO_REPEAT
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
    // Row 3: disabled spacer + punctuation + disabled spacer (2 + 36 + 2 = 40) - NO_REPEAT
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
    // Row 4: #+= (wide) + brackets/symbols (regular) + Backspace (wide) - mark #+=, Backspace as
    // CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // #+=
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
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 10), // Backspace
    // Row 5: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER (2 + 3 + 2 + 12 + 2 + 3 = 24) - mark
    // mode/control buttons as CUSTOM_1
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

// Alternative symbols layout (#+= mode)
// Provides additional symbols with [123] button to return to ?123 mode
static const char* const kb_map_alt_symbols[] = {
    // Row 1: Brackets and math symbols
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    // Row 2: Special characters (ASCII only - Unicode glyphs not in font)
    "_", "\\", "|", "~", "<", ">", "$", "&", "@", "*", "\n",
    // Row 3: [SPACER] Punctuation [SPACER]
    " ", ".", ",", "?", "!", "'", "\"", ";", ":", "-", " ", "\n",
    // Row 4: [123] + misc symbols + [BACKSPACE] (ASCII only - Unicode glyphs not in font)
    "123", "`", "^", "~", "-", "_", LV_SYMBOL_BACKSPACE, "\n",
    // Row 5: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER
    "XYZ", LV_SYMBOL_KEYBOARD, ",", SPACEBAR_TEXT, ".", LV_SYMBOL_NEW_LINE, ""};

static const lv_buttonmatrix_ctrl_t kb_ctrl_alt_symbols[] = {
    // Row 1: Brackets and math (equal width) - NO_REPEAT
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
    // Row 2: Special chars and currency (equal width) - NO_REPEAT
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
    // Row 3: disabled spacer + punctuation + disabled spacer (2 + 36 + 2 = 40) - NO_REPEAT
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
    // Row 4: 123 (wide) + misc symbols (regular) + Backspace (wide) - mark 123, Backspace as
    // CUSTOM_1
    static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS |
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 6), // 123
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
                                        LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 14), // Backspace
    // Row 5: XYZ + CLOSE + COMMA + SPACEBAR + PERIOD + ENTER (2 + 3 + 2 + 12 + 2 + 3 = 24) - mark
    // mode/control buttons as CUSTOM_1
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

const char* keyboard_layout_get_spacebar_text() {
    return SPACEBAR_TEXT;
}
