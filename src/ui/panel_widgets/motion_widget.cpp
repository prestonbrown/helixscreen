// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "motion_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_motion.h"

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_motion_widget() {
    register_widget_factory("motion",
                            [](const std::string&) { return std::make_unique<MotionWidget>(); });

    lv_xml_register_event_cb(nullptr, "motion_widget_clicked_cb", MotionWidget::clicked_cb);
}

MotionWidget::MotionWidget()
    : TiledPanelWidget("motion", TileSizing::Content{"", "", "Motion", false}) {}

MotionWidget::~MotionWidget() {
    detach();
}

void MotionWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    btn_ = lv_obj_find_by_name(widget_obj_, "motion_button");
    if (btn_) {
        lv_obj_add_event_cb(btn_, clicked_cb, LV_EVENT_CLICKED, this);
    }
}

void MotionWidget::detach() {
    btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void MotionWidget::handle_click() {
    open_motion_panel(parent_screen_);
}

void MotionWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionWidget] clicked_cb");
    auto* self = static_cast<MotionWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
