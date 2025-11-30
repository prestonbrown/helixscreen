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

#include <cstdint>

/**
 * @brief Toast notification manager for HelixScreen
 *
 * Manages temporary non-blocking toast notifications that appear at the
 * top-center of the screen and auto-dismiss after a configurable duration.
 *
 * Features:
 * - Single active toast (new notifications replace old ones)
 * - Auto-dismiss with configurable timer
 * - Manual dismiss via close button
 * - Severity-based color coding (info, success, warning, error)
 */

/**
 * @brief Toast notification severity levels
 */
enum class ToastSeverity {
    INFO,    ///< Informational message (blue)
    SUCCESS, ///< Success message (green)
    WARNING, ///< Warning message (orange)
    ERROR    ///< Error message (red)
};

/**
 * @brief Initialize the toast notification system
 *
 * Should be called during app initialization.
 */
void ui_toast_init();

/**
 * @brief Callback type for toast action button
 */
typedef void (*toast_action_callback_t)(void* user_data);

/**
 * @brief Show a toast notification
 *
 * Displays a toast notification with the specified severity and message.
 * If a toast is already visible, it will be replaced with the new one.
 *
 * @param severity Toast severity level (determines color)
 * @param message Message text to display
 * @param duration_ms Duration in milliseconds before auto-dismiss (default: 4000ms)
 */
void ui_toast_show(ToastSeverity severity, const char* message, uint32_t duration_ms = 4000);

/**
 * @brief Show a toast notification with an action button
 *
 * Displays a toast with an action button (e.g., "Undo"). The action callback
 * is invoked when the button is clicked. The toast auto-dismisses after
 * duration_ms, or when the close button is clicked.
 *
 * @param severity Toast severity level (determines color)
 * @param message Message text to display
 * @param action_text Text for the action button (e.g., "Undo")
 * @param action_callback Callback invoked when action button is clicked
 * @param user_data User data passed to the callback
 * @param duration_ms Duration in milliseconds before auto-dismiss (default: 5000ms)
 */
void ui_toast_show_with_action(ToastSeverity severity, const char* message,
                                const char* action_text, toast_action_callback_t action_callback,
                                void* user_data, uint32_t duration_ms = 5000);

/**
 * @brief Hide the currently visible toast
 *
 * Can be called to manually dismiss a toast before its timer expires.
 */
void ui_toast_hide();

/**
 * @brief Check if a toast is currently visible
 *
 * @return true if a toast is visible, false otherwise
 */
bool ui_toast_is_visible();
