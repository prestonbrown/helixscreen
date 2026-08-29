// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_editor.h"
#include "shaper_selection.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::calibration::SelectedShaper;
using helix::calibration::shaper_config_edits;
using helix::system::all_add_key;
using helix::system::ConfigEdit;

namespace {

SelectedShaper make_shaper(const std::string& type, float freq) {
    SelectedShaper s;
    s.type = type;
    s.frequency = freq;
    return s;
}

/// Value written for @p key, or "<absent>" when no edit touches it.
std::string edited_value(const std::vector<ConfigEdit>& edits, const std::string& key) {
    for (const auto& e : edits) {
        if (e.key == key)
            return e.value;
    }
    return "<absent>";
}

} // namespace

TEST_CASE("shaper_config_edits writes both axes", "[input_shaper][config_edits]") {
    auto edits = shaper_config_edits(make_shaper("mzv", 47.4f), make_shaper("ei", 35.0f));

    REQUIRE(edits.size() == 4);
    // ADD_KEY, so the same list works whether or not the printer already has the
    // key: apply_edits() self-heals ADD_KEY to SET_VALUE when the key is present.
    CHECK(all_add_key(edits));

    CHECK(edited_value(edits, "shaper_type_x") == "mzv");
    CHECK(edited_value(edits, "shaper_freq_x") == "47.4");
    CHECK(edited_value(edits, "shaper_type_y") == "ei");
    CHECK(edited_value(edits, "shaper_freq_y") == "35.0");
}

TEST_CASE("shaper_config_edits skips an axis with nothing valid", "[input_shaper][config_edits]") {
    SECTION("X only") {
        auto edits = shaper_config_edits(make_shaper("mzv", 47.4f), SelectedShaper{});
        REQUIRE(edits.size() == 2);
        CHECK(edited_value(edits, "shaper_type_x") == "mzv");
        CHECK(edited_value(edits, "shaper_freq_x") == "47.4");
        CHECK(edited_value(edits, "shaper_type_y") == "<absent>");
        CHECK(edited_value(edits, "shaper_freq_y") == "<absent>");
    }

    SECTION("Y only") {
        auto edits = shaper_config_edits(SelectedShaper{}, make_shaper("2hump_ei", 61.2f));
        REQUIRE(edits.size() == 2);
        CHECK(edited_value(edits, "shaper_type_y") == "2hump_ei");
        CHECK(edited_value(edits, "shaper_freq_y") == "61.2");
        CHECK(edited_value(edits, "shaper_type_x") == "<absent>");
    }

    SECTION("Neither axis valid yields no edits at all") {
        CHECK(shaper_config_edits(SelectedShaper{}, SelectedShaper{}).empty());
    }

    SECTION("A type with no frequency is not valid") {
        // 0 Hz would be written as a real config value and wedge Klipper.
        CHECK(shaper_config_edits(make_shaper("mzv", 0.0f), make_shaper("ei", 0.0f)).empty());
    }

    SECTION("A frequency with no type is not valid") {
        CHECK(shaper_config_edits(make_shaper("", 47.4f), make_shaper("", 35.0f)).empty());
    }
}

TEST_CASE("shaper_config_edits formats frequency to one decimal", "[input_shaper][config_edits]") {
    CHECK(edited_value(shaper_config_edits(make_shaper("mzv", 41.6f), SelectedShaper{}),
                       "shaper_freq_x") == "41.6");
    // A whole number keeps its .0 rather than collapsing to "32"
    CHECK(edited_value(shaper_config_edits(make_shaper("mzv", 32.0f), SelectedShaper{}),
                       "shaper_freq_x") == "32.0");
    // Rounds, does not truncate
    CHECK(edited_value(shaper_config_edits(make_shaper("mzv", 47.449f), SelectedShaper{}),
                       "shaper_freq_x") == "47.4");
    CHECK(edited_value(shaper_config_edits(make_shaper("mzv", 47.46f), SelectedShaper{}),
                       "shaper_freq_x") == "47.5");
}

TEST_CASE("shaper_config_edits writes the user's chip pick, not the recommendation",
          "[input_shaper][config_edits]") {
    // Klipper recommends mzv @ 47.4; the user tapped the EI chip in the chart.
    InputShaperResult result;
    result.axis = 'X';
    result.shaper_type = "mzv";
    result.shaper_freq = 47.4f;
    result.vibrations = 1.2f;
    result.max_accel = 5100.0f;
    result.all_shapers = {
        {"mzv", 47.4f, 1.2f, 0.05f, 5100.0f},
        {"ei", 58.2f, 0.4f, 0.09f, 4200.0f},
    };

    std::vector<ShaperResponseCurve> curves = {
        {"mzv", 47.4f, {}},
        {"ei", 58.2f, {}},
    };

    const SelectedShaper picked = helix::calibration::resolve_selected_shaper(result, curves, 1);
    REQUIRE(picked.from_selection);
    REQUIRE(picked.type == "ei");

    auto edits = shaper_config_edits(picked, SelectedShaper{});
    CHECK(edited_value(edits, "shaper_type_x") == "ei");
    CHECK(edited_value(edits, "shaper_freq_x") == "58.2");
    // The recommendation must not leak through
    CHECK(edited_value(edits, "shaper_type_x") != "mzv");
}

// Composition guard over the two units above: the edit list the save path builds
// has to survive apply_edits() on a printer that has never been calibrated, which
// is where the old SAVE_CONFIG path silently wrote the wrong shaper.
TEST_CASE("shaper_config_edits applied to a config with no [input_shaper]",
          "[input_shaper][config_edits]") {
    helix::system::KlipperConfigEditor editor;

    std::string content = "[printer]\nkinematics: corexy\nmax_velocity: 300\n"
                          "\n"
                          "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
                          "#*# [probe]\n"
                          "#*# z_offset = 1.234\n";

    auto edits = shaper_config_edits(make_shaper("ei", 58.2f), make_shaper("mzv", 35.0f));
    auto result = editor.apply_edits(content, "input_shaper", edits);
    REQUIRE(result.has_value());

    auto structure = editor.parse_structure(*result);
    REQUIRE(structure.sections.count("input_shaper") == 1);
    CHECK(structure.find_key("input_shaper", "shaper_type_x")->value == "ei");
    CHECK(structure.find_key("input_shaper", "shaper_freq_x")->value == "58.2");
    CHECK(structure.find_key("input_shaper", "shaper_type_y")->value == "mzv");
    CHECK(structure.find_key("input_shaper", "shaper_freq_y")->value == "35.0");

    // Klipper's own block stays below everything we wrote
    CHECK(result->find("[input_shaper]") < result->find("#*# <-"));
    CHECK(result->find("#*# z_offset = 1.234") != std::string::npos);

    // Re-applying is idempotent: no second section, no duplicate keys
    auto again = editor.apply_edits(*result, "input_shaper", edits);
    REQUIRE(again.has_value());
    CHECK(again->find("[input_shaper]") == again->rfind("[input_shaper]"));
    auto structure2 = editor.parse_structure(*again);
    CHECK(structure2.sections["input_shaper"].keys.size() == 4);
}
