// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_z_offset_config_edit.cpp
 * @brief The one-line config edit that persists a material's Z offset in the
 *        file defining [gcode_macro _START_PRINT_VARS] (prestonbrown/helixscreen#2142).
 *
 * The edit runs on a user's own config, so the contract is: change exactly one
 * line inside exactly one section, keep every other byte, and refuse whenever
 * the target is not unambiguous.
 */

#include "material_z_offset.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace mz = helix::material_zoffset;

namespace {

constexpr const char* kSection = "gcode_macro _start_print_vars";

std::string edited(const std::string& in, const char* material, int microns) {
    auto r = mz::edit_config_text(in, kSection, material, microns);
    INFO(r.error);
    REQUIRE(r.text.has_value());
    return *r.text;
}

std::string refusal(const std::string& in, const char* material = "PETG") {
    auto r = mz::edit_config_text(in, kSection, material, 80);
    CHECK_FALSE(r.text.has_value());
    CHECK_FALSE(r.error.empty());
    return r.error;
}

} // namespace

TEST_CASE("material zoffset config edit: replacing an existing line", "[material_zoffset]") {
    const std::string in = "[gcode_macro START_PRINT]\n"
                           "gcode:\n"
                           "  SET_GCODE_OFFSET Z={OFFSET}\n"
                           "\n"
                           "[gcode_macro _START_PRINT_VARS]\n"
                           "# copy this to overrides.cfg\n"
                           "variable_offset_PLA: 0\n"
                           "variable_offset_PETG: 0.02   # tuned on the satin sheet\n"
                           "variable_offset_DEFAULT: 0\n"
                           "variable_heat_soak: 0 # minutes\n"
                           "gcode:\n";
    CHECK(edited(in, "PETG", 80) == "[gcode_macro START_PRINT]\n"
                                    "gcode:\n"
                                    "  SET_GCODE_OFFSET Z={OFFSET}\n"
                                    "\n"
                                    "[gcode_macro _START_PRINT_VARS]\n"
                                    "# copy this to overrides.cfg\n"
                                    "variable_offset_PLA: 0\n"
                                    "variable_offset_PETG: 0.080   # tuned on the satin sheet\n"
                                    "variable_offset_DEFAULT: 0\n"
                                    "variable_heat_soak: 0 # minutes\n"
                                    "gcode:\n");
    // DEFAULT is the empty material; a negative value keeps its sign.
    CHECK(edited(in, "", -25).find("variable_offset_DEFAULT: -0.025\n") != std::string::npos);
}

TEST_CASE("material zoffset config edit: case and spelling", "[material_zoffset]") {
    // Klipper lower-cases section and option names, so any case is the same line.
    const std::string in = "[gcode_macro   _start_print_VARS]\n"
                           "Variable_Offset_petg = 0.01\n";
    CHECK(edited(in, "PETG", -150) == "[gcode_macro   _start_print_VARS]\n"
                                      "Variable_Offset_petg = -0.150\n");
}

TEST_CASE("material zoffset config edit: inserting a missing line", "[material_zoffset]") {
    SECTION("after the last variable_ option, before gcode:") {
        const std::string in = "[gcode_macro _START_PRINT_VARS]\n"
                               "variable_offset_PLA: 0\n"
                               "variable_heat_soak: 0\n"
                               "gcode:\n"
                               "  RESPOND MSG=hi\n"
                               "\n"
                               "[printer]\n";
        CHECK(edited(in, "asa", 120) == "[gcode_macro _START_PRINT_VARS]\n"
                                        "variable_offset_PLA: 0\n"
                                        "variable_heat_soak: 0\n"
                                        "variable_offset_ASA: 0.120\n"
                                        "gcode:\n"
                                        "  RESPOND MSG=hi\n"
                                        "\n"
                                        "[printer]\n");
    }
    SECTION("after a multi-line value, not inside it") {
        const std::string in = "[gcode_macro _START_PRINT_VARS]\n"
                               "variable_list: [1,\n"
                               "   2]\n"
                               "gcode:\n";
        CHECK(edited(in, "PLA", 10) == "[gcode_macro _START_PRINT_VARS]\n"
                                       "variable_list: [1,\n"
                                       "   2]\n"
                                       "variable_offset_PLA: 0.010\n"
                                       "gcode:\n");
    }
    SECTION("right after the header when the section has no variables") {
        CHECK(edited("[gcode_macro _START_PRINT_VARS]\ngcode:\n", "PLA", 0) ==
              "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0.000\ngcode:\n");
    }
    SECTION("a commented-out line does not count") {
        CHECK(edited("[gcode_macro _START_PRINT_VARS]\n#variable_offset_PLA: 0.3\n", "PLA", 50) ==
              "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0.050\n"
              "#variable_offset_PLA: 0.3\n");
    }
    SECTION("an indented line is a continuation, not an option") {
        CHECK(edited("[gcode_macro _START_PRINT_VARS]\ngcode:\n  variable_offset_PLA: 1\n", "PLA",
                     50) == "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0.050\n"
                            "gcode:\n  variable_offset_PLA: 1\n");
    }
}

TEST_CASE("material zoffset config edit: other sections are not touched", "[material_zoffset]") {
    const std::string in = "[gcode_macro _START_PRINT_VARS_OLD]\n"
                           "variable_offset_PETG: 0.5\n"
                           "[gcode_macro START_PRINT_VARS]\n"
                           "variable_offset_PETG: 0.6\n"
                           "#[gcode_macro _START_PRINT_VARS]\n"
                           "[gcode_macro _START_PRINT_VARS]\n"
                           "variable_offset_PETG: 0.7\n"
                           "[gcode_macro _START_PRINT_VARS_NEW]\n"
                           "variable_offset_PETG: 0.8\n";
    CHECK(mz::count_sections(in, kSection) == 1);
    CHECK(edited(in, "PETG", 90) == "[gcode_macro _START_PRINT_VARS_OLD]\n"
                                    "variable_offset_PETG: 0.5\n"
                                    "[gcode_macro START_PRINT_VARS]\n"
                                    "variable_offset_PETG: 0.6\n"
                                    "#[gcode_macro _START_PRINT_VARS]\n"
                                    "[gcode_macro _START_PRINT_VARS]\n"
                                    "variable_offset_PETG: 0.090\n"
                                    "[gcode_macro _START_PRINT_VARS_NEW]\n"
                                    "variable_offset_PETG: 0.8\n");
}

TEST_CASE("material zoffset config edit: refusals", "[material_zoffset]") {
    SECTION("no such section") {
        refusal("[printer]\nkinematics: corexy\n");
        CHECK(mz::count_sections("", kSection) == 0);
    }
    SECTION("the section twice") {
        const std::string twice = "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0\n"
                                  "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0\n";
        CHECK(mz::count_sections(twice, kSection) == 2);
        refusal(twice, "PLA");
    }
    SECTION("the material's line twice") {
        refusal("[gcode_macro _START_PRINT_VARS]\n"
                "variable_offset_PETG: 0\nvariable_offset_petg: 0.1\n");
    }
    SECTION("the material's value runs onto the next line") {
        refusal("[gcode_macro _START_PRINT_VARS]\nvariable_offset_PETG:\n  0.1\n");
    }
    SECTION("a material no option name can hold") {
        refusal("[gcode_macro _START_PRINT_VARS]\n", "PLA PRO");
    }
}

TEST_CASE("material zoffset config edit: line endings survive", "[material_zoffset]") {
    CHECK(edited("[gcode_macro _START_PRINT_VARS]\r\nvariable_offset_PLA: 0\r\ngcode:\r\n", "PLA",
                 50) == "[gcode_macro _START_PRINT_VARS]\r\nvariable_offset_PLA: 0.050\r\n"
                        "gcode:\r\n");
    CHECK(edited("[gcode_macro _START_PRINT_VARS]\r\nvariable_x: 1\r\n", "PLA", 50) ==
          "[gcode_macro _START_PRINT_VARS]\r\nvariable_x: 1\r\nvariable_offset_PLA: 0.050\r\n");
    // No final newline: the edited last line keeps none, an appended one gets one before it.
    CHECK(edited("[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0", "PLA", 50) ==
          "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PLA: 0.050");
    CHECK(edited("[gcode_macro _START_PRINT_VARS]\nvariable_x: 1", "PLA", 50) ==
          "[gcode_macro _START_PRINT_VARS]\nvariable_x: 1\nvariable_offset_PLA: 0.050");
}

TEST_CASE("material zoffset config edit: finding the file through includes", "[material_zoffset]") {
    CHECK(mz::include_targets("[include mainsail.cfg]\n"
                              "#[include old.cfg]\n"
                              "  [include indented.cfg]\n"
                              "[include macros/*.cfg]  # all of them\n"
                              "[printer]\n") ==
          std::vector<std::string>{"mainsail.cfg", "macros/*.cfg"});
    CHECK(mz::include_matches("macros/*.cfg", "macros/start.cfg"));
    CHECK_FALSE(mz::include_matches("macros/*.cfg", "macros/sub/start.cfg"));
    CHECK_FALSE(mz::include_matches("macros/*.cfg", "macros/start.cfg.bak"));
    CHECK(mz::include_matches("overrides.cfg", "overrides.cfg"));
    CHECK(mz::include_matches("k?.cfg", "k2.cfg"));
    CHECK_FALSE(mz::include_matches("k?.cfg", "k22.cfg"));
    // Klipper globs with Python's fnmatch: [...] is a class, [!...] its complement.
    CHECK(mz::include_matches("k[12].cfg", "k1.cfg"));
    CHECK_FALSE(mz::include_matches("k[12].cfg", "k3.cfg"));
    CHECK(mz::include_matches("k[0-9].cfg", "k7.cfg"));
    CHECK_FALSE(mz::include_matches("k[!0-9].cfg", "k7.cfg"));
    CHECK(mz::include_matches("timelapse.cf[g]", "timelapse.cfg"));
    CHECK_FALSE(mz::include_matches("k[12.cfg", "k1.cfg")); // an unclosed class matches nothing
}
