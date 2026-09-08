// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sensors.cpp
 * @brief Implementation of SensorSettingsOverlay
 */

#include "ui_settings_sensors.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_status_pill.h"
#include "ui_utils.h"

#include "accel_sensor_manager.h"
#include "app_globals.h"
#include "chamber_assignment_options.h"
#include "color_sensor_manager.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "humidity_sensor_manager.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "width_sensor_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SensorSettingsOverlay> g_sensor_settings_overlay;

SensorSettingsOverlay& get_sensor_settings_overlay() {
    if (!g_sensor_settings_overlay) {
        g_sensor_settings_overlay = std::make_unique<SensorSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SensorSettingsOverlay", []() { g_sensor_settings_overlay.reset(); });
    }
    return *g_sensor_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SensorSettingsOverlay::SensorSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SensorSettingsOverlay::~SensorSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SensorSettingsOverlay::register_callbacks() {
    // Master toggle callback for switch sensors (used by XML event_cb)
    lv_xml_register_event_cb(nullptr, "on_switch_master_toggle_changed",
                             on_switch_master_toggle_changed);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SensorSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "sensors_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SensorSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Update all sensor counts (populate called in on_activate)
    update_all_sensor_counts();

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void SensorSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_all_sensors();
}

void SensorSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// SWITCH SENSORS (Filament Runout/Motion)
// ============================================================================

std::vector<helix::FilamentSensorConfig>
SensorSettingsOverlay::get_standalone_switch_sensors() const {
    auto& mgr = helix::FilamentSensorManager::instance();
    auto all_sensors = mgr.get_sensors();

    const auto& discovery = get_printer_state().get_discovery();
    std::vector<helix::FilamentSensorConfig> standalone;
    for (const auto& sensor : all_sensors) {
        if (!PrinterHardware::is_ams_sensor(sensor.sensor_name, discovery)) {
            standalone.push_back(sensor);
        } else {
            spdlog::debug("[{}] Filtered out AMS sensor: {}", get_name(), sensor.sensor_name);
        }
    }
    return standalone;
}

void SensorSettingsOverlay::update_switch_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "switch_sensor_count");
    if (badge) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", get_standalone_switch_sensors().size());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_switch_sensors() {
    if (!overlay_root_) {
        return;
    }

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "switch_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find switch_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    // Get standalone sensors (excludes AMS/multi-material types)
    auto sensors = get_standalone_switch_sensors();

    spdlog::debug("[{}] Populating switch sensor list with {} sensors", get_name(), sensors.size());

    // Create a row for each sensor
    for (const auto& sensor : sensors) {
        // Create sensor row from XML component
        const char* attrs[] = {
            "sensor_name", sensor.sensor_name.c_str(), "sensor_type",
            sensor.type == helix::FilamentSensorType::MOTION ? "motion" : "switch", nullptr};
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(sensors_list, "filament_sensor_row", attrs));
        if (!row) {
            spdlog::error("[{}] Failed to create sensor row for {}", get_name(),
                          sensor.sensor_name);
            continue;
        }

        // Store klipper_name as user data for callbacks. The helper owns the
        // copy and frees it on LV_EVENT_DELETE; the row is created here, so the
        // user_data slot is ours (L069).
        if (!helix::ui::set_owned_user_string(row, sensor.klipper_name)) {
            spdlog::error("[{}] Failed to attach sensor name to row: {}", get_name(),
                          sensor.klipper_name);
            continue;
        }
        // Borrowed pointer into the row-owned copy; valid until the row dies,
        // which is also when the child handlers below stop firing.
        char* klipper_name = const_cast<char*>(helix::ui::get_owned_user_string(row));

        // Wire up enable toggle
        lv_obj_t* enable_toggle = lv_obj_find_by_name(row, "enable_toggle");
        lv_obj_t* enable_container = enable_toggle ? lv_obj_get_parent(enable_toggle) : nullptr;

        if (enable_toggle) {
            if (sensor.enabled) {
                lv_obj_add_state(enable_toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(enable_toggle, LV_STATE_CHECKED);
            }

            if (enable_container && sensor.role == helix::FilamentSensorRole::NONE) {
                lv_obj_add_flag(enable_container, LV_OBJ_FLAG_HIDDEN);
            }

            // Pass klipper_name via event user_data rather than lv_obj_set_user_data
            // on the child widget -- XML-created children may use user_data internally
            lv_obj_add_event_cb(
                enable_toggle,
                [](lv_event_t* e) {
                    auto* klipper_name_ptr = static_cast<const char*>(lv_event_get_user_data(e));
                    if (!klipper_name_ptr)
                        return;
                    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

                    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

                    auto& mgr = helix::FilamentSensorManager::instance();
                    mgr.set_sensor_enabled(klipper_name_ptr, enabled);
                    mgr.save_config_to_file();
                    spdlog::info("[SensorSettingsOverlay] Switch sensor {} enabled: {}",
                                 klipper_name_ptr, enabled ? "ON" : "OFF");
                },
                LV_EVENT_VALUE_CHANGED, klipper_name);
        }

        // Wire up role dropdown
        lv_obj_t* role_dropdown = lv_obj_find_by_name(row, "role_dropdown");
        if (role_dropdown) {
            lv_dropdown_set_selected(role_dropdown, static_cast<uint32_t>(sensor.role));

            // Pass klipper_name via event user_data rather than lv_obj_set_user_data
            // on the child widget -- XML-created children may use user_data internally
            lv_obj_add_event_cb(
                role_dropdown,
                [](lv_event_t* e) {
                    auto* klipper_name_ptr = static_cast<const char*>(lv_event_get_user_data(e));
                    if (!klipper_name_ptr)
                        return;
                    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

                    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
                    auto role = static_cast<helix::FilamentSensorRole>(index);

                    auto& mgr = helix::FilamentSensorManager::instance();
                    mgr.set_sensor_role(klipper_name_ptr, role);
                    mgr.save_config_to_file();
                    spdlog::info("[SensorSettingsOverlay] Switch sensor {} role changed to {}",
                                 klipper_name_ptr, helix::role_to_config_string(role));

                    // Show/hide enable toggle based on role
                    lv_obj_t* row_obj = lv_obj_get_parent(lv_obj_get_parent(dropdown));
                    lv_obj_t* toggle = lv_obj_find_by_name(row_obj, "enable_toggle");
                    if (toggle) {
                        lv_obj_t* container = lv_obj_get_parent(toggle);
                        if (role == helix::FilamentSensorRole::NONE) {
                            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                },
                LV_EVENT_VALUE_CHANGED, klipper_name);
        }

        spdlog::debug("[{}]   Created row for switch sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// PROBE SENSORS
// ============================================================================

void SensorSettingsOverlay::update_probe_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "probe_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::ProbeSensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_probe_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "probe_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find probe_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();

    spdlog::debug("[{}] Populating probe sensor list with {} sensors", get_name(), sensors.size());

    for (const auto& sensor : sensors) {
        // Create simple info row (probes are display-only here, configured via wizard)
        auto* row = lv_obj_create(sensors_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

        // Sensor name label
        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.sensor_name.c_str());
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_set_flex_grow(name_label, 1);

        // Type badge
        auto* type_label = lv_label_create(row);
        const char* type_str = "probe";
        switch (sensor.type) {
        case helix::sensors::ProbeSensorType::BLTOUCH:
            type_str = "BLTouch";
            break;
        case helix::sensors::ProbeSensorType::SMART_EFFECTOR:
            type_str = "Smart Effector";
            break;
        case helix::sensors::ProbeSensorType::EDDY_CURRENT:
            type_str = "Eddy";
            break;
        default:
            type_str = lv_tr("Probe");
            break;
        }
        lv_label_set_text(type_label, type_str);
        lv_obj_set_style_text_color(type_label, theme_manager_get_color("text_muted"), 0);

        spdlog::debug("[{}]   Created row for probe sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// WIDTH SENSORS
// ============================================================================

void SensorSettingsOverlay::update_width_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "width_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::WidthSensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_width_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "width_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find width_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::WidthSensorManager::instance();
    auto sensors = mgr.get_sensors();

    spdlog::debug("[{}] Populating width sensor list with {} sensors", get_name(), sensors.size());

    // Create a row for each sensor using XML component
    for (const auto& sensor : sensors) {
        // Create sensor row from XML component
        const char* type_str =
            sensor.type == helix::sensors::WidthSensorType::TSL1401CL ? "TSL1401CL" : "Hall";
        const char* attrs[] = {"sensor_name", sensor.sensor_name.c_str(), "sensor_type", type_str,
                               nullptr};
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(sensors_list, "width_sensor_row", attrs));
        if (!row) {
            spdlog::error("[{}] Failed to create sensor row for {}", get_name(),
                          sensor.sensor_name);
            continue;
        }

        // Store klipper_name as user data for callbacks. The helper owns the
        // copy and frees it on LV_EVENT_DELETE; the row is created here, so the
        // user_data slot is ours (L069).
        if (!helix::ui::set_owned_user_string(row, sensor.klipper_name)) {
            spdlog::error("[{}] Failed to attach sensor name to row: {}", get_name(),
                          sensor.klipper_name);
            continue;
        }
        // Borrowed pointer into the row-owned copy; valid until the row dies,
        // which is also when the child handlers below stop firing.
        char* klipper_name = const_cast<char*>(helix::ui::get_owned_user_string(row));

        // Wire up enable toggle
        lv_obj_t* enable_toggle = lv_obj_find_by_name(row, "enable_toggle");

        if (enable_toggle) {
            if (sensor.enabled) {
                lv_obj_add_state(enable_toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(enable_toggle, LV_STATE_CHECKED);
            }

            // Pass klipper_name via event user_data
            lv_obj_add_event_cb(
                enable_toggle,
                [](lv_event_t* e) {
                    auto* klipper_name_ptr = static_cast<const char*>(lv_event_get_user_data(e));
                    if (!klipper_name_ptr)
                        return;
                    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

                    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

                    auto& mgr = helix::sensors::WidthSensorManager::instance();
                    mgr.set_sensor_enabled(klipper_name_ptr, enabled);
                    mgr.save_config_to_file();
                    spdlog::info("[SensorSettingsOverlay] Width sensor {} enabled: {}",
                                 klipper_name_ptr, enabled ? "ON" : "OFF");
                },
                LV_EVENT_VALUE_CHANGED, klipper_name);
        }

        // Wire up role dropdown
        lv_obj_t* role_dropdown = lv_obj_find_by_name(row, "role_dropdown");
        if (role_dropdown) {
            lv_dropdown_set_selected(role_dropdown, static_cast<uint32_t>(sensor.role));

            // Pass klipper_name via event user_data
            lv_obj_add_event_cb(
                role_dropdown,
                [](lv_event_t* e) {
                    auto* klipper_name_ptr = static_cast<const char*>(lv_event_get_user_data(e));
                    if (!klipper_name_ptr)
                        return;
                    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

                    // NOTE: Dropdown option order must match WidthSensorRole enum:
                    //   0 = NONE, 1 = FLOW_COMPENSATION
                    // See also: ui_xml/width_sensor_row.xml dropdown options
                    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
                    auto role = static_cast<helix::sensors::WidthSensorRole>(index);

                    auto& mgr = helix::sensors::WidthSensorManager::instance();
                    mgr.set_sensor_role(klipper_name_ptr, role);
                    mgr.save_config_to_file();
                    spdlog::info("[SensorSettingsOverlay] Width sensor {} role changed to {}",
                                 klipper_name_ptr,
                                 helix::sensors::width_role_to_display_string(role));
                },
                LV_EVENT_VALUE_CHANGED, klipper_name);
        }

        spdlog::debug("[{}]   Created row for width sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// HUMIDITY SENSORS
// ============================================================================

void SensorSettingsOverlay::update_humidity_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "humidity_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::HumiditySensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_humidity_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "humidity_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find humidity_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::HumiditySensorManager::instance();
    auto sensors = mgr.get_sensors();

    spdlog::debug("[{}] Populating humidity sensor list with {} sensors", get_name(),
                  sensors.size());

    for (const auto& sensor : sensors) {
        auto* row = lv_obj_create(sensors_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.sensor_name.c_str());
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_set_flex_grow(name_label, 1);

        auto* type_label = lv_label_create(row);
        lv_label_set_text(type_label,
                          helix::sensors::humidity_type_to_display_string(sensor.type).c_str());
        lv_obj_set_style_text_color(type_label, theme_manager_get_color("text_muted"), 0);

        spdlog::debug("[{}]   Created row for humidity sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// ACCELEROMETER SENSORS
// ============================================================================

void SensorSettingsOverlay::update_accel_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "accel_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::AccelSensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_accel_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "accel_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find accel_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::AccelSensorManager::instance();
    auto sensors = mgr.get_sensors();

    spdlog::debug("[{}] Populating accel sensor list with {} sensors", get_name(), sensors.size());

    for (const auto& sensor : sensors) {
        auto* row = lv_obj_create(sensors_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.sensor_name.c_str());
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_set_flex_grow(name_label, 1);

        auto* type_label = lv_label_create(row);
        const char* type_str = "ADXL345";
        switch (sensor.type) {
        case helix::sensors::AccelSensorType::LIS2DW:
            type_str = "LIS2DW";
            break;
        case helix::sensors::AccelSensorType::LIS3DH:
            type_str = "LIS3DH";
            break;
        case helix::sensors::AccelSensorType::MPU9250:
            type_str = "MPU9250";
            break;
        case helix::sensors::AccelSensorType::ICM20948:
            type_str = "ICM20948";
            break;
        default:
            type_str = "ADXL345";
            break;
        }
        lv_label_set_text(type_label, type_str);
        lv_obj_set_style_text_color(type_label, theme_manager_get_color("text_muted"), 0);

        spdlog::debug("[{}]   Created row for accel sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// COLOR SENSORS
// ============================================================================

void SensorSettingsOverlay::update_color_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "color_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::ColorSensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

void SensorSettingsOverlay::populate_color_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "color_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find color_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::ColorSensorManager::instance();
    auto sensors = mgr.get_sensors();

    spdlog::debug("[{}] Populating color sensor list with {} sensors", get_name(), sensors.size());

    for (const auto& sensor : sensors) {
        auto* row = lv_obj_create(sensors_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.sensor_name.c_str());
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_set_flex_grow(name_label, 1);

        auto* type_label = lv_label_create(row);
        lv_label_set_text(type_label, "TD-1");
        lv_obj_set_style_text_color(type_label, theme_manager_get_color("text_muted"), 0);

        spdlog::debug("[{}]   Created row for color sensor: {}", get_name(), sensor.sensor_name);
    }
}

// ============================================================================
// TEMPERATURE SENSORS
// ============================================================================

void SensorSettingsOverlay::update_temperature_sensor_count() {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, "temp_sensor_count_label");
    if (badge) {
        auto& mgr = helix::sensors::TemperatureSensorManager::instance();
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", mgr.sensor_count());
        ui_status_pill_set_text(badge, buf);
    }
}

// ============================================================================
// CHAMBER ASSIGNMENT
// ============================================================================

namespace {

/// Labels shared by both chamber assignment dropdowns.
ChamberAssignmentLabels assignment_labels() {
    return ChamberAssignmentLabels{lv_tr("Auto"), lv_tr("(none detected)"), lv_tr("not detected"),
                                   lv_tr("None (disable)")};
}

} // namespace

void SensorSettingsOverlay::populate_chamber_assignment() {
    if (!overlay_root_)
        return;

    auto& settings = helix::SettingsManager::instance();
    auto& discovery = get_printer_state().get_discovery();

    // --- Chamber Heater Dropdown ---
    lv_obj_t* heater_dd = lv_obj_find_by_name(overlay_root_, "chamber_heater_dropdown");
    if (heater_dd) {
        std::vector<std::string> assignable;
        for (const auto& heater : discovery.heaters()) {
            // Skip bed and extruder heaters — only show generic heaters
            if (heater == "heater_bed" || heater.rfind("extruder", 0) == 0) {
                continue;
            }
            assignable.push_back(heater);
        }

        auto built = build_chamber_assignment_options(assignable, discovery.chamber_heater_name(),
                                                      settings.get_chamber_heater_assignment(),
                                                      "heater_generic ", assignment_labels());

        lv_dropdown_set_options(heater_dd, built.options.c_str());
        lv_dropdown_set_selected(heater_dd, built.selected);

        // Store heater names for the callback via user_data
        auto* names = new std::vector<std::string>(std::move(built.names));
        lv_obj_set_user_data(heater_dd, names);

        // Cleanup on delete
        lv_obj_add_event_cb(
            heater_dd,
            [](lv_event_t* e) {
                lv_obj_t* obj = lv_event_get_target_obj(e);
                if (auto* data =
                        static_cast<std::vector<std::string>*>(lv_obj_get_user_data(obj))) {
                    delete data;
                    lv_obj_set_user_data(obj, nullptr);
                }
            },
            LV_EVENT_DELETE, nullptr);

        // Value changed handler
        lv_obj_add_event_cb(
            heater_dd,
            [](lv_event_t* e) {
                auto* dropdown = lv_event_get_target_obj(e);
                auto* names_ptr =
                    static_cast<std::vector<std::string>*>(lv_obj_get_user_data(dropdown));
                if (!names_ptr)
                    return;

                uint32_t sel = lv_dropdown_get_selected(dropdown);
                std::string value;

                if (sel == 0) {
                    value = "auto";
                } else if (sel == names_ptr->size() + 1) {
                    value = "none";
                } else if (sel - 1 < names_ptr->size()) {
                    value = (*names_ptr)[sel - 1];
                } else {
                    return;
                }

                helix::SettingsManager::instance().set_chamber_heater_assignment(value);
                spdlog::info("[SensorSettings] Chamber heater assignment: {}", value);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // --- Chamber Sensor Dropdown ---
    lv_obj_t* sensor_dd = lv_obj_find_by_name(overlay_root_, "chamber_sensor_dropdown");
    if (sensor_dd) {
        auto built = build_chamber_assignment_options(
            discovery.sensors(), discovery.chamber_sensor_name(),
            settings.get_chamber_sensor_assignment(), "temperature_sensor ", assignment_labels());

        lv_dropdown_set_options(sensor_dd, built.options.c_str());
        lv_dropdown_set_selected(sensor_dd, built.selected);

        // Store sensor names for the callback via user_data
        auto* names = new std::vector<std::string>(std::move(built.names));
        lv_obj_set_user_data(sensor_dd, names);

        // Cleanup on delete
        lv_obj_add_event_cb(
            sensor_dd,
            [](lv_event_t* e) {
                lv_obj_t* obj = lv_event_get_target_obj(e);
                if (auto* data =
                        static_cast<std::vector<std::string>*>(lv_obj_get_user_data(obj))) {
                    delete data;
                    lv_obj_set_user_data(obj, nullptr);
                }
            },
            LV_EVENT_DELETE, nullptr);

        // Value changed handler
        lv_obj_add_event_cb(
            sensor_dd,
            [](lv_event_t* e) {
                auto* dropdown = lv_event_get_target_obj(e);
                auto* names_ptr =
                    static_cast<std::vector<std::string>*>(lv_obj_get_user_data(dropdown));
                if (!names_ptr)
                    return;

                uint32_t sel = lv_dropdown_get_selected(dropdown);
                std::string value;

                if (sel == 0) {
                    value = "auto";
                } else if (sel == names_ptr->size() + 1) {
                    value = "none";
                } else if (sel - 1 < names_ptr->size()) {
                    value = (*names_ptr)[sel - 1];
                } else {
                    return;
                }

                helix::SettingsManager::instance().set_chamber_sensor_assignment(value);
                spdlog::info("[SensorSettings] Chamber sensor assignment: {}", value);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
    }
}

// ============================================================================
// TEMPERATURE SENSORS
// ============================================================================

void SensorSettingsOverlay::populate_temperature_sensors() {
    if (!overlay_root_)
        return;

    lv_obj_t* sensors_list = lv_obj_find_by_name(overlay_root_, "temp_sensors_list");
    if (!sensors_list) {
        spdlog::debug("[{}] Could not find temp_sensors_list container", get_name());
        return;
    }

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(sensors_list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(sensors_list, i);
        helix::ui::safe_delete(child);
    }

    auto& mgr = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = mgr.get_sensors_sorted();

    spdlog::debug("[{}] Populating temperature sensor list with {} sensors", get_name(),
                  sensors.size());

    for (const auto& sensor : sensors) {
        // Skip CHAMBER-role sensors — they're represented by the chamber assignment dropdowns
        if (sensor.role == helix::sensors::TemperatureSensorRole::CHAMBER) {
            continue;
        }

        auto* row = lv_obj_create(sensors_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);

        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.display_name.c_str());
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_set_flex_grow(name_label, 1);

        auto* type_label = lv_label_create(row);
        const char* type_str = lv_tr("Sensor");
        switch (sensor.role) {
        case helix::sensors::TemperatureSensorRole::MCU:
            type_str = "MCU";
            break;
        case helix::sensors::TemperatureSensorRole::HOST:
            type_str = "Host";
            break;
        case helix::sensors::TemperatureSensorRole::AUXILIARY:
            type_str = "Aux";
            break;
        default:
            type_str = lv_tr("Sensor");
            break;
        }
        lv_label_set_text(type_label, type_str);
        lv_obj_set_style_text_color(type_label, theme_manager_get_color("text_muted"), 0);

        spdlog::debug("[{}]   Created row for temp sensor: {} ({})", get_name(),
                      sensor.display_name, type_str);
    }
}

// ============================================================================
// AGGREGATE METHODS
// ============================================================================

void SensorSettingsOverlay::populate_all_sensors() {
    populate_switch_sensors();
    populate_probe_sensors();
    populate_width_sensors();
    populate_humidity_sensors();
    populate_accel_sensors();
    populate_color_sensors();
    populate_chamber_assignment();
    populate_temperature_sensors();
}

void SensorSettingsOverlay::update_all_sensor_counts() {
    update_switch_sensor_count();
    update_probe_sensor_count();
    update_width_sensor_count();
    update_humidity_sensor_count();
    update_accel_sensor_count();
    update_color_sensor_count();
    update_temperature_sensor_count();
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SensorSettingsOverlay::handle_switch_master_toggle_changed(bool enabled) {
    auto& mgr = helix::FilamentSensorManager::instance();
    mgr.set_master_enabled(enabled);
    mgr.save_config_to_file();
    spdlog::info("[{}] Switch sensor master enabled: {}", get_name(), enabled ? "ON" : "OFF");
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SensorSettingsOverlay::on_switch_master_toggle_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SensorSettingsOverlay] on_switch_master_toggle_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_sensor_settings_overlay().handle_switch_master_toggle_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
