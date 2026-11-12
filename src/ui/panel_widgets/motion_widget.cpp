// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "motion_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_motion.h"

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_motion_widget() {
    register_widget_factory("motion", [](const std::string& instance_id) {
        return std::make_unique<MotionWidget>(instance_id);
    });

    lv_xml_register_event_cb(nullptr, "motion_widget_clicked_cb", MotionWidget::clicked_cb);
}

void open_motion_controls(lv_obj_t* parent_screen) {
    if (jump_to_full_page(kMotionFunction)) {
        return;
    }
    get_global_motion_panel().show(parent_screen);
}

MotionWidget::MotionWidget(const std::string& instance_id)
    : TiledPanelWidget(instance_id, TileSizing::Content{"", "", "Motion", false}),
      instance_id_(instance_id), full_page_(sizing_, kMotionFunction, []() -> OverlayBase& {
          return get_global_motion_panel();
      }) {}

MotionWidget::~MotionWidget() {
    detach();
}

void MotionWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    full_page_.attach(widget_obj);
}

void MotionWidget::detach() {
    full_page_.detach();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void MotionWidget::on_activate() {
    full_page_.activate();
}

void MotionWidget::on_deactivate() {
    full_page_.deactivate();
}

void MotionWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    TiledPanelWidget::on_size_changed(colspan, rowspan, width_px, height_px);
    full_page_.set_placement(fills_extra_page(), page_index());
}

void MotionWidget::handle_click() {
    // A full page's tile root is the page itself; a tap on its frame is not a
    // request to open the controls it already shows.
    if (full_page_.is_full_page()) {
        return;
    }
    open_motion_controls(parent_screen_);
}

void MotionWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionWidget] clicked_cb");
    auto* self = panel_widget_from_event<MotionWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
