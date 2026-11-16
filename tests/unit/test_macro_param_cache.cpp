// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_cache.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::CachedMacroInfo;
using helix::find_printer_stop_call;
using helix::MacroParamCache;
using helix::MacroParamKnowledge;

// ============================================================================
// MacroParamCache Tests
// ============================================================================

TEST_CASE("MacroParamCache populate categorizes params correctly", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro clean_nozzle"] = {
        {"gcode", "{% set TEMP = params.TEMP|default(240)|int %}\nG1 E10"}};
    config["gcode_macro cancel_print"] = {{"gcode", "TURN_OFF_HEATERS\nBASE_CANCEL"}};
    config["gcode_macro pause"] = {{"gcode", "SAVE_GCODE_STATE\nBASE_PAUSE"}};

    std::unordered_set<std::string> known_macros = {"CLEAN_NOZZLE", "CANCEL_PRINT", "PAUSE",
                                                    "GET_VARIABLE"};

    cache.populate_from_configfile(config, known_macros);

    SECTION("macro with params is KNOWN_PARAMS") {
        auto info = cache.get("CLEAN_NOZZLE");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
        REQUIRE(info.params.size() == 1);
        REQUIRE(info.params[0].name == "TEMP");
        REQUIRE(info.params[0].default_value == "240");
    }

    SECTION("macro without params is KNOWN_NO_PARAMS") {
        auto info = cache.get("CANCEL_PRINT");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
        REQUIRE(info.params.empty());
    }

    SECTION("macro without params (pause) is KNOWN_NO_PARAMS") {
        auto info = cache.get("PAUSE");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
        REQUIRE(info.params.empty());
    }

    SECTION("known macro not in configfile is UNKNOWN") {
        auto info = cache.get("GET_VARIABLE");
        REQUIRE(info.knowledge == MacroParamKnowledge::UNKNOWN);
        REQUIRE(info.params.empty());
    }
}

TEST_CASE("MacroParamCache get returns UNKNOWN for totally unknown macro", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    auto info = cache.get("NONEXISTENT_MACRO");
    REQUIRE(info.knowledge == MacroParamKnowledge::UNKNOWN);
    REQUIRE(info.params.empty());
}

TEST_CASE("MacroParamCache clear resets state", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro test_macro"] = {{"gcode", "{% set X = params.X|default(0) %}"}};

    cache.populate_from_configfile(config, {});

    REQUIRE(cache.get("TEST_MACRO").knowledge == MacroParamKnowledge::KNOWN_PARAMS);

    cache.clear();

    REQUIRE(cache.get("TEST_MACRO").knowledge == MacroParamKnowledge::UNKNOWN);
}

TEST_CASE("MacroParamCache case-insensitive lookup", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro print_start"] = {{"gcode", "{% set BED = params.BED_TEMP|default(60) %}"}};

    cache.populate_from_configfile(config, {});

    // All case variations should work
    REQUIRE(cache.get("PRINT_START").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(cache.get("print_start").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(cache.get("Print_Start").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
}

TEST_CASE("MacroParamCache uppercase configfile keys match known_macros", "[macro_param_cache]") {
    // Real Klipper returns uppercase keys like "gcode_macro CLEAN_NOZZLE"
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro CLEAN_NOZZLE"] = {{"gcode", "G1 X{start_x}"}, {"variable_start_x", "265"}};

    std::unordered_set<std::string> known_macros = {"CLEAN_NOZZLE"};
    cache.populate_from_configfile(config, known_macros);

    // Must NOT be UNKNOWN — the uppercase configfile key must match
    // variable_* fields are ignored (internal state, not user params)
    auto info = cache.get("CLEAN_NOZZLE");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
    REQUIRE(info.params.empty());
}

TEST_CASE("MacroParamCache multiple params extracted", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro start_print"] = {
        {"gcode", "{% set BED_TEMP = params.BED_TEMP|default(60)|float %}\n"
                  "{% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(200)|float %}\n"
                  "{% set CHAMBER_TEMP = params.CHAMBER_TEMP|default(0)|float %}\nG28"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("START_PRINT");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 3);
}

TEST_CASE("MacroParamCache handles missing gcode field", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro no_gcode"] = {{"description", "test"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("NO_GCODE");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
}

// ============================================================================
// variable_* fields are intentionally ignored (internal macro state)
// ============================================================================

TEST_CASE("MacroParamCache ignores variable_* fields", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro clean_nozzle"] = {{"gcode", "G1 X{start_x} Y{start_y}"},
                                          {"variable_start_x", "265"},
                                          {"variable_start_y", "298"},
                                          {"variable_wipe_qty", "4"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("CLEAN_NOZZLE");
    // No params.* references in template, variables ignored → no params
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
    REQUIRE(info.params.empty());
}

TEST_CASE("MacroParamCache params extracted but variables ignored", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro start_print"] = {{"gcode", "{% set BED = params.BED_TEMP|default(60) %}"},
                                         {"variable_idle_state", "false"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("START_PRINT");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    // Only the params.* reference, not the variable_*
    REQUIRE(info.params.size() == 1);
    CHECK(info.params[0].name == "BED_TEMP");
    CHECK(info.params[0].default_value == "60");
    CHECK_FALSE(info.params[0].is_variable);
}

TEST_CASE("MacroParamCache variable-only macro is KNOWN_NO_PARAMS", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro bedfanvars"] = {{"gcode", ""},
                                        {"variable_threshold", "100"},
                                        {"variable_fast", "0.6"},
                                        {"variable_slow", "0.2"}};

    cache.populate_from_configfile(config, {});

    // Variables are internal state, not user-facing params
    auto info = cache.get("BEDFANVARS");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
    REQUIRE(info.params.empty());
}

// ============================================================================
// Macros that stop the printer
// ============================================================================

TEST_CASE("find_printer_stop_call reads an action_emergency_stop call",
          "[macro_param_cache][printer_stop]") {
    SECTION("a double-quoted literal is the message") {
        const auto call =
            find_printer_stop_call("{action_emergency_stop(\"M729 is not supported\")}");
        CHECK(call.calls);
        CHECK(call.message == "M729 is not supported");
    }
    SECTION("a single-quoted literal with spacing is the message") {
        const auto call = find_printer_stop_call("G28\n{ action_emergency_stop( 'Stop here' ) }");
        CHECK(call.calls);
        CHECK(call.message == "Stop here");
    }
    SECTION("an argument Klipper has to evaluate is a call with no message") {
        CHECK(find_printer_stop_call("{action_emergency_stop(params.WHY)}").calls);
        CHECK(find_printer_stop_call("{action_emergency_stop(params.WHY)}").message.empty());
        const auto joined = find_printer_stop_call("{action_emergency_stop(\"Bad: \" ~ params.X)}");
        CHECK(joined.calls);
        CHECK(joined.message.empty());
        const auto escaped = find_printer_stop_call(R"({action_emergency_stop("say \"no\"")})");
        CHECK(escaped.calls);
        CHECK(escaped.message.empty());
    }
    SECTION("no argument is a call with no message") {
        const auto call = find_printer_stop_call("{action_emergency_stop()}");
        CHECK(call.calls);
        CHECK(call.message.empty());
    }
    SECTION("no call") {
        CHECK_FALSE(find_printer_stop_call("M117 hello\nG28").calls);
        CHECK_FALSE(find_printer_stop_call("{action_respond_info(\"x\")}").calls);
        CHECK_FALSE(find_printer_stop_call("{my_action_emergency_stop(\"x\")}").calls);
        CHECK_FALSE(find_printer_stop_call("{# action_emergency_stop is not used #}").calls);
        CHECK_FALSE(find_printer_stop_call("").calls);
    }
}

TEST_CASE("MacroParamCache records which macros stop the printer",
          "[macro_param_cache][printer_stop]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();
    REQUIRE_FALSE(cache.is_populated());

    nlohmann::json config;
    config["gcode_macro M729"] = {{"gcode", "{action_emergency_stop(\"M729 is not supported\")}"}};
    config["gcode_macro m8213"] = {{"gcode", "{action_emergency_stop(params.WHY)}"}};
    config["gcode_macro clean_nozzle"] = {{"gcode", "G1 E10"}};
    cache.populate_from_configfile(config, {});

    CHECK(cache.is_populated());
    CHECK(cache.get("M729").stops_printer);
    CHECK(cache.get("M729").stop_message == "M729 is not supported");
    CHECK(cache.get("M8213").stops_printer);
    CHECK(cache.get("M8213").stop_message.empty());
    CHECK_FALSE(cache.get("CLEAN_NOZZLE").stops_printer);
    CHECK(cache.printer_stop_commands() ==
          std::map<std::string, std::string>{{"M729", "M729 is not supported"}, {"M8213", ""}});

    cache.clear();
    CHECK_FALSE(cache.is_populated());
    CHECK(cache.printer_stop_commands().empty());
}

TEST_CASE("MacroParamCache generation moves whenever the macro set may have changed",
          "[macro_param_cache][printer_stop]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();
    const uint64_t cleared = cache.generation();

    nlohmann::json config;
    config["gcode_macro M729"] = {{"gcode", "{action_emergency_stop(\"no\")}"}};
    cache.populate_from_configfile(config, {});
    const uint64_t populated = cache.generation();
    CHECK(populated != cleared);
    CHECK(cache.is_populated());

    SECTION("a clear moves it again") {
        cache.clear();
        CHECK(cache.generation() != populated);
    }

    SECTION("a second populate moves it, so an answer from the first is not current") {
        cache.populate_from_configfile(config, {});
        CHECK(cache.generation() != populated);
    }

    SECTION("a configfile that is not an object populates nothing and still moves it") {
        cache.populate_from_configfile(nlohmann::json("not an object"), {});
        CHECK_FALSE(cache.is_populated());
        CHECK(cache.printer_stop_commands().empty());
        CHECK(cache.generation() != populated);
    }

    cache.clear();
}

// ============================================================================
// Which temperature targets a macro sets
// ============================================================================

using helix::template_temperature_use;

TEST_CASE("template_temperature_use reads heater and fan targets", "[macro_param_cache][m141]") {
    const auto use =
        template_temperature_use("SET_HEATER_TEMPERATURE HEATER=Chamber TARGET={params.S}\n"
                                 "  set_temperature_fan_target temperature_fan=exhaust target=30\n"
                                 "SET_HEATER_TEMPERATURE HEATER={printer.foo.heater} TARGET=0\n");
    CHECK(use.targets ==
          std::vector<std::string>{"heater chamber", "temperature_fan exhaust", "heater *"});
    CHECK(use.commands == std::vector<std::string>{"SET_HEATER_TEMPERATURE",
                                                   "SET_TEMPERATURE_FAN_TARGET",
                                                   "SET_HEATER_TEMPERATURE"});
}

TEST_CASE("template_temperature_use finds commands behind Jinja blocks",
          "[macro_param_cache][m141]") {
    const auto use = template_temperature_use("{% set s = params.S|default(0)|int %}\n"
                                              "{% if s > 40 %} _CHAMBER_HEAT S={s} {% endif %}\n"
                                              "{# a comment #}\n"
                                              "RESPOND MSG=\"ok\"\n");
    CHECK(use.targets.empty());
    CHECK(use.commands == std::vector<std::string>{"_CHAMBER_HEAT", "RESPOND"});
}

namespace {

/// A configfile with an M141 and the macros it may call. Bodies are what each
/// test case sets; a null body leaves the macro listed but unreadable.
void populate_m141(const nlohmann::json& macros) {
    nlohmann::json config = nlohmann::json::object();
    std::unordered_set<std::string> known;
    for (const auto& [name, body] : macros.items()) {
        known.insert(name);
        if (!body.is_null()) {
            config["gcode_macro " + name] = {{"gcode", body}};
        }
    }
    MacroParamCache::instance().populate_from_configfile(config, known);
}

} // namespace

TEST_CASE("macro_sets_target answers for the chamber heater", "[macro_param_cache][m141]") {
    auto& cache = MacroParamCache::instance();
    const std::string heater = "heater_generic chamber";

    SECTION("a fan-only M141 never reaches the heater") {
        populate_m141({{"M141", "SET_TEMPERATURE_FAN_TARGET temperature_fan=chamber "
                                "target={params.S|default(0)}"}});
        CHECK(cache.macro_sets_target("m141", heater) == std::optional<bool>(false));
        // The fan it names is a target too, just not this one.
        CHECK(cache.macro_sets_target("M141", "temperature_fan chamber") ==
              std::optional<bool>(true));
    }
    SECTION("an M141 that sets the heater reaches it") {
        populate_m141({{"M141", "SET_HEATER_TEMPERATURE HEATER=chamber TARGET={params.S}"}});
        CHECK(cache.macro_sets_target("M141", heater) == std::optional<bool>(true));
    }
    SECTION("a heater named by a template may be this one") {
        populate_m141({{"M141", "SET_HEATER_TEMPERATURE HEATER={h} TARGET={params.S}"}});
        CHECK(cache.macro_sets_target("M141", heater) == std::optional<bool>(true));
    }
    SECTION("a call into another macro is followed") {
        populate_m141(
            {{"M141", "{% if params.S|int > 0 %} _CHAMBER_SET S={params.S} {% endif %}"},
             {"_CHAMBER_SET", "SET_HEATER_TEMPERATURE HEATER=chamber TARGET={params.S}"}});
        CHECK(cache.macro_sets_target("M141", heater) == std::optional<bool>(true));
    }
    SECTION("a call into a macro with no readable body is unknown") {
        populate_m141({{"M141", "_CHAMBER_SET S={params.S}"}, {"_CHAMBER_SET", nullptr}});
        CHECK_FALSE(cache.macro_sets_target("M141", heater).has_value());
    }
    SECTION("macros that call each other do not loop") {
        populate_m141({{"M141", "_A"}, {"_A", "_B"}, {"_B", "_A"}});
        CHECK(cache.macro_sets_target("M141", heater) == std::optional<bool>(false));
    }
    SECTION("an M141 with no readable body is unknown") {
        populate_m141({{"M141", nullptr}});
        CHECK_FALSE(cache.macro_sets_target("M141", heater).has_value());
    }
    SECTION("a bare heater name matches a bare HEATER=") {
        populate_m141({{"M141", "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"}});
        CHECK(cache.macro_sets_target("M141", "extruder") == std::optional<bool>(true));
        CHECK(cache.macro_sets_target("M141", heater) == std::optional<bool>(false));
    }
    cache.clear();
}
