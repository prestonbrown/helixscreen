// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Characterization of every table-backed persisted setting, driven through the
// managers' public typed getters and setters: the default when the key is
// absent, the clamp on load and on set, the JSON type and path written, the
// XML subject name, the round trip through deinit + init, and which settings
// report a change to telemetry.

#include "../../include/settings_manager.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/telemetry_manager_test_access.h"
#include "audio_settings_manager.h"
#include "bed_drying.h"
#include "completion_alert_mode.h"
#include "config.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "input_settings_manager.h"
#include "persisted_setting.h"
#include "remote_screen_settings_manager.h"
#include "safety_settings_manager.h"
#include "system/telemetry_manager.h"
#include "system_settings_manager.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

enum class Mgr { Settings, Display, System, Input, Audio, Safety, RemoteScreen };

struct Row {
    const char* xml;
    Mgr mgr;
    bool per_printer; // path is relative to Config::df()
    const char* path;
    bool is_bool;
    int def, min, max;
    const char* telemetry; // nullptr: the setter reports nothing
    bool setter_clamps;    // false: the setter takes an enum and stores it as given
    std::function<void(int)> set;
    std::function<int()> get;
    std::function<lv_subject_t*()> subject; // the manager's named accessor, if any
};

SettingsManager& sm() {
    return SettingsManager::instance();
}
DisplaySettingsManager& dm() {
    return DisplaySettingsManager::instance();
}
SystemSettingsManager& sys() {
    return SystemSettingsManager::instance();
}
InputSettingsManager& im() {
    return InputSettingsManager::instance();
}
AudioSettingsManager& am() {
    return AudioSettingsManager::instance();
}
SafetySettingsManager& safety() {
    return SafetySettingsManager::instance();
}
RemoteScreenSettingsManager& rs() {
    return RemoteScreenSettingsManager::instance();
}

#define SETB(expr) [](int v) { expr(v != 0); }
#define SETI(expr) [](int v) { expr(v); }
#define GET(expr) []() -> int { return static_cast<int>(expr()); }
#define SUBJ(expr) []() -> lv_subject_t* { return expr(); }
#define NOSUBJ nullptr

const std::vector<Row>& rows() {
    static const std::vector<Row> r = {
        // SafetySettingsManager
        {"settings_estop_confirm", Mgr::Safety, false, "/safety/estop_require_confirmation", true,
         1, 0, 1, "estop_confirm", true, SETB(safety().set_estop_require_confirmation),
         GET(safety().get_estop_require_confirmation),
         SUBJ(safety().subject_estop_require_confirmation)},
        {"settings_cancel_escalation_enabled", Mgr::Safety, false,
         "/safety/cancel_escalation_enabled", true, 0, 0, 1, "cancel_escalation_enabled", true,
         SETB(safety().set_cancel_escalation_enabled), GET(safety().get_cancel_escalation_enabled),
         SUBJ(safety().subject_cancel_escalation_enabled)},
        {"settings_macro_confirm", Mgr::Safety, false, "/safety/macro_require_confirmation", true,
         1, 0, 1, "macro_confirm", true, SETB(safety().set_macro_require_confirmation),
         GET(safety().get_macro_require_confirmation),
         SUBJ(safety().subject_macro_require_confirmation)},
        {"settings_allow_cold_extrude", Mgr::Safety, false, "/safety/allow_cold_extrude", true, 0,
         0, 1, "allow_cold_extrude", true, SETB(safety().set_allow_cold_extrude),
         GET(safety().get_allow_cold_extrude), SUBJ(safety().subject_allow_cold_extrude)},
        {"settings_tool_change_confirm", Mgr::Safety, false,
         "/safety/tool_change_require_confirmation", true, 1, 0, 1, "tool_change_confirm", true,
         SETB(safety().set_tool_change_require_confirmation),
         GET(safety().get_tool_change_require_confirmation),
         SUBJ(safety().subject_tool_change_require_confirmation)},

        // RemoteScreenSettingsManager
        {"settings_remote_screen_enabled", Mgr::RemoteScreen, true, "remote_screen/enabled", true,
         0, 0, 1, "remote_screen_enabled", true, SETB(rs().set_enabled), GET(rs().get_enabled),
         SUBJ(rs().subject_enabled)},
        // Control only holds while sharing is on, so its setter turns sharing on first.
        {"settings_remote_screen_control", Mgr::RemoteScreen, true, "remote_screen/control", true,
         0, 0, 1, "remote_screen_control", true,
         [](int v) {
             if (!rs().get_enabled()) {
                 rs().set_enabled(true);
             }
             rs().set_control(v != 0);
         },
         GET(rs().get_control), SUBJ(rs().subject_control)},
        {"settings_remote_screen_webcam", Mgr::RemoteScreen, true, "remote_screen/webcam", true, 1,
         0, 1, "remote_screen_webcam", true, SETB(rs().set_webcam), GET(rs().get_webcam),
         SUBJ(rs().subject_webcam)},

        // InputSettingsManager
        {"settings_scroll_throw", Mgr::Input, false, "/input/scroll_throw", false, 25, 5, 50,
         "scroll_throw", true, SETI(im().set_scroll_throw), GET(im().get_scroll_throw),
         SUBJ(im().subject_scroll_throw)},
        {"settings_scroll_limit", Mgr::Input, false, "/input/scroll_limit", false, 10, 1, 20,
         "scroll_limit", true, SETI(im().set_scroll_limit), GET(im().get_scroll_limit),
         SUBJ(im().subject_scroll_limit)},
        {"settings_long_press_time", Mgr::Input, false, "/input/long_press_time", false, 500, 300,
         1500, "long_press_time", true, SETI(im().set_long_press_time),
         GET(im().get_long_press_time), SUBJ(im().subject_long_press_time)},
        {"settings_scroll_guard", Mgr::Input, false, "/input/scroll_guard", true, 0, 0, 1,
         "scroll_guard", true, SETB(im().set_scroll_guard), GET(im().get_scroll_guard),
         SUBJ(im().subject_scroll_guard)},
        {"settings_debug_touches", Mgr::Input, false, "/input/debug_touches", true, 0, 0, 1,
         "debug_touches", true, SETB(im().set_debug_touches), GET(im().get_debug_touches),
         SUBJ(im().subject_debug_touches)},
        {"settings_home_edit_mode_enabled", Mgr::Input, false, "/input/home_edit_mode_enabled",
         true, 1, 0, 1, "home_edit_mode_enabled", true, SETB(im().set_home_edit_mode_enabled),
         GET(im().get_home_edit_mode_enabled), SUBJ(im().subject_home_edit_mode_enabled)},
        {"settings_keypad_layout", Mgr::Input, false, "/input/keypad_layout", false, 0, 0, 1,
         "keypad_layout", true, [](int v) { im().set_keypad_layout(static_cast<KeypadLayout>(v)); },
         GET(im().get_keypad_layout), SUBJ(im().subject_keypad_layout)},

        // AudioSettingsManager
        {"settings_sounds_enabled", Mgr::Audio, false, "/sounds_enabled", true, 0, 0, 1,
         "sounds_enabled", true, SETB(am().set_sounds_enabled), GET(am().get_sounds_enabled),
         SUBJ(am().subject_sounds_enabled)},
        {"settings_ui_sounds_enabled", Mgr::Audio, false, "/ui_sounds_enabled", true, 1, 0, 1,
         "ui_sounds_enabled", true, SETB(am().set_ui_sounds_enabled),
         GET(am().get_ui_sounds_enabled), SUBJ(am().subject_ui_sounds_enabled)},
        {"settings_volume", Mgr::Audio, false, "/sounds/volume", false, 80, 0, 100, "volume", true,
         SETI(am().set_volume), GET(am().get_volume), SUBJ(am().subject_volume)},
        {"settings_completion_alert", Mgr::Audio, false, "/completion_alert", false, 2, 0, 2,
         "completion_alert", false,
         [](int v) { am().set_completion_alert_mode(static_cast<CompletionAlertMode>(v)); },
         GET(am().get_completion_alert_mode), SUBJ(am().subject_completion_alert)},

        // DisplaySettingsManager
        {"settings_sleep_while_printing", Mgr::Display, false, "/display/sleep_while_printing",
         true, 1, 0, 1, "sleep_while_printing", true, SETB(dm().set_sleep_while_printing),
         GET(dm().get_sleep_while_printing), SUBJ(dm().subject_sleep_while_printing)},
        {"settings_brightness", Mgr::Display, false, "/brightness", false, 80, 10, 100,
         "brightness", true, SETI(dm().set_brightness), GET(dm().get_brightness),
         SUBJ(dm().subject_brightness)},
        {"settings_use_system_keyboard", Mgr::Display, false, "/display/use_system_keyboard", true,
         0, 0, 1, "use_system_keyboard", true, SETB(dm().set_use_system_keyboard),
         GET(dm().get_use_system_keyboard), SUBJ(dm().subject_use_system_keyboard)},
        {"settings_hide_keyboard_with_hardware", Mgr::Display, false,
         "/display/hide_keyboard_with_hardware", true, 0, 0, 1, "hide_keyboard_with_hardware", true,
         SETB(dm().set_hide_keyboard_with_hardware), GET(dm().get_hide_keyboard_with_hardware),
         SUBJ(dm().subject_hide_keyboard_with_hardware)},
        {"settings_page_scroll_buttons", Mgr::Display, false, "/display/page_scroll_buttons", true,
         0, 0, 1, "page_scroll_buttons", true, SETB(dm().set_page_scroll_buttons),
         GET(dm().get_page_scroll_buttons), SUBJ(dm().subject_page_scroll_buttons)},
        {"settings_speed_flow_physical_units", Mgr::Display, false,
         "/display/speed_flow_physical_units", true, 0, 0, 1, "speed_flow_physical_units", true,
         SETB(dm().set_speed_flow_physical_units), GET(dm().get_speed_flow_physical_units),
         SUBJ(dm().subject_speed_flow_physical_units)},
        {"settings_print_status_show_camera", Mgr::Display, false,
         "/display/print_status_show_camera", true, 0, 0, 1, "print_status_show_camera", true,
         SETB(dm().set_print_status_show_camera), GET(dm().get_print_status_show_camera),
         SUBJ(dm().subject_print_status_show_camera)},
        {"settings_keep_navbar_visible", Mgr::Display, false, "/display/keep_navbar_visible", true,
         0, 0, 1, "keep_navbar_visible", true, SETB(dm().set_keep_navbar_visible),
         GET(dm().get_keep_navbar_visible), SUBJ(dm().subject_keep_navbar_visible)},
        {"settings_bed_mesh_render_mode", Mgr::Display, false, "/display/bed_mesh_render_mode",
         false, 0, 0, 2, "bed_mesh_render_mode", true, SETI(dm().set_bed_mesh_render_mode),
         GET(dm().get_bed_mesh_render_mode), SUBJ(dm().subject_bed_mesh_render_mode)},
        {"settings_gcode_render_mode", Mgr::Display, false, "/display/gcode_render_mode", false, 0,
         0, 3, "gcode_render_mode", true, SETI(dm().set_gcode_render_mode),
         GET(dm().get_gcode_render_mode), SUBJ(dm().subject_gcode_render_mode)},
        {"settings_time_format", Mgr::Display, false, "/display/time_format", false, 0, 0, 1,
         "time_format", false, [](int v) { dm().set_time_format(static_cast<TimeFormat>(v)); },
         GET(dm().get_time_format), SUBJ(dm().subject_time_format)},
        {"settings_glass_effects", Mgr::Display, false, "/display/glass_effects", true, 0, 0, 1,
         "glass_effects", true, SETB(dm().set_glass_effects), GET(dm().get_glass_effects),
         SUBJ(dm().subject_glass_effects)},

        // SystemSettingsManager
        {"update_channel", Mgr::System, false, "/update/channel", false, 0, 0, 2, "update_channel",
         true, SETI(sys().set_update_channel), GET(sys().get_update_channel),
         SUBJ(sys().subject_update_channel)},
        {"settings_telemetry_enabled", Mgr::System, false, "/telemetry_enabled", true, 0, 0, 1,
         nullptr, true, SETB(sys().set_telemetry_enabled), GET(sys().get_telemetry_enabled),
         SUBJ(sys().subject_telemetry_enabled)},
        {"settings_wifi_enabled", Mgr::System, false, "/wifi_enabled", true, 1, 0, 1,
         "wifi_enabled", true, SETB(sys().set_wifi_enabled), GET(sys().get_wifi_enabled),
         SUBJ(sys().subject_wifi_enabled)},

        // SettingsManager
        {"settings_z_movement_style", Mgr::Settings, true, "z_movement_style", false, 0, 0, 2,
         "z_movement_style", true,
         [](int v) { sm().set_z_movement_style(static_cast<ZMovementStyle>(v)); },
         GET(sm().get_z_movement_style), SUBJ(sm().subject_z_movement_style)},
        {"settings_enclosure_style", Mgr::Settings, true, "enclosure_style", false, 0, 0, 2,
         "enclosure_style", true,
         [](int v) { sm().set_enclosure_style(static_cast<bed_drying::EnclosureStyle>(v)); },
         GET(sm().get_enclosure_style), SUBJ(sm().subject_enclosure_style)},
        {"settings_extrude_speed", Mgr::Settings, true, "filament/extrude_speed", false, 5, 1, 50,
         "extrude_speed", true, SETI(sm().set_extrude_speed), GET(sm().get_extrude_speed),
         SUBJ(sm().subject_extrude_speed)},
        {"settings_jog_speed_xy", Mgr::Settings, true, "motion/jog_speed_xy", false, 6000, 60,
         60000, "jog_speed_xy", true, SETI(sm().set_jog_speed_xy), GET(sm().get_jog_speed_xy),
         NOSUBJ},
        {"settings_jog_speed_z", Mgr::Settings, true, "motion/jog_speed_z", false, 600, 60, 60000,
         "jog_speed_z", true, SETI(sm().set_jog_speed_z), GET(sm().get_jog_speed_z), NOSUBJ},
        {"settings_motion_show_actual_position", Mgr::Settings, true, "motion/show_actual_position",
         true, 0, 0, 1, "show_actual_position", true, SETB(sm().set_motion_show_actual_position),
         GET(sm().get_motion_show_actual_position), SUBJ(sm().subject_motion_show_actual_position)},
        {"settings_bed_map_clearance", Mgr::Settings, true, "motion/bed_map_clearance", false, 5,
         kBedMapClearanceMinMm, kBedMapClearanceMaxMm, "bed_map_clearance", true,
         SETI(sm().set_bed_map_clearance_mm), GET(sm().get_bed_map_clearance_mm), NOSUBJ},
        {"settings_motion_layout", Mgr::Settings, true, "motion/layout", false, 0, 0,
         kMotionLayoutCount - 1, "motion_layout", true,
         [](int v) { sm().set_motion_layout(static_cast<MotionLayout>(v)); },
         GET(sm().get_motion_layout), SUBJ(sm().subject_motion_layout)},
        {"settings_ams_eject_distance", Mgr::Settings, true, "ams/eject_distance", false, 0, 0,
         2000, "qidi_eject_distance", true, SETI(sm().set_ams_eject_distance),
         GET(sm().get_ams_eject_distance), SUBJ(sm().subject_ams_eject_distance)},
        {"settings_ams_eject_velocity", Mgr::Settings, true, "ams/eject_velocity", false, 0, 0, 300,
         "qidi_eject_velocity", true, SETI(sm().set_ams_eject_velocity),
         GET(sm().get_ams_eject_velocity), SUBJ(sm().subject_ams_eject_velocity)},
        {"settings_toolhead_style", Mgr::Settings, true, "appearance/toolhead_style", false, 0, 0,
         7, "toolhead_style", true,
         [](int v) { sm().set_toolhead_style(static_cast<ToolheadStyle>(v)); },
         GET(sm().get_toolhead_style), SUBJ(sm().subject_toolhead_style)},
        {"show_printer_switcher", Mgr::Settings, false, "/printers/show_printer_switcher", true, 0,
         0, 1, "show_printer_switcher", true, SETB(sm().set_show_printer_switcher),
         GET(sm().get_show_printer_switcher), SUBJ(sm().subject_show_printer_switcher)},
        {"show_widget_labels", Mgr::Settings, false, "/appearance/show_widget_labels", true, 0, 0,
         1, "show_widget_labels", true, SETB(sm().set_show_widget_labels),
         GET(sm().get_show_widget_labels), SUBJ(sm().subject_show_widget_labels)},
        {"auto_color_map", Mgr::Settings, true, "filament/auto_color_map", true, 0, 0, 1,
         "auto_color_map", true, SETB(sm().set_auto_color_map), GET(sm().get_auto_color_map),
         SUBJ(sm().subject_auto_color_map)},
        {"unload_after_print", Mgr::Settings, true, "ams/unload_after_print", true, 0, 0, 1,
         "afc_unload_after_print", true, SETB(sm().set_unload_after_print),
         GET(sm().get_unload_after_print), SUBJ(sm().subject_unload_after_print)},
        {"end_of_print_unload_tools", Mgr::Settings, true, "ams/end_of_print_unload_tools", false,
         0, 0, 255, "end_of_print_unload_tools", true, SETI(sm().set_end_of_print_unload_tools),
         GET(sm().get_end_of_print_unload_tools), SUBJ(sm().subject_end_of_print_unload_tools)},
        {"ams_always_show_bypass_spool", Mgr::Settings, true, "ams/always_show_bypass_spool", true,
         0, 0, 1, "ams_always_show_bypass_spool", true, SETB(sm().set_ams_always_show_bypass_spool),
         GET(sm().get_ams_always_show_bypass_spool),
         SUBJ(sm().subject_ams_always_show_bypass_spool)},
        {"ams_keep_spool_info_on_eject", Mgr::Settings, true, "ams/keep_spool_info_on_eject", true,
         1, 0, 1, "ams_keep_spool_info_on_eject", true, SETB(sm().set_ams_keep_spool_info_on_eject),
         GET(sm().get_ams_keep_spool_info_on_eject),
         SUBJ(sm().subject_ams_keep_spool_info_on_eject)},
        {"ams_tag_links_spoolman", Mgr::Settings, true, "ams/tag_links_spoolman", true, 1, 0, 1,
         "ams_tag_links_spoolman", true, SETB(sm().set_ams_tag_links_spoolman),
         GET(sm().get_ams_tag_links_spoolman), SUBJ(sm().subject_ams_tag_links_spoolman)},
        {"ams_force_bypass_controls", Mgr::Settings, true, "ams/force_bypass_controls", true, 0, 0,
         1, "ams_force_bypass_controls", true, SETB(sm().set_ams_force_bypass_controls),
         GET(sm().get_ams_force_bypass_controls), SUBJ(sm().subject_ams_force_bypass_controls)},
        {"filament_auto_cooldown", Mgr::Settings, true, "filament/auto_cooldown", true, 1, 0, 1,
         "filament_auto_cooldown", true, SETB(sm().set_filament_auto_cooldown),
         GET(sm().get_filament_auto_cooldown), SUBJ(sm().subject_filament_auto_cooldown)},
        {"filament_auto_open_editor", Mgr::Settings, true, "filament/auto_open_editor", true, 0, 0,
         1, "filament_auto_open_editor", true, SETB(sm().set_filament_auto_open_editor),
         GET(sm().get_filament_auto_open_editor), SUBJ(sm().subject_filament_auto_open_editor)},
        {"console_filter_temps", Mgr::Settings, false, "/console/filter_temps", true, 1, 0, 1,
         "console_filter_temps", true, SETB(sm().set_console_filter_temps),
         GET(sm().get_console_filter_temps), SUBJ(sm().subject_console_filter_temps)},
        {"console_filter_firmware_noise", Mgr::Settings, false, "/console/filter_firmware_noise",
         true, 1, 0, 1, "console_filter_firmware_noise", true,
         SETB(sm().set_console_filter_firmware_noise), GET(sm().get_console_filter_firmware_noise),
         SUBJ(sm().subject_console_filter_firmware_noise)},
        {"detection_enabled", Mgr::Settings, false, "/detection/enabled", true, 1, 0, 1,
         "detection_enabled", true, SETB(sm().set_detection_enabled),
         GET(sm().get_detection_enabled), SUBJ(sm().subject_detection_enabled)},
        {"detection_pause_on_detect", Mgr::Settings, false, "/detection/pause_on_detect", true, 1,
         0, 1, "detection_pause_on_detect", true, SETB(sm().set_detection_pause_on_detect),
         GET(sm().get_detection_pause_on_detect), SUBJ(sm().subject_detection_pause_on_detect)},
        {"detection_policy_u1", Mgr::Settings, true, "detection/policy_u1", false, 2, 0, 2,
         "detection_policy_u1", true, SETI(sm().set_detection_policy_u1),
         GET(sm().get_detection_policy_u1), NOSUBJ},
        {"settings_z_offset_save_mode", Mgr::Settings, true, "z_offset/save_mode", false, 0, 0, 2,
         "z_offset_save_mode", true, SETI(sm().set_z_offset_save_mode),
         GET(sm().get_z_offset_save_mode), SUBJ(sm().subject_z_offset_save_mode)},
        {"settings_belt_layout", Mgr::Settings, true, "belt_tension/layout", false, 0, 0, 4,
         "belt_layout", true, SETI(sm().set_belt_layout), GET(sm().get_belt_layout),
         SUBJ(sm().subject_belt_layout)},
    };
    return r;
}

#undef SETB
#undef SETI
#undef GET
#undef SUBJ
#undef NOSUBJ

std::string full_path(const Row& row) {
    return row.per_printer ? Config::get_instance()->df() + row.path : std::string(row.path);
}

void reinit(Mgr m) {
    switch (m) {
    case Mgr::Settings:
        sm().deinit_subjects();
        sm().init_subjects();
        break;
    case Mgr::Display:
        dm().deinit_subjects();
        dm().init_subjects();
        break;
    case Mgr::System:
        sys().deinit_subjects();
        sys().init_subjects();
        break;
    case Mgr::Input:
        im().deinit_subjects();
        im().init_subjects();
        break;
    case Mgr::Audio:
        am().deinit_subjects();
        am().init_subjects();
        break;
    case Mgr::Safety:
        safety().deinit_subjects();
        safety().init_subjects();
        break;
    case Mgr::RemoteScreen:
        rs().deinit_subjects();
        rs().init_subjects();
        break;
    }
}

void erase_path(const std::string& ptr) {
    auto& data = ConfigTestAccess::data(*Config::get_instance());
    const auto slash = ptr.rfind('/');
    const std::string parent = ptr.substr(0, slash);
    const std::string leaf = ptr.substr(slash + 1);
    if (parent.empty()) {
        data.erase(leaf);
        return;
    }
    const json::json_pointer jp(parent);
    if (data.contains(jp) && data[jp].is_object()) {
        data[jp].erase(leaf);
    }
}

const json* stored(const Row& row) {
    return Config::get_instance()->try_get_json(full_path(row));
}

/// A value inside the range that differs from the default.
int other_value(const Row& row) {
    return row.def != row.max ? row.max : row.min;
}

int subject_value(const Row& row) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, row.xml);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

void require_stored(const Row& row, int expected) {
    const json* node = stored(row);
    REQUIRE(node != nullptr);
    if (row.is_bool) {
        REQUIRE(node->is_boolean());
        REQUIRE(node->get<bool>() == (expected != 0));
    } else {
        REQUIRE(node->is_number_integer());
        REQUIRE(node->get<int>() == expected);
    }
}

void init_all() {
    sm().init_subjects();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Persisted settings: default when the key is absent",
                 "[persisted_setting]") {
    init_all();
    for (const auto& row : rows()) {
        INFO(row.xml);
        erase_path(full_path(row));
        reinit(row.mgr);
        CHECK(row.get() == row.def);
        CHECK(subject_value(row) == row.def);
        if (row.subject) {
            CHECK(row.subject() == lv_xml_get_subject(nullptr, row.xml));
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Persisted settings: set updates subject and config, reload keeps it",
                 "[persisted_setting]") {
    init_all();
    for (const auto& row : rows()) {
        INFO(row.xml);
        for (int v : {other_value(row), row.def}) {
            row.set(v);
            CHECK(row.get() == v);
            CHECK(subject_value(row) == v);
            require_stored(row, v);

            reinit(row.mgr);
            CHECK(row.get() == v);
            CHECK(subject_value(row) == v);
        }
    }
    // Leave telemetry off for whatever runs next in this process.
    sys().set_telemetry_enabled(false);
}

TEST_CASE_METHOD(LVGLTestFixture, "Persisted settings: out-of-range values clamp",
                 "[persisted_setting]") {
    init_all();
    for (const auto& row : rows()) {
        if (row.is_bool) {
            continue;
        }
        INFO(row.xml);
        auto* config = Config::get_instance();

        // On load.
        config->set<int>(full_path(row), row.min - 1);
        reinit(row.mgr);
        CHECK(row.get() == row.min);
        config->set<int>(full_path(row), row.max + 1);
        reinit(row.mgr);
        CHECK(row.get() == row.max);

        // On set.
        if (row.setter_clamps) {
            row.set(row.min - 1);
            CHECK(subject_value(row) == row.min);
            require_stored(row, row.min);
            row.set(row.max + 1);
            CHECK(subject_value(row) == row.max);
            require_stored(row, row.max);
        }
        row.set(row.def);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Persisted settings: exactly the telemetry-keyed rows report",
                 "[persisted_setting]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "helix_persisted_setting_telemetry";
    fs::create_directories(dir);

    auto& tm = TelemetryManager::instance();
    tm.shutdown();
    helix::ui::UpdateQueue::instance().drain();
    tm.init(dir.string());
    tm.set_enabled(true);

    init_all();
    for (const auto& row : rows()) {
        if (std::string(row.xml) == "settings_telemetry_enabled") {
            continue; // its setter switches the collector this test reads
        }
        INFO(row.xml);
        const int a = row.min;
        const int b = row.max;
        row.set(a);
        TelemetryManagerTestAccess::clear_pending_setting_changes(tm);
        row.set(b);
        const auto pending = TelemetryManagerTestAccess::pending_setting_changes(tm);
        if (row.telemetry) {
            REQUIRE(pending.size() == 1);
            CHECK(pending[0] ==
                  std::string(row.telemetry) + "|" + std::to_string(a) + "|" + std::to_string(b));
        } else {
            CHECK(pending.empty());
        }
        row.set(row.def);
    }

    TelemetryManagerTestAccess::clear_pending_setting_changes(tm);
    helix::ui::UpdateQueue::instance().drain();
    tm.set_enabled(false);
    tm.shutdown();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace {
/// Every manager's real static table, in the order the managers are listed.
std::vector<settings::TableView> real_tables() {
    return {SettingsManager::persisted_table(),
            DisplaySettingsManager::persisted_table(),
            SystemSettingsManager::persisted_table(),
            InputSettingsManager::persisted_table(),
            AudioSettingsManager::persisted_table(),
            SafetySettingsManager::persisted_table(),
            RemoteScreenSettingsManager::persisted_table()};
}
} // namespace

TEST_CASE("Persisted settings: every real row has a unique telemetry key unless exempt",
          "[persisted_setting][telemetry]") {
    // Settings that are deliberately not reported. Every PersistedSetting is an int or a bool,
    // so none of them can carry free text; a row lands here only when reporting its change
    // is meaningless.
    static const std::set<std::string> exempt = {
        // Its setter switches the collector that would carry the report.
        "settings_telemetry_enabled",
    };

    std::set<std::string> seen;
    size_t total = 0;
    size_t exempted = 0;
    for (const auto& table : real_tables()) {
        for (const auto& row : table) {
            INFO(row.xml_name);
            ++total;
            if (exempt.count(row.xml_name) != 0) {
                CHECK(row.telemetry_key == nullptr);
                ++exempted;
                continue;
            }
            REQUIRE(row.telemetry_key != nullptr);
            const std::string key = row.telemetry_key;
            CHECK_FALSE(key.empty());
            CHECK(std::all_of(key.begin(), key.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            }));
            CHECK(seen.insert(key).second);
        }
    }
    CHECK(exempted == exempt.size());
    CHECK(seen.size() == total - exempt.size());
}

TEST_CASE("Persisted settings: the test mirror matches the real tables",
          "[persisted_setting][telemetry]") {
    size_t total = 0;
    for (const auto& table : real_tables()) {
        for (const auto& real : table) {
            INFO(real.xml_name);
            ++total;
            const auto it = std::find_if(rows().begin(), rows().end(), [&](const Row& r) {
                return std::string(r.xml) == real.xml_name;
            });
            REQUIRE(it != rows().end());
            CHECK(std::string(it->path) == real.json_path);
            CHECK(it->per_printer == (real.scope == settings::Scope::PerPrinter));
            CHECK(it->is_bool == real.is_bool);
            CHECK(it->def == real.def);
            CHECK(it->min == real.min);
            CHECK(it->max == real.max);
            CHECK(std::string(it->telemetry ? it->telemetry : "") ==
                  std::string(real.telemetry_key ? real.telemetry_key : ""));
        }
    }
    CHECK(rows().size() == total);
}

TEST_CASE("PersistedSettings: get before init returns the row default", "[persisted_setting]") {
    enum class K : uint8_t { Count, Flag, COUNT };
    static constexpr settings::PersistedSetting table[] = {
        {"test_ps_count", "/test_ps/count", settings::Scope::Global, false, 42, 0, 100, nullptr},
        {"test_ps_flag", "/test_ps/flag", settings::Scope::Global, true, 1, 0, 1, nullptr},
    };
    settings::PersistedSettings<K, static_cast<size_t>(K::COUNT)> ps(table);
    CHECK(ps.get(K::Count) == 42);
    CHECK(ps.get_bool(K::Flag));
}
