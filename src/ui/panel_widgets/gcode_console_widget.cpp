// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_console_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_console.h"

#include "panel_widget_registry.h"
#include "printer_cache_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_gcode_console_widget() {
    register_widget_factory(
        "gcode_console", [](const std::string&) { return std::make_unique<GCodeConsoleWidget>(); });

    lv_xml_register_event_cb(nullptr, "gcode_console_clicked_cb", GCodeConsoleWidget::clicked_cb);
}

GCodeConsoleWidget::GCodeConsoleWidget() {
    // console_panel_ is a static, so it survives the printer switch that
    // destroys the ConsolePanel object - and teardown frees the orphaned
    // overlay widget, which would leave the cache dangling. Every
    // active-printer change fires this before teardown, so the cache never
    // outlives its widget.
    PrinterCacheRegistry::instance().register_invalidator("GCodeConsoleWidget",
                                                          []() { console_panel_ = nullptr; });
}

GCodeConsoleWidget::~GCodeConsoleWidget() {
    detach();
}

void GCodeConsoleWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Set user_data on the root lv_obj, NOT on the ui_button child.
    // ui_button allocates its own UiButtonData in user_data — overwriting it
    // leaks memory and breaks button style/contrast auto-updates.
    lv_obj_set_user_data(widget_obj_, this);

    btn_ = lv_obj_find_by_name(widget_obj_, "gcode_console_button");
    if (btn_) {
        lv_obj_add_event_cb(btn_, clicked_cb, LV_EVENT_CLICKED, this);
    }
}

void GCodeConsoleWidget::detach() {
    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void GCodeConsoleWidget::handle_click() {
    helix::ui::lazy_create_and_push_overlay<ConsolePanel>(get_global_console_panel, console_panel_,
                                                          parent_screen_, "Console",
                                                          "GCodeConsoleWidget", true);
}

void GCodeConsoleWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GCodeConsoleWidget] clicked_cb");
    auto* self = static_cast<GCodeConsoleWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
