// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_response_lines.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
nlohmann::json J(const char* s) {
    return nlohmann::json::parse(s);
}

std::vector<std::string> walk(const nlohmann::json& msg) {
    std::vector<std::string> out;
    for_each_gcode_response_line(msg, [&](const std::string& l) { out.push_back(l); });
    return out;
}
} // namespace

TEST_CASE("for_each_gcode_response_line walks both params shapes",
          "[gcode_response_lines][application]") {
    using V = std::vector<std::string>;

    SECTION("nested array") {
        REQUIRE(walk(J(R"({"params":[["a","b"]]})")) == V{"a", "b"});
    }
    SECTION("flat array of strings") {
        REQUIRE(walk(J(R"({"params":["a","b"]})")) == V{"a", "b"});
    }
    SECTION("non-string elements are skipped") {
        REQUIRE(walk(J(R"({"params":[["a",7,"b"]]})")) == V{"a", "b"});
    }
    SECTION("a frame without a usable params array yields nothing") {
        REQUIRE(walk(nlohmann::json::object()).empty());
        REQUIRE(walk(J(R"({"params":"x"})")).empty());
        REQUIRE(walk(J(R"({"params":[]})")).empty());
        REQUIRE(walk(J(R"({"params":[42]})")).empty());
    }
}

TEST_CASE("parse_layer_line truth table", "[gcode_response_lines][application]") {
    struct Row {
        const char* line;
        int current;
        int total;
    };
    const Row rows[] = {
        {"", -1, -1},
        {"ok", -1, -1},
        {"SET_PRINT_STATS_INFO CURRENT_LAYER=5", 5, -1},
        {"SET_PRINT_STATS_INFO TOTAL_LAYER=120", -1, 120},
        {"SET_PRINT_STATS_INFO TOTAL_LAYER=120 CURRENT_LAYER=3", 3, 120},
        {"SET_PRINT_STATS_INFO CURRENT_LAYER=0", 0, -1},
        {"CURRENT_LAYER=5", -1, -1}, // the command name gates the fields
        {";LAYER:12", 12, -1},
        {";LAYER:0", 0, -1},
        {";LAYER:", -1, -1}, // shorter than the 8-char minimum
        {" ;LAYER:12", -1, -1},
        {";LAYER_COUNT:99", -1, -1},
        {"SET_PRINT_STATS_INFO TOTAL_LAYER=9 ;LAYER:4", -1,
         9}, // the comment form is a whole-line match
    };
    for (const auto& r : rows) {
        INFO(r.line);
        const auto got = parse_layer_line(r.line);
        CHECK(got.current == r.current);
        CHECK(got.total == r.total);
    }
}
