// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_notification_history.h"

#include "ui_event_safety.h"
#include "ui_format_utils.h"
#include "ui_nav_manager.h"
#include "ui_notification_manager.h"
#include "ui_panel_common.h"
#include "ui_severity_card.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "system/update_checker.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

NotificationHistoryPanel::NotificationHistoryPanel(PrinterState& printer_state, IMoonrakerAPI* api,
                                                   NotificationHistory& history)
    : PanelBase(printer_state, api), history_(history) {
    // Dependencies stored for use in refresh()
}

NotificationHistoryPanel::~NotificationHistoryPanel() {
    deinit_subjects();
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void NotificationHistoryPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Has entries subject: 1 = has entries (show content), 0 = empty (show empty state)
    UI_MANAGED_SUBJECT_INT(has_entries_subject_, 0, "notification_has_entries", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (1 subject)", get_name());
}

void NotificationHistoryPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void NotificationHistoryPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Use standard overlay panel setup (wires back button automatically)
    ui_overlay_panel_setup_standard(panel_, parent_screen_, "overlay_header", "overlay_content");

    // Wire action button ("Clear All") to clear callback
    lv_obj_t* action_btn =
        ui_overlay_panel_wire_action_button(panel_, on_clear_clicked, "overlay_header", this);

    // Hide Clear All button when there are no notifications
    if (action_btn) {
        lv_obj_bind_flag_if_eq(action_btn, &has_entries_subject_, LV_OBJ_FLAG_HIDDEN, 0);
    }

    // Populate list
    refresh();

    spdlog::info("[{}] Setup complete", get_name());
}

// ============================================================================
// PUBLIC API
// ============================================================================

void NotificationHistoryPanel::refresh() {
    if (!panel_) {
        spdlog::warn("[{}] Cannot refresh - panel not created", get_name());
        return;
    }

    // Get all entries (filter buttons removed from UI for cleaner look)
    auto entries = history_.get_all();

    // Find content container
    lv_obj_t* overlay_content = lv_obj_find_by_name(panel_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] Could not find overlay_content", get_name());
        return;
    }

    // Clear existing items from content area
    // Action strings are freed automatically via LV_EVENT_DELETE callbacks
    helix::ui::safe_clean_children(overlay_content);

    // Update has_entries subject - XML bindings handle visibility reactively
    bool has_entries = !entries.empty();
    lv_subject_set_int(&has_entries_subject_, has_entries ? 1 : 0);

    // Create list items using severity_card for automatic color styling
    for (const auto& entry : entries) {
        // Format timestamp
        std::string timestamp_str = format_timestamp(entry.timestamp_ms);

        // Use title if present, otherwise use severity-based default
        const char* title = entry.title[0] ? entry.title : lv_tr("Notification");

        // Build attributes array - just pass semantic severity, widget handles colors
        const char* attrs[] = {"severity",  severity_to_string(entry.severity),
                               "title",     title,
                               "message",   entry.message,
                               "timestamp", timestamp_str.c_str(),
                               nullptr};

        // Create item from XML (severity_card sets border color automatically)
        lv_xml_create(overlay_content, "notification_history_item", attrs);

        // Find the most recently created item (last child)
        uint32_t child_cnt = lv_obj_get_child_count(overlay_content);
        lv_obj_t* item =
            (child_cnt > 0) ? lv_obj_get_child(overlay_content, static_cast<int32_t>(child_cnt - 1))
                            : nullptr;
        if (!item) {
            spdlog::error("[{}] Failed to create notification_history_item from XML", get_name());
            continue;
        }

        // Finalize severity styling for children (icon text and color)
        ui_severity_card_finalize(item);

        // If entry has an action, make it clickable
        // Store action string in the event callback's user_data (NOT lv_obj user_data,
        // which is already used by severity_card for the severity string)
        if (entry.action[0] != '\0') {
            auto* ctx = new (std::nothrow) ClickContext{this, {}};
            if (ctx) {
                strncpy(ctx->action, entry.action, sizeof(ctx->action) - 1);
                ctx->action[sizeof(ctx->action) - 1] = '\0';
                lv_obj_add_event_cb(item, on_item_clicked, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(item, on_item_deleted, LV_EVENT_DELETE, ctx);
                lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    // Mark all as read
    history_.mark_all_read();

    // Update status bar - badge count is 0 and bell goes gray (no unread)
    helix::ui::notification_update_count(0);
    helix::ui::notification_update(NotificationStatus::NONE);

    spdlog::debug("[{}] Refreshed: {} entries displayed", get_name(), entries.size());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

const char* NotificationHistoryPanel::severity_to_string(ToastSeverity severity) {
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

std::string NotificationHistoryPanel::format_timestamp(uint64_t timestamp_ms) {
    uint64_t now = lv_tick_get();

    // Handle edge case where timestamp is in future (shouldn't happen)
    uint64_t diff_ms = (timestamp_ms > now) ? 0 : (now - timestamp_ms);

    return helix::ui::format_relative_time(diff_ms);
}

// ============================================================================
// BUTTON HANDLERS
// ============================================================================

void NotificationHistoryPanel::handle_clear_clicked() {
    history_.clear();
    refresh();
    spdlog::info("[{}] History cleared by user", get_name());
}

// ============================================================================
// ACTION DISPATCH
// ============================================================================

void NotificationHistoryPanel::on_item_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[NotificationHistoryPanel] on_item_clicked");
    auto* ctx = static_cast<ClickContext*>(lv_event_get_user_data(e));
    if (ctx && ctx->panel && ctx->action[0] != '\0') {
        ctx->panel->dispatch_action(ctx->action);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void NotificationHistoryPanel::on_item_deleted(lv_event_t* e) {
    auto* ctx = static_cast<ClickContext*>(lv_event_get_user_data(e));
    delete ctx;
}

void NotificationHistoryPanel::dispatch_action(const char* action) {
    spdlog::info("[{}] Dispatching action: {}", get_name(), action);

    if (strcmp(action, "show_update_modal") == 0) {
        // Close notification history overlay first, then show update modal
        NavigationManager::instance().go_back();
        UpdateChecker::instance().show_update_notification();
    } else {
        spdlog::warn("[{}] Unknown action: {}", get_name(), action);
    }
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void NotificationHistoryPanel::on_clear_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[NotificationHistoryPanel] on_clear_clicked");
    auto* self = static_cast<NotificationHistoryPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_clear_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<NotificationHistoryPanel> g_notification_history_panel;

NotificationHistoryPanel& get_global_notification_history_panel() {
    if (!g_notification_history_panel) {
        g_notification_history_panel =
            std::make_unique<NotificationHistoryPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy(
            "NotificationHistoryPanel", []() { g_notification_history_panel.reset(); });
    }
    return *g_notification_history_panel;
}
