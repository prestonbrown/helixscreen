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

/**
 * @brief Add an info notification to history only (no toast)
 *
 * Creates a notification history entry with an action identifier.
 * When the user taps this entry in notification history, the action
 * is dispatched (e.g., "show_update_modal" re-shows the update modal).
 *
 * No toast is shown — use this when the user has already seen context
 * (e.g., just dismissed a modal) and a popup would be redundant.
 *
 * @param title Title for the history entry
 * @param message Message text
 * @param action Action identifier (e.g., "show_update_modal")
 */
void ui_notification_info_with_action(const char* title, const char* message, const char* action);

/**
 * @brief Add a warning notification to history only (no toast)
 *
 * ui_notification_info_with_action at WARNING severity, for a history entry the
 * user should act on. Pair it with ToastManager::show_with_action() when the
 * moment itself also needs a button.
 *
 * @param title Title for the history entry
 * @param message Message text
 * @param action Action identifier dispatched when the entry is tapped
 */
// NAMESPACE_OK: matches ui_notification_info_with_action, this file's existing global-scope API
void ui_notification_warning_with_action(const char* title, const char* message,
                                         const char* action);

/**
 * @brief Show an error toast with a second line saying what to do about it
 *
 * The toast renders @p message as the body and @p detail beneath it in the
 * smaller muted style. Notification history stores the two joined, since a
 * history row is a single line.
 *
 * Distinct name rather than an overload: ui_notification_error already has a
 * (title, message) shape, so a (message, detail) overload would be ambiguous at
 * every existing two-argument call site.
 *
 * **Thread-safe**: marshals to the LVGL main thread. Safe from any thread.
 *
 * @param message What happened
 * @param detail  What the user can do about it; empty renders as a plain toast
 */
void ui_notification_error_with_detail(const char* message, const char* detail);

/**
 * @brief Show a warning toast with a "what to do" second line
 * @see ui_notification_error_with_detail
 */
void ui_notification_warning_with_detail(const char* message, const char* detail);

/**
 * @brief Show a warning toast with a "what to do" second line that never
 *        auto-dismisses
 *
 * Same rendering as ui_notification_warning_with_detail, but the toast stays
 * on screen until its close button is tapped. For warnings where acting is not
 * optional; a refused helixscreen.env means every setting in it is ignored,
 * and an 8-second auto-dismiss would hide the problem again before the user
 * has read it.
 *
 * **Thread-safe**: marshals to the LVGL main thread. Safe from any thread.
 *
 * @param message What happened
 * @param detail  What the user can do about it; empty renders as a plain toast
 */
// NAMESPACE_OK: joins this header's global ui_notification_* free-function API
void ui_notification_warning_sticky(const char* message, const char* detail);
