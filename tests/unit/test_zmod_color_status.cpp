// SPDX-License-Identifier: GPL-3.0-or-later

#include "zmod_color_status.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("zmod_color slots are indexed by their 1-based ID", "[zmod_color]") {
    const json obj = {
        {"slots", json::array({{{"ID", "2"}, {"Material", "PETG"}, {"HEX", "0ACC38"}},
                               {{"ID", 1}, {"Material", "PLA"}, {"HEX", "FFFFFF"}}})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    REQUIRE(slots->size() == 4);
    REQUIRE((*slots)[0].has_value());
    CHECK((*slots)[0]->material == "PLA");
    CHECK((*slots)[1]->material == "PETG");
    CHECK((*slots)[1]->hex == "0ACC38");
    CHECK_FALSE((*slots)[2].has_value());
}

TEST_CASE("zmod_color's '?' material reads as unset", "[zmod_color]") {
    const json obj = {{"slots", json::array({{{"ID", "1"}, {"Material", "?"}, {"HEX", ""}}})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    REQUIRE((*slots)[0].has_value());
    CHECK((*slots)[0]->material.empty());
}

TEST_CASE("zmod_color drops out-of-range and malformed slot IDs", "[zmod_color]") {
    const json obj = {{"slots", json::array({{{"ID", "0"}, {"Material", "PLA"}},
                                             {{"ID", "5"}, {"Material", "PLA"}},
                                             {{"ID", "x"}, {"Material", "PLA"}},
                                             "not an object"})}};
    auto slots = zmod_color::parse_slots(obj, 4);
    REQUIRE(slots.has_value());
    for (const auto& s : *slots) {
        CHECK_FALSE(s.has_value());
    }
}

TEST_CASE("A zmod_color frame without slots is no news", "[zmod_color]") {
    CHECK_FALSE(zmod_color::parse_slots(json{{"active_tool_id", 2}}, 4).has_value());
    CHECK_FALSE(zmod_color::parse_palette(json{{"active_tool_id", 2}}).has_value());
    CHECK_FALSE(zmod_color::parse_valid_types(json{{"active_tool_id", 2}}).has_value());
}

TEST_CASE("zmod_color palette keeps firmware index order", "[zmod_color]") {
    const json obj = {{"palette", json::array({"FFFFFF", "fef043", "junk", "161616"})}};
    auto palette = zmod_color::parse_palette(obj);
    REQUIRE(palette.has_value());
    REQUIRE(palette->size() == 3);
    CHECK((*palette)[0] == std::pair<int, std::uint32_t>{0, 0xFFFFFF});
    CHECK((*palette)[1] == std::pair<int, std::uint32_t>{1, 0xFEF043});
    CHECK((*palette)[2] == std::pair<int, std::uint32_t>{3, 0x161616});
}

TEST_CASE("zmod_color valid_types drops the '?' sentinel", "[zmod_color]") {
    const json obj = {{"valid_types", json::array({"PLA", "PETG", "?"})}};
    auto types = zmod_color::parse_valid_types(obj);
    REQUIRE(types.has_value());
    CHECK(*types == std::vector<std::string>{"PLA", "PETG"});
}
