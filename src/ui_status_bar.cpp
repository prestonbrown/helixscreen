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

#include "ui_status_bar.h"

#include "ui_nav.h"
#include "ui_panel_notification_history.h"
#include "ui_theme.h"

#include "app_globals.h"
#include "printer_state.h"

#include <cstring>
#include <spdlog/spdlog.h>

// Forward declaration for class-based API
NotificationHistoryPanel& get_global_notification_history_panel();

// Cached widget references
static lv_obj_t* network_icon = nullptr;
static lv_obj_t* printer_icon = nullptr;
static lv_obj_t* notification_icon = nullptr;
static lv_obj_t* notification_badge = nullptr;
static lv_obj_t* notification_badge_count = nullptr;

// Cached state for combined printer icon logic
static int32_t cached_connection_state = 0;
static int32_t cached_klippy_state = 0; // 0=READY, 1=STARTUP, 2=SHUTDOWN, 3=ERROR

// Forward declaration
static void update_printer_icon_combined();

// Observer callback for network state changes
static void network_status_observer(lv_observer_t* observer, lv_subject_t* subject) {
    int32_t network_state = lv_subject_get_int(subject);
    spdlog::debug("[StatusBar] Network observer fired! State: {}", network_state);

    // Map integer to NetworkStatus enum
    NetworkStatus status = static_cast<NetworkStatus>(network_state);
    ui_status_bar_update_network(status);
}

// Observer callback for printer connection state changes
static void printer_connection_observer(lv_observer_t* observer, lv_subject_t* subject) {
    cached_connection_state = lv_subject_get_int(subject);
    spdlog::debug("[StatusBar] Connection state changed to: {}", cached_connection_state);
    update_printer_icon_combined();
}

// Observer callback for klippy state changes
static void klippy_state_observer(lv_observer_t* observer, lv_subject_t* subject) {
    cached_klippy_state = lv_subject_get_int(subject);
    spdlog::debug("[StatusBar] Klippy state changed to: {}", cached_klippy_state);
    update_printer_icon_combined();
}

// Combined logic to update printer icon based on both connection and klippy state
static void update_printer_icon_combined() {
    if (!printer_icon) {
        return;
    }

    // Klippy state takes precedence when connected
    // ConnectionState: 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED, 3=RECONNECTING, 4=FAILED
    // KlippyState: 0=READY, 1=STARTUP, 2=SHUTDOWN, 3=ERROR

    lv_color_t color;

    if (cached_connection_state == 2) { // CONNECTED to Moonraker
        // Check klippy state
        switch (cached_klippy_state) {
        case 1: // STARTUP (restarting)
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "warning_color"));
            spdlog::debug("[StatusBar] Klippy STARTUP -> printer icon, orange");
            break;
        case 2: // SHUTDOWN
        case 3: // ERROR
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "error_color"));
            spdlog::debug("[StatusBar] Klippy SHUTDOWN/ERROR -> printer icon, red");
            break;
        case 0: // READY
        default:
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "success_color"));
            spdlog::debug("[StatusBar] Klippy READY -> printer icon, green");
            break;
        }
    } else if (cached_connection_state == 4) { // FAILED
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "error_color"));
        spdlog::debug("[StatusBar] Connection FAILED -> printer icon, red");
    } else { // DISCONNECTED, CONNECTING, RECONNECTING
        if (get_printer_state().was_ever_connected()) {
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "warning_color"));
            spdlog::debug("[StatusBar] Disconnected (was connected) -> printer icon, yellow");
        } else {
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "text_secondary"));
            spdlog::debug("[StatusBar] Never connected -> printer icon, gray");
        }
    }

    // Printer icon is a Material Design image (lv_image), NOT an lv_label
    // Use image recolor to change the icon color (same as network_icon)
    lv_obj_set_style_image_recolor(printer_icon, color, 0);
    lv_obj_set_style_image_recolor_opa(printer_icon, LV_OPA_COVER, 0);
}

// Track notification panel to prevent multiple instances
static lv_obj_t* g_notification_panel_obj = nullptr;

// Event callback for notification history button
static void status_notification_history_clicked(lv_event_t* e) {
    spdlog::info("[StatusBar] Notification history button CLICKED!");

    // Prevent multiple panel instances - if panel already exists and is visible, ignore click
    if (g_notification_panel_obj && lv_obj_is_valid(g_notification_panel_obj) &&
        !lv_obj_has_flag(g_notification_panel_obj, LV_OBJ_FLAG_HIDDEN)) {
        spdlog::debug("[StatusBar] Notification panel already visible, ignoring click");
        return;
    }

    lv_obj_t* parent = lv_screen_active();

    // Get panel instance and init subjects BEFORE creating XML
    // (subjects must be registered for XML bindings to work)
    auto& panel = get_global_notification_history_panel();
    if (!panel.are_subjects_initialized()) {
        panel.init_subjects();
    }

    // Clean up old panel if it exists but is hidden/invalid
    if (g_notification_panel_obj) {
        if (lv_obj_is_valid(g_notification_panel_obj)) {
            lv_obj_delete(g_notification_panel_obj);
        }
        g_notification_panel_obj = nullptr;
    }

    // Now create XML component - bindings can find the registered subjects
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "notification_history_panel", nullptr));
    if (!panel_obj) {
        spdlog::error("[StatusBar] Failed to create notification_history_panel from XML");
        return;
    }

    // Store reference for duplicate prevention
    g_notification_panel_obj = panel_obj;

    // Setup panel (wires buttons, refreshes list)
    panel.setup(panel_obj, parent);

    ui_nav_push_overlay(panel_obj);
}

void ui_status_bar_register_callbacks() {
    // Register notification history callback (must be called BEFORE app_layout XML is created)
    lv_xml_register_event_cb(NULL, "status_notification_history_clicked",
                             status_notification_history_clicked);
    spdlog::debug("[StatusBar] Event callbacks registered");
}

void ui_status_bar_init() {
    spdlog::debug("[StatusBar] ui_status_bar_init() called");

    // Status icons are now in the navigation bar (sidebar bottom)
    // Search from screen root to find them anywhere in the widget tree
    lv_obj_t* screen = lv_screen_active();

    // Find status icons by name (search entire screen)
    network_icon = lv_obj_find_by_name(screen, "status_network_icon");
    printer_icon = lv_obj_find_by_name(screen, "status_printer_icon");

    // Bell icon and badge are nested in status_notification_history_container
    lv_obj_t* notif_container =
        lv_obj_find_by_name(screen, "status_notification_history_container");
    if (notif_container) {
        notification_icon = lv_obj_find_by_name(notif_container, "status_notification_icon");
        notification_badge = lv_obj_find_by_name(notif_container, "notification_badge");
        if (notification_badge) {
            notification_badge_count =
                lv_obj_find_by_name(notification_badge, "notification_badge_count");
        }
    }

    spdlog::debug(
        "[StatusBar] Widget lookup: network_icon={}, printer_icon={}, notification_icon={}",
        (void*)network_icon, (void*)printer_icon, (void*)notification_icon);

    if (!network_icon || !printer_icon || !notification_icon) {
        spdlog::error("[StatusBar] Failed to find status bar icon widgets");
        return;
    }

    if (!notification_badge || !notification_badge_count) {
        spdlog::warn("[StatusBar] Failed to find notification badge widgets");
    }

    // Observe network and printer states for reactive icon updates
    PrinterState& printer_state = get_printer_state();

    // Network status observer (fires immediately with current value on registration)
    lv_subject_t* net_subject = printer_state.get_network_status_subject();
    spdlog::debug("[StatusBar] Registering observer on network_status_subject at {}",
                  (void*)net_subject);
    lv_subject_add_observer(net_subject, network_status_observer, nullptr);

    // Printer connection observer (fires immediately with current value on registration)
    lv_subject_t* conn_subject = printer_state.get_printer_connection_state_subject();
    spdlog::debug("[StatusBar] Registering observer on printer_connection_state_subject at {}",
                  (void*)conn_subject);
    lv_subject_add_observer(conn_subject, printer_connection_observer, nullptr);

    // Klippy state observer (for RESTART/FIRMWARE_RESTART handling)
    lv_subject_t* klippy_subject = printer_state.get_klippy_state_subject();
    spdlog::debug("[StatusBar] Registering observer on klippy_state_subject at {}",
                  (void*)klippy_subject);
    lv_subject_add_observer(klippy_subject, klippy_state_observer, nullptr);

    // Set bell icon to neutral color (stays this way - badge color indicates severity)
    // Unlike network/printer icons which change color based on state, bell stays neutral
    if (notification_icon) {
        lv_color_t neutral = ui_theme_parse_color(lv_xml_get_const(NULL, "text_secondary"));
        lv_obj_set_style_image_recolor(notification_icon, neutral, 0);
        lv_obj_set_style_image_recolor_opa(notification_icon, LV_OPA_COVER, 0);
    }

    spdlog::debug("[StatusBar] Initialization complete");
}

void ui_status_bar_update_network(NetworkStatus status) {
    if (!network_icon) {
        spdlog::warn("Status bar not initialized, cannot update network icon");
        return;
    }

    // Network icon is a Material Design image (mat_lan - LAN network indicator)
    lv_color_t color;

    switch (status) {
    case NetworkStatus::CONNECTED:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "success_color"));
        break;
    case NetworkStatus::CONNECTING:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "warning_color"));
        break;
    case NetworkStatus::DISCONNECTED:
    default:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "text_secondary"));
        break;
    }

    // Update image recolor for Material Design icon
    lv_obj_set_style_image_recolor(network_icon, color, 0);
    lv_obj_set_style_image_recolor_opa(network_icon, LV_OPA_COVER, 0);
}

void ui_status_bar_update_printer(PrinterStatus status) {
    spdlog::debug("[StatusBar] ui_status_bar_update_printer() called with status={}",
                  static_cast<int>(status));

    if (!printer_icon) {
        spdlog::warn("[StatusBar] printer_icon is NULL, cannot update");
        return;
    }

    // Printer icon is a Material Design image (mat_printer_3d)
    // Color indicates state: green=ready, blue=printing, red=error, yellow=was connected,
    // gray=never connected
    lv_color_t color;

    switch (status) {
    case PrinterStatus::READY:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "success_color"));
        spdlog::debug("[StatusBar] Setting printer icon to green (ready)");
        break;
    case PrinterStatus::PRINTING:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "info_color"));
        spdlog::debug("[StatusBar] Setting printer icon to blue (printing)");
        break;
    case PrinterStatus::ERROR:
        color = ui_theme_parse_color(lv_xml_get_const(NULL, "error_color"));
        spdlog::debug("[StatusBar] Setting printer icon to red (error)");
        break;
    case PrinterStatus::DISCONNECTED:
    default:
        // Distinguish "never connected" (neutral) from "lost connection" (warning)
        if (get_printer_state().was_ever_connected()) {
            // Was connected before - show warning
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "warning_color"));
            spdlog::debug("[StatusBar] Setting printer icon to yellow (was connected)");
        } else {
            // Never connected - show neutral gray
            color = ui_theme_parse_color(lv_xml_get_const(NULL, "text_secondary"));
            spdlog::debug("[StatusBar] Setting printer icon to gray (never connected)");
        }
        break;
    }

    // Update image recolor for Material Design icon
    lv_obj_set_style_image_recolor(printer_icon, color, 0);
    lv_obj_set_style_image_recolor_opa(printer_icon, LV_OPA_COVER, 0);
    spdlog::debug("[StatusBar] Printer icon updated successfully");
}

void ui_status_bar_update_notification(NotificationStatus status) {
    if (!notification_badge) {
        spdlog::warn("Status bar not initialized, cannot update notification badge");
        return;
    }

    // Badge background color indicates highest severity:
    // red = error, yellow/orange = warning, blue = info
    // Bell icon stays neutral - badge color alone communicates urgency
    lv_color_t badge_color;

    switch (status) {
    case NotificationStatus::ERROR:
        badge_color = ui_theme_parse_color(lv_xml_get_const(NULL, "error_color"));
        break;
    case NotificationStatus::WARNING:
        badge_color = ui_theme_parse_color(lv_xml_get_const(NULL, "warning_color"));
        break;
    case NotificationStatus::INFO:
        badge_color = ui_theme_parse_color(lv_xml_get_const(NULL, "info_color"));
        break;
    case NotificationStatus::NONE:
    default:
        // Default to info color if somehow called with NONE but badge visible
        badge_color = ui_theme_parse_color(lv_xml_get_const(NULL, "info_color"));
        break;
    }

    // Update badge background color (not the bell icon)
    lv_obj_set_style_bg_color(notification_badge, badge_color, 0);
}

void ui_status_bar_update_notification_count(size_t count) {
    if (!notification_badge || !notification_badge_count) {
        spdlog::trace("Notification badge widgets not available");
        return;
    }

    if (count == 0) {
        // Hide badge when no unread notifications
        lv_obj_add_flag(notification_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Show badge and update count
        lv_obj_remove_flag(notification_badge, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(notification_badge_count, "%zu", count);
    }

    spdlog::trace("Notification count updated: {}", count);
}
