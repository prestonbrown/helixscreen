// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_button_cap.cpp
 * @brief The header action button must fit inside header_height at every tier
 *
 * header_bar.xml's action_button_height default is `content`, so most headers
 * self-size correctly. But three panels pass #button_height explicitly, and
 * button_height's ladder exceeds header_height's from the LARGE tier up
 * (72>60, 80>68, 96>80). The button then renders taller than the bar that
 * contains it, centers via style_flex_cross_place, and clips off the top of
 * the screen — measured at 480x800 as header h=68, button h=80, y=-6.
 *
 * header_button_height is the cap applied via style_max_height. This file pins
 * the invariant that makes the cap correct: it must be strictly smaller than
 * header_height at EVERY tier, or the cap is a no-op at that tier. It also
 * pins that the cap is actually WIRED to the XML elements — token values
 * alone don't catch a revert of the style_max_height attributes themselves.
 */

#include "theme_manager.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"

namespace {

/// Every tier suffix the responsive ladder can select.
constexpr const char* TIER_SUFFIXES[] = {
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
    for (const char* suffix : TIER_SUFFIXES) {
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

TEST_CASE("the oversized token that motivated the cap is still oversized",
          "[theme][header][header-button-cap]") {
    // Not a tautology: this asserts the PROBLEM still exists, so if someone
    // later retunes button_height to fit, this test tells them the cap became
    // redundant rather than letting it rot silently.
    const auto large = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_large");
    CHECK(to_px(large, "button_height") > to_px(large, "header_height"));
}

TEST_CASE("header_bar.xml actually applies the cap to both action buttons",
          "[theme][header][header-button-cap]") {
    // The three cases above only exercise TOKEN VALUES, via
    // theme_manager_parse_all_xml_for_suffix / theme_manager_token_uses_vertical_axis.
    // None of them read the <ui_button> elements themselves, so none would
    // notice if style_max_height="#header_button_height" were reverted from
    // action_button / action_button_2 while the token definitions stayed put —
    // that revert IS the regression this whole cap exists to prevent, and it
    // would leave all three token-only cases green. This case closes that gap
    // by reading header_bar.xml directly and pinning the attribute onto both
    // button elements, no LVGL init required.
    std::ifstream file("ui_xml/header_bar.xml");
    REQUIRE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    // Isolate each button's own opening tag (up to its first unescaped '>')
    // so the check is pinned to that element, not just "the attribute exists
    // somewhere in the file."
    auto element_has_cap = [&](const std::string& name) {
        const std::string needle = "name=\"" + name + "\"";
        const auto tag_start = xml.find(needle);
        REQUIRE(tag_start != std::string::npos);
        const auto tag_end = xml.find('>', tag_start);
        REQUIRE(tag_end != std::string::npos);
        const std::string tag = xml.substr(tag_start, tag_end - tag_start);
        return tag.find("style_max_height=\"#header_button_height\"") != std::string::npos;
    };

    // Exact-quoted match: "action_button" does not match "action_button_2"'s
    // element because the quote immediately follows the name.
    CHECK(element_has_cap("action_button"));
    CHECK(element_has_cap("action_button_2"));
}
