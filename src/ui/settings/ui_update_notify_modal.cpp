// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_notify_modal.h"

#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_settings_updates.h"
#include "ui_widget_ref.h"

#include "notice.h"
#include "system/update_checker.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

namespace {

// Empty while the dialog is down; a backdrop tap deletes the dialog without
// reaching hide_update_notification(), and the handle notices that itself.
WidgetRef s_notify_modal;

void on_update_notify_install(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_install");
    spdlog::info("[UpdateChecker] User chose to install update");
    hide_update_notification();
    helix::settings::get_updates_settings_overlay().show_update_download_modal(
        /*start_immediately=*/true);
    LVGL_SAFE_EVENT_CB_END();
}

void on_update_notify_ignore(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_ignore");
    spdlog::info("[UpdateChecker] User chose to ignore update");
    UpdateChecker::instance().dismiss_current_version();
    hide_update_notification();
    LVGL_SAFE_EVENT_CB_END();
}

void on_update_notify_close(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_notify_close");
    spdlog::info("[UpdateChecker] User closed update notification (remind later)");
    hide_update_notification();
    LVGL_SAFE_EVENT_CB_END();
}

void on_update_toggle_changelog(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdateChecker] on_update_toggle_changelog");
    auto* subject = UpdateChecker::instance().changelog_visible_subject();
    int current = lv_subject_get_int(subject);
    lv_subject_set_int(subject, current ? 0 : 1);
    LVGL_SAFE_EVENT_CB_END();
}

void register_notify_callbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    lv_xml_register_event_cb(nullptr, "on_update_notify_install", on_update_notify_install);
    lv_xml_register_event_cb(nullptr, "on_update_notify_ignore", on_update_notify_ignore);
    lv_xml_register_event_cb(nullptr, "on_update_notify_close", on_update_notify_close);
    lv_xml_register_event_cb(nullptr, "on_update_toggle_changelog", on_update_toggle_changelog);
    registered = true;
    spdlog::debug("[UpdateChecker] Notification callbacks registered");
}

const UiRequestRegistration s_show_request{UiRequestKind::ShowUpdateNotification,
                                           [](const UiRequest&) { show_update_notification(); }};

} // namespace

void show_update_notification() {
    spdlog::info("[UpdateChecker] Show update notification");
    register_notify_callbacks();
    if (!s_notify_modal) {
        s_notify_modal = ComponentModal::show_owned("update_notify_modal");
    }
}

void hide_update_notification() {
    if (s_notify_modal) {
        modal_hide(s_notify_modal);
        s_notify_modal = nullptr;
    }
}

} // namespace helix::ui
