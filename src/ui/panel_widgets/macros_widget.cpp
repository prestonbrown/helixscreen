// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macros_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_macros.h"

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_macros_widget() {
    register_widget_factory("macros",
                            [](const std::string&) { return std::make_unique<MacrosWidget>(); });

    lv_xml_register_event_cb(nullptr, "macros_widget_clicked_cb", MacrosWidget::clicked_cb);
}

MacrosWidget::MacrosWidget()
    : TiledPanelWidget("macros", TileSizing::Content{"", "", "Macros", false}) {}

MacrosWidget::~MacrosWidget() {
    detach();
}

void MacrosWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
}

void MacrosWidget::detach() {
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void MacrosWidget::handle_click() {
    get_global_macros_panel().show(parent_screen_);
}

void MacrosWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacrosWidget] clicked_cb");
    auto* self = panel_widget_from_event<MacrosWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
