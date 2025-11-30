// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_toast.h"

#include "ui_notification_history.h"
#include "ui_severity_card.h"
#include "ui_status_bar.h"
#include "ui_subject_registry.h"

#include <spdlog/spdlog.h>

// Active toast state
static lv_obj_t* active_toast = nullptr;
static lv_timer_t* dismiss_timer = nullptr;

// Action button state
static toast_action_callback_t action_callback = nullptr;
static void* action_user_data = nullptr;

// Subjects for action button (registered globally for XML binding)
static lv_subject_t toast_action_visible_subject;
static lv_subject_t toast_action_text_subject;
static char toast_action_text_buf[64] = "";
static bool subjects_initialized = false;

// Forward declarations
static void toast_dismiss_timer_cb(lv_timer_t* timer);
static void toast_close_btn_clicked(lv_event_t* e);
static void toast_action_btn_clicked(lv_event_t* e);

void ui_toast_init() {
    // Initialize action button subjects (only once)
    if (!subjects_initialized) {
        lv_subject_init_int(&toast_action_visible_subject, 0);
        lv_xml_register_subject(NULL, "toast_action_visible", &toast_action_visible_subject);

        lv_subject_init_pointer(&toast_action_text_subject, toast_action_text_buf);
        lv_xml_register_subject(NULL, "toast_action_text", &toast_action_text_subject);

        subjects_initialized = true;
    }

    spdlog::debug("Toast notification system initialized");
}

// Convert ToastSeverity enum to string for XML
static const char* severity_to_string(ToastSeverity severity) {
    switch (severity) {
    case ToastSeverity::ERROR:
        return "error";
    case ToastSeverity::WARNING:
        return "warning";
    case ToastSeverity::SUCCESS:
        return "success";
    case ToastSeverity::INFO:
    default:
        return "info";
    }
}

static NotificationStatus severity_to_notification_status(ToastSeverity severity) {
    switch (severity) {
    case ToastSeverity::INFO:
        return NotificationStatus::INFO;
    case ToastSeverity::SUCCESS:
        return NotificationStatus::INFO; // Treat success as info in status bar
    case ToastSeverity::WARNING:
        return NotificationStatus::WARNING;
    case ToastSeverity::ERROR:
        return NotificationStatus::ERROR;
    default:
        return NotificationStatus::NONE;
    }
}

// Internal helper to create and configure a toast
static void create_toast_internal(ToastSeverity severity, const char* message, uint32_t duration_ms,
                                   bool with_action) {
    if (!message) {
        spdlog::warn("Attempted to show toast with null message");
        return;
    }

    // Hide existing toast if any
    if (active_toast) {
        ui_toast_hide();
    }

    // Clear action state for basic toasts, keep for action toasts
    if (!with_action) {
        action_callback = nullptr;
        action_user_data = nullptr;
        lv_subject_set_int(&toast_action_visible_subject, 0);
    }

    // Create toast via XML component - just pass semantic severity
    const char* attrs[] = {"severity", severity_to_string(severity), "message", message, nullptr};

    lv_obj_t* screen = lv_screen_active();
    lv_xml_create(screen, "toast_notification", attrs);

    // Find the created toast (should be last child of screen)
    uint32_t child_cnt = lv_obj_get_child_count(screen);
    active_toast = (child_cnt > 0) ? lv_obj_get_child(screen, child_cnt - 1) : nullptr;

    if (!active_toast) {
        spdlog::error("Failed to create toast notification widget");
        return;
    }

    // Finalize severity styling for children (icon text and color)
    ui_severity_card_finalize(active_toast);

    // Wire up close button callback
    lv_obj_t* close_btn = lv_obj_find_by_name(active_toast, "toast_close_btn");
    if (close_btn) {
        lv_obj_add_event_cb(close_btn, toast_close_btn_clicked, LV_EVENT_CLICKED, nullptr);
    }

    // Wire up action button callback (if showing action toast)
    if (with_action) {
        lv_obj_t* action_btn = lv_obj_find_by_name(active_toast, "toast_action_btn");
        if (action_btn) {
            lv_obj_add_event_cb(action_btn, toast_action_btn_clicked, LV_EVENT_CLICKED, nullptr);
        }
    }

    // Create auto-dismiss timer
    dismiss_timer = lv_timer_create(toast_dismiss_timer_cb, duration_ms, nullptr);
    lv_timer_set_repeat_count(dismiss_timer, 1); // Run once then stop

    // Update status bar notification icon
    ui_status_bar_update_notification(severity_to_notification_status(severity));

    spdlog::debug("Toast shown: {} ({}ms, action={})", message, duration_ms, with_action);
}

void ui_toast_show(ToastSeverity severity, const char* message, uint32_t duration_ms) {
    create_toast_internal(severity, message, duration_ms, false);
}

void ui_toast_show_with_action(ToastSeverity severity, const char* message,
                                const char* action_text, toast_action_callback_t callback,
                                void* user_data, uint32_t duration_ms) {
    if (!action_text || !callback) {
        spdlog::warn("Toast action requires action_text and callback");
        ui_toast_show(severity, message, duration_ms);
        return;
    }

    // Store callback for when action button is clicked
    action_callback = callback;
    action_user_data = user_data;

    // Update action button text and visibility via subjects
    snprintf(toast_action_text_buf, sizeof(toast_action_text_buf), "%s", action_text);
    lv_subject_set_pointer(&toast_action_text_subject, toast_action_text_buf);
    lv_subject_set_int(&toast_action_visible_subject, 1);

    create_toast_internal(severity, message, duration_ms, true);
}

void ui_toast_hide() {
    if (!active_toast) {
        return;
    }

    // Cancel dismiss timer if active
    if (dismiss_timer) {
        lv_timer_delete(dismiss_timer);
        dismiss_timer = nullptr;
    }

    // Clear action state
    action_callback = nullptr;
    action_user_data = nullptr;
    lv_subject_set_int(&toast_action_visible_subject, 0);

    // Delete toast widget
    lv_obj_delete(active_toast);
    active_toast = nullptr;

    // Update bell color based on highest unread severity in history
    ToastSeverity highest = NotificationHistory::instance().get_highest_unread_severity();
    size_t unread = NotificationHistory::instance().get_unread_count();

    if (unread == 0) {
        ui_status_bar_update_notification(NotificationStatus::NONE);
    } else {
        ui_status_bar_update_notification(severity_to_notification_status(highest));
    }

    spdlog::debug("Toast hidden");
}

bool ui_toast_is_visible() {
    return active_toast != nullptr;
}

// Timer callback for auto-dismiss
static void toast_dismiss_timer_cb(lv_timer_t* timer) {
    (void)timer;
    ui_toast_hide();
}

// Close button callback
static void toast_close_btn_clicked(lv_event_t* e) {
    (void)e;
    ui_toast_hide();
}

// Action button callback
static void toast_action_btn_clicked(lv_event_t* e) {
    (void)e;

    // Store callback before hiding (hide clears action_callback)
    toast_action_callback_t cb = action_callback;
    void* data = action_user_data;

    // Hide the toast first
    ui_toast_hide();

    // Then invoke the callback
    if (cb) {
        spdlog::debug("Toast action button clicked - invoking callback");
        cb(data);
    }
}
