// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_pa.cpp
 * @brief Pressure advance by material (prestonbrown/helixscreen#2174): what a
 * tuner's "Save for <material>" writes, and which printers offer it.
 *
 * The values are read back at print start by _HELIX_MATERIAL_PA in
 * assets/config/helix_macros.cfg; tests/python/test_material_pa_macro.py
 * renders that macro against these names.
 */

#include "core/text_io.h"
#include "material_pa.h"
#include "material_z_offset.h"
#include "printer_discovery.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace mpa = helix::material_pa;
namespace mz = helix::material_zoffset;
namespace pa = helix::pa;

namespace {

PrinterDiscovery printer_with(std::initializer_list<const char*> macros, bool save_variables) {
    json objects = json::array({"gcode_move", "toolhead", "bed_mesh", "extruder"});
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

std::vector<std::string> script_lines(const std::string& script) {
    std::vector<std::string> out;
    for (std::string_view line : helix::text_io::Lines(script, '\n')) {
        out.emplace_back(line);
    }
    return out;
}

} // namespace

TEST_CASE("material pa: the key is the Z offset macro's key", "[material_pa]") {
    CHECK(mpa::storage_key("PLA") == "pla");
    CHECK(mpa::storage_key(" PETG-CF ") == "petg_cf");
    CHECK(mpa::storage_key("PLA+") == "pla_");
    CHECK(mpa::storage_key("PLA Silk.2") == "pla_silk_2");
    for (const char* m : {"PLA", "PETG-CF", "ASA", "PLA Silk"}) {
        INFO(m);
        CHECK(mpa::storage_key(m) == mz::helix_key(m));
    }
    // No DEFAULT entry: a material the slicer did not name prints with the config's.
    CHECK_FALSE(mpa::storage_key("").has_value());
    CHECK_FALSE(mpa::storage_key("   ").has_value());
    // Anything the macro's replace chain leaves outside [a-z0-9_] cannot be stored.
    CHECK_FALSE(mpa::storage_key("PLA/CF").has_value());
    CHECK_FALSE(mpa::storage_key("PLA\"").has_value());
}

TEST_CASE("material pa: a linear result writes K, its model and the switch", "[material_pa]") {
    const auto lines = script_lines(mpa::save_gcode("PETG-CF", mpa::linear_entry(0.0421f)));
    const std::vector<std::string> expected = {
        "SAVE_VARIABLE VARIABLE=helix_pa_petg_cf_advance VALUE=0.042100",
        "SAVE_VARIABLE VARIABLE=helix_pa_petg_cf_model VALUE='\"linear\"'",
        "SAVE_VARIABLE VARIABLE=helix_material_pa_enabled VALUE=1",
    };
    CHECK(lines == expected);
}

TEST_CASE("material pa: a nonlinear result writes every parameter, the model last",
          "[material_pa]") {
    pa::NonlinearValues v;
    v.model = pa::ModelKind::Tanh;
    v.linear_advance = 0.012f;
    v.nonlinear_offset = 0.16f;
    v.linearization_velocity = 1.0f;
    v.time_offset = 0.0028f;
    v.smooth_time = 0.02f;
    const auto lines = script_lines(mpa::save_gcode("PLA", mpa::nonlinear_entry(v)));
    const std::vector<std::string> expected = {
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_advance VALUE=0.012000",
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_offset VALUE=0.160000",
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_velocity VALUE=1.000000",
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_time_offset VALUE=0.002800",
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_smooth_time VALUE=0.020000",
        "SAVE_VARIABLE VARIABLE=helix_pa_pla_model VALUE='\"tanh\"'",
        "SAVE_VARIABLE VARIABLE=helix_material_pa_enabled VALUE=1",
    };
    CHECK(lines == expected);

    v.model = pa::ModelKind::Recipr;
    CHECK(script_lines(mpa::save_gcode("PLA", mpa::nonlinear_entry(v)))[5] ==
          "SAVE_VARIABLE VARIABLE=helix_pa_pla_model VALUE='\"recipr\"'");
}

TEST_CASE("material pa: a material with no key writes nothing", "[material_pa]") {
    CHECK(mpa::save_gcode("", mpa::linear_entry(0.04f)).empty());
    CHECK(mpa::save_gcode("PLA/CF", mpa::linear_entry(0.04f)).empty());
}

TEST_CASE("material pa: the switch", "[material_pa]") {
    CHECK(mpa::enable_gcode(true) == "SAVE_VARIABLE VARIABLE=helix_material_pa_enabled VALUE=1");
    CHECK(mpa::enable_gcode(false) == "SAVE_VARIABLE VARIABLE=helix_material_pa_enabled VALUE=0");
}

TEST_CASE("material pa: offered only where HelixScreen's macro applies it", "[material_pa]") {
    // The pack that carries it, with a store.
    CHECK(mz::pressure_advance_ready(
        printer_with({"HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL", "_HELIX_MATERIAL_PA"}, true)));
    // An older pack: the macro would ignore the entry.
    CHECK_FALSE(mz::pressure_advance_ready(
        printer_with({"HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL"}, true)));
    // No store to write.
    CHECK_FALSE(mz::pressure_advance_ready(
        printer_with({"HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL", "_HELIX_MATERIAL_PA"}, false)));
    // A printer whose own start macro keeps the Z table never calls ours.
    CHECK_FALSE(mz::pressure_advance_ready(
        printer_with({"_START_PRINT_VARS", "HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL_PA"}, true)));
}

TEST_CASE("material pa: the switch is read from save_variables", "[material_pa]") {
    const auto hw =
        printer_with({"HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL", "_HELIX_MATERIAL_PA"}, true);
    mz::Table t;
    REQUIRE_FALSE(t.pa_enabled.has_value());
    CHECK(mz::merge_status(
        hw, json{{"save_variables", {{"variables", {{"helix_material_pa_enabled", 1}}}}}}, t));
    CHECK(t.pa_enabled == true);
    // Entries are not Z offsets.
    CHECK(t.offsets.empty());
    CHECK(mz::merge_status(
        hw, json{{"save_variables", {{"variables", {{"helix_material_pa_enabled", 0}}}}}}, t));
    CHECK(t.pa_enabled == false);
}
