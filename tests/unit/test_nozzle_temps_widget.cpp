// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_temps_widget.cpp
 * @brief NozzleTempsWidget builds one row per distinct physical extruder.
 */

#include "src/ui/panel_widgets/nozzle_temps_widget.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Regression: an AFC BoxTurtle (and Happy Hare / ERCF) is one physical extruder
// fed by N spool lanes. ToolState models the lanes as N logical tools (T0..Tn)
// that all carry extruder_name="extruder", so iterating tools() builds N
// identical nozzle rows — the widget showed "150°" four times. The widget must
// collapse to distinct physical extruders.
TEST_CASE("NozzleTempsWidget: collapses multiplexed lanes to one nozzle row",
          "[nozzle_temps][panel_widget][afc]") {
    SECTION("AFC: 4 lanes feeding one extruder -> one row") {
        std::vector<ToolInfo> tools;
        for (int i = 0; i < 4; ++i) {
            ToolInfo t;
            t.index = i;
            t.name = "T" + std::to_string(i);
            t.extruder_name = "extruder"; // all lanes share the single extruder
            tools.push_back(std::move(t));
        }
        auto extruders = distinct_extruder_names(tools);
        REQUIRE(extruders.size() == 1);
        REQUIRE(extruders[0] == "extruder");
    }

    SECTION("Toolchanger: 4 distinct extruders -> four rows, order preserved") {
        std::vector<ToolInfo> tools;
        const char* names[] = {"extruder", "extruder1", "extruder2", "extruder3"};
        for (int i = 0; i < 4; ++i) {
            ToolInfo t;
            t.index = i;
            t.name = "T" + std::to_string(i);
            t.extruder_name = names[i];
            tools.push_back(std::move(t));
        }
        auto extruders = distinct_extruder_names(tools);
        REQUIRE(extruders.size() == 4);
        REQUIRE(extruders[0] == "extruder");
        REQUIRE(extruders[1] == "extruder1");
        REQUIRE(extruders[2] == "extruder2");
        REQUIRE(extruders[3] == "extruder3");
    }

    SECTION("Tools without an extruder mapping are dropped") {
        std::vector<ToolInfo> tools(2);
        tools[0].extruder_name = "extruder";
        tools[1].extruder_name = std::nullopt;
        auto extruders = distinct_extruder_names(tools);
        REQUIRE(extruders.size() == 1);
        REQUIRE(extruders[0] == "extruder");
    }

    SECTION("A tool naming an empty extruder heats nothing and gets no row") {
        std::vector<ToolInfo> tools(3);
        tools[0].extruder_name = "extruder";
        tools[1].extruder_name = std::string();
        tools[2].extruder_name = "extruder1";
        auto extruders = distinct_extruder_names(tools);
        REQUIRE(extruders == std::vector<std::string>{"extruder", "extruder1"});
    }

    SECTION("Empty tool list -> no rows") {
        REQUIRE(distinct_extruder_names({}).empty());
    }
}
