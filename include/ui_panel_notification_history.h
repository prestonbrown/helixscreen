// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification_history.h"
#include "ui_panel_base.h"

#include "subject_managed_panel.h"

#include <lvgl.h>

/**
 * @file ui_panel_notification_history.h
 * @brief Notification history overlay panel
 *
 * Displays a scrollable list of past notifications with clear-all
 * functionality. Shows severity-colored cards for each entry.
 *
 * ## Key Features:
 * - Lists all notifications from NotificationHistory service
 * - Clear All button to purge history
 * - Empty state when no notifications
 * - Marks notifications as read when viewed
 *
 * ## DI Pattern:
 * This panel demonstrates dependency injection with a service class:
 * - Constructor accepts NotificationHistory& (defaults to singleton)
 * - Enables unit testing with mock NotificationHistory
 * - Decouples panel from global state
 *
 * ## Migration Notes:
 * Phase 3 panel - first to demonstrate service injection pattern.
 * Uses overlay panel helpers (ui_overlay_panel_setup_standard).
 *
 * @see PanelBase for base class documentation
 * @see NotificationHistory for the injected service
 * @see ui_overlay_panel_setup_standard for overlay wiring
 */
class NotificationHistoryPanel : public PanelBase {
  public:
    /**
     * @brief Construct NotificationHistoryPanel with injected dependencies
     *
     * @param printer_state Reference to PrinterState (passed to base, not used directly)
     * @param api Pointer to IMoonrakerAPI (passed to base, not used directly)
     * @param history Reference to NotificationHistory service (defaults to singleton)
     */
    NotificationHistoryPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api,
                             NotificationHistory& history = NotificationHistory::instance());

    ~NotificationHistoryPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize subjects for reactive bindings
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize subjects to prevent dangling references
     */
    void deinit_subjects();

    /**
     * @brief Setup the notification history panel
     *
     * Wires back button and clear button handlers, then refreshes the list.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen for overlay navigation
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Notification History Panel";
    }
    const char* get_xml_component_name() const override {
        return "notification_history_panel";
    }

    //
    // === Public API ===
    //

    /**
     * @brief Refresh the notification list
     *
     * Called when panel is shown or after clear.
     * Rebuilds the list from NotificationHistory service.
     */
    void refresh();

  private:
    //
    // === Injected Dependencies ===
    //

    NotificationHistory& history_;

    //
    // === Subjects ===
    //

    /// RAII subject manager for automatic cleanup
    SubjectManager subjects_;

    /// Has entries subject (1 = has entries, 0 = empty)
    lv_subject_t has_entries_subject_;

    //
    // === Private Helpers ===
    //

    /**
     * @brief Convert ToastSeverity to XML string
     */
    static const char* severity_to_string(ToastSeverity severity);

    /**
     * @brief Format timestamp as relative time string
     */
    static std::string format_timestamp(uint64_t timestamp_ms);

    //
    // === Button Handlers ===
    //

    void handle_clear_clicked();

    //
    // === Per-item click context ===
    //

    /// Stored in event callback user_data (NOT lv_obj user_data, which belongs to
    /// the XML widget layer — e.g. severity_card uses it for the severity string).
    /// Freed automatically via LV_EVENT_DELETE callback when the item is destroyed.
    struct ClickContext {
        NotificationHistoryPanel* panel;
        char action[64];
    };

    //
    // === Static Trampolines ===
    //

    static void on_clear_clicked(lv_event_t* e);

    // Action dispatch
    static void on_item_clicked(lv_event_t* e);
    static void on_item_deleted(lv_event_t* e);
    void dispatch_action(const char* action);
};

// Global instance accessor (needed by main.cpp)
NotificationHistoryPanel& get_global_notification_history_panel();
