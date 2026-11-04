// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_names_modal.h"

#include "ui_callback_helpers.h"

#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix::ui {

void BeltNamesModal::register_callbacks() {
    register_xml_callbacks({
        {"belt_names_layout_cb",
         [](lv_event_t* e) {
             const char* value = static_cast<const char*>(lv_event_get_user_data(e));
             if (!value) {
                 return;
             }
             const int layout = std::atoi(value);
             spdlog::info("[BeltNames] Layout set to {}", layout);
             SettingsManager::instance().set_belt_layout(layout);
         }},
    });
}

void BeltNamesModal::on_show() {
    wire_ok_button("btn_names_done");
}

} // namespace helix::ui
