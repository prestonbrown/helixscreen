// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_button_cap.cpp
 * @brief Header buttons must fit inside header_height at every tier
 *
 * button_height's ladder exceeds header_height's from the LARGE tier up
 * (72>60, 80>68, 96>80), so a header button sized by it renders taller than
 * its bar and clips off the top of the screen. header_button_height is the
 * height every header_action takes and the z-offset save's cap. This file pins
 * that it is strictly smaller than header_height at EVERY tier, and that the
 * XML actually uses it: token values alone do not catch a revert of the
 * attributes.
 */

#include "theme_manager.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"

namespace {

/// Every tier suffix the responsive ladder can select.
constexpr const char* kTierSuffixes[] = {
    "_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge",
};

int to_px(const std::unordered_map<std::string, std::string>& table, const std::string& base) {
    auto it = table.find(base);
    REQUIRE(it != table.end());
    return std::stoi(it->second);
}

} // namespace

TEST_CASE("header_button_height fits inside header_height at every tier",
          "[theme][header][header-button-cap]") {
    for (const char* suffix : kTierSuffixes) {
        INFO("tier " << suffix);
        const auto px = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", suffix);

        const int header = to_px(px, "header_height");
        const int cap = to_px(px, "header_button_height");

        // Strictly less: equal means the button touches both edges, which is
        // what tiny/small already look like with #button_height and is the
        // visual defect this cap exists to prevent.
        CHECK(cap < header);

        // And not so small it stops being a touch target — at least half the bar.
        CHECK(cap >= header / 2);
    }
}

TEST_CASE("header_button_height resolves from the vertical axis",
          "[theme][header][header-button-cap]") {
    // header_height is on the vertical ladder. If the cap is not, a 480x800
    // portrait screen sizes its header from 800 (XLARGE -> 68) but its cap from
    // 480 (SMALL -> 38), producing a cap far tighter than intended.
    CHECK(theme_manager_token_uses_vertical_axis("header_button_height"));
    CHECK(theme_manager_token_uses_vertical_axis("header_height"));
}

TEST_CASE("the header button cap is applied where header buttons are styled",
          "[theme][header][header-button-cap]") {
    // The cases above read only TOKEN VALUES. A revert of the max_height
    // attribute itself, with the tokens left in place, is the regression the
    // cap exists to prevent, so this reads the XML.
    auto read = [](const char* path) {
        std::ifstream file(path);
        REQUIRE(file.is_open());
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };

    // The opening tag that carries `needle`, up to its first '>'.
    auto tag_with = [](const std::string& xml, const std::string& needle) {
        const auto tag_start = xml.find(needle);
        REQUIRE(tag_start != std::string::npos);
        const auto tag_end = xml.find('>', tag_start);
        REQUIRE(tag_end != std::string::npos);
        return xml.substr(tag_start, tag_end - tag_start);
    };

    const std::string header = read("ui_xml/header_bar.xml");
    CHECK(tag_with(header, "name=\"header_save_z_offset\"")
              .find("style_max_height=\"#header_button_height\"") != std::string::npos);

    const std::string action = read("ui_xml/header_action.xml");
    CHECK(
        tag_with(action, "<view extends=\"ui_button\"").find(" height=\"#header_button_height\"") !=
        std::string::npos);
}
