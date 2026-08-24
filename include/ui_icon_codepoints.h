// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>

// =============================================================================
// Material Design Icons - Codepoint Mapping for Font-Based Icon System
//
// This file maps icon short names to their UTF-8 encoded MDI codepoints.
// MDI uses Unicode Plane 15 Private Use Area (0xF0000+)
// UTF-8 encoding: 4 bytes per codepoint
//
// Formula: F3 B0+((cp>>12)&0x0F) 80+((cp>>6)&0x3F) 80+(cp&0x3F)
// Example: F02DC -> F3 B0 8B 9C
//
// Source: https://pictogrammers.com/library/mdi/
// =============================================================================

namespace ui_icon {

// Icon codepoint mapping structure
struct IconMapping {
    const char* name;
    const char* codepoint;
};

// clang-format off

// Complete icon mapping table
// Sorted alphabetically by name for efficient binary search
static const IconMapping ICON_MAP[] = {
    // Alert/Status icons
    {"alert",              "\xF3\xB0\x80\xA6"},  // F0026 alert/triangle-exclamation
    {"alert_circle",       "\xF3\xB0\x80\xA8"},  // F0028 alert-circle
    {"alert_octagon",      "\xF3\xB0\x80\xA9"},  // F0029 alert-octagon
    {"alpha_a_circle",     "\xF3\xB0\xAF\xAC"},  // F0BEC alpha-a-circle (auto indicator)
    {"animation",          "\xF3\xB0\x97\x98"},  // F05D8 animation (stacked rectangles)
    {"animation_play",     "\xF3\xB0\xA4\xBA"},  // F093A animation-play (framerate)

    // Arrow icons
    {"arrow_down",         "\xF3\xB0\x81\x85"},  // F0045 arrow-down (head descends)
    {"arrow_down_bold",    "\xF3\xB0\x9C\xAE"},  // F072E arrow-down-bold (probe sensors)
    {"arrow_expand_down",  "\xF3\xB0\x9E\x96"},  // F0796 arrow-expand-down (bed drops - CoreXY)
    {"arrow_expand_up",    "\xF3\xB0\x9E\x99"},  // F0799 arrow-expand-up (bed rises - CoreXY)
    {"arrow_left",         "\xF3\xB0\x81\x8D"},  // F004D arrow-left
    {"arrow_left_bold",    "\xF3\xB0\x9C\xB1"},  // F0731 arrow-left-bold
    {"arrow_left_right",   "\xF3\xB0\xB9\xB3"},  // F0E73 arrow-left-right (bidirectional)
    {"arrow_left_thick",   "\xF3\xB0\x81\x8E"},  // F004E arrow-left-thick
    {"arrow_right",        "\xF3\xB0\x81\x94"},  // F0054 arrow-right
    {"arrow_right_bold",   "\xF3\xB0\x9C\xB4"},  // F0734 arrow-right-bold
    {"arrow_up",             "\xF3\xB0\x81\x9D"},  // F005D arrow-up (head ascends)
    {"arrow_up_bold_circle", "\xF3\xB0\x81\x9F"},  // F005F arrow-up-bold-circle
    {"arrow_up_circle",      "\xF3\xB0\xB3\xA1"},  // F0CE1 arrow-up-circle (load/activate)
    {"arrow_up_down",        "\xF3\xB0\xB9\xB9"},  // F0E79 arrow-up-down (bidirectional)
    {"arrow_up_right",       "\xF3\xB1\x9E\xBF"},  // F17BF arrow-up-right (retraction)
    {"axis_arrow",         "\xF3\xB0\xB5\x89"},  // F0D49 axis-arrow (all 3 axes)
    {"axis_x_arrow",       "\xF3\xB0\xB5\x8C"},  // F0D4C axis-x-arrow
    {"axis_y_arrow",       "\xF3\xB0\xB5\x91"},  // F0D51 axis-y-arrow
    {"axis_z_arrow",       "\xF3\xB0\xB5\x95"},  // F0D55 axis-z-arrow

    // Back/navigation
    {"back",               "\xF3\xB0\x85\x81"},  // F0141 chevron-left (back)
    {"backspace",          "\xF3\xB0\xAD\x9C"},  // F0B5C backspace-outline
    {"bed",                "\xF3\xB0\x8B\xA3"},  // F02E3 bed
    {"bell",               "\xF3\xB0\x82\x9A"},  // F009A bell/notifications
    {"block_helper",       "\xF3\xB0\x82\xAD"},  // F00AD block-helper/prohibited
    {"bluetooth",          "\xF3\xB0\x82\xAF"},  // F00AF bluetooth
    {"bluetooth_connect",  "\xF3\xB0\x82\xB1"},  // F00B1 bluetooth-connect
    {"book",               "\xF3\xB0\x97\x9A"},  // F05DA book-open-page-variant (documentation)
    {"bug",                "\xF3\xB0\x83\xA4"},  // F00E4 bug (debug bundle)

    // Camera icons
    {"camera_timer",       "\xF3\xB0\x84\x89"},  // F0109 camera-timer (timelapse)

    // Cancel/Close/Check
    {"cancel",             "\xF3\xB0\x9C\xBA"},  // F073A cancel
    {"chart_line",         "\xF3\xB0\x84\xAA"},  // F012A chart-line
    {"check",              "\xF3\xB0\x84\xAC"},  // F012C check
    {"check_bold",         "\xF3\xB0\xB8\x9E"},  // F0E1E check-bold
    {"check_box",               "\xF3\xB0\x84\xB2"},  // F0132 checkbox-marked (filled/checked)
    {"check_box_outline_blank", "\xF3\xB0\x84\xB1"},  // F0131 checkbox-blank-outline (empty/unchecked)
    {"check_circle",       "\xF3\xB0\x97\xA1"},  // F05E1 check-circle-outline
    {"chevron_down",       "\xF3\xB0\x85\x80"},  // F0140 chevron-down
    {"chevron_left",       "\xF3\xB0\x85\x81"},  // F0141 chevron-left
    {"chevron_right",      "\xF3\xB0\x85\x82"},  // F0142 chevron-right
    {"chevron_up",         "\xF3\xB0\x85\x83"},  // F0143 chevron-up
    {"clock",              "\xF3\xB0\x85\x90"},  // F0150 clock-outline
    {"close",              "\xF3\xB0\x85\x96"},  // F0156 close/xmark
    {"close_circle",       "\xF3\xB0\x85\x99"},  // F0159 close-circle
    {"code_braces",        "\xF3\xB0\x85\xA9"},  // F0169 code-braces
    {"code_tags",          "\xF3\xB0\x85\xB4"},  // F0174 code-tags
    {"cog",                "\xF3\xB0\x92\x93"},  // F0493 cog/settings
    {"console",            "\xF3\xB0\x86\x8D"},  // F018D console/terminal
    {"content_copy",       "\xF3\xB0\x86\x8F"},  // F018F content-copy (duplicate)
    {"coolant_temperature","\xF3\xB0\x8F\x88"},  // F03C8 coolant-temperature
    {"cooldown",           "\xF3\xB0\x9C\x97"},  // F0717 snowflake
    {"crosshairs_gps",     "\xF3\xB0\x86\xA4"},  // F01A4 crosshairs-gps (probe)
    {"cube",               "\xF3\xB0\x86\xA7"},  // F01A7 cube-outline
    {"cube_outline",       "\xF3\xB0\x86\xA7"},  // F01A7 cube-outline
    {"cursor_move",        "\xF3\xB0\x86\xBE"},  // F01BE cursor-move (4-way arrows)

    // Dashboard/Database
    {"dashboard",          "\xF3\xB0\x95\xAE"},  // F056E view-dashboard
    {"database",           "\xF3\xB0\x86\xBC"},  // F01BC database
    {"debug_step_over",    "\xF3\xB0\x86\xB7"},  // F01B7 debug-step-over (skip objects)
    {"delete",             "\xF3\xB0\x86\xB4"},  // F01B4 delete/trash
    {"dots_vertical",      "\xF3\xB0\x87\x99"},  // F01D9 dots-vertical/advanced
    {"download",           "\xF3\xB0\x87\x9A"},  // F01DA download
    {"eject",              "\xF3\xB0\x87\xAA"},  // F01EA eject (lane eject)
    {"electric_switch",    "\xF3\xB0\xBA\x9F"},  // F0E9F electric-switch

    // Engine/Motor
    {"engine",             "\xF3\xB0\x87\xBA"},  // F01FA engine/motor
    {"engine_off",         "\xF3\xB0\xA9\x86"},  // F0A46 engine-off/motor-off
    {"ethernet",           "\xF3\xB0\x88\x80"},  // F0200 ethernet
    {"expand_all",         "\xF3\xB0\x81\x8C"},  // F004C arrow-expand-all/move
    {"eye",                "\xF3\xB0\x88\x88"},  // F0208 eye
    {"eye_off",            "\xF3\xB0\x88\x89"},  // F0209 eye-off

    // Fan icons
    {"fan",                "\xF3\xB0\x88\x90"},  // F0210 fan
    {"fan_off",            "\xF3\xB0\xA0\x9D"},  // F081D fan-off
    {"filament",           "\xF3\xB0\xB9\x9B"},  // F0E5B printer-3d-nozzle
    {"filament_alert",     "\xF3\xB1\x87\x80"},  // F11C0 printer-3d-nozzle-alert
    {"filter",             "\xF3\xB0\x88\xB2"},  // F0232 filter (funnel)
    {"fine_tune",          "\xF3\xB0\x98\xAE"},  // F062E tune
    {"fire",               "\xF3\xB0\x88\xB8"},  // F0238 fire
    {"flash",              "\xF3\xB0\x89\x81"},  // F0241 flash (lightning bolt)
    {"folder",             "\xF3\xB0\x89\x8B"},  // F024B folder
    {"folder_arrow_up",    "\xF3\xB1\xA7\xB0"},  // F19F0 folder-arrow-up (parent dir)
    {"folder_home",        "\xF3\xB1\x82\xB5"},  // F10B5 folder-home
    {"folder_open",        "\xF3\xB0\x9D\xB0"},  // F0770 folder-open
    {"folder_upload",      "\xF3\xB0\x89\x99"},  // F0259 folder-upload
    {"fridge_industrial",  "\xF3\xB1\x97\xAE"},  // F15EE fridge-industrial (chamber)

    // Grid icons
    {"gauge",              "\xF3\xB0\x8A\x9A"},  // F029A gauge
    {"grid_large",         "\xF3\xB0\x9D\x98"},  // F0758 grid-large
    {"grid_off",           "\xF3\xB0\x8B\x82"},  // F02C2 grid-off
    {"grid_view",          "\xF3\xB0\x95\xB0"},  // F0570 view-grid

    // Heat/Heating
    {"heat_wave",          "\xF3\xB1\xA9\x85"},  // F1A45 heat-wave (thermal lines)
    {"heater",             "\xF3\xB1\xA2\xB8"},  // F18B8 printer-3d-nozzle-heat
    {"help",               "\xF3\xB0\x98\xA5"},  // F0625 help-circle-outline (help & support)
    {"help_circle",        "\xF3\xB0\x8B\x97"},  // F02D7 help-circle/question
    {"home",               "\xF3\xB0\x8B\x9C"},  // F02DC home
    {"home_import",        "\xF3\xB0\xBE\x9C"},  // F0F9C home-import-outline (home-z)
    {"home_thermometer",   "\xF3\xB0\xBD\x94"},  // F0F54 home-thermometer
    {"hourglass",          "\xF3\xB0\x94\x9F"},  // F051F timer-sand (hourglass)

    // Info/Image
    {"image_area",           "\xF3\xB0\x8B\xAB"},  // F02EB image-area (photo/picture)
    {"image_broken_variant", "\xF3\xB0\x8B\xAE"}, // F02EE image-broken-variant (fallback)
    {"inbox_outline",      "\xF3\xB1\x89\xB4"},  // F1274 inbox-outline
    {"infinity",           "\xF3\xB0\x9B\xA4"},  // F06E4 infinity (endless spool)
    {"info",               "\xF3\xB0\x8B\xBC"},  // F02FC information
    {"info_circle",        "\xF3\xB0\x8B\xBC"},  // F02FC information (alias for FontAwesome compat)
    {"info_outline",       "\xF3\xB0\x8B\xBD"},  // F02FD information-outline
    {"inputshaper",        "\xF3\xB0\xA5\x9B"},  // F095B sine-wave
    {"inventory",          "\xF3\xB0\x8F\x96"},  // F03D6 package-variant

    // Keyboard
    {"keyboard",           "\xF3\xB0\x8C\x8C"},  // F030C keyboard

    // LAN/Network
    {"lan",                "\xF3\xB0\x8C\x97"},  // F0317 lan
    {"layers",             "\xF3\xB0\x8C\xA8"},  // F0328 layers
    {"leaf",               "\xF3\xB0\x8C\xAA"},  // F032A leaf
    {"led_strip",          "\xF3\xB0\x9F\x96"},  // F07D6 led-strip
    {"light",              "\xF3\xB0\x8C\xB5"},  // F0335 lightbulb
    {"light_off",          "\xF3\xB0\xB9\x8F"},  // F0E4F lightbulb-off
    {"lightbulb_on",       "\xF3\xB0\x9B\xA8"},  // F06E8 lightbulb-on (100%)
    {"lightbulb_on_10",    "\xF3\xB1\xA9\x8E"},  // F1A4E lightbulb-on-10
    {"lightbulb_on_20",    "\xF3\xB1\xA9\x8F"},  // F1A4F lightbulb-on-20
    {"lightbulb_on_30",    "\xF3\xB1\xA9\x90"},  // F1A50 lightbulb-on-30
    {"lightbulb_on_40",    "\xF3\xB1\xA9\x91"},  // F1A51 lightbulb-on-40
    {"lightbulb_on_50",    "\xF3\xB1\xA9\x92"},  // F1A52 lightbulb-on-50
    {"lightbulb_on_60",    "\xF3\xB1\xA9\x93"},  // F1A53 lightbulb-on-60
    {"lightbulb_on_70",    "\xF3\xB1\xA9\x94"},  // F1A54 lightbulb-on-70
    {"lightbulb_on_80",    "\xF3\xB1\xA9\x95"},  // F1A55 lightbulb-on-80
    {"lightbulb_on_90",    "\xF3\xB1\xA9\x96"},  // F1A56 lightbulb-on-90
    {"lightbulb_outline",  "\xF3\xB0\x8C\xB6"},  // F0336 lightbulb-outline (OFF)
    {"limit",              "\xF3\xB0\xB9\x8F"},  // Use same as light_off or find better
    {"link",               "\xF3\xB0\x8C\xB9"},  // F0339 link (tool mapping)
    {"list",               "\xF3\xB0\x89\xB9"},  // F0279 format-list-bulleted
    {"lock",               "\xF3\xB0\x8C\xBE"},  // F033E lock
    {"lock_open",          "\xF3\xB0\xBF\x86"},  // F0FC6 lock-open-variant
    {"magnet",             "\xF3\xB0\x8D\x87"},  // F0347 magnet (Klicky probe)
    {"magnify",            "\xF3\xB0\x8D\x89"},  // F0349 magnify (search/discovery)
    {"magnify_scan",       "\xF3\xB1\x89\xB6"},  // F1276 magnify-scan (BT discovery)

    // Misc/Math
    {"message",            "\xF3\xB0\x8D\xA9"},  // F0369 message-text (discord/chat)
    {"minus",              "\xF3\xB0\x8D\xB4"},  // F0374 minus
    {"move",               "\xF3\xB0\x81\x8C"},  // F004C arrow-expand-all
    {"network",            "\xF3\xB0\x88\x80"},  // F0200 ethernet (network)
    {"notifications",      "\xF3\xB0\x82\x9A"},  // F009A bell

    {"palette",            "\xF3\xB0\x8F\x98"},  // F03D8 palette (color sensors)

    // Pause/Pencil/Play/Plus
    {"pause",              "\xF3\xB0\x8F\xA4"},  // F03E4 pause
    {"pencil",             "\xF3\xB0\x8F\xAB"},  // F03EB pencil (edit)
    {"play",               "\xF3\xB0\x90\x8A"},  // F040A play
    {"play_circle",        "\xF3\xB0\x90\x8C"},  // F040C play-circle
    {"plus",               "\xF3\xB0\x90\x95"},  // F0415 plus
    {"power",              "\xF3\xB0\x90\xA5"},  // F0425 power
    {"power_cycle",        "\xF3\xB0\xA4\x81"},  // F0901 power-cycle
    {"power_off",          "\xF3\xB0\xA4\x82"},  // F0902 power-off
    {"power_on",           "\xF3\xB0\xA4\x83"},  // F0903 power-on
    {"power_plug",         "\xF3\xB0\x9A\xA5"},  // F06A5 power-plug
    {"power_plug_battery", "\xF3\xB1\xB0\xBB"},  // F1C3B power-plug-battery
    {"power_plug_off",     "\xF3\xB0\x9A\xA6"},  // F06A6 power-plug-off
    {"power_plug_outline", "\xF3\xB1\x90\xA5"},  // F1425 power-plug-outline
    {"power_socket",       "\xF3\xB0\x90\xA7"},  // F0427 power-socket
    {"power_socket_au",    "\xF3\xB0\xA4\x85"},  // F0905 power-socket-au
    {"power_socket_ch",    "\xF3\xB0\xBE\xB3"},  // F0FB3 power-socket-ch
    {"power_socket_de",    "\xF3\xB1\x84\x87"},  // F1107 power-socket-de
    {"power_socket_eu",    "\xF3\xB0\x9F\xA7"},  // F07E7 power-socket-eu
    {"power_socket_fr",    "\xF3\xB1\x84\x88"},  // F1108 power-socket-fr
    {"power_socket_it",    "\xF3\xB1\x93\xBF"},  // F14FF power-socket-it
    {"power_socket_jp",    "\xF3\xB1\x84\x89"},  // F1109 power-socket-jp
    {"power_socket_uk",    "\xF3\xB0\x9F\xA8"},  // F07E8 power-socket-uk
    {"power_socket_us",    "\xF3\xB0\x9F\xA9"},  // F07E9 power-socket-us
    {"power_standby",      "\xF3\xB0\xA4\x86"},  // F0906 power-standby
    {"print",              "\xF3\xB0\x90\xAA"},  // F042A printer
    {"printer",            "\xF3\xB0\x90\xAA"},  // F042A printer (alias)
    {"printer_3d",         "\xF3\xB0\x90\xAB"},  // F042B printer-3d
    {"progress_clock",     "\xF3\xB0\xA6\x96"},  // F0996 progress-clock (phase tracking)
    {"prohibited",         "\xF3\xB0\x82\xAD"},  // F00AD block-helper
    {"puzzle_outline",     "\xF3\xB0\xA9\xA6"},  // F0A66 puzzle-outline (plugin)
    {"qrcode_scan",        "\xF3\xB0\x90\xB3"},  // F0433 qrcode-scan
    {"question_circle",    "\xF3\xB0\x8B\x97"},  // F02D7 help-circle (alias for FontAwesome compat)
    {"rabbit",             "\xF3\xB0\xA4\x87"},  // F0907 rabbit (Happy Hare logo)

    // Radiator/Heating
    {"radiator",           "\xF3\xB0\x90\xB8"},  // F0438 radiator
    {"redo",               "\xF3\xB0\x91\x8E"},  // F044E redo (clockwise arrow - tighten)
    {"refresh",            "\xF3\xB0\x91\x90"},  // F0450 refresh
    {"restart",            "\xF3\xB0\x9C\x89"},  // F0709 restart
    {"resume",             "\xF3\xB0\x90\x8C"},  // F040C play-circle (resume)
    {"robot",              "\xF3\xB0\x9A\xA9"},  // F06A9 robot (auto-controlled)
    {"rotate_3d",          "\xF3\xB0\xBB\x87"},  // F0EC7 rotate-3d (orbit view)
    {"rotate_left",        "\xF3\xB0\x91\xA5"},  // F0465 rotate-left (CCW)
    {"rotate_right",       "\xF3\xB0\x91\xA7"},  // F0467 rotate-right (CW)
    {"router",             "\xF3\xB0\x91\xA9"},  // F0469 router-wireless
    {"ruler",              "\xF3\xB0\x91\xAD"},  // F046D ruler (print height)

    // Script / SD card / Send
    {"script_text",        "\xF3\xB0\xAF\x82"},  // F0BC2 script-text (macro/script)
    {"sd",                 "\xF3\xB0\x91\xB9"},  // F0479 sd
    {"send",               "\xF3\xB0\x92\x8A"},  // F048A send
    {"sensor",             "\xF3\xB0\xB6\x91"},  // F0D91 motion-sensor (sensor placeholder)
    {"settings",           "\xF3\xB0\x92\x93"},  // F0493 cog
    {"sine_wave",          "\xF3\xB0\xA5\x9B"},  // F095B sine-wave
    {"sleep",              "\xF3\xB0\x92\xB2"},  // F04B2 sleep (moon/zzz)
    {"source_branch",      "\xF3\xB0\x98\xAC"},  // F062C source-branch (bypass/fork)
    {"speed",              "\xF3\xB0\x93\x85"},  // F04C5 speedometer
    {"speed_down",         "\xF3\xB0\xBE\x86"},  // F0F86 speedometer-slow
    {"speed_up",           "\xF3\xB0\xBE\x85"},  // F0F85 speedometer-medium
    {"spirit_level",       "\xF3\xB1\x93\xB1"},  // F14F1 spirit-level (QGL/Z-Tilt)
    {"spoolman",           "\xF3\xB0\x88\xAF"},  // F022F film (same as filament)
    {"star",               "\xF3\xB0\x93\x8E"},  // F04CE star (favorites/recent)
    {"stop",               "\xF3\xB0\x93\x9B"},  // F04DB stop
    {"swap_vertical",      "\xF3\xB0\x93\xA2"},  // F04E2 swap-vertical
    {"sync",               "\xF3\xB0\x93\xA6"},  // F04E6 sync (auto-detect)
    {"sysinfo",            "\xF3\xB0\x8B\xBC"},  // F02FC information
    {"target",             "\xF3\xB0\x93\xBE"},  // F04FE target (touch calibration)
    {"television_classic", "\xF3\xB0\x9F\xB4"},  // F07F4 television-classic (screensaver)

    // Temperature
    {"thermometer",        "\xF3\xB0\x94\x8F"},  // F050F thermometer
    {"thermometer_lines",  "\xF3\xB0\x94\x90"},  // F0510 thermometer-lines
    {"thermometer_minus",  "\xF3\xB0\xB8\x84"},  // F0E04 thermometer-minus
    {"thermometer_off",    "\xF3\xB1\x94\xB1"},  // F1531 thermometer-off
    {"thermometer_plus",   "\xF3\xB0\xB8\x85"},  // F0E05 thermometer-plus
    {"thermometer_probe",  "\xF3\xB1\xAC\xAB"},  // F1B2B thermometer-probe
    {"toolbox_outline",    "\xF3\xB0\xA6\xAD"},  // F09AD toolbox-outline (tools section)
    {"tortoise",           "\xF3\xB0\xB4\xBB"},  // F0D3B tortoise (AFC/Box Turtle logo)
    {"toy_brick_outline",  "\xF3\xB1\x8A\x8D"},  // F128D toy-brick-outline (building block)
    {"train_flatbed",      "\xF3\xB1\xAC\xB5"},  // F1B35 train-car-flatbed (print bed base)
    {"translate",          "\xF3\xB0\x97\x8A"},  // F05CA translate (language selection)
    {"trash_can_outline",  "\xF3\xB0\xA9\xBA"},  // F0A7A trash-can-outline (delete)
    {"tray_arrow_up",      "\xF3\xB0\x84\x9D"},  // F011D tray-arrow-up (unload/eject)
    {"triangle_exclamation", "\xF3\xB0\x80\xA6"}, // F0026 alert (alias for FontAwesome compat)
    {"tune",               "\xF3\xB0\x98\xAE"},  // F062E tune
    {"tune_variant",       "\xF3\xB1\x95\x82"},  // F1542 tune-variant
    {"tune_vertical_variant", "\xF3\xB1\x95\x83"},  // F1543 tune-vertical-variant
    {"undo",               "\xF3\xB0\x95\x8C"},  // F054C undo (counter-clockwise arrow - loosen)

    // Update/USB
    {"update",             "\xF3\xB0\x9A\xB0"},  // F06B0 update
    {"usb",                "\xF3\xB0\x95\x93"},  // F0553 usb

    // Vibrate/Video/View icons
    {"vibrate",            "\xF3\xB0\x95\xA6"},  // F0566 vibrate
    {"video",              "\xF3\xB0\x95\xA7"},  // F0567 video (timelapse)
    {"video_off",          "\xF3\xB0\x95\xA8"},  // F0568 video-off (empty state)
    {"view_dashboard",     "\xF3\xB0\x95\xAE"},  // F056E view-dashboard
    {"view_grid",          "\xF3\xB0\x95\xB0"},  // F0570 view-grid
    {"view_list",          "\xF3\xB0\x95\xB2"},  // F0572 view-list
    {"volume_high",        "\xF3\xB0\x95\xBE"},  // F057E volume-high
    {"volume_low",         "\xF3\xB0\x96\x80"},  // F0580 volume-medium
    {"volume_off",         "\xF3\xB0\x96\x81"},  // F0581 volume-off

    // Water/Weight/WiFi icons
    {"water",              "\xF3\xB0\x96\x8C"},  // F058C water/droplet
    {"waveform",           "\xF3\xB1\x91\xBD"},  // F147D waveform (accelerometers)
    {"weight",             "\xF3\xB0\x96\xA1"},  // F05A1 weight
    {"wifi",               "\xF3\xB0\x96\xA9"},  // F05A9 wifi
    {"wifi_alert",         "\xF3\xB1\x9A\xB5"},  // F16B5 wifi-alert
    {"wifi_check",         "\xF3\xB1\x9A\xBD"},  // F16BD wifi-check
    {"wifi_lock",          "\xF3\xB1\x9A\xBF"},  // F16BF wifi-lock
    {"wifi_off",           "\xF3\xB0\x96\xAA"},  // F05AA wifi-off
    {"wifi_strength_1",    "\xF3\xB0\xA4\x9F"},  // F091F wifi-strength-1
    {"wifi_strength_1_alert", "\xF3\xB0\xA4\xA0"}, // F0920 wifi-strength-1-alert
    {"wifi_strength_1_lock", "\xF3\xB0\xA4\xA1"},  // F0921 wifi-strength-1-lock
    {"wifi_strength_2",    "\xF3\xB0\xA4\xA2"},  // F0922 wifi-strength-2
    {"wifi_strength_2_lock", "\xF3\xB0\xA4\xA4"},  // F0924 wifi-strength-2-lock
    {"wifi_strength_3",    "\xF3\xB0\xA4\xA5"},  // F0925 wifi-strength-3
    {"wifi_strength_3_lock", "\xF3\xB0\xA4\xA7"},  // F0927 wifi-strength-3-lock
    {"wifi_strength_4",    "\xF3\xB0\xA4\xA8"},  // F0928 wifi-strength-4
    {"wifi_strength_4_lock", "\xF3\xB0\xA4\xAA"},  // F092A wifi-strength-4-lock

    // Wrench/Tools
    {"wrench",             "\xF3\xB1\x8C\xA3"},  // F1323 hammer-wrench

    // X (xmark alias)
    {"xmark",              "\xF3\xB0\x85\x96"},  // F0156 close (alias for FontAwesome compat)

    // Z-axis
    {"z_closer",           "\xF3\xB0\x9C\xAE"},  // F072E arrow-down-bold (z closer)
    {"z_farther",          "\xF3\xB0\x9C\xB7"},  // F0737 arrow-up-bold (z farther)
};

// clang-format on

// Number of icons in the mapping table
static constexpr size_t ICON_MAP_SIZE = sizeof(ICON_MAP) / sizeof(ICON_MAP[0]);

/**
 * @brief Look up an icon codepoint by name
 * @param name Icon short name (e.g., "home", "wifi", "settings")
 * @return UTF-8 encoded codepoint string, or nullptr if not found
 */
inline const char* lookup_codepoint(const char* name) {
    if (!name)
        return nullptr;

    // Binary search for efficiency (table is sorted alphabetically)
    size_t left = 0;
    size_t right = ICON_MAP_SIZE;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        int cmp = strcmp(name, ICON_MAP[mid].name);

        if (cmp == 0) {
            return ICON_MAP[mid].codepoint;
        } else if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return nullptr;
}

/**
 * @brief Legacy name mapping - converts mat_*_img names to short names
 * @param legacy_name Name like "mat_home_img"
 * @return Short name like "home", or original name if not legacy format
 *
 * This function is provided for transition purposes but should not be used
 * in new code. Use short names directly.
 */
inline const char* strip_legacy_prefix(const char* legacy_name) {
    if (!legacy_name)
        return nullptr;

    // Check for "mat_" prefix
    if (strncmp(legacy_name, "mat_", 4) != 0) {
        return legacy_name; // Not a legacy name
    }

    // Strip "mat_" prefix
    const char* stripped = legacy_name + 4;

    // Check for "_img" suffix and strip it
    size_t len = strlen(stripped);
    if (len > 4 && strcmp(stripped + len - 4, "_img") == 0) {
        // Return static buffer - NOT thread safe, but OK for single-threaded LVGL
        static char buffer[64];
        size_t copy_len = len - 4;
        if (copy_len >= sizeof(buffer))
            copy_len = sizeof(buffer) - 1;
        strncpy(buffer, stripped, copy_len);
        buffer[copy_len] = '\0';
        return buffer;
    }

    return stripped;
}

} // namespace ui_icon
