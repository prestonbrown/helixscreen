// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_notification.cpp
 * @brief Thread-safe notification toast system callable from any thread
 *
 * @pattern Auto-detects background thread context; marshals to main thread
 * @threading Safe from any thread - automatically uses helix::ui::async_call() when needed
 *
 * @see "Thread-safe:" comment in show() implementation
 */

#include "ui_notification.h"

#include "ui_modal.h"
#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_observer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

// Forward declarations
static void notification_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
// Thread tracking for auto-detection
static std::thread::id g_main_thread_id;
static std::atomic<bool> g_main_thread_id_initialized{false};

// RAII observer guard for automatic cleanup
static ObserverGuard s_notification_observer;

// Check if we're on the LVGL main thread
static bool is_main_thread() {
    if (!g_main_thread_id_initialized.load()) {
        return true; // Before initialization, assume main thread
    }
    return std::this_thread::get_id() == g_main_thread_id;
}

// ============================================================================
// Helper structures and callbacks for background thread marshaling
// ============================================================================

// Fixed-size async data structs - eliminates malloc+strdup overhead
// Sizes match NotificationHistoryEntry for consistency
struct AsyncMessageData {
    char title[64]; // Empty string if no title
    char message[256];
    ToastSeverity severity;
    uint32_t duration_ms;
    bool has_title;
};

struct AsyncErrorData {
    char title[64];
    char message[256];
    bool modal;
    bool has_title;
};

// Async callbacks for lv_async_call (called on main thread)
static void async_message_callback(void* user_data) {
    AsyncMessageData* data = (AsyncMessageData*)user_data;
    if (data && data->message[0] != '\0') {
        // Format display message with title if present
        char display_buf[324]; // title(64) + ": "(2) + message(256) + null
        if (data->has_title) {
            snprintf(display_buf, sizeof(display_buf), "%s: %s", data->title, data->message);
        } else {
            strncpy(display_buf, data->message, sizeof(display_buf) - 1);
            display_buf[sizeof(display_buf) - 1] = '\0';
        }
        ToastManager::instance().show(data->severity, display_buf, data->duration_ms);

        // Add to history
        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = data->severity;
        entry.was_modal = false;
        entry.was_read = false;

        if (data->has_title) {
            strncpy(entry.title, data->title, sizeof(entry.title) - 1);
            entry.title[sizeof(entry.title) - 1] = '\0';
        } else {
            entry.title[0] = '\0';
        }
        strncpy(entry.message, data->message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    }
    delete data;
}

static void async_error_callback(void* user_data) {
    AsyncErrorData* data = (AsyncErrorData*)user_data;
    if (data && data->message[0] != '\0') {
        if (data->modal && data->has_title) {
            // Check if a modal with the same title is already showing
            lv_obj_t* existing_modal = helix::ui::modal_get_top();
            if (existing_modal) {
                // The modal_dialog.xml uses "dialog_title" for the title label
                lv_obj_t* title_label = lv_obj_find_by_name(existing_modal, "dialog_title");
                if (title_label) {
                    const char* existing_title = lv_label_get_text(title_label);
                    if (existing_title && strcmp(existing_title, data->title) == 0) {
                        spdlog::debug("[Notification] Skipping duplicate modal (async): '{}'",
                                      data->title);
                        delete data;
                        return;
                    }
                }
            }

            // Show modal dialog for critical errors
            helix::ui::modal_show_alert(data->title, data->message, ModalSeverity::Error, "OK");

            helix::ui::notification_update(NotificationStatus::ERROR);
        } else {
            // Show toast for non-critical errors
            ToastManager::instance().show(ToastSeverity::ERROR, data->message, 6000);
        }

        // Add to history
        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = ToastSeverity::ERROR;
        entry.was_modal = data->modal;
        entry.was_read = false;

        if (data->has_title) {
            strncpy(entry.title, data->title, sizeof(entry.title) - 1);
            entry.title[sizeof(entry.title) - 1] = '\0';
        } else {
            entry.title[0] = '\0';
        }

        strncpy(entry.message, data->message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    }
    delete data;
}

// ============================================================================
// Public API functions (thread-safe with auto-detection)
// ============================================================================

void ui_notification_init() {
    // Capture main thread ID for thread-safety detection
    g_main_thread_id = std::this_thread::get_id();
    g_main_thread_id_initialized.store(true);

    // Add observer to handle notification emissions (RAII guard ensures cleanup)
    // (Subject itself is initialized in app_globals_init_subjects())
    s_notification_observer =
        ObserverGuard(&get_notification_subject(), notification_observer_cb, nullptr);

    spdlog::debug("[Notification] Notification system initialized (main thread ID captured)");
}

void ui_notification_deinit() {
    s_notification_observer.reset();
    spdlog::debug("[Notification] Notification observer released");
}

void ui_notification_info(const char* message) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show info notification with null message");
        return;
    }

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        ToastManager::instance().show(ToastSeverity::INFO, message, 4000);

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = ToastSeverity::INFO;
        entry.was_modal = false;
        entry.was_read = false;
        entry.title[0] = '\0';
        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncMessageData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async notification");
            return;
        }

        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';
        data->title[0] = '\0';
        data->has_title = false;
        data->severity = ToastSeverity::INFO;
        data->duration_ms = 4000;

        helix::ui::async_call(async_message_callback, data);
    }
}

void ui_notification_success(const char* message) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show success notification with null message");
        return;
    }

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        ToastManager::instance().show(ToastSeverity::SUCCESS, message, 4000);

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = ToastSeverity::SUCCESS;
        entry.was_modal = false;
        entry.was_read = false;
        entry.title[0] = '\0';
        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncMessageData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async notification");
            return;
        }

        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';
        data->title[0] = '\0';
        data->has_title = false;
        data->severity = ToastSeverity::SUCCESS;
        data->duration_ms = 4000;

        helix::ui::async_call(async_message_callback, data);
    }
}

void ui_notification_warning(const char* message) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show warning notification with null message");
        return;
    }

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        ToastManager::instance().show(ToastSeverity::WARNING, message, 5000);

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = ToastSeverity::WARNING;
        entry.was_modal = false;
        entry.was_read = false;
        entry.title[0] = '\0';
        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncMessageData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async notification");
            return;
        }

        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';
        data->title[0] = '\0';
        data->has_title = false;
        data->severity = ToastSeverity::WARNING;
        data->duration_ms = 5000;

        helix::ui::async_call(async_message_callback, data);
    }
}

// ============================================================================
// Titled variants (display "Title: message" in toast, store title in history)
// ============================================================================

// Helper to show titled notification (reduces code duplication)
static void show_titled_notification(const char* title, const char* message, ToastSeverity severity,
                                     uint32_t duration_ms) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show notification with null message");
        return;
    }
    if (!title) {
        spdlog::warn("[Notification] Titled notification called with null title");
        return;
    }

    // Format display message as "Title: message"
    std::string display_msg = std::string(title) + ": " + message;

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        ToastManager::instance().show(severity, display_msg.c_str(), duration_ms);

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = severity;
        entry.was_modal = false;
        entry.was_read = false;
        strncpy(entry.title, title, sizeof(entry.title) - 1);
        entry.title[sizeof(entry.title) - 1] = '\0';
        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncMessageData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async notification");
            return;
        }

        strncpy(data->title, title, sizeof(data->title) - 1);
        data->title[sizeof(data->title) - 1] = '\0';
        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';
        data->has_title = true;
        data->severity = severity;
        data->duration_ms = duration_ms;

        helix::ui::async_call(async_message_callback, data);
    }
}

void ui_notification_info(const char* title, const char* message) {
    show_titled_notification(title, message, ToastSeverity::INFO, 4000);
}

void ui_notification_info_with_action(const char* title, const char* message, const char* action) {
    if (!message || !action) {
        spdlog::warn("[Notification] info_with_action called with null message or action");
        return;
    }

    // Build entry directly — no toast, history only
    NotificationHistoryEntry entry = {};
    entry.timestamp_ms = lv_tick_get();
    entry.severity = ToastSeverity::INFO;
    entry.was_modal = false;
    entry.was_read = false;

    if (title) {
        strncpy(entry.title, title, sizeof(entry.title) - 1);
        entry.title[sizeof(entry.title) - 1] = '\0';
    }
    strncpy(entry.message, message, sizeof(entry.message) - 1);
    entry.message[sizeof(entry.message) - 1] = '\0';
    strncpy(entry.action, action, sizeof(entry.action) - 1);
    entry.action[sizeof(entry.action) - 1] = '\0';

    if (is_main_thread()) {
        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        auto* data = new (std::nothrow) NotificationHistoryEntry(entry);
        if (!data) {
            spdlog::error("[Notification] Failed to allocate for async action notification");
            return;
        }
        helix::ui::async_call(
            [](void* user_data) {
                auto* e = static_cast<NotificationHistoryEntry*>(user_data);
                NotificationHistory::instance().add(*e);
                helix::ui::notification_update_count(
                    NotificationHistory::instance().get_unread_count());
                delete e;
            },
            data);
    }

    spdlog::info("[Notification] History-only notification: '{}' action='{}'", message, action);
}

void ui_notification_success(const char* title, const char* message) {
    show_titled_notification(title, message, ToastSeverity::SUCCESS, 4000);
}

void ui_notification_warning(const char* title, const char* message) {
    show_titled_notification(title, message, ToastSeverity::WARNING, 5000);
}

void ui_notification_error(const char* title, const char* message, bool modal) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show error notification with null message");
        return;
    }

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        if (modal && title) {
            // Check if a modal with the same title is already showing
            // This prevents duplicate modals when multiple components report the same error
            lv_obj_t* existing_modal = helix::ui::modal_get_top();
            if (existing_modal) {
                // The modal_dialog.xml uses "dialog_title" for the title label
                lv_obj_t* title_label = lv_obj_find_by_name(existing_modal, "dialog_title");
                if (title_label) {
                    const char* existing_title = lv_label_get_text(title_label);
                    if (existing_title && strcmp(existing_title, title) == 0) {
                        spdlog::debug("[Notification] Skipping duplicate modal: '{}'", title);
                        return;
                    }
                }
            }

            // Show modal dialog for critical errors
            helix::ui::modal_show_alert(title, message, ModalSeverity::Error, "OK");

            helix::ui::notification_update(NotificationStatus::ERROR);
        } else {
            // Show toast for non-critical errors
            ToastManager::instance().show(ToastSeverity::ERROR, message, 6000);
        }

        // Add to history
        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = ToastSeverity::ERROR;
        entry.was_modal = modal;
        entry.was_read = false;

        if (title) {
            strncpy(entry.title, title, sizeof(entry.title) - 1);
            entry.title[sizeof(entry.title) - 1] = '\0';
        } else {
            entry.title[0] = '\0';
        }

        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_update_count(NotificationHistory::instance().get_unread_count());
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncErrorData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async error notification");
            return;
        }

        // Copy title (can be nullptr)
        if (title) {
            strncpy(data->title, title, sizeof(data->title) - 1);
            data->title[sizeof(data->title) - 1] = '\0';
            data->has_title = true;
        } else {
            data->title[0] = '\0';
            data->has_title = false;
        }

        // Copy message
        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';

        data->modal = modal;

        helix::ui::async_call(async_error_callback, data);
    }
}

// ============================================================================
// Subject observer and modal callbacks
// ============================================================================

// Subject observer callback - routes notifications to appropriate display
static void notification_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;

    // Get notification data from subject
    NotificationData* data = (NotificationData*)lv_subject_get_pointer(subject);
    if (!data) {
        // Silently ignore - this happens during initialization when subject is nullptr
        return;
    }
    if (!data->message) {
        spdlog::warn("[Notification] Notification observer received data with null message");
        return;
    }

    // Route to modal or toast based on show_modal flag
    if (data->show_modal) {
        ui_notification_error(data->title, data->message, true);
    } else {
        // Route to toast based on severity
        switch (data->severity) {
        case ToastSeverity::INFO:
            ui_notification_info(data->message);
            break;
        case ToastSeverity::SUCCESS:
            ui_notification_success(data->message);
            break;
        case ToastSeverity::WARNING:
            ui_notification_warning(data->message);
            break;
        case ToastSeverity::ERROR:
            ui_notification_error(nullptr, data->message, false);
            break;
        }
    }

    spdlog::debug("[Notification] Notification routed: modal={}, severity={}, msg={}",
                  data->show_modal, static_cast<int>(data->severity), data->message);
}
