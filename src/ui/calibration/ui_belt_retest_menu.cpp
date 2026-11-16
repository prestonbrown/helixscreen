// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_belt_retest_menu.h"

#include "ui_callback_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void BeltRetestMenu::register_callbacks() {
    register_xml_callbacks({
        {"belt_retest_menu_a_cb", [](lv_event_t*) { pick(kActionSlot0); }},
        {"belt_retest_menu_b_cb", [](lv_event_t*) { pick(kActionSlot1); }},
    });
}

void BeltRetestMenu::pick(int action) {
    auto* self = ContextMenu::active_as<BeltRetestMenu>();
    if (!self) {
        spdlog::warn("[BeltRetestMenu] No active instance for event");
        return;
    }
    self->dispatch_action(action);
}

} // namespace helix::ui
