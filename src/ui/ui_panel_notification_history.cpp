// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_notification_history.h"

#include "ui_event_safety.h"
#include "ui_format_utils.h"
#include "ui_nav.h"
#include "ui_notification_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "system/update_checker.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <map>
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
    UI_MANAGED_SUBJECT_INT(row_count_subject_, 0, "notification_row_count", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (2 subjects)", get_name());
}

void NotificationHistoryPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    history_version_observer_.reset();
    subjects_.deinit_all();
    for (auto* pool : {&severity_pool_, &title_pool_, &message_pool_, &has_message_pool_,
                       &time_pool_, &has_action_pool_}) {
        pool->reclaim();
    }
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

    // Refresh the list when a notification arrives while the panel is open.
    // The observer attaches at most once per panel lifetime — setup() runs
    // again on every reopen, and the subject is owned by the manager singleton
    // that outlives this panel.
    if (!history_version_observer_) {
        lv_subject_t* version_subject = helix::ui::notification_history_version_subject();
        if (version_subject) {
            history_version_observer_ = helix::ui::observe<int>(
                version_subject, this,
                [](NotificationHistoryPanel* p, int value) {
                    p->handle_history_version_change(value);
                },
                NotificationManager::instance().get_subjects_lifetime());
        } else {
            spdlog::warn("[{}] Notification history version subject not initialized", get_name());
        }
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

    // Update has_entries subject - XML bindings handle visibility reactively
    bool has_entries = !entries.empty();
    lv_subject_set_int(&has_entries_subject_, has_entries ? 1 : 0);

    // Fill the rows' subjects, then publish the count: the <repeat> builds rows
    // bound to slots that already hold their values.
    const size_t n = entries.size();
    for (auto* pool : {&severity_pool_, &title_pool_, &message_pool_, &has_message_pool_,
                       &time_pool_, &has_action_pool_}) {
        pool->ensure_size(n);
    }
    row_actions_.assign(n, std::string{});
    for (size_t i = 0; i < n; ++i) {
        const auto& entry = entries[i];
        // An untitled entry (every plain toast) leads with its message; the icon and
        // border already carry the severity, so a placeholder title says nothing.
        const bool has_title = entry.title[0] != '\0';
        const char* message = has_title ? entry.message : "";
        severity_pool_.set_int(i, severity_index(entry.severity));
        title_pool_.set_string(i, has_title ? entry.title : entry.message);
        message_pool_.set_string(i, message);
        has_message_pool_.set_int(i, message[0] != '\0' ? 1 : 0);
        time_pool_.set_string(i, format_timestamp(entry.timestamp_ms));
        row_actions_[i] = entry.action;
        has_action_pool_.set_int(i, entry.action[0] != '\0' ? 1 : 0);
    }
    lv_subject_set_int(&row_count_subject_, static_cast<int>(n));

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

int NotificationHistoryPanel::severity_index(ToastSeverity severity) {
    switch (severity) {
    case ToastSeverity::ERROR:
        return 3;
    case ToastSeverity::WARNING:
        return 2;
    case ToastSeverity::SUCCESS:
        return 1;
    case ToastSeverity::INFO:
    default:
        return 0;
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

void NotificationHistoryPanel::handle_history_version_change(int32_t version) {
    if (version == last_applied_history_version_) {
        return;
    }
    last_applied_history_version_ = version;

    // A hidden panel is not destroyed (NavigationManager::go_back() only hides
    // the overlay), so the observer stays attached and would otherwise rebuild
    // an invisible list and mark it read. setup() already does an unconditional
    // refresh() on every real (re)open, which catches up on anything that
    // arrived while hidden. A panel hidden only until its queued push lands is
    // opening, and setup() has already run, so it refreshes now.
    if (!panel_ || !helix::nav::is_showing(panel_)) {
        return;
    }
    refresh();
}

void NotificationHistoryPanel::handle_clear_clicked() {
    history_.clear();
    // clear() bumps the store revision; the manager publishes it on its next
    // badge refresh. Record it now so that publish does not rebuild the list
    // again once it lands.
    last_applied_history_version_ = static_cast<int32_t>(history_.version());
    refresh();
    spdlog::info("[{}] History cleared by user", get_name());
}

// ============================================================================
// ACTION DISPATCH
// ============================================================================

void NotificationHistoryPanel::handle_item_clicked(size_t index) {
    if (index < row_actions_.size() && !row_actions_[index].empty()) {
        // Copied: dispatching closes the panel, and a refresh may replace the row.
        const std::string action = row_actions_[index];
        dispatch_action(action.c_str());
    }
}

void NotificationHistoryPanel::dispatch_action(const char* action) {
    spdlog::info("[{}] Dispatching action: {}", get_name(), action);

    if (strcmp(action, "show_update_modal") == 0) {
        // Close notification history overlay first, then show update modal
        helix::nav::go_back();
        UpdateChecker::instance().show_update_notification();
    } else if (auto handler = helix::ui::notification_action_handler(action)) {
        helix::nav::go_back();
        handler();
    } else {
        spdlog::warn("[{}] Unknown action: {}", get_name(), action);
    }
}

namespace {

using ActionHandler = std::function<void(const std::string&)>;

std::map<std::string, ActionHandler>& notification_action_handlers() {
    static std::map<std::string, ActionHandler> handlers;
    return handlers;
}

} // namespace

namespace helix::ui {

void set_notification_action_handler(const std::string& id, ActionHandler handler) {
    if (handler) {
        notification_action_handlers()[id] = std::move(handler);
    } else {
        notification_action_handlers().erase(id);
    }
}

std::function<void()> notification_action_handler(const std::string& action) {
    const auto colon = action.find(':');
    const std::string id = action.substr(0, colon);
    const std::string arg = colon == std::string::npos ? std::string{} : action.substr(colon + 1);
    auto& handlers = notification_action_handlers();
    auto it = handlers.find(id);
    if (it == handlers.end()) {
        return {};
    }
    return [handler = it->second, arg] { handler(arg); };
}

} // namespace helix::ui

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

NotificationHistoryPanel& get_global_notification_history_panel() {
    return helix::lazy_global<NotificationHistoryPanel>("NotificationHistoryPanel",
                                                        get_printer_state(), nullptr);
}
