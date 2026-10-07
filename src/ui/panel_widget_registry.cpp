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
// reaches 46. MAX_TRACKS is the widest grid the layout engine will ever build,
// so this is "uncapped" spelled in the same units. The cap still bounds the
// edit-mode resize handle, which is its real job.
constexpr int BAND_COLSPAN = GridLayout::MAX_TRACKS;

// Category shorthands — the table below is column-aligned and the fully
// qualified enumerators would not fit.
constexpr WidgetCategory CAT_PRINT = WidgetCategory::PrintStatus;
constexpr WidgetCategory CAT_TEMP = WidgetCategory::Temperature;
constexpr WidgetCategory CAT_FILAMENT = WidgetCategory::Filament;
constexpr WidgetCategory CAT_CONTROLS = WidgetCategory::Controls;
constexpr WidgetCategory CAT_SYSTEM = WidgetCategory::System;

// clang-format off
static std::vector<PanelWidgetDef> s_widget_defs = {
    //                                                                                                                                          hint                                cat              en  col row min_c min_r max_c max_r  multi  half_c half_r merges_card
    {"printer_image",    TR_NOOP("Printer Image"),    "rotate_3d",        TR_NOOP("A picture of your printer"),    nullptr,              nullptr,                               CAT_PRINT,    true,  4, 4, 2, 2, 8, 6, false, true, true, false},
    {"print_status",     TR_NOOP("Print Status"),     "printer_3d",       TR_NOOP("Progress, file, and time remaining"),     nullptr,              nullptr,                               CAT_PRINT,    true,  4, 4, 4, 2, BAND_COLSPAN, 6, false, true, true, false},
    {"shutdown",         TR_NOOP("Shutdown/Reboot"),   "power",            TR_NOOP("Shut down or restart the host"),  "platform_host_power_supported", "Not available on Android",                               CAT_SYSTEM,   false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"lock",             TR_NOOP("Lock Screen"),       "lock",             TR_NOOP("Lock the screen with your PIN"),      nullptr,              nullptr,                               CAT_SYSTEM,   false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"power_device",     TR_NOOP("Power"),            "power_cycle",      TR_NOOP("Switch a power device on or off"),            "power_device_count", "Requires Moonraker power device",     CAT_CONTROLS, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, true, true, true},
    {"network",          TR_NOOP("Network"),          "wifi_strength_4",  TR_NOOP("Wi-Fi or ethernet connection status"),          nullptr,              nullptr,                               CAT_SYSTEM,   false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"firmware_restart", TR_NOOP("Firmware Restart"),  "refresh",          TR_NOOP("Restart Klipper without rebooting"), nullptr,              nullptr,                               CAT_SYSTEM,   false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"tool_switcher",    TR_NOOP("Tool Switcher"),     "arrow_left_right", TR_NOOP("Switch the active tool or extruder"),    nullptr,              nullptr,                               CAT_CONTROLS, false, 2, 2, 1, 1, 4, 4, false, true, true},
    {"led",              TR_NOOP("LED Light"),         "lightbulb_outline",TR_NOOP("Turn the printer's lights on or off"),        "led_controllable",   "No LED strips detected",              CAT_CONTROLS, true,  2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, true, true, true},
    {"led_controls",     TR_NOOP("LED Controls"),      "led_strip",        TR_NOOP("Open color and brightness controls"),     "led_has_devices",    "No LED strips detected",              CAT_CONTROLS, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"fan_stack",        TR_NOOP("Fan Speeds"),        "fan",              TR_NOOP("Part, hotend, and auxiliary fan speeds"),       nullptr,              nullptr,                               CAT_TEMP,     true,  2, 2, 2, 2, 6, 4, true, true, true},
    {"fan",              TR_NOOP("Fan"),               "fan",              TR_NOOP("The speed of one fan you choose"),              nullptr,              nullptr,                               CAT_TEMP,     false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, true, true, true},
    {"temperature",      TR_NOOP("Nozzle Temperature"),"thermometer",      TR_NOOP("Set and watch the nozzle temperature"), nullptr,            nullptr,                               CAT_TEMP,     true,  2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"nozzle_temps",     TR_NOOP("Nozzle Temperatures"),"thermometer",      TR_NOOP("Every extruder's temperature at once"), nullptr,           nullptr,                               CAT_TEMP,     false, 2, 4, 2, 2, 4, 6, false, true, true, false},
    {"bed_temperature",  TR_NOOP("Bed Temperature"),   "radiator",         TR_NOOP("Set and watch the bed temperature"),    nullptr,            nullptr,                               CAT_TEMP,     true, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"chamber_temperature", TR_NOOP("Chamber Temperature"), "fridge_industrial", TR_NOOP("Set and watch the chamber temperature"), "printer_has_chamber", "No chamber temperature sensor detected", CAT_TEMP,     false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"temp_stack",       TR_NOOP("Temperatures"),      "thermometer",      TR_NOOP("Nozzle, bed, and chamber in one widget"),     nullptr,              nullptr,                               CAT_TEMP,     false, 2, 2, 2, 2, 6, 4, false, true, true},
    {"thermistor",       TR_NOOP("Temperature Sensors"), "thermometer",    TR_NOOP("Readings from extra temperature sensors"), "temp_sensor_count", "No temperature sensors detected", CAT_TEMP,     false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, true, true, true},
    {"temp_graph",       TR_NOOP("Temperature Graph"), "chart_line",       TR_NOOP("Temperatures plotted over time"), nullptr,         nullptr,                               CAT_TEMP,     false, 4, 4, 2, 2, BAND_COLSPAN, 8, true, true, true, false},
    {"preheat",          TR_NOOP("Preheat"),           "heat_wave",        TR_NOOP("Warm up for a material in one tap"),            nullptr,            nullptr,                               CAT_TEMP,     false, 6, 2, 4, 2, 8, 2, false, true, false},
    {"ams",              TR_NOOP("Multi-Filament System Status"),        "filament",         TR_NOOP("Slot colors, materials, and levels"),       "ams_slot_count",     "Requires Multi-Filament System or MMU hardware",        CAT_FILAMENT, false, 2, 2, 2, 2, 8, 4, false, true, true},
    {"bypass",           TR_NOOP("Bypass"),           "source_branch",    TR_NOOP("Toggle external spool bypass"), "ams_supports_bypass", "Requires a filament system with bypass", CAT_FILAMENT, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"active_spool",     TR_NOOP("Active Spool"),      "inventory",  TR_NOOP("The Spoolman spool currently loaded"),     nullptr,                  nullptr,                           CAT_FILAMENT, false, 2, 2, 1, 1, 8, 4, false, true, true},
    {"filament",         TR_NOOP("Filament Sensor"),   "filament_alert",   TR_NOOP("Whether filament is loaded"),  "filament_sensor_count", "No filament sensor detected",      CAT_FILAMENT, true, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"humidity",         TR_NOOP("Humidity"),          "water",            TR_NOOP("Humidity inside the enclosure"),         "humidity_sensor_count", "No humidity sensor detected",       CAT_FILAMENT, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"width_sensor",     TR_NOOP("Width Sensor"),      "ruler",            TR_NOOP("Measured filament diameter"),     "width_sensor_count", "No width sensor detected",            CAT_FILAMENT, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"favorite_macro", TR_NOOP("Macro Button"),    "play",             TR_NOOP("Run a macro you choose, in one tap"),     nullptr,              nullptr,                               CAT_CONTROLS, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, true, true, true},
    {"macros",           TR_NOOP("Macros"),            "script_text",      TR_NOOP("Browse and run your Klipper macros"),           nullptr,              nullptr,                               CAT_CONTROLS, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"motion",           TR_NOOP("Motion"),            "cursor_move",      TR_NOOP("Open motion controls for jogging"),           nullptr,              nullptr,                               CAT_CONTROLS, false, 2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
    {"clock",            TR_NOOP("Digital Clock"),     "clock",            TR_NOOP("The time, date, and system uptime"),    nullptr,              nullptr,                               CAT_SYSTEM,   false, 4, 2, 2, 2, 6, 6, false, true, true},
    {"control_buttons",  TR_NOOP("Print Controls"),    "pause",            TR_NOOP("Pause, resume, or stop the print"),   nullptr,              nullptr,                               CAT_PRINT,    false, 4, 2, 2, 2, 8, 4},
    {"job_queue",        TR_NOOP("Job Queue"),         "progress_clock",   TR_NOOP("Print jobs waiting to run"),        nullptr,              nullptr,                               CAT_PRINT,    false, 4, 4, 4, 4, 8, 6, false, true, true},
    //                                                                                                                                          hint                                cat              en  col row min_c min_r max_c max_r  multi  half_c half_r merges_card
    {"tips",             TR_NOOP("Tips"),              "help_circle",      TR_NOOP("Rotating tips for using your printer"),             nullptr,              nullptr,                               CAT_SYSTEM,   true,  8, 4, 4, 2, BAND_COLSPAN, 4, false, true, true, false},
    // Two cells wide by one tall: the FlowGuard scale is horizontal, and it
    // carries a label at each end (#1017). One cell across had the arc, its
    // value and its mode text stacked in a box narrower than the words —
    // reported as showing "nothing useful". The minimum matches the default so
    // a drag cannot put it back there; height still scales down to one cell.
    {"clog_detection",   TR_NOOP("Clog Detection"),    "gauge",            TR_NOOP("Clog and flow health while printing"),   "clog_meter_mode",    "Requires clog detection hardware",    CAT_FILAMENT, false, 4, 2, 4, 2, 8, 4, false, true, true},
    {"filament_buffer",  TR_NOOP("Filament Buffer"),   "arrow_up_down",    TR_NOOP("Filament buffer position against its target"), "buffer_present", "Requires a filament pressure sensor or sync feedback", CAT_FILAMENT, false, 2, 2, 2, 2, 4, 2, false, false, false},
    {"print_stats",      TR_NOOP("Print Stats"),       "printer_3d",       TR_NOOP("Total prints, success rate, and time"),      nullptr,              nullptr,                               CAT_PRINT,    false, 4, 4, 4, 2, 6, 4, false, true, true},
    // Two cells square by default: at two cells tall the tile shows a live tail
    // of the console output, and below that it is the icon tile.
    {"gcode_console",    TR_NOOP("GCode Console"),     "console",          TR_NOOP("Send G-code and read the replies"),    nullptr,              nullptr,                               CAT_CONTROLS, false, 4, 4, 1, 2, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
#if HELIX_HAS_CAMERA
    {"camera",           TR_NOOP("Camera"),            "webcam",           TR_NOOP("Live view from your webcam"),           nullptr,              nullptr,                               CAT_PRINT,    false, 4, 4, 2, 2, 8, 6, false, true, true, false},
#endif
    {"notifications",    TR_NOOP("Notifications"),     "notifications",    TR_NOOP("Alerts and messages waiting for you"),    nullptr,              nullptr,                               CAT_SYSTEM,   true,  2, 2, 1, 1, BAND_COLSPAN, BAND_COLSPAN, false, true, true},
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
    register_gcode_console_widget();
    register_macros_widget();
    register_motion_widget();
    register_filament_sensor_widget();
    register_preheat_widget();
    register_active_spool_widget();
    register_bypass_widget();
    register_tool_switcher_widget();
    register_nozzle_temps_widget();
#if HELIX_HAS_CAMERA
    register_camera_widget();
#endif

    spdlog::debug("[PanelWidgetRegistry] All widget factories registered");

    // Last, so "already claimed" is the truth: tiles no widget class took
    // get a sizing-only instance.
    register_tile_widgets();
}

} // namespace helix
