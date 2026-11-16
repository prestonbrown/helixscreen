// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_console_settings.h"

#include "ui_callback_helpers.h"

#include "settings_manager.h"

void ConsoleSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_console_filter_temps_changed",
         [](lv_event_t* e) {
             helix::SettingsManager::instance().set_console_filter_temps(
                 helix::ui::event_checked(e));
         }},
        {"on_console_filter_firmware_noise_changed",
         [](lv_event_t* e) {
             helix::SettingsManager::instance().set_console_filter_firmware_noise(
                 helix::ui::event_checked(e));
         }},
    });
}
