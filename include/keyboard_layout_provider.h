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
 * @file keyboard_layout_provider.h
 * @brief Keyboard layout data for on-screen keyboard
 *
 * Provides button maps and control arrays for different keyboard modes:
 * - Lowercase alphabet
 * - Uppercase alphabet (caps lock and one-shot)
 * - Numbers and symbols
 * - Alternative symbols
 *
 * Layouts are designed in Gboard style (no number row on alpha keyboard).
 */

/** @brief Keyboard mode enumeration */
enum keyboard_layout_mode_t {
    KEYBOARD_LAYOUT_ALPHA_LC,        ///< Lowercase alphabet
    KEYBOARD_LAYOUT_ALPHA_UC,        ///< Uppercase alphabet
    KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, ///< Numbers and symbols
    KEYBOARD_LAYOUT_ALT_SYMBOLS      ///< Alternative symbols (#+= mode)
};

/**
 * @brief Get button map for a keyboard layout
 *
 * @param mode Keyboard mode
 * @param caps_lock_active true if caps lock is active (affects uppercase layout)
 * @return Button map array (null-terminated strings)
 */
const char* const* keyboard_layout_get_map(keyboard_layout_mode_t mode, bool caps_lock_active);

/**
 * @brief Get control map for a keyboard layout
 *
 * @param mode Keyboard mode
 * @return Control array (button widths and flags)
 */
const lv_buttonmatrix_ctrl_t* keyboard_layout_get_ctrl_map(keyboard_layout_mode_t mode);

/**
 * @brief Get spacebar text constant
 * @return Two-space string used for spacebar rendering
 */
const char* keyboard_layout_get_spacebar_text();
