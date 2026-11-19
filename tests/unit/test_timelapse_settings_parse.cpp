// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_timelapse_settings_parse.cpp
 * @brief Reading GET /machine/timelapse/settings the way Moonraker sends it
 *
 * Run with: ./build/bin/helix-tests "[timelapse][settings_parse]"
 */

#include "moonraker_timelapse_api.h"
#include "moonraker_types.h"

#include "../catch_amalgamated.hpp"

using helix::parse_timelapse_settings;

namespace {
// GET /machine/timelapse/settings from a Voron 2.4 running moonraker-timelapse.
constexpr const char* kRealBody =
    R"JSON({"result":{"enabled":true,"mode":"layermacro","camera":"","snapshoturl":"http://localhost:8080/?action=snapshot","stream_delay_compensation":0.05,"gcode_verbose":false,"parkhead":false,"parkpos":"back_left","park_custom_pos_x":10.0,"park_custom_pos_y":10.0,"park_custom_pos_dz":0.0,"park_travel_speed":100,"park_retract_speed":15,"park_extrude_speed":15,"park_retract_distance":1.0,"park_extrude_distance":1.0,"park_time":0.1,"fw_retract":false,"hyperlapse_cycle":30,"autorender":false,"constant_rate_factor":23,"output_framerate":30,"pixelformat":"yuv420p","time_format_code":"%Y%m%d_%H%M","extraoutputparams":"","variable_fps":false,"targetlength":10,"variable_fps_min":5,"variable_fps_max":60,"rotation":0,"flip_x":false,"flip_y":false,"duplicatelastframe":5,"previewimage":true,"saveframes":false,"blockedsettings":[]}})JSON";
} // namespace

TEST_CASE("timelapse settings are read from Moonraker's result wrapper",
          "[timelapse][settings_parse]") {
    TimelapseSettings s;
    REQUIRE(parse_timelapse_settings(kRealBody, s));
    // Both differ from the defaults, so a parse that misses "result" fails here.
    CHECK(s.enabled);
    CHECK_FALSE(s.autorender);
    CHECK(s.mode == "layermacro");
    CHECK(s.output_framerate == 30);
    CHECK(s.park_retract_distance == 1);
    CHECK(s.park_extrude_speed == 15.0);
    CHECK(s.hyperlapse_cycle == 30);
}

TEST_CASE("timelapse settings still read from a bare dict", "[timelapse][settings_parse]") {
    TimelapseSettings s;
    REQUIRE(parse_timelapse_settings(
        R"({"enabled":true,"autorender":false,"mode":"hyperlapse","park_retract_distance":2.0,)"
        R"("hyperlapse_cycle":null})",
        s));
    CHECK(s.enabled);
    CHECK_FALSE(s.autorender);
    CHECK(s.mode == "hyperlapse");
    CHECK(s.park_retract_distance == 2);
    CHECK(s.hyperlapse_cycle == 30);
}

TEST_CASE("a timelapse settings body that is not an object is refused",
          "[timelapse][settings_parse]") {
    TimelapseSettings s;
    s.mode = "untouched";
    CHECK_FALSE(parse_timelapse_settings("not json", s));
    CHECK_FALSE(parse_timelapse_settings("[1,2]", s));
    CHECK(s.mode == "untouched");
}
