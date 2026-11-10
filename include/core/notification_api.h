// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file notification_api.h
 * @brief The plain-text notification calls, declared where every layer can reach them.
 *
 * Implemented in src/ui/notify/ui_notification.cpp and safe from any thread;
 * ui_notification.h carries the rest of the notification API.
 */

/**
 * @brief Show an informational toast notification
 *
 * Displays a non-blocking blue toast message that auto-dismisses after 4 seconds.
 *
 * **Thread-safe**: Automatically detects if called from background thread and
 * marshals to LVGL main thread. Safe to call from any thread.
 *
 * @param message Message text to display
 */
void ui_notification_info(const char* message);

/**
 * @brief Show an informational toast notification with title
 *
 * Like ui_notification_info but includes a title. The toast displays "Title: message"
 * and the title is stored separately in notification history.
 *
 * @param title Title for context (displayed as "Title: message")
 * @param message Message text to display
 */
void ui_notification_info(const char* title, const char* message);

/**
 * @brief Show a success toast notification
 *
 * Displays a non-blocking green toast message that auto-dismisses after 4 seconds.
 *
 * **Thread-safe**: Automatically detects if called from background thread and
 * marshals to LVGL main thread. Safe to call from any thread.
 *
 * @param message Message text to display
 */
void ui_notification_success(const char* message);

/**
 * @brief Show a success toast notification with title
 *
 * Like ui_notification_success but includes a title. The toast displays "Title: message"
 * and the title is stored separately in notification history.
 *
 * @param title Title for context (displayed as "Title: message")
 * @param message Message text to display
 */
void ui_notification_success(const char* title, const char* message);

/**
 * @brief Show a warning notification
 *
 * Displays a non-blocking orange toast message that auto-dismisses after 5 seconds.
 *
 * **Thread-safe**: Automatically detects if called from background thread and
 * marshals to LVGL main thread. Safe to call from any thread.
 *
 * @param message Message text to display
 */
void ui_notification_warning(const char* message);

/**
 * @brief Show a warning notification with title
 *
 * Like ui_notification_warning but includes a title. The toast displays "Title: message"
 * and the title is stored separately in notification history.
 *
 * @param title Title for context (displayed as "Title: message")
 * @param message Message text to display
 */
void ui_notification_warning(const char* title, const char* message);

/**
 * @brief Show an error notification
 *
 * Can display either a blocking modal dialog or a toast notification depending
 * on the modal parameter. Critical errors should use modal=true.
 *
 * **Thread-safe**: Automatically detects if called from background thread and
 * marshals to LVGL main thread. Safe to call from any thread.
 *
 * @param title Error title (used for modal dialogs, can be nullptr for toasts)
 * @param message Error message text
 * @param modal If true, shows blocking modal dialog; if false, shows toast
 */
void ui_notification_error(const char* title, const char* message, bool modal = true);
