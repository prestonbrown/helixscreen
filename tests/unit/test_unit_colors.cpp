// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_unit_colors.cpp
 * @brief A printed unit's colors: presets, their saved form, and the region layers they tint.
 */

#include "unit_colors.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

TEST_CASE("unit colors: presets are named, keyed once, and set all three regions",
          "[unit-colors]") {
    std::set<std::string> ids;
    for (const UnitColorPreset& preset : unit_color_presets()) {
        CHECK(std::string(preset.name).size() > 0);
        CHECK(ids.insert(preset.id).second);
        const UnitColors colors = preset.colors();
        CHECK(colors.main == preset.main);
        CHECK(colors.accent == preset.accent);
        CHECK(colors.panels == preset.panels);
        CHECK(find_unit_color_preset(preset.id) == &preset);
    }
    CHECK(ids.size() >= 2);
    CHECK(find_unit_color_preset("no_such_preset") == nullptr);
}

TEST_CASE("unit colors: saved as hex by region, unset regions left out", "[unit-colors]") {
    UnitColors colors;
    CHECK(unit_colors_to_json(colors) == json::object());
    colors.set(UnitColorRegion::Main, 0x112233);
    colors.set(UnitColorRegion::Panels, 0xABCDEF);
    const json saved = unit_colors_to_json(colors);
    CHECK(saved == json{{"main", "#112233"}, {"panels", "#ABCDEF"}});
    CHECK(unit_colors_from_json(saved) == colors);
    CHECK_FALSE(unit_colors_from_json(saved).accent.has_value());
}

TEST_CASE("unit colors: an unreadable region reads back unset", "[unit-colors]") {
    const json saved = {{"main", "not a color"}, {"accent", 42}, {"panels", "#00ff00"}};
    const UnitColors colors = unit_colors_from_json(saved);
    CHECK_FALSE(colors.main.has_value());
    CHECK_FALSE(colors.accent.has_value());
    CHECK(colors.panels == 0x00FF00u);
    CHECK(unit_colors_from_json(json::array()).empty());
}

TEST_CASE("unit colors: a drawing tints only the regions it bakes and the user colored",
          "[unit-colors]") {
    UnitColors colors;
    colors.main = 0x101010;
    colors.accent = 0xC02020;

    // No masks baked: the drawing stays as baked, whatever the user picked.
    CHECK(unit_color_layers(0, colors).empty());

    const uint8_t masks = unit_color_mask_bit(UnitArtDepth::Back, UnitColorRegion::Main) |
                          unit_color_mask_bit(UnitArtDepth::Back, UnitColorRegion::Panels) |
                          unit_color_mask_bit(UnitArtDepth::Front, UnitColorRegion::Main) |
                          unit_color_mask_bit(UnitArtDepth::Front, UnitColorRegion::Accent);
    // Nothing picked: no layers.
    CHECK(unit_color_layers(masks, UnitColors{}).empty());

    // Back before front, each depth in region order; panels have a mask but no color.
    const std::vector<UnitColorLayer> layers = unit_color_layers(masks, colors);
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].stem == "back_main");
    CHECK(layers[0].color == 0x101010u);
    CHECK(layers[1].stem == "front_main");
    CHECK(layers[2].stem == "front_accent");
    CHECK(layers[2].depth == UnitArtDepth::Front);
    CHECK(layers[2].color == 0xC02020u);
}

TEST_CASE("unit colors: each depth and region has its own mask bit", "[unit-colors]") {
    std::set<int> bits;
    for (UnitArtDepth depth : {UnitArtDepth::Back, UnitArtDepth::Front})
        for (UnitColorRegion region : kUnitColorRegions)
            CHECK(bits.insert(unit_color_mask_bit(depth, region)).second);
    CHECK(bits.size() == 6);
}
