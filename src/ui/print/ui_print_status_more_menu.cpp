// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_status_more_menu.h"

#include "ui_callback_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void PrintStatusMoreMenu::register_callbacks() {
    register_xml_callbacks({
        {"print_status_more_camera_cb", [](lv_event_t*) { pick(kActionCamera); }},
        {"print_status_more_timelapse_cb", [](lv_event_t*) { pick(kActionTimelapse); }},
        {"print_status_more_light_cb", [](lv_event_t*) { pick(kActionLight); }},
    });
}

void PrintStatusMoreMenu::pick(int action) {
    auto* self = ContextMenu::active_as<PrintStatusMoreMenu>();
    if (!self) {
        spdlog::warn("[PrintStatusMoreMenu] No active instance for event");
        return;
    }
    self->dispatch_action(action);
}

} // namespace helix::ui
