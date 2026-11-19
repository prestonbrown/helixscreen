// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_z_offset.cpp
 * @brief The per-material Z offset capability module (prestonbrown/helixscreen#2142).
 *
 * Generic code reaches a storage scheme only through a printer's object list,
 * so these tests do the same: each row is selected by the macros a printer
 * exposes, never by name.
 */

#include "material_z_offset.h"
#include "printer_discovery.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace mz = helix::material_zoffset;

namespace {

/// A printer whose objects list carries the given objects and gcode macros.
PrinterDiscovery printer_with(std::initializer_list<const char*> macros, bool save_variables) {
    json objects = json::array({"gcode_move", "toolhead", "bed_mesh"});
    if (save_variables) {
        objects.push_back("save_variables");
    }
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

PrinterDiscovery vars_printer() {
    return printer_with({"START_PRINT", "_START_PRINT_VARS"}, false);
}

PrinterDiscovery helix_printer(bool save_variables = true) {
    return printer_with({"_HELIX_STATE", "HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL"},
                        save_variables);
}

json vars_frame(const json& vars) {
    return json{{"gcode_macro _START_PRINT_VARS", vars}};
}

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE("material zoffset: which printers get a row", "[material_zoffset]") {
    SECTION("a plain printer has none") {
        auto hw = printer_with({"START_PRINT"}, true);
        CHECK_FALSE(mz::supported(hw));
        CHECK(mz::required_status_objects(hw).empty());
        CHECK_FALSE(mz::write_plan(hw, {}, "PLA", 50).has_value());
    }
    SECTION("_START_PRINT_VARS: no save_variables needed, applied by the start macro itself") {
        auto hw = vars_printer();
        CHECK(mz::supported(hw));
        CHECK(mz::write_ready(hw));
        CHECK_FALSE(mz::reports_application(hw));
        CHECK(mz::required_status_objects(hw) ==
              std::vector<std::string>{"gcode_macro _START_PRINT_VARS"});
    }
    SECTION("HelixScreen's macro reads save_variables, meshes and its own record") {
        auto hw = helix_printer();
        CHECK(mz::supported(hw));
        CHECK(mz::write_ready(hw));
        CHECK(mz::reports_application(hw));
        auto objs = mz::required_status_objects(hw);
        CHECK(std::find(objs.begin(), objs.end(), "save_variables") != objs.end());
        CHECK(std::find(objs.begin(), objs.end(), "bed_mesh") != objs.end());
        CHECK(std::find(objs.begin(), objs.end(), "gcode_macro _HELIX_MATERIAL") != objs.end());
    }
    SECTION("HelixScreen's macro without [save_variables] matches but cannot write yet") {
        auto hw = helix_printer(false);
        CHECK(mz::supported(hw));
        CHECK_FALSE(mz::write_ready(hw));
    }
    SECTION("both: the start macro's own store wins") {
        auto hw = printer_with({"_START_PRINT_VARS", "HELIX_MATERIAL_OFFSET"}, true);
        auto plan = mz::write_plan(hw, {}, "PLA", 50);
        REQUIRE(plan.has_value());
        CHECK(plan->edits_config_file);
    }
    SECTION("firmware that owns the Z offset gets no row") {
        // ZMOD's macro, and a renaming SET_GCODE_OFFSET wrapper.
        for (const char* owner : {"SAVE_ZMOD_DATA", "SET_GCODE_OFFSET"}) {
            CAPTURE(owner);
            auto hw = printer_with({owner, "_START_PRINT_VARS", "HELIX_MATERIAL_OFFSET"}, true);
            CHECK_FALSE(mz::supported(hw));
            CHECK(mz::required_status_objects(hw).empty());
            CHECK_FALSE(mz::write_plan(hw, {}, "PLA", 50).has_value());
            CHECK(mz::start_hint_gcode(hw, "PLA").empty());
        }
    }
}

// ============================================================================
// Reading
// ============================================================================

TEST_CASE("material zoffset: reading _START_PRINT_VARS", "[material_zoffset]") {
    auto hw = vars_printer();
    mz::Table t;
    REQUIRE(mz::merge_status(hw,
                             vars_frame({{"offset_pla", 0.05},
                                         {"offset_default", -0.02},
                                         {"offset_probe", 0.3},
                                         {"heat_soak", 5},
                                         {"offset_abs", "oops"}}),
                             t));
    CHECK(t.offsets == std::map<std::string, int>{{"pla", 50}, {"default", -20}});

    SECTION("a delta frame adds without clearing") {
        CHECK(mz::merge_status(hw, vars_frame({{"offset_petg", -0.1499999}}), t));
        CHECK(t.offsets ==
              std::map<std::string, int>{{"pla", 50}, {"default", -20}, {"petg", -150}});
    }
    SECTION("a frame without the macro is no news") {
        CHECK_FALSE(mz::merge_status(hw, json{{"toolhead", json::object()}}, t));
        CHECK_FALSE(mz::merge_status(hw, vars_frame({{"offset_pla", 0.05}}), t));
        CHECK(t.offsets.size() == 2);
    }
}

TEST_CASE("material zoffset: reading HelixScreen's macro", "[material_zoffset]") {
    auto hw = helix_printer();
    mz::Table t;
    json frame = {
        {"save_variables",
         {{"variables",
           {{"helix_zoffset_petg", 0.08}, {"helix_zoffset_default", 0}, {"zoffset", 1.0}}}}},
        {"bed_mesh", {{"profiles", {{"helix_petg", json::object()}, {"default", json::object()}}}}},
        {"gcode_macro _HELIX_MATERIAL", {{"applied", "PETG"}, {"apply_count", 3}}}};
    REQUIRE(mz::merge_status(hw, frame, t));
    CHECK(t.offsets == std::map<std::string, int>{{"petg", 80}, {"default", 0}});
    CHECK(t.meshes == std::set<std::string>{"petg"});
    CHECK(t.applied == "PETG");
    CHECK(t.apply_count == 3);

    SECTION("a profiles object replaces the mesh set") {
        CHECK(mz::merge_status(hw, json{{"bed_mesh", {{"profiles", json::object()}}}}, t));
        CHECK(t.meshes.empty());
    }
    SECTION("the _START_PRINT_VARS schema is not read on this printer") {
        CHECK_FALSE(mz::merge_status(hw, vars_frame({{"offset_abs", 0.2}}), t));
    }
}

TEST_CASE("material zoffset: which entry a material uses", "[material_zoffset]") {
    auto vars = vars_printer();
    auto helix = helix_printer();
    SECTION("keys") {
        CHECK(mz::material_key(vars, "PETG") == "petg");
        CHECK(mz::material_key(vars, " petg ") == "petg");
        CHECK(mz::material_key(vars, "") == "default");
        CHECK(mz::material_key(vars, "PETG-CF") == "petg-cf");
        CHECK_FALSE(mz::material_key(vars, "PLA PRO").has_value());
        CHECK_FALSE(mz::material_key(vars, "PLA:1").has_value());
        // The macro's Jinja maps '-', ' ', '+' and '.' to '_'.
        CHECK(mz::material_key(helix, "PETG-CF") == "petg_cf");
        CHECK(mz::material_key(helix, "PLA+") == "pla_");
        CHECK_FALSE(mz::material_key(helix, "PA/CF").has_value());
        CHECK(mz::display_name("petg") == "PETG");
        CHECK(mz::display_name("default") == "DEFAULT");
    }
    SECTION("own entry, then DEFAULT, then nothing") {
        mz::Table t;
        t.offsets = {{"petg", 80}};
        CHECK(mz::applied_offset(vars, t, "petg") == 80);
        CHECK_FALSE(mz::applied_offset(vars, t, "ABS").has_value());
        t.offsets["default"] = -10;
        CHECK(mz::applied_offset(vars, t, "ABS") == -10);
        CHECK(mz::applied_offset(vars, t, "") == -10);
    }
}

// ============================================================================
// Writing
// ============================================================================

TEST_CASE("material zoffset: what a save sends", "[material_zoffset]") {
    SECTION("_START_PRINT_VARS: live variable plus the file") {
        auto hw = vars_printer();
        mz::Table t;
        t.offsets = {{"petg", 0}, {"default", 0}};
        auto plan = mz::write_plan(hw, t, "PETG", 80);
        REQUIRE(plan.has_value());
        CHECK(plan->gcode == "SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=offset_petg "
                             "VALUE=0.080");
        CHECK(plan->edits_config_file);
        CHECK(plan->live_now);
        CHECK(mz::config_section(hw) == "gcode_macro _start_print_vars");

        auto def = mz::write_plan(hw, t, "", -25);
        REQUIRE(def.has_value());
        CHECK(def->gcode == "SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=offset_default "
                            "VALUE=-0.025");
    }
    SECTION("_START_PRINT_VARS: a material the macro does not define yet is file-only") {
        // SET_GCODE_VARIABLE rejects a variable the section does not declare.
        auto plan = mz::write_plan(vars_printer(), {}, "ASA", 120);
        REQUIRE(plan.has_value());
        CHECK(plan->gcode.empty());
        CHECK(plan->edits_config_file);
        CHECK_FALSE(plan->live_now);
    }
    SECTION("HelixScreen's macro: SAVE_VARIABLE, which persists at once") {
        auto hw = helix_printer();
        auto plan = mz::write_plan(hw, {}, "PETG-CF", 1234);
        REQUIRE(plan.has_value());
        CHECK(plan->gcode == "SAVE_VARIABLE VARIABLE=helix_zoffset_petg_cf VALUE=1.234");
        CHECK_FALSE(plan->edits_config_file);
        CHECK(plan->live_now);
        CHECK(mz::config_section(hw).empty());
        CHECK_FALSE(mz::write_plan(hw, {}, "PA/CF", 10).has_value());
    }
    SECTION("no plan ever folds the offset into the probe or restarts Klipper") {
        for (const auto& hw : {vars_printer(), helix_printer()}) {
            for (const char* m : {"PLA", "", "PETG"}) {
                auto plan = mz::write_plan(hw, {{{"pla", 1}}, {}, "", -1}, m, 50);
                REQUIRE(plan.has_value());
                CHECK(plan->gcode.find("Z_OFFSET_APPLY") == std::string::npos);
                CHECK(plan->gcode.find("SAVE_CONFIG") == std::string::npos);
            }
        }
    }
}

TEST_CASE("material zoffset: meshes and the screen's material hint", "[material_zoffset]") {
    auto helix = helix_printer();
    auto vars = vars_printer();
    CHECK(mz::stores_mesh(helix));
    CHECK(mz::mesh_profile_name(helix, "PETG") == "helix_petg");
    CHECK(mz::mesh_profile_name(helix, "PA/CF").empty());
    CHECK_FALSE(mz::stores_mesh(vars));
    CHECK(mz::mesh_profile_name(vars, "PETG").empty());

    CHECK(mz::start_hint_gcode(helix, "PETG") ==
          "SET_GCODE_VARIABLE MACRO=_HELIX_MATERIAL VARIABLE=next VALUE='\"PETG\"'");
    CHECK(mz::start_hint_gcode(helix, "PA/CF").empty());
    // The vendor start macro takes the material from the slicer only.
    CHECK(mz::start_hint_gcode(vars, "PETG").empty());
}

// ============================================================================
// The primary material
// ============================================================================

TEST_CASE("material zoffset: the primary material", "[material_zoffset]") {
    using TM = mz::ToolMaterial;
    CHECK(mz::primary_material({{0, 12.0, "PLA"}, {1, 40.5, "PETG"}, {2, 3.0, "TPU"}}) == "PETG");
    CHECK(mz::primary_material({{0, 10.0, "PLA"}, {1, 10.0, "PETG"}}) == "PLA");
    CHECK(mz::primary_material({{1, 10.0, "PETG"}, {0, 10.0, "PLA"}}) == "PLA");
    // The heaviest tool has no material: the heaviest one that names one.
    CHECK(mz::primary_material({{0, 50.0, ""}, {1, 10.0, "ABS"}}) == "ABS");
    // No grams at all: the first named.
    CHECK(mz::primary_material({TM{0, 0.0, ""}, TM{1, 0.0, "ASA"}, TM{2, 0.0, "PLA"}}) == "ASA");
    CHECK(mz::primary_material({{-1, 30.0, "PETG"}}) == "PETG");
    CHECK(mz::primary_material({}).empty());

    CHECK(mz::first_filament("PLA;PETG") == "PLA");
    CHECK(mz::first_filament(" PETG ") == "PETG");
    CHECK(mz::first_filament("").empty());
}

TEST_CASE("material zoffset: the material a fresh mesh belongs to", "[material_zoffset]") {
    REQUIRE(mz::usual_bed_temp("PLA") > 0);
    REQUIRE(mz::usual_bed_temp("PLA") < mz::usual_bed_temp("ABS"));
    CHECK(mz::usual_bed_temp("NOT-A-MATERIAL") == -1);
    const std::vector<std::string> list = {"PLA", "PETG", "ABS"};
    CHECK(mz::closest_material(list, mz::usual_bed_temp("PLA") - 3) == "PLA");
    CHECK(mz::closest_material(list, mz::usual_bed_temp("ABS") + 10) == "ABS");
    CHECK(mz::closest_material({"NOT-A-MATERIAL"}, 60).empty());
}
