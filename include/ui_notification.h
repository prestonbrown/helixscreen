// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_toast_manager.h"

#include "core/notification_api.h"
#include "lvgl.h"

#include <functional>
#include <string>

/**
 * @brief Unified notification API for HelixScreen
 *
 * Provides a high-level interface for showing notifications throughout the app.
 * Routes notifications to appropriate display mechanisms:
 * - Non-critical messages → toast notifications (auto-dismiss)
 * - Critical errors → modal dialogs (require acknowledgment)
 *
 * **Thread Safety:**
 * All functions are thread-safe. They automatically detect if called from a
 * background thread and use helix::ui::queue_update() to marshal to the LVGL main thread.
 * Safe to call from any thread (main thread, libhv callbacks, WiFi events, etc.).
 *
 * Also integrates with the reactive subject system so any module can emit
 * notifications without direct dependencies on UI code.
 */

/**
 * @brief Notification data structure for reactive subject
 *
 * Used to emit notifications via lv_subject_t for decoupled notification
 * from any module in the application.
 *
 * Usage:
 * ```cpp
 * #include "app_globals.h"
 * NotificationData notif = {severity, title, message, show_modal};
 * lv_subject_set_pointer(&get_notification_subject(), &notif);
 * ```
 */
typedef struct {
    ToastSeverity severity; ///< Notification severity level
    const char* title;      ///< Title for modal dialogs (can be nullptr for toasts)
    const char* message;    ///< Notification message text
    bool show_modal;        ///< true = modal dialog, false = toast notification
} NotificationData;

/**
 * @brief Initialize the notification system
 *
 * Sets up subject observers and prepares the notification infrastructure.
 * Must be called during app initialization after app_globals_init_subjects().
 * Also captures the main thread ID for automatic thread-safety detection.
 */
void ui_notification_init();

/**
 * @brief Release notification observer for soft restart
 *
 * Resets the notification observer guard so it doesn't hold a dangling
 * pointer after subjects are torn down. Called during tear_down_printer_state().
 */
void ui_notification_deinit();

/**
 * @brief Show a blocking error modal for a fault ON THE PRINTER
 *
 * Renders exactly like ui_notification_error(title, message, modal=true) — same
 * widget, same dedup-by-title, same history entry — but the dialog is handed to
 * helix::ui::track_fault_modal() (fault_modal_registry.h) so it can be retired
 * once the condition it describes is gone.
 *
 * Use this only when the message describes the PRINTER's state (a Klipper
 * shutdown, an MCU that stopped answering, a filament-system fault). A
 * HelixScreen-side failure ("failed to load the wizard screen") must stay on
 * ui_notification_error: Klipper recovering says nothing about whether that
 * screen will load, and sweeping it away would hide a problem that is still
 * real. See #1266.
 *
 * **Thread-safe**: marshals to the LVGL main thread. Safe from any thread.
 *
 * @param title Error title (required — a modal without one degrades to a toast)
 * @param message Error message text
 */
void ui_notification_printer_fault(const char* title, const char* message);

namespace helix {
namespace ui {

/**
 * @brief What one ui_notification_* call surfaced.
 *
 * Reported on the calling thread before threading, the startup queue or the
 * user's toast threshold decide how (and whether) it is shown.
 */
struct NotificationEvent {
    ToastSeverity severity = ToastSeverity::INFO;
    std::string title;   ///< The title the call was given; empty when untitled
    std::string message; ///< The text of the history row the call adds
    std::string action;  ///< Set only for the history-only *_with_action rows
    bool sticky = false; ///< Stays on screen until dismissed
};

/**
 * @brief Install the one observer every ui_notification_* call reports to.
 *
 * An empty function removes it. The test binary reads what code under test
 * surfaced through it. Not synchronized: install it before the calls it
 * should see.
 */
void set_notification_observer(std::function<void(const NotificationEvent&)> observer);

} // namespace ui
} // namespace helix
