// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>
#include <string_view>

namespace helix {

// Forward declarations — widget registration functions (defined in each widget .cpp)
void register_fan_stack_widget();
void register_temperature_widget();
void register_temp_stack_widget();
void register_network_widget();
void register_led_widget();
void register_led_controls_widget();
void register_thermistor_widget();
void register_favorite_macro_widgets();
void register_tips_widget();
void register_humidity_widget();
void register_width_sensor_widget();
void register_printer_image_widget();
void register_print_status_widget();
void register_shutdown_widget();
void register_lock_widget();
void register_macros_widget();
void register_motion_widget();
void register_clock_widget();
void register_control_buttons_widget();
void register_job_queue_widget();
void register_clog_detection_widget();
void register_print_stats_widget();
void register_gcode_console_widget();
void register_bed_temperature_widget();
void register_chamber_temperature_widget();
void register_preheat_widget();
void register_power_device_widget();
void register_fan_widget();
void register_temp_graph_widget();
void register_tool_switcher_widget();
void register_nozzle_temps_widget();
void register_active_spool_widget();
void register_bypass_widget();
#if HELIX_HAS_CAMERA
void register_camera_widget();
#endif

// Vector order defines the default display order on the home panel.
// NOTE: Factories are registered at runtime via init_widget_registrations(),
// NOT during static initialization. Do not add file-scope self-registration.
// clang-format off
static std::vector<PanelWidgetDef> s_widget_defs = {
    //                                                                                                        gate_subject          gate_hint                              default col row min_c min_r max_c max_r  multi  half_c half_r
    {"printer_image",    "Printer Image",    "rotate_3d",        "3D printer visualization",    nullptr,              nullptr,                               true,  2, 2, 1, 1, 4, 3},
    {"print_status",     "Print Status",     "printer_3d",       "Print progress and file selection",     nullptr,              nullptr,                               true,  2, 2, 2, 1, 4, 3},
    {"shutdown",         "Shutdown/Reboot",   "power",            "Shutdown or reboot the printer host",  "platform_host_power_supported", "Not available on Android",       false, 1, 1, 1, 1, 1, 1, false, true, false},
    {"lock",             "Lock Screen",       "lock",             "PIN-protected screen lock",      nullptr,              nullptr,                               false, 1, 1, 1, 1, 1, 1, false, true, false},
    {"power_device",     "Power",            "power_cycle",      "Toggle Moonraker power devices",            "power_device_count", "Requires Moonraker power device",     false, 1, 1, 1, 1, 1, 1, true},
    {"network",          "Network",          "wifi_strength_4",  "Wi-Fi and ethernet connection status",          nullptr,              nullptr,                               false, 1, 1, 1, 1, 2, 1},
    {"firmware_restart", "Firmware Restart",  "refresh",          "Restart Klipper firmware", nullptr,              nullptr,                               false, 1, 1, 1, 1, 1, 1, false, true, false},
    {"tool_switcher",    "Tool Switcher",     "arrow_left_right", "Quick tool switching for multi-tool printers",    nullptr,              nullptr,                               false, 1, 1, 1, 1, 2, 2},
    {"led",              "LED Light",         "lightbulb_outline","Quick toggle, long press for full control",        "led_controllable",   "No LED strips detected",              true,  1, 1, 1, 1, 2, 1},
    {"led_controls",     "LED Controls",      "led_strip",        "Open LED color and brightness controls",     "led_controllable",   "No LED strips detected",              false, 1, 1, 1, 1, 1, 1, false, true, false},
    {"fan_stack",        "Fan Speeds",        "fan",              "Part, hotend, and auxiliary fan speeds",       nullptr,              nullptr,                               true,  1, 1, 1, 1, 3, 2, true},
    {"fan",              "Fan",               "fan",              "Monitor a single fan speed",              nullptr,              nullptr,                               false, 1, 1, 1, 1, 2, 1, true},
    {"temperature",      "Nozzle Temperature","thermometer",      "Monitor and set nozzle temperature", nullptr,            nullptr,                               true,  1, 1, 1, 1, 2, 2},
    {"nozzle_temps",     "Nozzle Temperatures","thermometer",      "All extruder temperatures with progress bars", nullptr,           nullptr,                               false, 1, 2, 1, 1, 2, 3},
    {"bed_temperature",  "Bed Temperature",   "radiator",         "Monitor and set bed temperature",    nullptr,            nullptr,                               false, 1, 1, 1, 1, 2, 2},
    {"chamber_temperature", "Chamber Temperature", "fridge_industrial", "Monitor and set chamber temperature", "printer_has_chamber", "No chamber temperature sensor detected", false, 1, 1, 1, 1, 2, 2},
    {"temp_stack",       "Temperatures",      "thermometer",      "Nozzle, bed, and chamber temps stacked",     nullptr,              nullptr,                               false, 1, 1, 1, 1, 3, 2},
    {"thermistor",       "Temperature Sensors", "thermometer",    "Monitor temperature sensors (single or carousel)", "temp_sensor_count", "No temperature sensors detected", false, 1, 1, 1, 1, 2, 1, true},
    {"temp_graph",       "Temperature Graph", "chart_line",       "Live temperature graph with configurable sensors", nullptr,         nullptr,                               false, 2, 2, 1, 1, 6, 4, true},
    {"preheat",          "Preheat",           "heat_wave",        "Quick preheat with material selection",            nullptr,            nullptr,                               false, 3, 1, 2, 1, 4, 1},
    {"ams",              "Multi-Filament System Status",        "filament",         "Multi-Filament System spool status and control",       "ams_slot_count",     "Requires Multi-Filament System or MMU hardware",        false, 1, 1, 1, 1, 4, 2},
    {"bypass",           "Bypass",            "source_branch",    "Toggle external spool bypass",  "ams_supports_bypass", "Requires a filament system with bypass", false, 1, 1, 1, 1, 2, 1},
    {"active_spool",     "Active Spool",      "inventory",  "Currently loaded spool info",     nullptr,                  nullptr,                           false, 1, 1, 1, 1, 4, 2},
    {"filament",         "Filament Sensor",   "filament_alert",   "Filament runout detection status",  "filament_sensor_count", "No filament sensor detected",      true, 1, 1, 1, 1, 2, 1},
    {"humidity",         "Humidity",          "water",            "Enclosure humidity sensor readings",         "humidity_sensor_count", "No humidity sensor detected",       false, 1, 1, 1, 1, 2, 2},
    {"width_sensor",     "Width Sensor",      "ruler",            "Filament width sensor readings",     "width_sensor_count", "No width sensor detected",            false, 1, 1, 1, 1, 2, 2},
    {"favorite_macro", "Macro Button",    "play",             "Run a configured macro with one tap",     nullptr,              nullptr,                               false, 1, 1, 1, 1, 2, 1, true},
    {"macros",           "Macros",            "script_text",      "Browse and execute Klipper macros",           nullptr,              nullptr,                               false, 1, 1, 1, 1, 1, 1},
    {"motion",           "Motion",            "cursor_move",      "Jump directly to motion control / jogging",           nullptr,              nullptr,                               false, 1, 1, 1, 1, 1, 1},
    {"clock",            "Digital Clock",     "clock",            "Current time and date",    nullptr,              nullptr,                               false, 2, 1, 1, 1, 3, 3, false, true, true},
    {"control_buttons",  "Print Controls",    "pause",            "Pause/resume and stop the active print",   nullptr,              nullptr,                               false, 2, 1, 2, 1, 2, 1},
    {"job_queue",        "Job Queue",         "progress_clock",   "Queued print jobs",        nullptr,              nullptr,                               false, 2, 2, 2, 1, 4, 3},
    //                                                                                                        gate_subject          gate_hint                              default col row min_c min_r max_c max_r  multi  half_c half_r
    {"tips",             "Tips",              "help_circle",      "Rotating tips and helpful information",             nullptr,              nullptr,                               true,  4, 2, 2, 1, 6, 2},
    {"clog_detection",   "Clog Detection",    "gauge",            "Filament clog/flow detection meter",   "clog_meter_mode",    "Requires clog detection hardware",    false, 1, 1, 1, 1, 2, 2},
    {"print_stats",      "Print Stats",       "printer_3d",       "Print history statistics",      nullptr,              nullptr,                               false, 2, 2, 2, 1, 3, 2},
    {"gcode_console",    "GCode Console",     "console",          "Open G-code command console",    nullptr,              nullptr,                               false, 1, 1, 1, 1, 1, 1},
#if HELIX_HAS_CAMERA
    {"camera",           "Camera",            "video",            "Live webcam feed",           nullptr,              nullptr,                               false, 2, 2, 1, 1, 4, 3},
#endif
    {"notifications",    "Notifications",     "notifications",    "Pending alerts and system messages",    nullptr,              nullptr,                               true,  1, 1, 1, 1, 2, 1},
};
// clang-format on

const std::vector<PanelWidgetDef>& get_all_widget_defs() {
    return s_widget_defs;
}

const PanelWidgetDef* find_widget_def(std::string_view id) {
    auto it = std::find_if(s_widget_defs.begin(), s_widget_defs.end(),
                           [&id](const PanelWidgetDef& def) { return id == def.id; });
    if (it != s_widget_defs.end())
        return &*it;

    // Multi-instance: strip ":N" suffix and retry
    auto colon = id.rfind(':');
    if (colon != std::string_view::npos) {
        auto base = id.substr(0, colon);
        it = std::find_if(
            s_widget_defs.begin(), s_widget_defs.end(),
            [&base](const PanelWidgetDef& def) { return base == def.id && def.multi_instance; });
        if (it != s_widget_defs.end())
            return &*it;
    }
    return nullptr;
}

size_t widget_def_count() {
    return s_widget_defs.size();
}

void register_widget_factory(std::string_view id, WidgetFactory factory) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.factory = std::move(factory);
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Factory registration failed: '{}' not found", id);
}

void register_widget_subjects(std::string_view id, SubjectInitFn init_fn) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.init_subjects = std::move(init_fn);
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Subject init registration failed: '{}' not found", id);
}

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
    register_humidity_widget();
    register_width_sensor_widget();
    register_shutdown_widget();
    register_lock_widget();
    register_clog_detection_widget();
    register_print_stats_widget();
    register_gcode_console_widget();
    register_macros_widget();
    register_motion_widget();
    register_preheat_widget();
    register_active_spool_widget();
    register_bypass_widget();
    register_tool_switcher_widget();
    register_nozzle_temps_widget();
#if HELIX_HAS_CAMERA
    register_camera_widget();
#endif

    spdlog::debug("[PanelWidgetRegistry] All widget factories registered");
}

} // namespace helix
