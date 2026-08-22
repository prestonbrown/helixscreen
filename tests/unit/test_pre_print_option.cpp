// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pre_print_option.cpp
 * @brief Phase 1 tests for the pre-print-options framework.
 *
 * Covers JSON parsing for all four strategy variants, error handling,
 * option-set sorting, lookup, and rendering helpers.
 */

#include "pre_print_option.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

// ============================================================================
// parse_pre_print_option — strategy variants
// ============================================================================

TEST_CASE("parse_pre_print_option parses MacroParam strategy", "[pre_print_option][parse]") {
    json j = {{"id", "bed_mesh"},
              {"label_key", "pre_print_option.bed_mesh.label"},
              {"description_key", "pre_print_option.bed_mesh.description"},
              {"icon", ""},
              {"category", "mechanical"},
              {"order", 10},
              {"default_enabled", true},
              {"strategy", "macro_param"},
              {"param_name", "BED_MESH"},
              {"enable_value", "1"},
              {"skip_value", "0"},
              {"default_value", "1"}};

    auto parsed = parse_pre_print_option(j);
    REQUIRE(parsed.has_value());

    CHECK(parsed->id == "bed_mesh");
    CHECK(parsed->label_key == "pre_print_option.bed_mesh.label");
    CHECK(parsed->description_key == "pre_print_option.bed_mesh.description");
    CHECK(parsed->icon == "");
    CHECK(parsed->category == PrePrintCategory::Mechanical);
    CHECK(parsed->order == 10);
    CHECK(parsed->default_enabled == true);
    CHECK(parsed->strategy_kind == PrePrintStrategyKind::MacroParam);

    REQUIRE(std::holds_alternative<PrePrintStrategyMacroParam>(parsed->strategy));
    const auto& p = std::get<PrePrintStrategyMacroParam>(parsed->strategy);
    CHECK(p.param_name == "BED_MESH");
    CHECK(p.enable_value == "1");
    CHECK(p.skip_value == "0");
    CHECK(p.default_value == "1");
}

TEST_CASE("parse_pre_print_option parses PreStartGcode strategy", "[pre_print_option][parse]") {
    json j = {{"id", "ai_detect"},
              {"category", "monitoring"},
              {"order", 10},
              {"default_enabled", false},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "LOAD_AI_RUN SWITCH={value}"}};

    auto parsed = parse_pre_print_option(j);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "ai_detect");
    CHECK(parsed->category == PrePrintCategory::Monitoring);
    CHECK(parsed->strategy_kind == PrePrintStrategyKind::PreStartGcode);

    REQUIRE(std::holds_alternative<PrePrintStrategyPreStartGcode>(parsed->strategy));
    const auto& p = std::get<PrePrintStrategyPreStartGcode>(parsed->strategy);
    CHECK(p.gcode_template == "LOAD_AI_RUN SWITCH={value}");
}

TEST_CASE("parse_pre_print_option parses QueueAheadJob strategy", "[pre_print_option][parse]") {
    json j = {{"id", "pa_cal"},
              {"category", "quality"},
              {"strategy", "queue_ahead_job"},
              {"job_path", "/calibration/pa_cal.gcode"}};

    auto parsed = parse_pre_print_option(j);
    REQUIRE(parsed.has_value());
    CHECK(parsed->category == PrePrintCategory::Quality);
    CHECK(parsed->strategy_kind == PrePrintStrategyKind::QueueAheadJob);

    REQUIRE(std::holds_alternative<PrePrintStrategyQueueAheadJob>(parsed->strategy));
    const auto& p = std::get<PrePrintStrategyQueueAheadJob>(parsed->strategy);
    CHECK(p.job_path == "/calibration/pa_cal.gcode");
}

TEST_CASE("parse_pre_print_option parses RuntimeCommand strategy", "[pre_print_option][parse]") {
    json j = {{"id", "waste_chute"},
              {"category", "monitoring"},
              {"strategy", "runtime_command"},
              {"command_enabled", "WASTE_CHUTE_OPEN"},
              {"command_disabled", "WASTE_CHUTE_CLOSE"}};

    auto parsed = parse_pre_print_option(j);
    REQUIRE(parsed.has_value());
    CHECK(parsed->strategy_kind == PrePrintStrategyKind::RuntimeCommand);

    REQUIRE(std::holds_alternative<PrePrintStrategyRuntimeCommand>(parsed->strategy));
    const auto& p = std::get<PrePrintStrategyRuntimeCommand>(parsed->strategy);
    CHECK(p.command_enabled == "WASTE_CHUTE_OPEN");
    CHECK(p.command_disabled == "WASTE_CHUTE_CLOSE");
}

// ============================================================================
// parse_pre_print_option — error paths
// ============================================================================

TEST_CASE("parse_pre_print_option rejects unknown strategy string", "[pre_print_option][parse]") {
    json j = {{"id", "weird"}, {"strategy", "telepathy"}, {"param_name", "X"}};

    auto parsed = parse_pre_print_option(j);
    CHECK_FALSE(parsed.has_value());
}

TEST_CASE("parse_pre_print_option rejects missing required fields", "[pre_print_option][parse]") {
    SECTION("missing id") {
        json j = {{"strategy", "macro_param"}, {"param_name", "X"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("missing strategy") {
        json j = {{"id", "x"}, {"param_name", "X"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("MacroParam missing param_name") {
        json j = {{"id", "x"}, {"strategy", "macro_param"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("MacroParam missing enable_value") {
        // Empty enable_value would render as "PARAM=" (no value) — silent
        // garbage that the macro will misparse. Reject at parse time.
        json j = {
            {"id", "x"}, {"strategy", "macro_param"}, {"param_name", "PARAM"}, {"skip_value", "0"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("MacroParam missing skip_value") {
        json j = {{"id", "x"},
                  {"strategy", "macro_param"},
                  {"param_name", "PARAM"},
                  {"enable_value", "1"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("MacroParam empty enable_value rejected") {
        json j = {{"id", "x"},
                  {"strategy", "macro_param"},
                  {"param_name", "PARAM"},
                  {"enable_value", ""},
                  {"skip_value", "0"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }

    SECTION("PreStartGcode missing gcode_template") {
        json j = {{"id", "x"}, {"strategy", "pre_start_gcode"}};
        CHECK_FALSE(parse_pre_print_option(j).has_value());
    }
}

// ============================================================================
// parse_pre_print_option_set — set-level behavior
// ============================================================================

TEST_CASE("parse_pre_print_option_set yields empty set for empty options array",
          "[pre_print_option][set]") {
    json j = {{"macro_name", "START_PRINT"}, {"options", json::array()}};

    auto set = parse_pre_print_option_set(j);
    CHECK(set.options.empty());
    // Note: empty() checks macro_name, setup_gcode, AND options — this
    // set has a macro_name so it's not empty.
    CHECK_FALSE(set.empty());

    SECTION("totally empty input is empty()") {
        auto totally_empty = parse_pre_print_option_set(json::object());
        CHECK(totally_empty.empty());
    }
}

TEST_CASE("parse_pre_print_option_set sorts by (category, order)", "[pre_print_option][set]") {
    // Intentionally out-of-order input: monitoring first, then mixed orders.
    json j = {{"macro_name", "START_PRINT"},
              {"options", json::array({
                              {{"id", "ai_detect"},
                               {"category", "monitoring"},
                               {"order", 10},
                               {"strategy", "pre_start_gcode"},
                               {"gcode_template", "LOAD_AI_RUN SWITCH={value}"}},
                              {{"id", "qgl"},
                               {"category", "mechanical"},
                               {"order", 20},
                               {"strategy", "macro_param"},
                               {"param_name", "QGL"},
                               {"enable_value", "1"},
                               {"skip_value", "0"}},
                              {{"id", "nozzle_clean"},
                               {"category", "quality"},
                               {"order", 10},
                               {"strategy", "macro_param"},
                               {"param_name", "NOZZLE_CLEAN"},
                               {"enable_value", "1"},
                               {"skip_value", "0"}},
                              {{"id", "bed_mesh"},
                               {"category", "mechanical"},
                               {"order", 10},
                               {"strategy", "macro_param"},
                               {"param_name", "BED_MESH"},
                               {"enable_value", "1"},
                               {"skip_value", "0"}},
                          })}};

    auto set = parse_pre_print_option_set(j);
    REQUIRE(set.options.size() == 4);

    // Expected order: mechanical/10 (bed_mesh), mechanical/20 (qgl),
    //                 quality/10 (nozzle_clean), monitoring/10 (ai_detect)
    CHECK(set.options[0].id == "bed_mesh");
    CHECK(set.options[1].id == "qgl");
    CHECK(set.options[2].id == "nozzle_clean");
    CHECK(set.options[3].id == "ai_detect");
}

TEST_CASE("PrePrintOptionSet::find locates options by id", "[pre_print_option][set]") {
    json j = {{"macro_name", "START_PRINT"},
              {"options", json::array({{{"id", "bed_mesh"},
                                        {"category", "mechanical"},
                                        {"strategy", "macro_param"},
                                        {"param_name", "BED_MESH"},
                                        {"enable_value", "1"},
                                        {"skip_value", "0"}}})}};

    auto set = parse_pre_print_option_set(j);

    CHECK(set.find("missing") == nullptr);
    const auto* found = set.find("bed_mesh");
    REQUIRE(found != nullptr);
    CHECK(found->id == "bed_mesh");
}

// ============================================================================
// Rendering helpers
// ============================================================================

TEST_CASE("render_macro_param emits KEY=value for both states", "[pre_print_option][render]") {
    json j = {{"id", "bed_mesh"},
              {"strategy", "macro_param"},
              {"param_name", "BED_MESH"},
              {"enable_value", "1"},
              {"skip_value", "0"}};

    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    CHECK(render_macro_param(*opt, true) == "BED_MESH=1");
    CHECK(render_macro_param(*opt, false) == "BED_MESH=0");
}

TEST_CASE("pre_start_gcode defaults to emitting when disabled", "[pre_print_option][render]") {
    // Existing behaviour: one template serves both states via {value}, e.g.
    // "LOAD_AI_RUN SWITCH=0". Options that omit the field must keep it.
    json j = {{"id", "ai_detect"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "LOAD_AI_RUN SWITCH={value}"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());
    const auto* p = std::get_if<PrePrintStrategyPreStartGcode>(&opt->strategy);
    REQUIRE(p != nullptr);
    CHECK(p->emit_when_disabled);
}

TEST_CASE("pre_start_gcode can opt out of emitting when disabled", "[pre_print_option][render]") {
    // A macro with no on/off switch (Creality BED_MESH_CALIBRATE_START_PRINT)
    // can only be expressed as "run it, or send nothing at all".
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "BED_MESH_CALIBRATE_START_PRINT"},
              {"emit_when_disabled", false}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());
    const auto* p = std::get_if<PrePrintStrategyPreStartGcode>(&opt->strategy);
    REQUIRE(p != nullptr);
    CHECK_FALSE(p->emit_when_disabled);
}

TEST_CASE("render_pre_start_gcode interpolates {file}", "[pre_print_option][render]") {
    // The Creality mesh macro takes GCODE_FILE and trims the sweep to the
    // object's footprint; without it the printer probes the whole bed, which
    // on a K2 Plus is ~390s instead of ~120s.
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='{file}'"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    PreStartGcodeContext ctx;
    ctx.filename = "part.gcode";
    CHECK(render_pre_start_gcode(*opt, true, ctx) ==
          "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='part.gcode'");
}

TEST_CASE("render_pre_start_gcode interpolates the job temperatures",
          "[pre_print_option][render]") {
    // Creality's mesh macro falls back to custom_macro.default_bed_temp (50C)
    // when BED_TEMP is absent, so it cools down from print temp, meshes at the
    // wrong temperature, and START_PRINT then reheats. Observed on a K2 Plus:
    // a 100C bed dropped to a 50C target and sat there for 20 minutes.
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='{file}' "
                                 "BED_TEMP={bed_temp} EXTRUDER_TEMP={extruder_temp}"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    PreStartGcodeContext ctx;
    ctx.filename = "part.gcode";
    ctx.bed_temp = 105;
    ctx.extruder_temp = 260;

    CHECK(render_pre_start_gcode(*opt, true, ctx) ==
          "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='part.gcode' BED_TEMP=105 "
          "EXTRUDER_TEMP=260");
}

TEST_CASE("render_pre_start_gcode renders unknown temperatures as zero",
          "[pre_print_option][render]") {
    // 0 lets the firmware macro apply its own default rather than receiving a
    // literal placeholder it would choke on.
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "M BED_TEMP={bed_temp} EXTRUDER_TEMP={extruder_temp}"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());
    CHECK(render_pre_start_gcode(*opt, true) == "M BED_TEMP=0 EXTRUDER_TEMP=0");
}

TEST_CASE("render_pre_start_gcode {?ext} segment emits only with a known extruder temp",
          "[pre_print_option][render]") {
    // Creality K1 firmware reads CX_ROUGH_G28's extruder temperature through
    // Python get_float(minval=180), which rejects EXTRUDER_TEMP=0 as out of
    // range — an unknown extruder temp must omit the parameter entirely (the
    // firmware then applies its own default), unlike the Jinja-guarded K2
    // macros whose `(params.EXTRUDER_TEMP|float)` test treats 0 as unset.
    // The bed parameter carries no marker: get_float(minval=0) accepts it,
    // and a bed target of 0 is a real cold-bed configuration that must pass
    // through rather than read as "unknown".
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template",
               "CX_ROUGH_G28{?ext} EXTRUDER_TEMP={extruder_temp}{/?} BED_TEMP={bed_temp}\n"
               "CX_NOZZLE_CLEAR"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    SECTION("extruder temp known keeps the segment") {
        PreStartGcodeContext ctx;
        ctx.filename = "part.gcode";
        ctx.bed_temp = 55;
        ctx.extruder_temp = 220;
        CHECK(render_pre_start_gcode(*opt, true, ctx) ==
              "CX_ROUGH_G28 EXTRUDER_TEMP=220 BED_TEMP=55\nCX_NOZZLE_CLEAR");
    }
    SECTION("unknown extruder temp drops only the parameter") {
        CHECK(render_pre_start_gcode(*opt, true) == "CX_ROUGH_G28 BED_TEMP=0\nCX_NOZZLE_CLEAR");
    }
    SECTION("known bed with unknown extruder keeps the bed value") {
        PreStartGcodeContext ctx;
        ctx.bed_temp = 55;
        CHECK(render_pre_start_gcode(*opt, true, ctx) ==
              "CX_ROUGH_G28 BED_TEMP=55\nCX_NOZZLE_CLEAR");
    }
    SECTION("cold bed with known extruder passes the zero through") {
        PreStartGcodeContext ctx;
        ctx.bed_temp = 0;
        ctx.extruder_temp = 220;
        CHECK(render_pre_start_gcode(*opt, true, ctx) ==
              "CX_ROUGH_G28 EXTRUDER_TEMP=220 BED_TEMP=0\nCX_NOZZLE_CLEAR");
    }
}

TEST_CASE("render_pre_start_gcode leaves {file} empty when no filename is supplied",
          "[pre_print_option][render]") {
    json j = {{"id", "bed_mesh"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='{file}'"}};
    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    // Degrades to a full-bed mesh rather than emitting a literal "{file}".
    CHECK(render_pre_start_gcode(*opt, true) == "BED_MESH_CALIBRATE_START_PRINT GCODE_FILE=''");
}

TEST_CASE("render_pre_start_gcode interpolates {value}", "[pre_print_option][render]") {
    json j = {{"id", "ai_detect"},
              {"strategy", "pre_start_gcode"},
              {"gcode_template", "LOAD_AI_RUN SWITCH={value}"}};

    auto opt = parse_pre_print_option(j);
    REQUIRE(opt.has_value());

    CHECK(render_pre_start_gcode(*opt, true) == "LOAD_AI_RUN SWITCH=1");
    CHECK(render_pre_start_gcode(*opt, false) == "LOAD_AI_RUN SWITCH=0");
}
