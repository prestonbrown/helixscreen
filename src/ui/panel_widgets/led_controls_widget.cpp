// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_controls_widget.h"

#include "ui_event_safety.h"

#include "led/ui_led_control_overlay.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {

void register_led_controls_widget() {
    register_widget_factory("led_controls", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<LedControlsWidget>();
    });
    lv_xml_register_event_cb(nullptr, "on_led_controls_clicked",
                             LedControlsWidget::on_led_controls_clicked);
}

LedControlsWidget::~LedControlsWidget() {
    detach();
}

void LedControlsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
}

void LedControlsWidget::detach() {
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void LedControlsWidget::on_led_controls_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlsWidget] on_led_controls_clicked");
    auto* self = panel_widget_from_event<LedControlsWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlsWidget::handle_clicked() {
    spdlog::debug("[LedControlsWidget] Clicked - opening LED control overlay");
    open_led_control_overlay(parent_screen_, "");
}

} // namespace helix
