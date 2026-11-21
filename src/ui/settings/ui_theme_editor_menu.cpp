// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_theme_editor_menu.h"

#include "ui_callback_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void ThemeEditorMenu::register_callbacks() {
    register_xml_callbacks({
        {"theme_editor_menu_reset_cb", [](lv_event_t*) { pick(kActionReset); }},
        {"theme_editor_menu_save_as_cb", [](lv_event_t*) { pick(kActionSaveAs); }},
    });
}

void ThemeEditorMenu::pick(int action) {
    auto* self = ContextMenu::active_as<ThemeEditorMenu>();
    if (!self) {
        spdlog::warn("[ThemeEditorMenu] No active instance for event");
        return;
    }
    self->dispatch_action(action);
}

} // namespace helix::ui
