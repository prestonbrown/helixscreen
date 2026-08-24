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
#include "ui_notification_threshold.h"
#include "ui_observer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "fault_modal_registry.h"
#include "pending_startup_warnings.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

// Forward declarations
static void notification_observer_cb(lv_observer_t* observer, lv_subject_t* subject);
static void show_notification(const char* title, const char* message, ToastSeverity severity,
                              uint32_t duration_ms);
// Thread tracking for auto-detection
static std::thread::id g_main_thread_id;
static std::atomic<bool> g_main_thread_id_initialized{false};

// True if `sev` meets the user's toast threshold (#1213). Reads the header-only
// inline cache that SafetySettingsManager pushes to; safe on any thread.
static bool should_show_toast(ToastSeverity sev) {
    return helix::ui::notifications::severity_meets_threshold(
        static_cast<int>(sev), helix::ui::notifications::get_min_toast_severity_cache());
}

// RAII observer guard for automatic cleanup
static ObserverGuard s_notification_observer;

// Check if we're on the LVGL main thread
static bool is_main_thread() {
    if (!g_main_thread_id_initialized.load()) {
        return true; // Before initialization, assume main thread
    }
    return std::this_thread::get_id() == g_main_thread_id;
}

// Redirect notifications to PendingStartupWarnings when ToastManager::init() has
// not run yet. Without this, early NOTIFY_* calls (e.g. from config restore in
// Application phase 2 or TipsManager in phase 4) queue a toast that fires inside
// the splash's lv_timer_handler, before subjects are initialized and the
// toast_notification XML component is registered — which emits bogus
// "type=0" subject warnings and "toast_notification is not a known widget"
// errors on every boot after a settings restore / tips load failure.
// Application::run() drains PendingStartupWarnings right after ToastManager::init().
static bool try_defer_to_startup_queue(ToastSeverity severity, const char* display_msg) {
    if (!display_msg || !*display_msg)
        return false;
    if (ToastManager::instance().is_initialized())
        return false;

    helix::PendingStartupWarnings::Severity sev;
    switch (severity) {
    case ToastSeverity::INFO:
        sev = helix::PendingStartupWarnings::Severity::INFO;
        break;
    case ToastSeverity::SUCCESS:
        sev = helix::PendingStartupWarnings::Severity::SUCCESS;
        break;
    case ToastSeverity::WARNING:
        sev = helix::PendingStartupWarnings::Severity::WARNING;
        break;
    case ToastSeverity::ERROR:
        sev = helix::PendingStartupWarnings::Severity::ERROR;
        break;
    }
    helix::PendingStartupWarnings::instance().enqueue(sev, display_msg);
    return true;
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
    bool fault; ///< Raised by ui_notification_printer_fault() — track for sweeping
};

// Format "title: message" (or just message when no title) into caller-provided
// buffer. Buffer must be at least 324 bytes (matches title(64)+": "(2)+message(256)+null).
static void format_titled_display(char* out, size_t out_size, bool has_title, const char* title,
                                  const char* message) {
    if (has_title) {
        snprintf(out, out_size, "%s: %s", title, message);
    } else {
        strncpy(out, message, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

// Async callbacks for lv_async_call (called on main thread)
static void async_message_callback(void* user_data) {
    AsyncMessageData* data = (AsyncMessageData*)user_data;
    if (data && data->message[0] != '\0') {
        char display_buf[324]; // title(64) + ": "(2) + message(256) + null
        format_titled_display(display_buf, sizeof(display_buf), data->has_title, data->title,
                              data->message);
        if (try_defer_to_startup_queue(data->severity, display_buf)) {
            delete data;
            return;
        }
        if (should_show_toast(data->severity)) {
            ToastManager::instance().show(data->severity, display_buf, data->duration_ms);
        }

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
        helix::ui::notification_refresh_from_history();
    }
    delete data;
}

static void async_error_callback(void* user_data) {
    AsyncErrorData* data = (AsyncErrorData*)user_data;
    if (data && data->message[0] != '\0') {
        // Pre-init: drop to startup queue (modal degrades to toast — ui_dialog
        // XML isn't registered yet either).
        {
            char display_buf[324];
            format_titled_display(display_buf, sizeof(display_buf), data->has_title, data->title,
                                  data->message);
            if (try_defer_to_startup_queue(ToastSeverity::ERROR, display_buf)) {
                delete data;
                return;
            }
        }

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
            lv_obj_t* dialog =
                helix::ui::modal_show_alert(data->title, data->message, ModalSeverity::Error, "OK");
            if (data->fault) {
                helix::ui::track_fault_modal(dialog);
            }

            helix::ui::notification_update(NotificationStatus::ERROR);
        } else {
            // Show toast for non-critical errors (still subject to the user's
            // min-severity gate; modal errors above bypass it deliberately).
            if (should_show_toast(ToastSeverity::ERROR)) {
                ToastManager::instance().show(ToastSeverity::ERROR, data->message, 6000);
            }
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
        helix::ui::notification_refresh_from_history();
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

// ============================================================================
// Titled / untitled variants (display "Title: message" in toast when titled,
// store title in history)
// ============================================================================

void ui_notification_info(const char* message) {
    show_notification(nullptr, message, ToastSeverity::INFO, 4000);
}

void ui_notification_success(const char* message) {
    show_notification(nullptr, message, ToastSeverity::SUCCESS, 4000);
}

void ui_notification_warning(const char* message) {
    show_notification(nullptr, message, ToastSeverity::WARNING, 5000);
}

// Helper to show notification with optional title. When title is null/empty the
// toast displays just the message; otherwise it shows "Title: message" and the
// title is stored in history.
static void show_notification(const char* title, const char* message, ToastSeverity severity,
                              uint32_t duration_ms) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show notification with null message");
        return;
    }

    const bool has_title = title && *title;
    std::string display_msg = has_title ? (std::string(title) + ": " + message) : message;

    if (try_defer_to_startup_queue(severity, display_msg.c_str()))
        return;

    if (is_main_thread()) {
        // Main thread: call LVGL directly
        if (should_show_toast(severity)) {
            ToastManager::instance().show(severity, display_msg.c_str(), duration_ms);
        }

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = severity;
        entry.was_modal = false;
        entry.was_read = false;
        if (has_title) {
            strncpy(entry.title, title, sizeof(entry.title) - 1);
            entry.title[sizeof(entry.title) - 1] = '\0';
        } else {
            entry.title[0] = '\0';
        }
        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_refresh_from_history();
    } else {
        // Background thread: marshal to main thread
        auto* data = new (std::nothrow) AsyncMessageData{};
        if (!data) {
            spdlog::error("[Notification] Failed to allocate memory for async notification");
            return;
        }

        if (has_title) {
            strncpy(data->title, title, sizeof(data->title) - 1);
            data->title[sizeof(data->title) - 1] = '\0';
        } else {
            data->title[0] = '\0';
        }
        data->has_title = has_title;
        strncpy(data->message, message, sizeof(data->message) - 1);
        data->message[sizeof(data->message) - 1] = '\0';
        data->severity = severity;
        data->duration_ms = duration_ms;

        helix::ui::async_call(async_message_callback, data);
    }
}

void ui_notification_info(const char* title, const char* message) {
    show_notification(title, message, ToastSeverity::INFO, 4000);
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
        helix::ui::notification_refresh_from_history();
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
                helix::ui::notification_refresh_from_history();
                delete e;
            },
            data);
    }

    spdlog::info("[Notification] History-only notification: '{}' action='{}'", message, action);
}

void ui_notification_success(const char* title, const char* message) {
    show_notification(title, message, ToastSeverity::SUCCESS, 4000);
}

void ui_notification_warning(const char* title, const char* message) {
    show_notification(title, message, ToastSeverity::WARNING, 5000);
}

namespace {

struct DetailNotification {
    ToastSeverity severity;
    std::string message;
    std::string detail;
    uint32_t duration_ms;
};

/// Two-line toast + a joined history row.
///
/// Uses queue_update() rather than the is_main_thread() fork the single-line
/// paths use: the payload is two variable-length strings, and the fixed 64/256
/// char arrays those paths marshal through exist only to avoid a malloc per
/// toast on the hot single-line path. Everything below the queue therefore runs
/// on the main thread unconditionally, which is what NotificationHistory and
/// lv_tick_get() need.
void show_detail_notification(ToastSeverity severity, const char* message, const char* detail,
                              uint32_t duration_ms) {
    if (!message || !*message) {
        spdlog::warn("[Notification] Attempted to show detail notification with empty message");
        return;
    }
    if (!detail || !*detail) {
        // Nothing to add — fall through to the ordinary single-line paths so
        // there is exactly one implementation of "toast with just a message".
        if (severity == ToastSeverity::ERROR) {
            ui_notification_error(nullptr, message, false);
        } else {
            ui_notification_warning(message);
        }
        return;
    }

    auto params = std::make_unique<DetailNotification>(
        DetailNotification{severity, message, detail, duration_ms});

    helix::ui::queue_update<DetailNotification>(std::move(params), [](DetailNotification* p) {
        // A history row is one line, and the pre-init startup queue only carries
        // one string, so both get the halves joined.
        const std::string joined = p->message + " - " + p->detail;

        if (try_defer_to_startup_queue(p->severity, joined.c_str()))
            return;

        if (should_show_toast(p->severity)) {
            ToastManager::instance().show_with_detail(p->severity, p->message.c_str(),
                                                      p->detail.c_str(), p->duration_ms);
        }

        NotificationHistoryEntry entry = {};
        entry.timestamp_ms = lv_tick_get();
        entry.severity = p->severity;
        entry.was_modal = false;
        entry.was_read = false;
        entry.title[0] = '\0';
        strncpy(entry.message, joined.c_str(), sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        NotificationHistory::instance().add(entry);
        helix::ui::notification_refresh_from_history();

        if (p->severity == ToastSeverity::ERROR) {
            helix::ui::notification_update(NotificationStatus::ERROR);
        }
    });
}

} // namespace

void ui_notification_error_with_detail(const char* message, const char* detail) {
    show_detail_notification(ToastSeverity::ERROR, message, detail, 8000);
}

void ui_notification_warning_with_detail(const char* message, const char* detail) {
    show_detail_notification(ToastSeverity::WARNING, message, detail, 8000);
}

// Shared body for ui_notification_error() and ui_notification_printer_fault().
// @p fault only decides whether the resulting dialog joins the sweep registry;
// presentation is identical either way.
static void show_error_notification(const char* title, const char* message, bool modal,
                                    bool fault) {
    if (!message) {
        spdlog::warn("[Notification] Attempted to show error notification with null message");
        return;
    }

    // Pre-init (before ToastManager::init() and modal XML registration) route to
    // the startup queue. The queue drains as plain toasts — modal dialogs are
    // degraded to error toasts in that window, since ui_dialog's XML component
    // also isn't registered yet.
    {
        const bool has_title = title && *title;
        std::string display = has_title ? (std::string(title) + ": " + message) : message;
        if (try_defer_to_startup_queue(ToastSeverity::ERROR, display.c_str()))
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
            lv_obj_t* dialog =
                helix::ui::modal_show_alert(title, message, ModalSeverity::Error, "OK");
            if (fault) {
                helix::ui::track_fault_modal(dialog);
            }

            helix::ui::notification_update(NotificationStatus::ERROR);
        } else {
            // Show toast for non-critical errors (subject to the min-severity gate;
            // modal errors above bypass it deliberately).
            if (should_show_toast(ToastSeverity::ERROR)) {
                ToastManager::instance().show(ToastSeverity::ERROR, message, 6000);
            }
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
        helix::ui::notification_refresh_from_history();
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
        data->fault = fault;

        helix::ui::async_call(async_error_callback, data);
    }
}

void ui_notification_error(const char* title, const char* message, bool modal) {
    show_error_notification(title, message, modal, /*fault=*/false);
}

void ui_notification_printer_fault(const char* title, const char* message) {
    show_error_notification(title, message, /*modal=*/true, /*fault=*/true);
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
