// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/load_filament_expression_default.h"
#include "../ui_test_utils.h"
#include "favorite_macro_widget.h"
#include "lvgl.h"

#include "../catch_amalgamated.hpp"

using helix::macro_param_placeholder;
using helix::MacroDefaultKind;
using helix::MacroParam;
using helix::parse_macro_params;

// ============================================================================
// parse_macro_params Tests
// ============================================================================

TEST_CASE("parse_macro_params - no params", "[macro_params]") {
    auto result = parse_macro_params("G28\nG1 X0 Y0 Z5");
    REQUIRE(result.empty());
}

TEST_CASE("parse_macro_params - empty string", "[macro_params]") {
    auto result = parse_macro_params("");
    REQUIRE(result.empty());
}

TEST_CASE("parse_macro_params - dot access", "[macro_params]") {
    auto result = parse_macro_params("{% set extruder_temp = params.EXTRUDER_TEMP %}\n"
                                     "{% set bed_temp = params.BED_TEMP %}");

    REQUIRE(result.size() == 2);
    CHECK(result[0].name == "EXTRUDER_TEMP");
    CHECK(result[1].name == "BED_TEMP");
}

TEST_CASE("parse_macro_params - bracket access single quotes", "[macro_params]") {
    auto result = parse_macro_params("{% set temp = params['EXTRUDER'] %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "EXTRUDER");
}

TEST_CASE("parse_macro_params - bracket access double quotes", "[macro_params]") {
    auto result = parse_macro_params(R"({% set temp = params["BED_TEMP"] %})");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "BED_TEMP");
}

TEST_CASE("parse_macro_params - with default values", "[macro_params]") {
    auto result = parse_macro_params("{% set extruder_temp = params.EXTRUDER_TEMP|default(220) %}\n"
                                     "{% set bed_temp = params.BED_TEMP|default(60) %}");

    REQUIRE(result.size() == 2);
    CHECK(result[0].name == "EXTRUDER_TEMP");
    CHECK(result[0].default_value == "220");
    CHECK(result[1].name == "BED_TEMP");
    CHECK(result[1].default_value == "60");
}

TEST_CASE("parse_macro_params - default with space before pipe", "[macro_params]") {
    auto result = parse_macro_params("{% set speed = params.SPEED | default(100) %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "SPEED");
    CHECK(result[0].default_value == "100");
}

TEST_CASE("parse_macro_params - string default with quotes", "[macro_params]") {
    auto result = parse_macro_params(R"({% set material = params.MATERIAL|default('PLA') %})");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "MATERIAL");
    CHECK(result[0].default_value == "PLA");
}

TEST_CASE("parse_macro_params - deduplication", "[macro_params]") {
    auto result = parse_macro_params("{% set temp = params.TEMP %}\n"
                                     "{% if params.TEMP > 200 %}\n"
                                     "M104 S{params.TEMP}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "TEMP");
}

TEST_CASE("parse_macro_params - mixed dot and bracket access", "[macro_params]") {
    auto result = parse_macro_params("{% set temp = params.EXTRUDER_TEMP|default(200) %}\n"
                                     "{% set bed = params['BED_TEMP']|default(60) %}\n"
                                     R"({% set material = params["MATERIAL"] %})");

    REQUIRE(result.size() == 3);

    // Check all names present (order may vary between dot and bracket)
    std::set<std::string> names;
    for (const auto& p : result) {
        names.insert(p.name);
    }
    CHECK(names.count("EXTRUDER_TEMP") == 1);
    CHECK(names.count("BED_TEMP") == 1);
    CHECK(names.count("MATERIAL") == 1);
}

TEST_CASE("parse_macro_params - cross-syntax dedup", "[macro_params]") {
    // Same param referenced via both dot and bracket
    auto result = parse_macro_params("{% set t = params.TEMP %}\n"
                                     "{% set t2 = params['TEMP'] %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "TEMP");
}

TEST_CASE("parse_macro_params - case normalization", "[macro_params]") {
    auto result = parse_macro_params("{% set t = params.temp %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "TEMP");
}

TEST_CASE("parse_macro_params - real-world PRINT_START", "[macro_params]") {
    std::string gcode = R"(
{% set extruder_temp = params.EXTRUDER_TEMP|default(200)|float %}
{% set bed_temp = params.BED_TEMP|default(60)|float %}
{% set chamber_temp = params.CHAMBER_TEMP|default(0)|float %}
{% set filament_type = params.FILAMENT_TYPE|default('PLA') %}
M140 S{bed_temp}
M104 S{extruder_temp}
{% if chamber_temp > 0 %}
  M141 S{chamber_temp}
{% endif %}
)";

    auto result = parse_macro_params(gcode);
    REQUIRE(result.size() == 4);

    // Find specific params
    std::map<std::string, std::string> param_map;
    for (const auto& p : result) {
        param_map[p.name] = p.default_value;
    }

    CHECK(param_map["EXTRUDER_TEMP"] == "200");
    CHECK(param_map["BED_TEMP"] == "60");
    CHECK(param_map["CHAMBER_TEMP"] == "0");
    CHECK(param_map["FILAMENT_TYPE"] == "PLA");
}

// ============================================================================
// 'in params' conditional pattern tests
// ============================================================================

TEST_CASE("parse_macro_params - single-quoted in params", "[macro_params]") {
    auto result = parse_macro_params("{% if 'PURGE_LINE' in params %}\n"
                                     "  PURGE\n"
                                     "{% endif %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "PURGE_LINE");
    CHECK(result[0].default_value.empty());
}

TEST_CASE("parse_macro_params - double-quoted in params", "[macro_params]") {
    auto result = parse_macro_params(R"({% if "BED_TEMP" in params %})");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "BED_TEMP");
}

TEST_CASE("parse_macro_params - not in params", "[macro_params]") {
    auto result = parse_macro_params("{% if 'SKIP_HEAT' not in params %}\n"
                                     "  M104 S200\n"
                                     "{% endif %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "SKIP_HEAT");
}

TEST_CASE("parse_macro_params - in params dedup with dot access", "[macro_params]") {
    // Dot access finds BED_TEMP with default; conditional should not add duplicate
    auto result = parse_macro_params("{% set bed = params.BED_TEMP|default(60) %}\n"
                                     "{% if 'BED_TEMP' in params %}\n"
                                     "  ; override\n"
                                     "{% endif %}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "BED_TEMP");
    CHECK(result[0].default_value == "60"); // Keeps dot-access default, not clobbered
}

TEST_CASE("parse_macro_params - in params mixed with dot access", "[macro_params]") {
    std::string gcode = R"(
{% set bed_temp = params.BED_TEMP|default(60)|float %}
{% if 'PURGE_LINE' in params %}
  PURGE
{% endif %}
{% if "CHAMBER_TEMP" in params %}
  M141 S{params.CHAMBER_TEMP}
{% endif %}
)";

    auto result = parse_macro_params(gcode);
    REQUIRE(result.size() == 3);

    std::map<std::string, std::string> param_map;
    for (const auto& p : result) {
        param_map[p.name] = p.default_value;
    }

    CHECK(param_map.count("BED_TEMP") == 1);
    CHECK(param_map["BED_TEMP"] == "60");
    CHECK(param_map.count("PURGE_LINE") == 1);
    CHECK(param_map["PURGE_LINE"].empty());
    CHECK(param_map.count("CHAMBER_TEMP") == 1);
}

// ============================================================================
// parse_raw_macro_params Tests
// ============================================================================

TEST_CASE("parse_raw_macro_params - basic", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("TEMP=200 SPEED=50");
    REQUIRE(result.size() == 2);
    CHECK(result["TEMP"] == "200");
    CHECK(result["SPEED"] == "50");
}

TEST_CASE("parse_raw_macro_params - empty input", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("");
    CHECK(result.empty());
}

TEST_CASE("parse_raw_macro_params - whitespace only", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("   ");
    CHECK(result.empty());
}

TEST_CASE("parse_raw_macro_params - skips missing equals", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("JUSTVALUE TEMP=200");
    REQUIRE(result.size() == 1);
    CHECK(result["TEMP"] == "200");
}

TEST_CASE("parse_raw_macro_params - reports the tokens it could not read", "[macro_params]") {
    std::vector<std::string> rejected;
    auto result = helix::parse_raw_macro_params("JUSTVALUE temp=200 =5", &rejected);
    CHECK(result == std::map<std::string, std::string>{{"TEMP", "200"}});
    CHECK(rejected == std::vector<std::string>{"JUSTVALUE", "=5"});
}

TEST_CASE("format_raw_macro_params - parses back to the same values", "[macro_params]") {
    const std::map<std::string, std::string> values{{"PURGE", "1"}, {"LANE", "lane2"}};
    CHECK(helix::format_raw_macro_params(values) == "LANE=lane2 PURGE=1");
    CHECK(helix::parse_raw_macro_params(helix::format_raw_macro_params(values)) == values);
    CHECK(helix::format_raw_macro_params({}).empty());
}

TEST_CASE("parse_raw_macro_params - extra whitespace", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("  TEMP=200   SPEED=50  ");
    REQUIRE(result.size() == 2);
    CHECK(result["TEMP"] == "200");
    CHECK(result["SPEED"] == "50");
}

TEST_CASE("parse_raw_macro_params - uppercases keys", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("temp=200 speed=50");
    REQUIRE(result.size() == 2);
    CHECK(result["TEMP"] == "200");
    CHECK(result["SPEED"] == "50");
}

TEST_CASE("parse_raw_macro_params - preserves value case", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("NAME=MyVariable");
    REQUIRE(result.size() == 1);
    CHECK(result["NAME"] == "MyVariable");
}

TEST_CASE("parse_raw_macro_params - skips equals at start", "[macro_params]") {
    auto result = helix::parse_raw_macro_params("=value TEMP=200");
    REQUIRE(result.size() == 1);
    CHECK(result["TEMP"] == "200");
}

TEST_CASE("parse_macro_params - no default value", "[macro_params]") {
    auto result = parse_macro_params("{% set temp = params.TEMP %}\n"
                                     "M104 S{temp}");

    REQUIRE(result.size() == 1);
    CHECK(result[0].name == "TEMP");
    CHECK(result[0].default_value.empty());
}

// ============================================================================
// Default kinds, and the hint an empty field shows
// ============================================================================

namespace {

MacroParam only_param(const std::string& gcode) {
    auto result = parse_macro_params(gcode);
    REQUIRE(result.size() == 1);
    return result[0];
}

} // namespace

TEST_CASE("parse_macro_params - a default read from printer state is an expression",
          "[macro_params][macro_defaults]") {
    lv_init_safe();
    auto result = parse_macro_params(helix::test::kLoadFilamentExpressionDefault);

    REQUIRE(result.size() == 1);
    const MacroParam& param = result[0];
    CHECK(param.name == "EXTRUDER_TEMP");
    CHECK(param.default_kind == MacroDefaultKind::Expression);
    // Template source means nothing to the person at the screen, and it is not a
    // value they could type back in.
    CHECK(macro_param_placeholder(param) != "global.extruder_temp.loading");
    CHECK(macro_param_placeholder(param) == std::string(lv_tr("Printer default")));
}

TEST_CASE("macro_param_placeholder - a literal default is shown as written",
          "[macro_params][macro_defaults]") {
    lv_init_safe();

    SECTION("an integer") {
        MacroParam p = only_param("{% set t = params.TEMP|default(220)|int %}");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(macro_param_placeholder(p) == "220");
    }
    SECTION("a signed decimal") {
        MacroParam p = only_param("{% set z = params.Z_ADJUST|default(-0.5)|float %}");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(macro_param_placeholder(p) == "-0.5");
    }
    SECTION("a single-quoted string") {
        MacroParam p = only_param(R"({% set m = params.MATERIAL|default('PLA') %})");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(macro_param_placeholder(p) == "PLA");
    }
    SECTION("a double-quoted string") {
        MacroParam p = only_param(R"({% set m = params.MATERIAL|default("PETG") %})");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(macro_param_placeholder(p) == "PETG");
    }
    SECTION("a boolean") {
        MacroParam p = only_param("{% set w = params.WIPE|default(false) %}");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(macro_param_placeholder(p) == "false");
    }
}

TEST_CASE("macro_param_placeholder - no default shows the parameter name",
          "[macro_params][macro_defaults]") {
    lv_init_safe();
    MacroParam p = only_param("{% set t = params.TEMP %}");

    CHECK(p.default_kind == MacroDefaultKind::Absent);
    CHECK(macro_param_placeholder(p) == "TEMP");
}

TEST_CASE("parse_macro_params - a default containing parentheses is read whole",
          "[macro_params][macro_defaults]") {
    SECTION("a parenthesised expression") {
        MacroParam p = only_param(
            "{% set s = params.SPEED|default((printer.toolhead.max_velocity * 60))|int %}");
        CHECK(p.default_value == "(printer.toolhead.max_velocity * 60)");
        CHECK(p.default_kind == MacroDefaultKind::Expression);
    }
    SECTION("a parenthesis inside a quoted string") {
        MacroParam p = only_param(R"({% set m = params.MSG|default('done (ok)') %})");
        CHECK(p.default_value == "done (ok)");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
    }
}

TEST_CASE("parse_macro_params - quotes at both ends do not make a string literal",
          "[macro_params][macro_defaults]") {
    MacroParam p = only_param(R"({% set n = params.NAME|default('a' ~ suffix ~ 'b') %})");

    CHECK(p.default_kind == MacroDefaultKind::Expression);
    CHECK(p.default_value == "'a' ~ suffix ~ 'b'");
}

// ============================================================================
// Template shapes at the edges of the default filter
// ============================================================================

TEST_CASE("parse_macro_params - a template that ends at a parameter name",
          "[macro_params][macro_defaults]") {
    SECTION("dot access") {
        MacroParam p = only_param("M117 {params.MSG");
        CHECK(p.name == "MSG");
        CHECK(p.default_kind == MacroDefaultKind::Absent);
        CHECK(p.default_value.empty());
    }
    SECTION("bracket access") {
        MacroParam p = only_param("M117 {params['MSG']");
        CHECK(p.name == "MSG");
        CHECK(p.default_kind == MacroDefaultKind::Absent);
    }
}

TEST_CASE("parse_macro_params - a default whose parenthesis never closes is no default",
          "[macro_params][macro_defaults]") {
    MacroParam p = only_param("{% set t = params.TEMP|default(220 %}\nM109 S{t}");

    CHECK(p.name == "TEMP");
    CHECK(p.default_kind == MacroDefaultKind::Absent);
    CHECK(p.default_value.empty());
}

TEST_CASE("parse_macro_params - an escaped quote does not end a string default",
          "[macro_params][macro_defaults]") {
    SECTION("single quotes") {
        MacroParam p = only_param(R"GC({% set m = params.MSG|default('it\'s done (ok)') %})GC");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(p.default_value == R"GC(it\'s done (ok))GC");
    }
    SECTION("double quotes") {
        MacroParam p = only_param(R"GC({% set m = params.MSG|default("say \"hi\"") %})GC");
        CHECK(p.default_kind == MacroDefaultKind::Literal);
        CHECK(p.default_value == R"GC(say \"hi\")GC");
    }
}

TEST_CASE("parse_macro_params - Jinja's capitalised None and True are literals",
          "[macro_params][macro_defaults]") {
    MacroParam none = only_param("{% set x = params.X|default(None) %}");
    CHECK(none.default_kind == MacroDefaultKind::Literal);
    CHECK(none.default_value == "None");

    MacroParam yes = only_param("{% set y = params.Y|default(True) %}");
    CHECK(yes.default_kind == MacroDefaultKind::Literal);
    CHECK(yes.default_value == "True");
}

TEST_CASE("parse_macro_params - whitespace before the default's parenthesis",
          "[macro_params][macro_defaults]") {
    MacroParam p = only_param("{% set t = params.TEMP | default (5) | int %}");

    CHECK(p.default_kind == MacroDefaultKind::Literal);
    CHECK(p.default_value == "5");
}

TEST_CASE("parse_macro_params - a default that reads another parameter yields both",
          "[macro_params][macro_defaults]") {
    auto result = parse_macro_params("{% set a = params.A|default(params.B|default(1)) %}");

    REQUIRE(result.size() == 2);
    // A's default is computed on the printer from B, so it is not a value to show.
    CHECK(result[0].name == "A");
    CHECK(result[0].default_kind == MacroDefaultKind::Expression);
    CHECK(result[0].default_value == "params.B|default(1)");
    CHECK(result[1].name == "B");
    CHECK(result[1].default_kind == MacroDefaultKind::Literal);
    CHECK(result[1].default_value == "1");
}
