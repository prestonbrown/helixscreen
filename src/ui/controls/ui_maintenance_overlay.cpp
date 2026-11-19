// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_maintenance_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"

#include "src/ui/panel_widgets/maintenance_model.h"

#include <string>

namespace helix {

void MaintenanceOverlay::register_callbacks() {
    register_xml_callbacks({
        {"maintenance_card_cb",
         [](lv_event_t* e) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] maintenance_card_cb");
             if (auto meter = ui::event_user_int(e))
                 maintenance_model::open_task_page(*meter);
             LVGL_SAFE_EVENT_CB_END();
         }},
        {"maintenance_intervals_cb",
         [](lv_event_t*) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] maintenance_intervals_cb");
             get_maintenance_intervals_overlay().show(lv_screen_active());
             LVGL_SAFE_EVENT_CB_END();
         }},
    });
}

void MaintenanceTaskOverlay::register_callbacks() {
    register_xml_callbacks({
        {"maintenance_mark_done_cb",
         [](lv_event_t*) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceTaskOverlay] maintenance_mark_done_cb");
             maintenance_model::confirm_reset(maintenance_model::task_meter());
             LVGL_SAFE_EVENT_CB_END();
         }},
    });
}

void MaintenanceIntervalsOverlay::apply_options(lv_obj_t* root) {
    if (!root)
        return;
    const std::string hours = maintenance_model::interval_options();
    const std::string days = maintenance_model::day_options();
    const auto fill = [root](const std::string& name, const std::string& options, int selected) {
        lv_obj_t* dropdown = lv_obj_find_by_name(root, name.c_str());
        if (!dropdown)
            return;
        // DECLARATIVE_OK: the options are data, built from the one choices table
        lv_dropdown_set_options(dropdown, options.c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected));
    };
    for (int i = 0; i < maintenance::kMeterCount; ++i) {
        const auto& meter = maintenance_model::state().meters[static_cast<size_t>(i)];
        fill("mt_hours_" + std::to_string(i), hours,
             maintenance::interval_choice_index(meter.interval_h));
        fill("mt_days_" + std::to_string(i), days, maintenance::day_choice_index(meter.interval_d));
    }
}

void MaintenanceIntervalsOverlay::before_show() {
    apply_options(overlay_root_);
}

void MaintenanceIntervalsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"maintenance_interval_cb",
         [](lv_event_t* e) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceIntervals] maintenance_interval_cb");
             if (auto meter = ui::event_user_int(e))
                 maintenance_model::set_interval_choice(*meter, ui::event_selected(e));
             LVGL_SAFE_EVENT_CB_END();
         }},
        {"maintenance_days_cb",
         [](lv_event_t* e) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceIntervals] maintenance_days_cb");
             if (auto meter = ui::event_user_int(e))
                 maintenance_model::set_days_choice(*meter, ui::event_selected(e));
             LVGL_SAFE_EVENT_CB_END();
         }},
    });
}

} // namespace helix
