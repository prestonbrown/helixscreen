// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_hardware.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_nav.h"
#include "ui_panel_power.h"
#include "ui_settings_fans.h"
#include "ui_settings_led.h"
#include "ui_settings_sensors.h"
#include "ui_spoolman_overlay.h"

#include "app_globals.h"
#include "hardware_validator.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// the include path, so forward-declare rather than including the header.
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

#include <spdlog/spdlog.h>

namespace helix::settings {

void HardwareSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_camera_view_clicked",
         [](lv_event_t*) {
#if HELIX_HAS_CAMERA
             helix::open_standalone_camera_fullscreen(
                 get_hardware_settings_overlay().parent_screen_);
#else
             spdlog::debug("[HardwareSettingsOverlay] Camera support disabled in this build");
#endif
         }},
        {"on_ams_settings_clicked",
         [](lv_event_t*) {
             auto& overlay = helix::ui::get_ams_device_operations_overlay();
             overlay.show(get_hardware_settings_overlay().parent_screen_);
         }},
        {"on_fans_settings_clicked",
         [](lv_event_t*) {
             get_fan_settings_overlay().show(get_hardware_settings_overlay().parent_screen_);
         }},
        {"on_filament_sensors_clicked",
         [](lv_event_t*) {
             get_sensor_settings_overlay().show(get_hardware_settings_overlay().parent_screen_);
         }},
        {"on_led_settings_clicked",
         [](lv_event_t*) {
             get_led_settings_overlay().show(get_hardware_settings_overlay().parent_screen_);
         }},
        {"on_power_devices_clicked",
         [](lv_event_t*) {
             lv_obj_t* overlay = get_global_power_panel().get_or_create_overlay(
                 get_hardware_settings_overlay().parent_screen_);
             if (overlay) {
                 helix::nav::push_overlay(overlay);
             } else {
                 spdlog::error("[HardwareSettingsOverlay] Failed to open Power panel");
             }
         }},
        {"on_spoolman_settings_clicked",
         [](lv_event_t*) {
             auto& overlay = helix::ui::get_spoolman_overlay();
             if (IMoonrakerAPI* api = get_moonraker_api()) {
                 overlay.set_api(api);
             }
             overlay.show(get_hardware_settings_overlay().parent_screen_);
         }},
    });
}

lv_obj_t* HardwareSettingsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    bind_hardware_health_row(overlay_root_);
    return overlay_root_;
}

void bind_hardware_health_row(lv_obj_t* overlay_root) {
    lv_obj_t* row =
        helix::ui::find_required(overlay_root, "row_hardware_health", "HardwareSettingsOverlay");
    if (!row) {
        return;
    }

    lv_obj_t* label = helix::ui::find_required(row, "label", "HardwareSettingsOverlay");
    if (label) {
        lv_label_bind_text(
            label,
            get_printer_state().hardware_validation_state().get_hardware_issues_label_subject(),
            "%s");
    }

    // Tint the icon so criticality is legible without opening the overlay.
    // Neither style matches at OK, leaving the icon on the variant="secondary"
    // colour the XML gives it.
    lv_obj_t* row_icon = helix::ui::find_required(row, "row_icon", "HardwareSettingsOverlay");
    if (row_icon) {
        lv_subject_t* level =
            get_printer_state().hardware_validation_state().get_hardware_status_level_subject();
        auto& theme = ThemeManager::instance();
        lv_obj_bind_style(row_icon, theme.get_style(StyleRole::IconWarning), LV_PART_MAIN, level,
                          static_cast<int>(HardwareStatusLevel::ATTENTION));
        lv_obj_bind_style(row_icon, theme.get_style(StyleRole::IconDanger), LV_PART_MAIN, level,
                          static_cast<int>(HardwareStatusLevel::CRITICAL));
    }

    spdlog::debug("[HardwareSettingsOverlay] Hardware health row bound to live status");
}

} // namespace helix::settings
