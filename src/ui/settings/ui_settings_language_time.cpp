// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_language_time.cpp
 * @brief Implementation of LanguageTimeSettingsOverlay
 */

#include "ui_settings_language_time.h"

#include "ui_callback_helpers.h"

#include "display_settings_manager.h"
#include "system_settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix::settings {

using helix::ui::event_selected;

void LanguageTimeSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_language_changed",
         [](lv_event_t* e) {
             SystemSettingsManager::instance().set_language_by_index(event_selected(e));
         }},
        {"on_timezone_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_timezone_by_index(event_selected(e));
         }},
        {"on_time_format_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_time_format(
                 static_cast<TimeFormat>(event_selected(e)));
         }},
    });
}

void LanguageTimeSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_language_dropdown();
    init_timezone_dropdown();
}

// The option lists are built at runtime, so both dropdowns are populated and
// selected here. Time format binds to its subject in XML.
void LanguageTimeSettingsOverlay::init_language_dropdown() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_language", get_name());
    if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
        lv_dropdown_set_options(dropdown, SystemSettingsManager::get_language_options());
        int index = SystemSettingsManager::instance().get_language_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Language dropdown initialized (index={})", get_name(), index);
    }
}

void LanguageTimeSettingsOverlay::init_timezone_dropdown() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_timezone", get_name());
    if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
        std::string options = DisplaySettingsManager::get_timezone_options();
        lv_dropdown_set_options(dropdown, options.c_str());
        int index = DisplaySettingsManager::instance().get_timezone_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Timezone dropdown initialized (index={}, tz={})", get_name(), index,
                      DisplaySettingsManager::instance().get_timezone());
    }
}

} // namespace helix::settings
