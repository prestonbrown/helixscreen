// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bypass_widget.h"

#include "ui_event_safety.h"

#include "ams_state.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_bypass_widget() {
    register_widget_factory("bypass",
                            [](const std::string&) { return std::make_unique<BypassWidget>(); });

    lv_xml_register_event_cb(nullptr, "bypass_widget_clicked_cb", BypassWidget::clicked_cb);
}

BypassWidget::BypassWidget() = default;

BypassWidget::~BypassWidget() {
    detach();
}

void BypassWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    (void)parent_screen; // unused override param (lock_widget idiom)
    widget_obj_ = widget_obj;
    if (!widget_obj_) {
        return;
    }
    lv_obj_set_user_data(widget_obj_, this);

    // External-spool color dot: styles cannot bind a dynamic color, so the
    // dot's bg color follows the color subject through the sanctioned
    // observer path. Runs from attach() too — instances are recycled across
    // rebuilds. The dot is resolved by name inside the callback (not captured)
    // so a recycled instance never writes through a stale pointer.
    spool_color_observer_ = helix::ui::observe_int_sync<BypassWidget>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](BypassWidget* self, int color) {
            if (!self || !self->widget_obj_) {
                return;
            }
            auto* dot = lv_obj_find_by_name(self->widget_obj_, "bypass_color_dot");
            if (!dot) {
                return;
            }
            // XML styles cannot bind non-constant colors (same exception
            // class as the spool canvas in active_spool_widget).
            // DECLARATIVE_OK: dynamic color from a subject
            lv_obj_set_style_bg_color(dot, lv_color_hex(static_cast<uint32_t>(color) & 0xFFFFFF),
                                      0);
        },
        AmsState::instance().get_subjects_lifetime());

    spdlog::debug("[BypassWidget] attached");
}

void BypassWidget::detach() {
    spool_color_observer_.reset();
    // Abort any pending unload→enable chain: the tile is going away (widget
    // recycled or screen torn down) and the controller's self-observer must
    // not fire the enable for a chain nobody is waiting on.
    toggle_.cancel_pending();
    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
}

void BypassWidget::handle_click() {
    toggle_.toggle();
}

void BypassWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BypassWidget] clicked_cb");
    // XML event_cb recovery idiom (tips/fan widgets): the callback is
    // registered globally with nullptr user_data, so the instance comes from
    // the current target's lv_obj user_data set in attach().
    if (auto* self = panel_widget_from_event<BypassWidget>(e)) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
