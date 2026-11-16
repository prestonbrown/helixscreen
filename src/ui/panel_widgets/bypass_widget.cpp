// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bypass_widget.h"

#include "ui_event_safety.h"
#include "ui_external_spool_menu.h"
#include "ui_swatch.h"

#include "ams_state.h"
#include "core/observer_factory.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_bypass_widget() {
    register_widget_factory("bypass",
                            [](const std::string&) { return std::make_unique<BypassWidget>(); });

    lv_xml_register_event_cb(nullptr, "bypass_widget_clicked_cb", BypassWidget::clicked_cb);
}

BypassWidget::BypassWidget()
    : TiledPanelWidget("bypass", TileSizing::Content{"", "", "Bypass", false}) {}

BypassWidget::~BypassWidget() {
    detach();
}

void BypassWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    if (!widget_obj_) {
        return;
    }

    // The external spool's colour dot, painted from the whole spool record
    // (its colours and finish), which the colour subject announces changes to.
    // Runs from attach() too: instances are recycled across rebuilds. The dot
    // is resolved by name inside the callback (not captured) so a recycled
    // instance never writes through a stale pointer.
    spool_color_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [](BypassWidget* self, int color) {
            if (!self || !self->widget_obj_) {
                return;
            }
            auto* dot = lv_obj_find_by_name(self->widget_obj_, "bypass_color_dot");
            if (!dot) {
                return;
            }
            if (const auto spool = AmsState::instance().get_external_spool_info()) {
                helix::ui::apply_swatch_color(dot, *spool);
            } else {
                helix::ui::apply_swatch_color(dot, static_cast<uint32_t>(color) & 0xFFFFFF, "");
            }
        },
        AmsState::instance().get_subjects_lifetime());

    // The spool row under the caption is drawn only while bypass is engaged, so
    // the tile is measured with it exactly then. Immediate, so the re-measure
    // lands in the same turn the row's own binding shows it, never a frame later.
    bypass_active_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_bypass_active_subject(), this,
        [](BypassWidget* self, int active) {
            TileSizing::Content content = self->sizing_.content();
            // The row's text is panel_widget_bypass.xml#bypass_material_label, a
            // text_tiny, which draws in font_xs; the row is one line of it.
            content.detail_font = active != 0 ? "font_xs" : nullptr;
            self->sizing_.set_content(std::move(content));
            self->sizing_.remeasure();
        },
        AmsState::instance().get_subjects_lifetime(), helix::ui::Dispatch::Immediate);

    spdlog::debug("[BypassWidget] attached");
}

void BypassWidget::detach() {
    spool_color_observer_.reset();
    bypass_active_observer_.reset();
    // The menu holds a callback capturing this widget, and the tile is going
    // away (recycled or torn down).
    context_menu_.reset();
    parent_screen_ = nullptr;
    // Abort any pending unload→enable chain: the tile is going away (widget
    // recycled or screen torn down) and the controller's self-observer must
    // not fire the enable for a chain nobody is waiting on.
    toggle_.cancel_pending();
    widget_obj_ = nullptr;
}

void BypassWidget::handle_click() {
    if (!parent_screen_ || !widget_obj_) {
        return;
    }
    // No on_load / on_unload: with no operation stepper on the home screen there
    // is nothing this surface would add to the dispatch. The controller is what
    // it must supply — the menu engages bypass through it before a load, and
    // drives the toggle entry with it.
    helix::ui::ExternalSpoolMenuHooks hooks;
    hooks.toggle = &toggle_;
    helix::ui::show_external_spool_menu(parent_screen_, widget_obj_, context_menu_,
                                        std::move(hooks));
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
