// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_fan_analyzer.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

// Real K1C M106 macro (simplified)
static const char* kK1cM106Macro = R"(
{% set fans = printer["gcode_macro PRINTER_PARAM"].fans|int %}
{% set fan = 0 %}
{% set value = 0 %}
{% if params.P is defined %}
{% set tmp = params.P|int %}
{% if tmp < fans %}
{% set fan = tmp %}
{% endif %}
{% endif %}
{% if params.S is defined %}
{% set tmp = params.S|float %}
{% else %}
{% set tmp = 255 %}
{% endif %}
{% if tmp > 0 %}
{% if fan == 0 %}
{% set value = (255 - printer["gcode_macro PRINTER_PARAM"].fan0_min) / 255 * tmp %}
SET_PIN PIN=fan0 VALUE={value|int}
{% elif fan == 2 %}
{% set value = (255 - printer["gcode_macro PRINTER_PARAM"].fan2_min) / 255 * tmp %}
SET_PIN PIN=fan2 VALUE={value|int}
{% endif %}
{% endif %}
)";

static const char* kK1cM107Macro = R"(
{% set fans = printer["gcode_macro PRINTER_PARAM"].fans|int %}
{% if params.P is defined %}
{% if params.P|int < fans %}
SET_PIN PIN=fan{params.P|int} VALUE=0
{% else %}
SET_PIN PIN=fan0 VALUE=0
{% endif %}
{% else %}
SET_PIN PIN=fan0 VALUE=0
SET_PIN PIN=fan2 VALUE=0
{% endif %}
)";

static const char* kK1cM141Macro = R"(
{% if 'S' in params|upper %}
{% if printer["temperature_fan chamber_fan"].speed > 0.0 %}
SET_PIN PIN=fan1 VALUE=255
{% else %}
SET_PIN PIN=fan1 VALUE=0
{% endif %}
{% if params.S|int > 0 %}
SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET={params.S|default(35)}
{% else %}
SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET=35
{% endif %}
{% endif %}
)";

TEST_CASE("MacroFanAnalyzer: extracts fan indices from M106 macro", "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m106"]["gcode"] = kK1cM106Macro;
    config["gcode_macro m107"]["gcode"] = kK1cM107Macro;

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("detects fan0 from SET_PIN PIN=fan0") {
        REQUIRE(result.fan_indices.count("output_pin fan0") == 1);
        REQUIRE(result.fan_indices["output_pin fan0"] == 0);
    }

    SECTION("detects fan2 from SET_PIN PIN=fan2") {
        REQUIRE(result.fan_indices.count("output_pin fan2") == 1);
        REQUIRE(result.fan_indices["output_pin fan2"] == 2);
    }
}

TEST_CASE("MacroFanAnalyzer: extracts role hints from M141 macro", "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m141"]["gcode"] = kK1cM141Macro;

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("fan1 referenced in M141 gets chamber exhaust role hint") {
        REQUIRE(result.role_hints.count("output_pin fan1") == 1);
        REQUIRE(result.role_hints["output_pin fan1"] == "Chamber Exhaust");
    }
}

TEST_CASE("MacroFanAnalyzer: handles missing macros gracefully", "[macro_fan_analyzer]") {
    json config; // empty

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("no fan indices") {
        REQUIRE(result.fan_indices.empty());
    }

    SECTION("no role hints") {
        REQUIRE(result.role_hints.empty());
    }
}

TEST_CASE("MacroFanAnalyzer: handles malformed gcode gracefully", "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m106"]["gcode"] = "this is not real gcode";

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("no fan indices from garbage") {
        REQUIRE(result.fan_indices.empty());
    }
}
