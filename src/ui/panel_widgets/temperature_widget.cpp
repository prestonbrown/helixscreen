// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_temp_control.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_temperature_widget() {
    register_widget_factory("temperature", []() {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TempControlPanel>();
        return std::make_unique<TemperatureWidget>(ps, tcp);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "temp_clicked_cb", TemperatureWidget::temp_clicked_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::centi_to_degrees;

TemperatureWidget::TemperatureWidget(PrinterState& printer_state, TempControlPanel* temp_panel)
    : printer_state_(printer_state), temp_control_panel_(temp_panel) {}

TemperatureWidget::~TemperatureWidget() {
    detach();
}

void TemperatureWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    // Store this pointer on the button that has the event_cb in XML,
    // not on the outer container — event current_target is the button.
    temp_btn_ = lv_obj_find_by_name(widget_obj_, "temp_btn");
    if (temp_btn_) {
        lv_obj_set_user_data(temp_btn_, this);
    }

    // Set up temperature observers with alive guard
    using helix::ui::observe_int_sync;
    std::weak_ptr<bool> weak_alive = alive_;

    extruder_temp_observer_ =
        observe_int_sync<TemperatureWidget>(printer_state_.get_active_extruder_temp_subject(), this,
                                            [weak_alive](TemperatureWidget* self, int temp) {
                                                if (weak_alive.expired())
                                                    return;
                                                self->on_extruder_temp_changed(temp);
                                            });
    extruder_target_observer_ = observe_int_sync<TemperatureWidget>(
        printer_state_.get_active_extruder_target_subject(), this,
        [weak_alive](TemperatureWidget* self, int target) {
            if (weak_alive.expired())
                return;
            self->on_extruder_target_changed(target);
        });

    // Attach heating icon animator
    lv_obj_t* temp_icon = lv_obj_find_by_name(widget_obj_, "nozzle_icon_glyph");
    if (temp_icon) {
        temp_icon_animator_.attach(temp_icon);
        cached_extruder_temp_ =
            lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        cached_extruder_target_ =
            lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        temp_icon_animator_.update(cached_extruder_temp_, cached_extruder_target_);
        spdlog::debug("[TemperatureWidget] Heating icon animator attached");
    }

    spdlog::debug("[TemperatureWidget] Attached");
}

void TemperatureWidget::detach() {
    *alive_ = false;
    temp_icon_animator_.detach();
    extruder_temp_observer_.reset();
    extruder_target_observer_.reset();

    // Clean up lazily-created overlay (child of parent_screen_, not widget container)
    if (nozzle_temp_panel_) {
        NavigationManager::instance().unregister_overlay_instance(nozzle_temp_panel_);
        helix::ui::safe_delete(nozzle_temp_panel_);
    }

    if (temp_btn_) {
        lv_obj_set_user_data(temp_btn_, nullptr);
        temp_btn_ = nullptr;
    }
    if (widget_obj_) {
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[TemperatureWidget] Detached");
}

void TemperatureWidget::on_extruder_temp_changed(int temp_centi) {
    cached_extruder_temp_ = temp_centi;
    update_temp_icon_animation();
    spdlog::trace("[TemperatureWidget] Extruder temp: {}°C", centi_to_degrees(temp_centi));
}

void TemperatureWidget::on_extruder_target_changed(int target_centi) {
    cached_extruder_target_ = target_centi;
    update_temp_icon_animation();
    spdlog::trace("[TemperatureWidget] Extruder target: {}°C", centi_to_degrees(target_centi));
}

void TemperatureWidget::update_temp_icon_animation() {
    temp_icon_animator_.update(cached_extruder_temp_, cached_extruder_target_);
}

void TemperatureWidget::handle_temp_clicked() {
    spdlog::info("[TemperatureWidget] Temperature icon clicked - opening nozzle temp panel");

    if (!temp_control_panel_) {
        spdlog::error("[TemperatureWidget] TempControlPanel not initialized");
        NOTIFY_ERROR("Temperature panel not available");
        return;
    }

    // Create nozzle temp panel on first access (lazy initialization)
    if (!nozzle_temp_panel_ && parent_screen_) {
        spdlog::debug("[TemperatureWidget] Creating nozzle temperature panel...");

        nozzle_temp_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "nozzle_temp_panel", nullptr));
        if (nozzle_temp_panel_) {
            temp_control_panel_->setup_nozzle_panel(nozzle_temp_panel_, parent_screen_);
            NavigationManager::instance().register_overlay_instance(
                nozzle_temp_panel_, temp_control_panel_->get_nozzle_lifecycle());
            lv_obj_add_flag(nozzle_temp_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[TemperatureWidget] Nozzle temp panel created and initialized");
        } else {
            spdlog::error("[TemperatureWidget] Failed to create nozzle temp panel from XML");
            NOTIFY_ERROR("Failed to load temperature panel");
            return;
        }
    }

    if (nozzle_temp_panel_) {
        NavigationManager::instance().push_overlay(nozzle_temp_panel_);
    }
}

void TemperatureWidget::temp_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TemperatureWidget] temp_clicked_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<TemperatureWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_temp_clicked();
    } else {
        spdlog::warn("[TemperatureWidget] temp_clicked_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}
