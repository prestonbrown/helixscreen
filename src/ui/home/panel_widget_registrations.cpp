// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_layout.h"
#include "helix_features.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

static_assert(kWidgetMaxTracks == GridLayout::kMaxTracks,
              "the registry's span cap must be the widest grid GridLayout builds");

// Forward declarations — widget registration functions (defined in each widget .cpp)
void register_fan_stack_widget();
void register_temperature_widget();
void register_temp_stack_widget();
void register_network_widget();
void register_tile_widgets();
void register_led_widget();
void register_led_controls_widget();
void register_thermistor_widget();
void register_favorite_macro_widgets();
void register_tips_widget();
void register_unit_tile_widgets();
void register_printer_image_widget();
void register_print_status_widget();
void register_shutdown_widget();
void register_lock_widget();
void register_macros_widget();
void register_motion_widget();
void register_filament_ops_widget();
void register_filament_sensor_widget();
void register_clock_widget();
void register_control_buttons_widget();
void register_job_queue_widget();
void register_clog_detection_widget();
void register_filament_buffer_widget();
void register_print_stats_widget();
void register_maintenance_widget();
#if HELIX_HAS_CONSOLE
void register_gcode_console_widget();
#endif
void register_bed_temperature_widget();
void register_chamber_temperature_widget();
void register_preheat_widget();
void register_power_device_widget();
void register_fan_widget();
void register_temp_graph_widget();
void register_tool_switcher_widget();
void register_nozzle_temps_widget();
void register_speed_flow_widget();
void register_dryer_widget();
void register_z_offset_widget();
void register_bed_mesh_tile_widget();
#if HELIX_HAS_TOOLHEAD_WIDGET
void register_toolhead_widget();
#endif
void register_host_health_widget();
void register_exclude_map_widget();
void register_active_spool_widget();
void register_bypass_widget();
void register_display_message_widget();
#if HELIX_HAS_CAMERA
void register_camera_widget();
#endif

void init_widget_registrations() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    register_printer_image_widget();
    register_print_status_widget();
    register_power_device_widget();
    register_network_widget();
    register_temperature_widget();
    register_bed_temperature_widget();
    register_chamber_temperature_widget();
    register_temp_stack_widget();
    register_led_widget();
    register_led_controls_widget();
    register_fan_stack_widget();
    register_fan_widget();
    register_thermistor_widget();
    register_temp_graph_widget();
    register_favorite_macro_widgets();
    register_clock_widget();
    register_control_buttons_widget();
    register_job_queue_widget();
    register_tips_widget();
    register_unit_tile_widgets();
    register_shutdown_widget();
    register_lock_widget();
    register_clog_detection_widget();
    register_filament_buffer_widget();
    register_print_stats_widget();
    register_maintenance_widget();
#if HELIX_HAS_CONSOLE
    register_gcode_console_widget();
#endif
    register_macros_widget();
    register_motion_widget();
    register_filament_ops_widget();
    register_filament_sensor_widget();
    register_preheat_widget();
    register_active_spool_widget();
    register_bypass_widget();
    register_display_message_widget();
    register_tool_switcher_widget();
    register_nozzle_temps_widget();
    register_speed_flow_widget();
    register_dryer_widget();
    register_z_offset_widget();
    register_bed_mesh_tile_widget();
#if HELIX_HAS_TOOLHEAD_WIDGET
    register_toolhead_widget();
#endif
    register_host_health_widget();
    register_exclude_map_widget();
#if HELIX_HAS_CAMERA
    register_camera_widget();
#endif

    spdlog::debug("[PanelWidgetRegistry] All widget factories registered");

    // Last, so "already claimed" is the truth: tiles no widget class took
    // get a sizing-only instance.
    register_tile_widgets();
}

} // namespace helix
