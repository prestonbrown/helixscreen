// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_maintenance_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"

#include "src/ui/panel_widgets/maintenance_model.h"

#include <string>

namespace helix {

namespace {

template <int Meter> void on_interval_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] on_interval_changed");
    maintenance_model::set_interval_choice(Meter, ui::event_selected(e));
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void MaintenanceOverlay::apply_interval_options(lv_obj_t* root) {
    if (!root)
        return;
    const std::string options = maintenance_model::interval_options();
    for (int i = 0; i < maintenance::kMeterCount; ++i) {
        const std::string row_name = "row_interval_" + std::to_string(i);
        lv_obj_t* row = lv_obj_find_by_name(root, row_name.c_str());
        lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
        if (!dropdown)
            continue;
        // DECLARATIVE_OK: the options are data, built from the one interval table
        lv_dropdown_set_options(dropdown, options.c_str());
        const auto& meter = maintenance_model::state().meters[static_cast<size_t>(i)];
        lv_dropdown_set_selected(
            dropdown, static_cast<uint32_t>(maintenance::interval_choice_index(meter.interval_h)));
    }
}

void MaintenanceOverlay::before_show() {
    apply_interval_options(overlay_root_);
}

void MaintenanceOverlay::register_callbacks() {
    register_xml_callbacks({
        {"maintenance_reset_cb",
         [](lv_event_t* e) {
             LVGL_SAFE_EVENT_CB_BEGIN("[MaintenanceOverlay] maintenance_reset_cb");
             if (auto meter = ui::event_user_int(e))
                 maintenance_model::confirm_reset(*meter);
             LVGL_SAFE_EVENT_CB_END();
         }},
        {"maintenance_interval_0_cb", on_interval_changed<0>},
        {"maintenance_interval_1_cb", on_interval_changed<1>},
        {"maintenance_interval_2_cb", on_interval_changed<2>},
    });
}

} // namespace helix
