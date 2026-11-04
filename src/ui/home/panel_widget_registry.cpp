// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_registry.h"

#include "grid_layout.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

namespace helix {

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
void register_humidity_widget();
void register_width_sensor_widget();
void register_printer_image_widget();
void register_print_status_widget();
void register_shutdown_widget();
void register_lock_widget();
void register_macros_widget();
void register_motion_widget();
void register_filament_sensor_widget();
void register_clock_widget();
void register_control_buttons_widget();
void register_job_queue_widget();
void register_clog_detection_widget();
void register_filament_buffer_widget();
void register_print_stats_widget();
void register_maintenance_widget();
void register_gcode_console_widget();
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

// Vector order defines the default display order on the home panel.
// NOTE: Factories are registered at runtime via init_widget_registrations(),
// NOT during static initialization. Do not add file-scope self-registration.
//
// Spans are in GRID TRACKS, and a track is half a cell
// (GridLayout::TRACKS_PER_CELL). A widget with neither half-cell flag set must
// therefore span an even number of tracks on that axis: edit mode snaps it to a
// whole cell, so an odd span is a size the user could never restore after one
// drag. Only the axes marked half_c / half_r may carry an odd number.
//
// The grid divides each panel axis by the same cell edge, so a track is very
// nearly the same number of pixels on every panel and these numbers are authored
// once for all of them. tests/unit/test_registry_span_bands.cpp pins the pixel
// band each span lands in, per shipping geometry.

// Max colspan for a band-shaped widget — one that reads as a full-width strip
// rather than a tile. There is no width that is wrong for these, and any finite
// cap makes a full-width default layout unauthorable on the wider grids: large
// and xlarge are 16 tracks, xxlarge reaches 26 at 1080p, and an ultrawide panel
// reaches 46. kMaxTracks is the widest grid the layout engine will ever build,
// so this is "uncapped" spelled in the same units. The cap still bounds the
// edit-mode resize handle, which is its real job.
constexpr int kBandColspan = GridLayout::kMaxTracks;

// Category shorthands — the table below is column-aligned and the fully
// qualified enumerators would not fit.
constexpr WidgetCategory kCatPrint = WidgetCategory::PrintStatus;
constexpr WidgetCategory kCatTemp = WidgetCategory::Temperature;
constexpr WidgetCategory kCatFilament = WidgetCategory::Filament;
constexpr WidgetCategory kCatControls = WidgetCategory::Controls;
constexpr WidgetCategory kCatSystem = WidgetCategory::System;

// clang-format off
static std::vector<PanelWidgetDef> s_widget_defs = {
    //                                                                                                                                          hint                                cat              en  col row min_c min_r max_c max_r  multi  half_c half_r merges_card
    {"printer_image",    TR_NOOP("Printer Image"),    "rotate_3d",        TR_NOOP("A picture of your printer"),    nullptr,              nullptr,                               kCatPrint,    true,  4, 4, 2, 2, 8, 6, false, true, true, false},
    {"print_status",     TR_NOOP("Print Status"),     "printer_3d",       TR_NOOP("Progress, file, and time remaining"),     nullptr,              nullptr,                               kCatPrint,    true,  4, 4, 4, 2, kBandColspan, 6, false, true, true, false},
    {"shutdown",         TR_NOOP("Shutdown/Reboot"),   "power",            TR_NOOP("Shut down or restart the host"),  "platform_host_power_supported", "Not available on Android",                               kCatSystem,   false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true, true, nullptr, nullptr, WidgetScope::Screen},
    {"lock",             TR_NOOP("Lock Screen"),       "lock",             TR_NOOP("Lock the screen with your PIN"),      nullptr,              nullptr,                               kCatSystem,   false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true, true, nullptr, nullptr, WidgetScope::Screen},
    {"power_device",     TR_NOOP("Power"),            "power_cycle",      TR_NOOP("Switch a power device on or off"),            "power_device_count", "Requires Moonraker power device",     kCatControls, false, 2, 2, 1, 1, kBandColspan, kBandColspan, true, true, true},
    {"network",          TR_NOOP("Network"),          "wifi_strength_4",  TR_NOOP("Wi-Fi or ethernet connection status"),          nullptr,              nullptr,                               kCatSystem,   false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true, true, nullptr, nullptr, WidgetScope::Screen},
    {"firmware_restart", TR_NOOP("Firmware Restart"),  "cog_refresh",      TR_NOOP("Restart Klipper without rebooting"), nullptr,              nullptr,                               kCatSystem,   false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"tool_switcher",    TR_NOOP("Tool Switcher"),     "arrow_left_right", TR_NOOP("Switch the active tool or extruder"),    nullptr,              nullptr,                               kCatControls, false, 2, 2, 1, 1, 8, 4, false, true, true},
    {"led",              TR_NOOP("LED Light"),         "lightbulb_outline",TR_NOOP("Turn the printer's lights on or off"),        "led_controllable",   "No LED strips detected",              kCatControls, true,  2, 2, 1, 1, kBandColspan, kBandColspan, true, true, true},
    {"led_controls",     TR_NOOP("LED Controls"),      "led_strip",        TR_NOOP("Open color and brightness controls"),     "led_has_devices",    "No LED strips detected",              kCatControls, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"fan_stack",        TR_NOOP("Fan Speeds"),        "fan",              TR_NOOP("Part, hotend, and auxiliary fan speeds"),       nullptr,              nullptr,                               kCatTemp,     true,  2, 2, 2, 2, 6, 4, true, true, true},
    {"fan",              TR_NOOP("Fan"),               "fan",              TR_NOOP("The speed of one fan you choose"),              nullptr,              nullptr,                               kCatTemp,     false, 2, 2, 1, 1, kBandColspan, kBandColspan, true, true, true},
    {"temperature",      TR_NOOP("Nozzle Temperature"),"thermometer",      TR_NOOP("Set and watch the nozzle temperature"), nullptr,            nullptr,                               kCatTemp,     true,  2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"nozzle_temps",     TR_NOOP("Nozzle Temperatures"),"thermometer",      TR_NOOP("Every extruder's temperature at once"), nullptr,           nullptr,                               kCatTemp,     false, 2, 4, 2, 2, 4, 6, false, true, true, false},
    {"bed_temperature",  TR_NOOP("Bed Temperature"),   "radiator",         TR_NOOP("Set and watch the bed temperature"),    nullptr,            nullptr,                               kCatTemp,     true, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"chamber_temperature", TR_NOOP("Chamber Temperature"), "fridge_industrial", TR_NOOP("Set and watch the chamber temperature"), "printer_has_chamber", "No chamber temperature sensor detected", kCatTemp,     false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"temp_stack",       TR_NOOP("Temperatures"),      "thermometer",      TR_NOOP("Nozzle, bed, and chamber in one widget"),     nullptr,              nullptr,                               kCatTemp,     false, 2, 2, 2, 2, 6, 4, false, true, true},
    {"thermistor",       TR_NOOP("Temperature Sensors"), "thermometer",    TR_NOOP("Readings from extra temperature sensors"), "temp_sensor_count", "No temperature sensors detected", kCatTemp,     false, 2, 2, 1, 1, kBandColspan, kBandColspan, true, true, true},
    {"temp_graph",       TR_NOOP("Temperature Graph"), "chart_line",       TR_NOOP("Temperatures plotted over time"), nullptr,         nullptr,                               kCatTemp,     false, 4, 4, 2, 2, kBandColspan, 8, true, true, true, false},
    {"preheat",          TR_NOOP("Preheat"),           "heat_wave",        TR_NOOP("Warm up for a material in one tap"),            nullptr,            nullptr,                               kCatTemp,     false, 6, 2, 4, 2, 8, 2, false, true, false},
    {"ams",              TR_NOOP("Multi-Filament System Status"),        "filament",         TR_NOOP("Slot colors, materials, and levels"),       "ams_slot_count",     "Requires Multi-Filament System or MMU hardware",        kCatFilament, false, 2, 2, 2, 2, 8, 4, false, true, true},
    {"bypass",           TR_NOOP("Bypass"),           "source_branch",    TR_NOOP("Toggle external spool bypass"), "ams_supports_bypass", "Requires a filament system with bypass", kCatFilament, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"active_spool",     TR_NOOP("Active Spool"),      "inventory",  TR_NOOP("The Spoolman spool currently loaded"),     nullptr,                  nullptr,                           kCatFilament, false, 2, 2, 1, 1, 8, 4, false, true, true},
    {"filament",         TR_NOOP("Filament Sensor"),   "filament_sensor_alert",   TR_NOOP("Whether filament is loaded"),  "filament_sensor_count", "No filament sensor detected",      kCatFilament, true, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"humidity",         TR_NOOP("Humidity"),          "water",            TR_NOOP("Humidity inside the enclosure"),         "humidity_sensor_count", "No humidity sensor detected",       kCatFilament, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"width_sensor",     TR_NOOP("Width Sensor"),      "ruler",            TR_NOOP("Measured filament diameter"),     "width_sensor_count", "No width sensor detected",            kCatFilament, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"favorite_macro", TR_NOOP("Macro Button"),    "play",             TR_NOOP("Run a macro you choose, in one tap"),     nullptr,              nullptr,                               kCatControls, false, 2, 2, 1, 1, kBandColspan, kBandColspan, true, true, true},
    {"macros",           TR_NOOP("Macros"),            "script_text",      TR_NOOP("Browse and run your Klipper macros"),           nullptr,              nullptr,                               kCatControls, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"motion",           TR_NOOP("Motion"),            "cursor_move",      TR_NOOP("Open motion controls for jogging"),           nullptr,              nullptr,                               kCatControls, false, 2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true},
    {"clock",            TR_NOOP("Digital Clock"),     "clock",            TR_NOOP("The time, date, and system uptime"),    nullptr,              nullptr,                               kCatSystem,   false, 4, 2, 2, 2, 6, 6, false, true, true, true, nullptr, nullptr, WidgetScope::Screen},
    {"host_health",      TR_NOOP("Host Health"),       "gauge",            TR_NOOP("Host and MCU load, temperature, and memory"), "perf_available",   "Requires Moonraker performance data", kCatSystem,   false, 4, 2, 1, 1, 8, 8, false, true, true},
    {"control_buttons",  TR_NOOP("Print Controls"),    "pause",            TR_NOOP("Pause, resume, or stop the print"),   nullptr,              nullptr,                               kCatPrint,    false, 4, 2, 2, 2, 8, 4},
    {"job_queue",        TR_NOOP("Job Queue"),         "progress_clock",   TR_NOOP("Print jobs waiting to run"),        "printer_has_job_queue", TR_NOOP("Job queue not enabled in Moonraker"), kCatPrint, false, 4, 4, 2, 1, 8, 6, false, true, true},
    {"speed_flow",       TR_NOOP("Speed / Flow"),      "speed",            TR_NOOP("Speed and flow overrides, with a tap to tune"), nullptr,           nullptr,                               kCatPrint,    false, 4, 2, 1, 1, 8, 4, false, true, true},
    {"exclude_map",      TR_NOOP("Exclude Objects"),   "debug_step_over",  TR_NOOP("Objects in the print, with a tap to open the skip map"), "printer_has_exclude_object", "Requires Klipper exclude_object", kCatPrint,    false, 4, 4, 1, 1, 8, 6, false, true, true},
    {"dryer",            TR_NOOP("Filament Dryer"),    "heat_wave",        TR_NOOP("Drying progress and humidity for boxes, the chamber, and the bed"), "home_dryer_available", "No filament dryer detected",         kCatFilament, false, 2, 2, 1, 1, 6, 8, false, true, true},
    {"z_offset",         TR_NOOP("Z-Offset"),          "axis_z_arrow",     TR_NOOP("Live Z-offset, with baby steps and Save"), nullptr,           nullptr,                               kCatPrint,    false, 4, 2, 1, 1, 8, 4, false, true, true},
    {"bed_mesh_tile",    TR_NOOP("Bed Mesh"),          "grid_large",       TR_NOOP("Heightmap and flatness of the active bed mesh"), "printer_has_bed_mesh", "No bed mesh support detected",      kCatControls, false, 4, 4, 1, 1, 12, 8, false, true, true},
#if HELIX_HAS_TOOLHEAD_WIDGET
    {"toolhead",         TR_NOOP("Toolhead"),          "axis_arrow",       TR_NOOP("Toolhead position and homing, with a plate view and Home All"), nullptr,           nullptr,                               kCatControls, false, 4, 4, 1, 1, 8, 8, false, true, true},
#endif
    //                                                                                                                                          hint                                cat              en  col row min_c min_r max_c max_r  multi  half_c half_r merges_card
    {"tips",             TR_NOOP("Tips"),              "help_circle",      TR_NOOP("Rotating tips for using your printer"),             nullptr,              nullptr,                               kCatSystem,   true,  8, 4, 4, 2, kBandColspan, 4, false, true, true, false, nullptr, nullptr, WidgetScope::Screen},
    // Two cells wide by one tall: the FlowGuard scale is horizontal, and it
    // carries a label at each end (#1017). One cell across had the arc, its
    // value and its mode text stacked in a box narrower than the words —
    // reported as showing "nothing useful". The minimum matches the default so
    // a drag cannot put it back there; height still scales down to one cell.
    {"clog_detection",   TR_NOOP("Clog Detection"),    "gauge",            TR_NOOP("Clog and flow health while printing"),   "clog_meter_mode",    "Requires a clog detector or filament motion sensor", kCatFilament, false, 4, 2, 4, 2, 8, 4, false, true, true},
    {"filament_buffer",  TR_NOOP("Filament Buffer"),   "arrow_up_down",    TR_NOOP("Filament buffer position against its target"), "buffer_present", "Requires a filament pressure sensor or sync feedback", kCatFilament, false, 2, 2, 2, 2, 4, 2, false, false, false},
    // One half-row tall is a status line: a single line of the message, scrolling when it
    // is too long. Taller boxes wrap it.
    {"display_message",  TR_NOOP("Printer Message"),   "message",          TR_NOOP("The message your printer's macros show (M117)"), nullptr,  nullptr,                           kCatPrint,    false, 4, 2, 2, 1, kBandColspan, 6, false, true, true},
    {"print_stats",      TR_NOOP("Print Stats"),       "printer_3d",       TR_NOOP("Print hours this week and lifetime totals"),      nullptr,              nullptr,                               kCatPrint,    false, 4, 4, 2, 1, 6, 4, false, true, true},
    {"maintenance",      TR_NOOP("Maintenance"),       "wrench",           TR_NOOP("Service hours for the filter, nozzle, and rails"), nullptr,           nullptr,                               kCatSystem,   false, 2, 2, 1, 1, 8, 4, false, true, true},
    // Two cells square by default: at two cells tall the tile shows a live tail
    // of the console output, and below that it is the icon tile.
    {"gcode_console",    TR_NOOP("GCode Console"),     "console",          TR_NOOP("Send G-code and read the replies"),    nullptr,              nullptr,                               kCatControls, false, 4, 4, 1, 2, kBandColspan, kBandColspan, false, true, true},
#if HELIX_HAS_CAMERA
    {"camera",           TR_NOOP("Camera"),            "webcam",           TR_NOOP("Live view from your webcam"),           nullptr,              nullptr,                               kCatPrint,    false, 4, 4, 2, 2, 8, 6, false, true, true, false},
#endif
    {"notifications",    TR_NOOP("Notifications"),     "notifications",    TR_NOOP("Alerts and messages waiting for you"),    nullptr,              nullptr,                               kCatSystem,   true,  2, 2, 1, 1, kBandColspan, kBandColspan, false, true, true, true, nullptr, nullptr, WidgetScope::Screen},
};
// clang-format on

// Vector order is the catalog display order — most-reached group first.
// Intentionally coarser than the doc's section list; see WidgetCategory.
static const std::vector<WidgetCategoryDef> s_widget_categories = {
    {WidgetCategory::PrintStatus, "Print & Status", "Print & Status", "printer_3d"},
    {WidgetCategory::Temperature, "Temperature & Cooling", "Temperature & Cooling", "thermometer"},
    {WidgetCategory::Filament, "Filament", "Filament", "filament"},
    {WidgetCategory::Controls, "Controls", "Controls", "script_text"},
    {WidgetCategory::System, "System", "System", "power"},
    // Last: plugin tiles are the least-reached group, and the row only
    // appears once a runtime definition exists (populate_category_rows skips
    // a category with nothing available on this printer).
    {WidgetCategory::Plugins, TR_NOOP("Plugins"), TR_NOOP("Plugins"), "puzzle_outline"},
};

const std::vector<WidgetCategoryDef>& get_widget_categories() {
    return s_widget_categories;
}

const WidgetCategoryDef* find_widget_category(WidgetCategory id) {
    auto it = std::find_if(s_widget_categories.begin(), s_widget_categories.end(),
                           [id](const WidgetCategoryDef& cat) { return cat.id == id; });
    return it != s_widget_categories.end() ? &*it : nullptr;
}

namespace {
struct RuntimeSlot {
    RuntimeWidgetDef def;
    bool active = false;
};
// Node-based and never erased: a slot's `def.id` buffer lives for the process,
// so LVGL user_data holding a def's id stays readable until the row rebuilds.
std::map<std::string, RuntimeSlot, std::less<>>& runtime_slots() {
    static auto* slots = new std::map<std::string, RuntimeSlot, std::less<>>();
    return *slots;
}
std::vector<PanelWidgetDef> s_all_defs;
bool s_all_defs_dirty = true;
uint64_t s_runtime_generation = 0;
size_t s_active_runtime_defs = 0;

void rebuild_all_defs() {
    s_all_defs = s_widget_defs;
    for (auto& [id, slot] : runtime_slots()) {
        (void)id;
        if (!slot.active)
            continue;
        const RuntimeWidgetDef& r = slot.def;
        PanelWidgetDef d{
            r.id.c_str(), r.display_name.c_str(), r.icon.c_str(), r.description.c_str(), nullptr,
            nullptr,      WidgetCategory::Plugins};
        d.default_enabled = false;
        d.colspan = r.colspan;
        d.rowspan = r.rowspan;
        d.max_colspan = r.max_colspan;
        d.max_rowspan = r.max_rowspan;
        d.supports_half_col = r.supports_half_col;
        d.supports_half_row = r.supports_half_row;
        d.factory = r.factory;
        s_all_defs.push_back(d);
    }
    s_all_defs_dirty = false;
}
} // namespace

const std::vector<PanelWidgetDef>& get_all_widget_defs() {
    // With no runtime def the built-ins are the whole list, so there is no second
    // copy of the table: builds that host no plugins (the ESP32 image) never make one.
    if (s_active_runtime_defs == 0)
        return s_widget_defs;
    if (s_all_defs_dirty)
        rebuild_all_defs();
    return s_all_defs;
}

const PanelWidgetDef* find_widget_def(std::string_view id) {
    const auto& defs = get_all_widget_defs();
    auto it = std::find_if(defs.begin(), defs.end(),
                           [&id](const PanelWidgetDef& def) { return id == def.id; });
    if (it != defs.end())
        return &*it;

    // Multi-instance: strip ":N" suffix and retry
    auto colon = id.rfind(':');
    if (colon != std::string_view::npos) {
        auto base = id.substr(0, colon);
        it = std::find_if(defs.begin(), defs.end(), [&base](const PanelWidgetDef& def) {
            return base == def.id && def.multi_instance;
        });
        if (it != defs.end())
            return &*it;
    }
    return nullptr;
}

bool is_screen_scoped(const std::string& widget_id) {
    const PanelWidgetDef* def = find_widget_def(widget_id);
    return def && def->scope == WidgetScope::Screen;
}

size_t widget_def_count() {
    return get_all_widget_defs().size();
}

void register_widget_factory(std::string_view id, WidgetFactory factory) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.factory = factory;
            // Edit the published copy in place rather than marking it dirty: a
            // rebuild replaces the whole vector, dropping edits a caller still
            // holds through a def pointer (tests patch spans that way).
            if (!s_all_defs_dirty) {
                for (auto& d : s_all_defs) {
                    if (id == d.id) {
                        d.factory = factory;
                        break;
                    }
                }
            }
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Factory registration failed: '{}' not found", id);
}

void register_widget_subjects(std::string_view id, SubjectInitFn init_fn) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.init_subjects = init_fn;
            if (!s_all_defs_dirty) {
                for (auto& d : s_all_defs) {
                    if (id == d.id) {
                        d.init_subjects = init_fn;
                        break;
                    }
                }
            }
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Subject init registration failed: '{}' not found", id);
}

bool register_runtime_widget_def(RuntimeWidgetDef def) {
    for (const auto& b : s_widget_defs) {
        if (def.id == b.id)
            return false;
    }
    auto& slots = runtime_slots();
    auto it = slots.find(def.id);
    if (it == slots.end()) {
        std::string key = def.id;
        slots.emplace(std::move(key), RuntimeSlot{std::move(def), true});
        ++s_active_runtime_defs;
    } else {
        // Field by field, leaving `id` alone: its buffer is what catalog rows
        // point at.
        RuntimeWidgetDef& cur = it->second.def;
        cur.display_name = std::move(def.display_name);
        cur.icon = std::move(def.icon);
        cur.description = std::move(def.description);
        cur.colspan = def.colspan;
        cur.rowspan = def.rowspan;
        cur.max_colspan = def.max_colspan;
        cur.max_rowspan = def.max_rowspan;
        cur.supports_half_col = def.supports_half_col;
        cur.supports_half_row = def.supports_half_row;
        cur.factory = std::move(def.factory);
        if (!it->second.active)
            ++s_active_runtime_defs;
        it->second.active = true;
    }
    s_all_defs_dirty = true;
    ++s_runtime_generation;
    return true;
}

void unregister_runtime_widget_def(std::string_view id) {
    auto& slots = runtime_slots();
    auto it = slots.find(id);
    if (it == slots.end() || !it->second.active)
        return;
    it->second.active = false;
    it->second.def.factory = nullptr;
    if (--s_active_runtime_defs == 0)
        std::vector<PanelWidgetDef>().swap(s_all_defs); // back to the built-in table alone
    s_all_defs_dirty = true;
    ++s_runtime_generation;
}

uint64_t runtime_widget_generation() {
    return s_runtime_generation;
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
    register_filament_buffer_widget();
    register_print_stats_widget();
    register_maintenance_widget();
    register_gcode_console_widget();
    register_macros_widget();
    register_motion_widget();
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
