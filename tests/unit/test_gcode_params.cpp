// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/gcode_params.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::gcode_param_value;

TEST_CASE("gcode_param_value reads KEY=value as Klipper does", "[gcode_params]") {
    CHECK(gcode_param_value("SET_FAN_SPEED FAN=nevermore SPEED=0.5", "SPEED") == "0.5");
    CHECK(gcode_param_value("EXCLUDE_OBJECT_DEFINE NAME='part 1' CENTER=1,2", "NAME") == "part 1");
    CHECK(gcode_param_value("M117 MSG=\"hello there\"", "MSG") == "hello there");
    CHECK_FALSE(gcode_param_value("SET_FAN_SPEED FAN=a", "SPEED"));
    CHECK_FALSE(gcode_param_value("SET_FAN_SPEED SPEED=", "SPEED"));
    CHECK_FALSE(gcode_param_value("M117 MSG=''", "MSG"));
}

TEST_CASE("gcode_param_value matches a whole key, never the tail of a longer one",
          "[gcode_params]") {
    const char* line = "SET_PRESSURE_ADVANCE TIME_OFFSET=0.002 OFFSET=0.1";
    CHECK(gcode_param_value(line, "OFFSET") == "0.1");
    CHECK(gcode_param_value(line, "TIME_OFFSET") == "0.002");
    CHECK_FALSE(gcode_param_value("SET_PRESSURE_ADVANCE TIME_OFFSET=0.002", "OFFSET"));
}
