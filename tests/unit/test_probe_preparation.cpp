// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"
#include "probe_preparation.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::probe_prep::Operation;
using helix::probe_prep::Preparation;
using helix::probe_prep::resolve_from_rules;
using json = nlohmann::json;

namespace {

/// The rule we intend to ship for ZMOD, verbatim in shape.
json zmod_rule(const char* id = "zmod_tare", const char* macro = "LOAD_CELL_TARE") {
    return json{
        {"id", id},
        {"when",
         json::array({json{{"type", "macro_match"}, {"field", "macros"}, {"pattern", macro}}})},
        {"operations",
         json::array({"screws_tilt", "bed_mesh", "probe_accuracy", "z_offset_calibrate"})},
        {"gcode", json::array({macro})},
        {"label", "Zeroing load cell"},
        {"timeout_s", 60},
        {"reason", "load-cell zero drifts with bed screws"},
    };
}

const std::unordered_set<std::string> kZmodMacros = {"LOAD_CELL_TARE", "BED_LEVEL_SCREWS_TUNE",
                                                     "SCREWS_TILT_CALCULATE"};
const std::unordered_set<std::string> kStockMacros = {"SCREWS_TILT_CALCULATE", "G28"};

} // namespace

TEST_CASE("No rules means send nothing", "[calibration][probe_prep]") {
    SECTION("empty array") {
        REQUIRE(resolve_from_rules(json::array(), kZmodMacros, {}, Operation::ScrewsTilt).empty());
    }
    SECTION("null - the key is absent from the database") {
        REQUIRE(resolve_from_rules(json(), kZmodMacros, {}, Operation::ScrewsTilt).empty());
    }
    SECTION("wrong type entirely") {
        REQUIRE(
            resolve_from_rules(json("nonsense"), kZmodMacros, {}, Operation::ScrewsTilt).empty());
    }
}

TEST_CASE("A rule fires only when its predicate holds", "[calibration][probe_prep]") {
    const json rules = json::array({zmod_rule()});

    SECTION("macro present - fires") {
        const Preparation p = resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt);
        REQUIRE_FALSE(p.empty());
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
        REQUIRE(p.label == "Zeroing load cell");
        REQUIRE(p.extra_timeout_ms == 60000);
        REQUIRE(p.rule_id == "zmod_tare");
    }

    SECTION("macro absent - stays silent instead of sending Unknown command") {
        REQUIRE(resolve_from_rules(rules, kStockMacros, {}, Operation::ScrewsTilt).empty());
    }
}

TEST_CASE("Operations gate which probes get prepared", "[calibration][probe_prep]") {
    json rule = zmod_rule();
    rule["operations"] = json::array({"bed_mesh"});
    const json rules = json::array({rule});

    REQUIRE_FALSE(resolve_from_rules(rules, kZmodMacros, {}, Operation::BedMesh).empty());
    REQUIRE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt).empty());
    REQUIRE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ProbeAccuracy).empty());
}

TEST_CASE("Multiple when entries are ANDed", "[calibration][probe_prep]") {
    json rule = zmod_rule();
    rule["when"] = json::array({
        json{{"type", "macro_match"}, {"field", "macros"}, {"pattern", "LOAD_CELL_TARE"}},
        json{{"type", "macro_match"}, {"field", "macros"}, {"pattern", "SOME_OTHER_MACRO"}},
    });
    const json rules = json::array({rule});

    SECTION("one of two present - does NOT fire") {
        REQUIRE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt).empty());
    }
    SECTION("both present - fires") {
        std::unordered_set<std::string> macros = kZmodMacros;
        macros.insert("SOME_OTHER_MACRO");
        REQUIRE_FALSE(resolve_from_rules(rules, macros, {}, Operation::ScrewsTilt).empty());
    }
}

/**
 * The rename case that drove the design: ship a rule per macro name and let the
 * predicate act as the version detector. Both rules ship; the printer selects.
 */
TEST_CASE("First matching rule wins, so a rename ships as a second rule",
          "[calibration][probe_prep]") {
    const json rules = json::array({
        zmod_rule("zmod_tare_v2", "LOAD_CELL_ZERO"),
        zmod_rule("zmod_tare_v1", "LOAD_CELL_TARE"),
    });

    SECTION("new firmware picks the new name") {
        const Preparation p =
            resolve_from_rules(rules, {"LOAD_CELL_ZERO"}, {}, Operation::ScrewsTilt);
        REQUIRE(p.rule_id == "zmod_tare_v2");
        REQUIRE(p.gcode == "LOAD_CELL_ZERO");
    }
    SECTION("old firmware picks the old name") {
        const Preparation p = resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt);
        REQUIRE(p.rule_id == "zmod_tare_v1");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
    SECTION("neither present - nothing is sent") {
        REQUIRE(resolve_from_rules(rules, kStockMacros, {}, Operation::ScrewsTilt).empty());
    }
}

TEST_CASE("gcode accepts a bare string or an array", "[calibration][probe_prep]") {
    SECTION("string form") {
        json rule = zmod_rule();
        rule["gcode"] = "LOAD_CELL_TARE";
        REQUIRE(
            resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt).gcode ==
            "LOAD_CELL_TARE");
    }
    SECTION("multi-command array joins with newlines") {
        json rule = zmod_rule();
        rule["gcode"] = json::array({"M104 S130", "M140 S80", "LOAD_CELL_TARE"});
        REQUIRE(
            resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt).gcode ==
            "M104 S130\nM140 S80\nLOAD_CELL_TARE");
    }
}

/**
 * Fail-closed is the whole safety story here: these rules execute motion and
 * heating on someone's printer. An unrecognised predicate must never be read as
 * "no objection".
 */
TEST_CASE("Unrecognised predicates fail closed", "[calibration][probe_prep]") {
    SECTION("unknown predicate type does NOT match") {
        json rule = zmod_rule();
        rule["when"] = json::array({json{{"type", "phase_of_moon"}, {"pattern", "waxing"}}});
        REQUIRE(resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt)
                    .empty());
    }
    SECTION("a known predicate alongside an unknown one still fails") {
        json rule = zmod_rule();
        rule["when"] = json::array({
            json{{"type", "macro_match"}, {"field", "macros"}, {"pattern", "LOAD_CELL_TARE"}},
            json{{"type", "phase_of_moon"}, {"pattern", "waxing"}},
        });
        REQUIRE(resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt)
                    .empty());
    }
    SECTION("a rule with no when clause at all does NOT match everything") {
        json rule = zmod_rule();
        rule.erase("when");
        REQUIRE(resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt)
                    .empty());
    }
}

TEST_CASE("Malformed rules are skipped without taking the list down", "[calibration][probe_prep]") {
    // A throw here would cost the whole database, the way a bad "version" field
    // once took out printer detection entirely.
    const json rules = json::array({
        json("not even an object"),
        json{{"id", "no_gcode"},
             {"when", json::array({json{{"type", "macro_match"}, {"pattern", "LOAD_CELL_TARE"}}})},
             {"operations", json::array({"screws_tilt"})}},
        json{{"id", "gcode_wrong_type"},
             {"when", json::array({json{{"type", "macro_match"}, {"pattern", "LOAD_CELL_TARE"}}})},
             {"operations", json::array({"screws_tilt"})},
             {"gcode", 42}},
        zmod_rule("good_rule"),
    });

    Preparation p;
    REQUIRE_NOTHROW(p = resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt));
    REQUIRE(p.rule_id == "good_rule");
}

TEST_CASE("A rule can be disabled by a drop-in", "[calibration][probe_prep]") {
    json rule = zmod_rule();
    rule["enabled"] = false;
    REQUIRE(
        resolve_from_rules(json::array({rule}), kZmodMacros, {}, Operation::ScrewsTilt).empty());
}

/**
 * When the ScrewsTilt slot resolves to BED_LEVEL_SCREWS_TUNE, that macro already
 * tares at base.cfg:211. Taring twice is harmless today but stops being harmless
 * the moment a rule grows a heat step.
 */
TEST_CASE("skip_if_macro_in stands down for a self-preparing macro", "[calibration][probe_prep]") {
    json rule = zmod_rule();
    rule["skip_if_macro_in"] = json::array({"BED_LEVEL_SCREWS_TUNE"});
    const json rules = json::array({rule});

    SECTION("bare command - preparation still applies") {
        REQUIRE_FALSE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt,
                                         "SCREWS_TILT_CALCULATE")
                          .empty());
    }
    SECTION("self-preparing macro - preparation stands down") {
        REQUIRE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt,
                                   "BED_LEVEL_SCREWS_TUNE")
                    .empty());
    }
    SECTION("case-insensitive, since macro names round-trip through config") {
        REQUIRE(resolve_from_rules(rules, kZmodMacros, {}, Operation::ScrewsTilt,
                                   "bed_level_screws_tune")
                    .empty());
    }
    SECTION("only affects the named macro, not other operations") {
        REQUIRE_FALSE(resolve_from_rules(rules, kZmodMacros, {}, Operation::BedMesh, "").empty());
    }
}

TEST_CASE("operation_key round-trips every enum value", "[calibration][probe_prep]") {
    // Guards against a new Operation being added without a database key, which
    // would silently make every rule for it dead data.
    REQUIRE(std::string(helix::probe_prep::operation_key(Operation::ScrewsTilt)) == "screws_tilt");
    REQUIRE(std::string(helix::probe_prep::operation_key(Operation::BedMesh)) == "bed_mesh");
    REQUIRE(std::string(helix::probe_prep::operation_key(Operation::ProbeAccuracy)) ==
            "probe_accuracy");
    REQUIRE(std::string(helix::probe_prep::operation_key(Operation::ZOffsetCalibrate)) ==
            "z_offset_calibrate");
}

TEST_CASE("object_exists keys a rule off the module object, not the command name",
          "[1529][calibration][probe_prep]") {
    json rule = json{{"id", "obj_tare"},
                     {"when", json::array({json{{"type", "object_exists"},
                                                {"field", "printer_objects"},
                                                {"pattern", "load_cell_probe"}}})},
                     {"operations", json::array({"screws_tilt"})},
                     {"gcode", json::array({"LOAD_CELL_TARE"})}};
    const json rules = json::array({rule});

    SECTION("object present - fires") {
        const Preparation p =
            resolve_from_rules(rules, kStockMacros, {"load_cell_probe"}, Operation::ScrewsTilt);
        REQUIRE(p.rule_id == "obj_tare");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
    SECTION("object absent - stays silent") {
        REQUIRE(
            resolve_from_rules(rules, kStockMacros, {"toolhead"}, Operation::ScrewsTilt).empty());
    }
    SECTION("case-insensitive, like the detection heuristic") {
        REQUIRE_FALSE(
            resolve_from_rules(rules, kStockMacros, {"LOAD_CELL_PROBE"}, Operation::ScrewsTilt)
                .empty());
    }
    SECTION("anchors pin the whole name") {
        json anchored = rule;
        anchored["when"] = json::array({json{{"type", "object_exists"}, {"pattern", "^box$"}}});
        const json anchored_rules = json::array({anchored});
        REQUIRE(resolve_from_rules(anchored_rules, kStockMacros, {"gcode_macro BOX_UNLOAD"},
                                   Operation::ScrewsTilt)
                    .empty());
        REQUIRE_FALSE(
            resolve_from_rules(anchored_rules, kStockMacros, {"box"}, Operation::ScrewsTilt)
                .empty());
    }
}

/**
 * The shipped rules, as data. On COSMOS the load_cell module registers
 * LOAD_CELL_TARE as a command - never a gcode_macro - so macro_match cannot see
 * it and the ZMOD rule is dead data there. The COSMOS rule keys off the
 * load_cell_probe object together with COSMOS's own _COSMOS_SETTINGS macro, so a
 * load cell on any other firmware is not sent a command it may not define.
 * First-match-wins keeps the macro path authoritative when a printer exposes both.
 */
TEST_CASE("Shipped rules: COSMOS tares via its object, the ZMOD via its macro",
          "[1529][calibration][probe_prep]") {
    const json rules = helix::probe_prep::database_rules();
    REQUIRE(rules.is_array());
    REQUIRE(rules.size() >= 2);

    const std::unordered_set<std::string> cosmos_macros = {"_COSMOS_SETTINGS",
                                                           "SCREWS_TILT_CALCULATE", "G28"};
    const std::vector<std::string> cosmos_objects = {"load_cell_probe", "toolhead"};

    SECTION("COSMOS: probe accuracy tares with LOAD_CELL_TARE") {
        const Preparation p =
            resolve_from_rules(rules, cosmos_macros, cosmos_objects, Operation::ProbeAccuracy);
        REQUIRE(p.rule_id == "cosmos_load_cell_tare");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
    SECTION("COSMOS: z-offset calibration gets the tare too") {
        const Preparation p =
            resolve_from_rules(rules, cosmos_macros, cosmos_objects, Operation::ZOffsetCalibrate);
        REQUIRE(p.rule_id == "cosmos_load_cell_tare");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
    SECTION("COSMOS: screws tilt gets the tare too") {
        // Only a self-preparing shipped sequence skips it, and that never reaches the rules.
        const Preparation p =
            resolve_from_rules(rules, cosmos_macros, cosmos_objects, Operation::ScrewsTilt);
        REQUIRE(p.rule_id == "cosmos_load_cell_tare");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
    SECTION("COSMOS: bed mesh is left alone") {
        // COSMOS's BED_MESH_CALIBRATE tares the load cell itself.
        REQUIRE(
            resolve_from_rules(rules, cosmos_macros, cosmos_objects, Operation::BedMesh).empty());
    }
    SECTION("load_cell_probe without _COSMOS_SETTINGS stays silent") {
        const std::unordered_set<std::string> other_macros = {"SCREWS_TILT_CALCULATE", "G28"};
        REQUIRE(resolve_from_rules(rules, other_macros, cosmos_objects, Operation::ProbeAccuracy)
                    .empty());
        REQUIRE(resolve_from_rules(rules, other_macros, cosmos_objects, Operation::ZOffsetCalibrate)
                    .empty());
    }
    SECTION("_COSMOS_SETTINGS without load_cell_probe stays silent") {
        REQUIRE(resolve_from_rules(rules, cosmos_macros, {"toolhead"}, Operation::ProbeAccuracy)
                    .empty());
    }
    SECTION("ZMOD: the macro rule fires first") {
        std::unordered_set<std::string> zmod = cosmos_macros;
        zmod.insert("LOAD_CELL_TARE");
        const Preparation p =
            resolve_from_rules(rules, zmod, cosmos_objects, Operation::ProbeAccuracy);
        REQUIRE(p.rule_id == "zmod_load_cell_tare");
        REQUIRE(p.gcode == "LOAD_CELL_TARE");
    }
}

TEST_CASE("resolve() hands the discovered objects and macros to the rules",
          "[1529][calibration][probe_prep]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"load_cell_probe", "gcode_macro _COSMOS_SETTINGS"}));

    const Preparation p = helix::probe_prep::resolve(hw, Operation::ProbeAccuracy);
    REQUIRE(p.rule_id == "cosmos_load_cell_tare");
}
