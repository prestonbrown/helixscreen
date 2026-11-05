// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_discovery_real_hardware.cpp
 * @brief PrinterDiscovery object classification, driven by REAL device payloads.
 *
 * The object lists below were captured verbatim from `printer.objects.list` on
 * physical printers. They exist because the `output_pin` classifier is a
 * name-based heuristic (include/printer_discovery.h): a pin is treated as a fan
 * if its name starts with FAN, as an LED if it contains LIGHT/LED/LAMP, and as a
 * speaker if it contains BEEPER/BUZZER/SPEAKER. Everything else is deliberately
 * ignored.
 *
 * That heuristic is load-bearing safety logic, not cosmetics. `output_pin` is a
 * generic Klipper primitive and users wire it to arbitrary hardware. The
 * Snapmaker U1 fixture below is the reason these tests exist: it defines
 * `output_pin e0_heat_sw` .. `e3_heat_sw`, per-extruder hotend heater power
 * switches. If a future change loosens the heuristic so those match, HelixScreen
 * would list heater switches as "lights" and write them via SET_LED / SET_PIN
 * from the LED UI. These tests fail loudly if that ever happens.
 *
 * Synthetic fixtures cannot catch that class of regression, because whoever
 * loosens the heuristic writes the synthetic fixture too. Real captures can.
 *
 * Captured 2026-07-26. Re-capture with:
 *   curl -s http://<printer>:7125/printer/objects/list
 */

#include "printer_discovery.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// True if @p haystack contains exactly @p needle.
bool has(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

/// True if any entry of @p haystack contains @p fragment as a substring.
bool any_contains(const std::vector<std::string>& haystack, const std::string& fragment) {
    return std::any_of(haystack.begin(), haystack.end(),
                       [&](const std::string& s) { return s.find(fragment) != std::string::npos; });
}

} // namespace

// ============================================================================
// Creality K1C — output_pin used for BOTH a light and fans
// ============================================================================

TEST_CASE("PrinterDiscovery: K1C classifies output_pin light vs fans by name",
          "[printer_discovery][real_hardware]") {
    const nlohmann::json objects =
        nlohmann::json::array({"webhooks",
                               "configfile",
                               "mcu",
                               "mcu nozzle_mcu",
                               "mcu leveling_mcu",
                               "mcu rpi",
                               "gcode_macro xyz_ready",
                               "gcode_macro _IF_HOME_Z",
                               "gcode_macro _IF_MOVE_XY",
                               "gcode_macro _HOME_X",
                               "gcode_macro _HOME_Y",
                               "gcode_macro _HOME_Z",
                               "gcode_macro PRINTER_PARAM",
                               "gcode_macro AUTOTUNE_SHAPERS",
                               "gcode_macro LOAD_MATERIAL_CLOSE_FAN2",
                               "gcode_macro LOAD_MATERIAL_RESTORE_FAN2",
                               "gcode_macro SET_E_MIN_CURRENT",
                               "gcode_macro RESTORE_E_CURRENT",
                               "gcode_macro LOAD_MATERIAL",
                               "gcode_macro QUIT_MATERIAL",
                               "gcode_macro Qmode",
                               "gcode_macro Qmode_exit",
                               "gcode_macro M204",
                               "gcode_macro M205",
                               "gcode_macro M106",
                               "gcode_macro M107",
                               "gcode_macro M141",
                               "gcode_macro M900",
                               "gcode_macro WAIT_TEMP_START",
                               "gcode_macro WAIT_TEMP_END",
                               "gcode_macro PRINT_CALIBRATION",
                               "gcode_macro FIRST_FLOOR_PAUSE_POSITION",
                               "gcode_macro ACCURATE_G28",
                               "gcode_macro START_PRINT",
                               "gcode_macro PRINT_PREPARED",
                               "gcode_macro PRINT_PREPARE_CLEAR",
                               "gcode_macro END_PRINT_POINT_WITHOUT_LIFTING",
                               "gcode_macro END_PRINT_POINT",
                               "gcode_macro END_PRINT",
                               "gcode_macro FIRST_FLOOR_PAUSE",
                               "gcode_macro FIRST_FLOOR_RESUME",
                               "gcode_macro PAUSE",
                               "gcode_macro INPUTSHAPER",
                               "gcode_macro BEDPID",
                               "gcode_macro TUNOFFINPUTSHAPER",
                               "gcode_macro RESUME",
                               "gcode_macro CANCEL_PRINT",
                               "gcode_macro G29",
                               "gcode_move",
                               "print_stats",
                               "fan_feedback",
                               "custom_macro",
                               "gcode_macro product_param",
                               "idle_timeout",
                               "virtual_sdcard",
                               "heaters",
                               "temperature_sensor mcu_temp",
                               "temperature_sensor chamber_temp",
                               "temperature_fan chamber_fan",
                               "tmc2209 stepper_x",
                               "tmc2209 stepper_y",
                               "tmc2209 stepper_z",
                               "tmc2209 extruder",
                               "heater_bed",
                               "pause_resume",
                               "filament_switch_sensor filament_sensor",
                               "filament_switch_sensor filament_sensor_2",
                               "heater_fan hotend_fan",
                               "output_pin fan0",
                               "output_pin fan1",
                               "output_pin fan2",
                               "output_pin LED",
                               "probe",
                               "bed_mesh",
                               "display_status",
                               "exclude_object",
                               "motion_report",
                               "query_endstops",
                               "system_stats",
                               "manual_probe",
                               "toolhead",
                               "extruder"});

    PrinterDiscovery hw;
    hw.parse_objects(objects);

    SECTION("the LED output_pin is classified as a light") {
        // [output_pin led] scale=1.0 pwm=true — a genuine chamber light.
        REQUIRE(has(hw.leds(), "output_pin LED"));
        REQUIRE(hw.has_led());
    }

    SECTION("the fan output_pins are classified as fans, not lights") {
        // [output_pin fan0/1/2] scale=255 — part/aux fans on this machine.
        // Same SET_PIN gcode as the light above; only the NAME distinguishes them.
        REQUIRE(has(hw.fans(), "output_pin fan0"));
        REQUIRE(has(hw.fans(), "output_pin fan1"));
        REQUIRE(has(hw.fans(), "output_pin fan2"));
        CHECK_FALSE(has(hw.leds(), "output_pin fan0"));
        CHECK_FALSE(has(hw.leds(), "output_pin fan1"));
        CHECK_FALSE(has(hw.leds(), "output_pin fan2"));
    }
}

// ============================================================================
// Snapmaker U1 — output_pin used for hotend heater switches
// ============================================================================

TEST_CASE("PrinterDiscovery: U1 heater-switch output_pins are never lights or fans",
          "[printer_discovery][real_hardware]") {
    const nlohmann::json objects =
        nlohmann::json::array({"gcode",
                               "webhooks",
                               "exception_manager",
                               "configfile",
                               "mcu",
                               "mcu e0",
                               "mcu e1",
                               "mcu e2",
                               "mcu e3",
                               "gcode_move",
                               "print_stats",
                               "gcode_macro _PL_SAVE_VARIABLE",
                               "virtual_sdcard",
                               "pause_resume",
                               "display_status",
                               "gcode_macro M600",
                               "gcode_macro T4",
                               "gcode_macro T5",
                               "gcode_macro T6",
                               "gcode_macro T7",
                               "gcode_macro T8",
                               "gcode_macro T9",
                               "gcode_macro T10",
                               "gcode_macro T11",
                               "gcode_macro T12",
                               "gcode_macro T13",
                               "gcode_macro T14",
                               "gcode_macro T15",
                               "gcode_macro T16",
                               "gcode_macro T17",
                               "gcode_macro T18",
                               "gcode_macro T19",
                               "gcode_macro T20",
                               "gcode_macro T21",
                               "gcode_macro T22",
                               "gcode_macro T23",
                               "gcode_macro T24",
                               "gcode_macro T25",
                               "gcode_macro T26",
                               "gcode_macro T27",
                               "gcode_macro T28",
                               "gcode_macro T29",
                               "gcode_macro T30",
                               "gcode_macro T31",
                               "gcode_macro MOVE_TO_DISCARD_FILAMENT_POSITION",
                               "gcode_macro SHAPER_CALIBRATE",
                               "gcode_macro PRINT_START",
                               "gcode_macro PRINT_STRAT",
                               "gcode_macro PRINT_END",
                               "gcode_macro CANCEL_PRINT",
                               "gcode_macro INNER_PAUSE",
                               "gcode_macro INNER_RESUME",
                               "gcode_macro SET_PAUSE_NEXT_LAYER",
                               "gcode_macro SET_PAUSE_AT_LAYER",
                               "gcode_macro SET_PRINT_STATS_INFO",
                               "gcode_macro _CLIENT_EXTRUDE",
                               "gcode_macro _CLIENT_RETRACT",
                               "gcode_macro INNER_FILAMENT_UNLOAD",
                               "gcode_macro AUTO_FEEDING",
                               "gcode_macro MANUAL_FEEDING",
                               "gcode_macro FEEDING_RUNOUT_EVENT_HANDLE",
                               "gcode_macro ROUGHLY_CLEAN_NOZZLE",
                               "gcode_macro ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD",
                               "gcode_macro FINELY_CLEAN_NOZZLE_STAGE_1",
                               "gcode_macro FINELY_CLEAN_NOZZLE_STAGE_2",
                               "gcode_macro SM_PRINT_CHECK_SWITCH_EXTRUDER",
                               "gcode_macro SM_PRINT_EXTRUDER_PREHEAT",
                               "gcode_macro SM_PRINT_AUTO_FEED",
                               "gcode_macro SM_PRINT_END_AUTO_UNLOAD_FILAMENT",
                               "gcode_macro SM_PRINT_START_LINE",
                               "gcode_macro SM_PRINT_START_LINE_EXTRUDER_0",
                               "gcode_macro SM_PRINT_START_LINE_EXTRUDER_1",
                               "gcode_macro SM_PRINT_START_LINE_EXTRUDER_2",
                               "gcode_macro SM_PRINT_START_LINE_EXTRUDER_3",
                               "gcode_macro INNER_ROUGHLY_CLEAN_NOZZLE_BASE_DISCARD",
                               "gcode_macro INNER_DISCARD_FILAMENT_BASE_DISCARD",
                               "gcode_macro INNER_CUTOFF_BASE_DISCARD",
                               "gcode_macro INNER_FLUSH_FILAMENT",
                               "gcode_macro INNER_MANUAL_FEED_STAGE_PREPARE",
                               "gcode_macro INNER_MANUAL_FEED_STAGE_EXTRUDE",
                               "gcode_macro INNER_MANUAL_FEED_STAGE_FLUSH",
                               "gcode_macro INNER_MANUAL_FEED_STAGE_FINISH",
                               "gcode_macro INNER_MANUAL_FEED_STAGE_CANCEL",
                               "gcode_macro INNER_FLOW_MEASURE_END_BASE_DISCARD",
                               "gcode_macro INNER_FLOW_CALIB_END_BASE_DISCARD",
                               "gcode_macro INNER_PREEXTRUDE_FILAMENT",
                               "gcode_macro INNER_DETECT_NOZZLE_STAGE_1",
                               "gcode_macro INNER_DETECT_NOZZLE_STAGE_2",
                               "gcode_macro CONTROL_EXTRUDE_ACTION",
                               "gcode_macro CONTROL_RETRACT_ACTION",
                               "exclude_object",
                               "print_task_config",
                               "mqtt",
                               "timelapse",
                               "filament_detect",
                               "resonance_tester",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PREHOMING",
                               "gcode_macro _DETECT_PLATE",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PREHEAT",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_HEAT_T0",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_HEAT_T1",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_HEAT_T2",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_HEAT_T3",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_AUTO_CLEAN_T0",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_AUTO_CLEAN_T1",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_AUTO_CLEAN_T2",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_AUTO_CLEAN_T3",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN_T0",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN_T1",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN_T2",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN_T3",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_WAIT_COOL_T0",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_WAIT_COOL_T1",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_WAIT_COOL_T2",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_WAIT_COOL_T3",
                               "gcode_macro EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_T0",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_T1",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_T2",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_T3",
                               "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT",
                               "gcode_macro XYZ_OFFSET_CALIBRATE",
                               "gcode_macro XYZ_OFFSET_CALIBRATE_ALL",
                               "heaters",
                               "stepper_enable",
                               "tmc2240 stepper_x",
                               "tmc2240 stepper_y",
                               "tmc2209 stepper_z",
                               "heater_bed",
                               "temperature_sensor cavity",
                               "fan_generic cavity_fan",
                               "led cavity_led",
                               "purifier",
                               "bed_mesh",
                               "homing_precise_corexy",
                               "heater_fan power_fan",
                               "adc_current_sensor I_AD",
                               "power_loss_check",
                               "auto_screws_tilt_adjust",
                               "gcode_macro AUTO_BED_MESH_CALIBRATE",
                               "gcode_macro SHAKE_Z",
                               "gcode_macro SENSORLESS_HOME_X",
                               "gcode_macro SENSORLESS_HOME_Y",
                               "gcode_macro SENSORLESS_HOME_Z",
                               "gcode_macro _HOMING_PRECISE_COREXY_ADVANCED",
                               "gcode_macro _CLIENT_VARIABLE",
                               "tmc2209 extruder",
                               "fan",
                               "heater_fan e0_nozzle_fan",
                               "output_pin e0_heat_sw",
                               "filament_motion_sensor e0_filament",
                               "filament_entangle_detect e0_filament",
                               "extruder_offset_calibration",
                               "probe",
                               "power_loss_check e0",
                               "tmc2209 extruder1",
                               "fan_generic e1_fan",
                               "heater_fan e1_nozzle_fan",
                               "output_pin e1_heat_sw",
                               "filament_motion_sensor e1_filament",
                               "filament_entangle_detect e1_filament",
                               "power_loss_check e1",
                               "tmc2209 extruder2",
                               "fan_generic e2_fan",
                               "heater_fan e2_nozzle_fan",
                               "output_pin e2_heat_sw",
                               "filament_motion_sensor e2_filament",
                               "filament_entangle_detect e2_filament",
                               "power_loss_check e2",
                               "tmc2209 extruder3",
                               "fan_generic e3_fan",
                               "heater_fan e3_nozzle_fan",
                               "output_pin e3_heat_sw",
                               "filament_motion_sensor e3_filament",
                               "filament_entangle_detect e3_filament",
                               "power_loss_check e3",
                               "filament_feed left",
                               "filament_feed right",
                               "gcode_macro _FILAMENT_FEED_VARIABLE",
                               "idle_timeout",
                               "filament_parameters",
                               "defect_detection",
                               "gcode_macro PAUSE",
                               "gcode_macro RESUME",
                               "motion_report",
                               "query_endstops",
                               "system_stats",
                               "manual_probe",
                               "machine_state_manager",
                               "toolhead",
                               "extruder",
                               "extruder1",
                               "extruder2",
                               "extruder3"});

    PrinterDiscovery hw;
    hw.parse_objects(objects);

    SECTION("the native cavity LED is discovered") {
        REQUIRE(has(hw.leds(), "led cavity_led"));
        REQUIRE(hw.has_led());
    }

    SECTION("hotend heater switches are NOT lights and NOT fans") {
        // THIS IS THE POINT OF THIS FILE. [output_pin e0_heat_sw] .. [e3_heat_sw]
        // are non-PWM digital pins switching hotend heater power on a 4-extruder
        // toolchanger. They must never surface in the LED or fan UI, because
        // HelixScreen writes those with SET_PIN.
        for (const char* sw : {"output_pin e0_heat_sw", "output_pin e1_heat_sw",
                               "output_pin e2_heat_sw", "output_pin e3_heat_sw"}) {
            CHECK_FALSE(has(hw.leds(), sw));
            CHECK_FALSE(has(hw.fans(), sw));
        }
        // Belt and braces: nothing named "heat_sw" leaked into either list.
        CHECK_FALSE(any_contains(hw.leds(), "heat_sw"));
        CHECK_FALSE(any_contains(hw.fans(), "heat_sw"));
    }
}

// ============================================================================
// Voron V2.4 (Kalico) — neopixels and led_effect, no output_pin
// ============================================================================

TEST_CASE("PrinterDiscovery: Voron neopixels and led_effects discovered",
          "[printer_discovery][real_hardware]") {
    const nlohmann::json objects =
        nlohmann::json::array({"gcode",
                               "webhooks",
                               "configfile",
                               "canbus_stats mcu",
                               "mcu",
                               "canbus_stats Turtle_1",
                               "mcu Turtle_1",
                               "canbus_stats EBBCan",
                               "mcu EBBCan",
                               "mcu btt_pi",
                               "gcode_move",
                               "print_stats",
                               "virtual_sdcard",
                               "pause_resume",
                               "display_status",
                               "gcode_macro CANCEL_PRINT",
                               "gcode_macro PAUSE",
                               "gcode_macro RESUME",
                               "gcode_macro SET_PAUSE_NEXT_LAYER",
                               "gcode_macro SET_PAUSE_AT_LAYER",
                               "gcode_macro SET_PRINT_STATS_INFO",
                               "gcode_macro _TOOLHEAD_PARK_PAUSE_CANCEL",
                               "gcode_macro _CLIENT_EXTRUDE",
                               "gcode_macro _CLIENT_RETRACT",
                               "gcode_macro _CLIENT_LINEAR_MOVE",
                               "gcode_macro AFC_CALIBRATION",
                               "gcode_macro AFC_RESET",
                               "gcode_macro AFC_TEST_LANES",
                               "gcode_macro AFC_GET_TD_ONE_DATA",
                               "gcode_macro AFC_STATS",
                               "gcode_macro AFC_QUIET_MODE",
                               "gcode_macro AFC_STATUS",
                               "gcode_macro TURN_ON_AFC_LED",
                               "gcode_macro TURN_OFF_AFC_LED",
                               "gcode_macro AFC_CHANGE_BLADE",
                               "gcode_macro AFC_TOGGLE_MACRO",
                               "gcode_macro UNSET_LANE_LOADED",
                               "gcode_macro AFC_RESET_STATS",
                               "AFC",
                               "filament_switch_sensor tool_start",
                               "filament_switch_sensor tool_end",
                               "gcode_macro UPDATE_TOOLHEAD_SENSORS",
                               "gcode_macro SAVE_EXTRUDER_VALUES",
                               "AFC_extruder extruder",
                               "filament_switch_sensor Turtle_1_expanded",
                               "filament_switch_sensor Turtle_1_compressed",
                               "gcode_macro QUERY_BUFFER",
                               "AFC_buffer Turtle_1",
                               "gcode_macro BT_TOOL_UNLOAD",
                               "gcode_macro BT_CHANGE_TOOL",
                               "gcode_macro BT_LANE_EJECT",
                               "gcode_macro BT_LANE_MOVE",
                               "gcode_macro BT_RESUME",
                               "gcode_macro BT_PREP",
                               "gcode_macro AFC_DISABLE_SKEW",
                               "gcode_macro AFC_ENABLE_SKEW",
                               "gcode_macro AFC_BRUSH",
                               "gcode_macro _BRUSH_SERVO",
                               "gcode_macro AFC_CUT",
                               "gcode_macro _MOVE_TO_CUTTER_PIN",
                               "gcode_macro _DO_CUT_MOTION",
                               "gcode_macro _CUTTER_SERVO",
                               "gcode_macro _CLEAR_PIN",
                               "gcode_macro AFC_KICK",
                               "gcode_macro AFC_PARK",
                               "gcode_macro AFC_POOP",
                               "gcode_macro _AFC_GLOBAL_VARS",
                               "gcode_macro _AFC_CUT_TIP_VARS",
                               "gcode_macro _AFC_POOP_VARS",
                               "gcode_macro _AFC_KICK_VARS",
                               "gcode_macro _AFC_BRUSH_VARS",
                               "gcode_macro _AFC_PARK_VARS",
                               "AFC_BoxTurtle Turtle_1",
                               "heaters",
                               "temperature_sensor Turtle_1",
                               "gcode_macro AFC_RESET_MOTOR_TIME",
                               "query_endstops",
                               "filament_switch_sensor lane1_prep",
                               "filament_switch_sensor lane1_load",
                               "gcode_macro SET_LANE_LOADED",
                               "gcode_macro AFC_RECOVER_LANE",
                               "stepper_enable",
                               "motion_report",
                               "gcode_macro AFC_STEPPER_HOME",
                               "AFC_stepper lane1",
                               "tmc2209 AFC_stepper lane1",
                               "filament_switch_sensor lane2_prep",
                               "filament_switch_sensor lane2_load",
                               "AFC_stepper lane2",
                               "tmc2209 AFC_stepper lane2",
                               "filament_switch_sensor lane3_prep",
                               "filament_switch_sensor lane3_load",
                               "AFC_stepper lane3",
                               "tmc2209 AFC_stepper lane3",
                               "filament_switch_sensor lane4_prep",
                               "filament_switch_sensor lane4_load",
                               "AFC_stepper lane4",
                               "tmc2209 AFC_stepper lane4",
                               "filament_switch_sensor Turtle_1_Hub",
                               "AFC_hub Turtle_1",
                               "AFC_led AFC_Indicator",
                               "neopixel Turtle_Corner_Indicators",
                               "gcode_macro VORON_PURGE",
                               "gcode_macro SMART_PARK",
                               "gcode_macro _KAMP_Settings",
                               "gcode_macro _BEDFANVARS",
                               "fan_generic bed_fans",
                               "gcode_macro BEDFANSSLOW",
                               "gcode_macro BEDFANSFAST",
                               "gcode_macro BEDFANSOFF",
                               "gcode_macro SET_HEATER_TEMPERATURE",
                               "gcode_macro M190",
                               "gcode_macro M140",
                               "gcode_macro TURN_OFF_HEATERS",
                               "led_effect temperature_warning",
                               "led_effect filament_runout",
                               "led_effect print_progress",
                               "led_effect calm_breathing",
                               "led_effect rainbow_wave",
                               "led_effect work_light",
                               "led_effect error_cascade",
                               "led_effect calibration",
                               "led_effect nightlight",
                               "led_effect dual_chase",
                               "led_effect temp_visualization",
                               "led_effect fireplace_base",
                               "led_effect fireplace_flicker",
                               "led_effect fireplace_flicker2",
                               "led_effect sequential_strobe",
                               "gcode_macro _STATUS_SEQUENTIAL_FADE",
                               "gcode_macro _STATUS_TEMP_WARNING",
                               "gcode_macro _STATUS_FILAMENT_RUNOUT",
                               "gcode_macro _STATUS_PRINT_PROGRESS",
                               "gcode_macro _STATUS_CALM",
                               "gcode_macro _STATUS_RAINBOW_WAVE",
                               "gcode_macro _STATUS_ERROR",
                               "gcode_macro _STATUS_CALIBRATION",
                               "gcode_macro _STATUS_NIGHTLIGHT",
                               "gcode_macro _STATUS_DUAL_CHASE",
                               "gcode_macro _STATUS_TEMP_VISUALIZATION",
                               "gcode_macro _STATUS_FIREPLACE",
                               "gcode_macro DESCRIBE_COLOR",
                               "led_effect case_lights_on",
                               "led_effect bt_corners_on",
                               "led_effect bt_corners_heating",
                               "led_effect bt_corners_printing",
                               "led_effect bt_corners_progress",
                               "led_effect sb_logo_busy",
                               "led_effect sb_logo_cleaning",
                               "led_effect sb_logo_calibrating_z",
                               "led_effect sb_logo_heating",
                               "led_effect sb_logo_cooling",
                               "led_effect sb_logo_homing",
                               "led_effect sb_logo_leveling",
                               "led_effect sb_logo_meshing",
                               "led_effect sb_logo_printing",
                               "led_effect sb_logo_standby",
                               "led_effect sb_logo_part_ready",
                               "led_effect sb_nozzle_heating",
                               "led_effect sb_nozzle_cooling",
                               "led_effect sb_nozzle_standby",
                               "led_effect sb_nozzle_part_ready",
                               "led_effect sb_critical_error",
                               "led_effect rainbow",
                               "led_effect set_nozzle_leds",
                               "led_effect set_logo_leds",
                               "gcode_macro set_logo_leds_off",
                               "gcode_macro set_logo_leds_on",
                               "gcode_macro set_nozzle_leds_on",
                               "gcode_macro set_nozzle_leds_off",
                               "gcode_macro status_off",
                               "gcode_macro status_ready",
                               "gcode_macro status_part_ready",
                               "gcode_macro status_busy",
                               "gcode_macro status_heating",
                               "gcode_macro status_cooling",
                               "gcode_macro status_leveling",
                               "gcode_macro status_homing",
                               "gcode_macro status_cleaning",
                               "gcode_macro status_meshing",
                               "gcode_macro status_calibrating_z",
                               "gcode_macro status_printing",
                               "gcode_macro _CLIENT_VARIABLE",
                               "gcode_macro PROBECALIBRATE",
                               "gcode_macro G32",
                               "gcode_macro M141",
                               "gcode_macro CLEAN_NOZZLE",
                               "gcode_macro PRINT_START",
                               "gcode_macro START_PRINT",
                               "gcode_macro PRINT_END",
                               "gcode_macro _CANCEL_USER",
                               "gcode_macro TOOLCHANGE",
                               "gcode_macro UPDATE_DISPLAY",
                               "gcode_macro LOAD_FILAMENT",
                               "gcode_macro UNLOAD_FILAMENT",
                               "gcode_macro PURGE_FILAMENT",
                               "gcode_macro LOAD_MATERIAL",
                               "gcode_macro _LOAD_MATERIAL_SELECT",
                               "gcode_macro _LOAD_MATERIAL_HEATUP",
                               "gcode_macro _LOAD_MATERIAL_ACTION",
                               "gcode_macro _LOAD_MATERIAL_END",
                               "gcode_macro _FILAMENT_RUNOUT_EVENT",
                               "gcode_macro M600",
                               "gcode_macro _INTERACTIVE_LOAD_END",
                               "gcode_macro DUMP_VARIABLES",
                               "gcode_macro GET_VARIABLE",
                               "gcode_macro SET_ACTIVE_SPOOL",
                               "gcode_macro CLEAR_ACTIVE_SPOOL",
                               "gcode_macro POWER_OFF",
                               "gcode_macro QRCode_Scan",
                               "gcode_macro _OBICO_LAYER_CHANGE",
                               "gcode_macro OBICO_LINK_STATUS",
                               "gcode_macro _OBICO_RELINK",
                               "skew_correction",
                               "idle_timeout",
                               "gcode_macro excitate_axis_at_freq",
                               "gcode_macro axes_map_calibration",
                               "gcode_macro compare_belts_responses",
                               "gcode_macro axes_shaper_calibration",
                               "gcode_macro create_vibrations_profile",
                               "exclude_object",
                               "save_variables",
                               "gcode_macro _Sensorless_Homing_Variables",
                               "gcode_macro _HOME_X",
                               "gcode_macro _HOME_Y",
                               "temperature_sensor chamber",
                               "temperature_sensor EBB_NTC",
                               "temperature_sensor BTT-MCU",
                               "temperature_host BTT-PI",
                               "temperature_sensor BTT-PI",
                               "tmc2240 stepper_x",
                               "tmc2240 stepper_y",
                               "tmc2209 stepper_z",
                               "tmc2209 stepper_z1",
                               "tmc2209 stepper_z2",
                               "tmc2209 stepper_z3",
                               "tmc2209 extruder",
                               "heater_bed",
                               "fan",
                               "heater_fan hotend_fan",
                               "heater_fan skirt_fan",
                               "heater_fan skirt_fan_2",
                               "temperature_host btt_pi",
                               "temperature_fan btt_pi",
                               "neopixel sb_leds",
                               "neopixel case_lights",
                               "probe",
                               "quad_gantry_level",
                               "bed_mesh",
                               "telemetry",
                               "system_stats",
                               "manual_probe",
                               "toolhead",
                               "extruder",
                               "filament_switch_sensor virtual_bypass",
                               "filament_switch_sensor quiet_mode"});

    PrinterDiscovery hw;
    hw.parse_objects(objects);

    SECTION("neopixel strips are discovered as LEDs") {
        REQUIRE(has(hw.leds(), "neopixel sb_leds"));
        REQUIRE(has(hw.leds(), "neopixel case_lights"));
        REQUIRE(hw.has_led());
    }

    SECTION("led_effect objects are detected") {
        // This machine defines 40+ [led_effect] sections; SET_LED_EFFECT is
        // discretionary gcode, so the effect backend must see them.
        REQUIRE(hw.has_led_effects());
    }

    SECTION("no output_pin means no output_pin lights or fans") {
        CHECK_FALSE(any_contains(hw.leds(), "output_pin"));
        CHECK_FALSE(any_contains(hw.fans(), "output_pin"));
    }
}
