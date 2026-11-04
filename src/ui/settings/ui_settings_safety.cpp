// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_safety.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"

#include "audio_settings_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;

void SafetySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_estop_confirm_changed",
         [](lv_event_t* e) {
             bool on = event_checked(e);
             SafetySettingsManager::instance().set_estop_require_confirmation(on);
             EmergencyStopOverlay::instance().set_require_confirmation(on);
         }},
        {"on_cancel_escalation_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_cancel_escalation_enabled(event_checked(e));
         }},
        {"on_cancel_escalation_timeout_changed",
         [](lv_event_t* e) {
             static constexpr int kTimeoutSeconds[] = {15, 30, 60, 120};
             int index = std::clamp(event_selected(e), 0, 3);
             SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(
                 kTimeoutSeconds[index]);
         }},
        {"on_completion_alert_changed",
         [](lv_event_t* e) {
             AudioSettingsManager::instance().set_completion_alert_mode(
                 static_cast<CompletionAlertMode>(event_selected(e)));
         }},
        {"on_min_toast_severity_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_min_toast_severity(event_selected(e));
         }},
        {"on_macro_confirm_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_macro_require_confirmation(event_checked(e));
         }},
        {"on_detection_enabled_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_detection_enabled(event_checked(e));
         }},
        {"on_detection_pause_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_detection_pause_on_detect(event_checked(e));
         }},
    });
}

} // namespace helix::settings
