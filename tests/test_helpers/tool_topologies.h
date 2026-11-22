// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "printer_discovery.h"
#include "tool_state.h"

#include <initializer_list>
#include <string>

#include "hv/json.hpp"

namespace helix::test {

/// A klipper-toolchanger printer with @p heads heads as discovery enumerates
/// it: tool Ti on extruder i ("extruder", "extruder1", ...).
inline PrinterDiscovery make_head_discovery(int heads) {
    nlohmann::json objects = nlohmann::json::array({"toolchanger"});
    for (int i = 0; i < heads; ++i) {
        objects.push_back("tool T" + std::to_string(i));
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    }
    PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}

/// @p heads heads, one tool each, every tool printing through its own
/// extruder ("extruder", "extruder1", ...) and sourcing lane i.
inline ToolTopology make_head_topology(int heads, int active_tool = 0) {
    ToolTopology topo;
    topo.tool_count = heads;
    topo.active_tool = active_tool;
    for (int i = 0; i < heads; ++i) {
        topo.tool_to_slot.push_back(i);
        topo.tool_extruder.push_back(i == 0 ? std::string("extruder")
                                            : "extruder" + std::to_string(i));
    }
    return topo;
}

/// A seven-head changer whose AFC units publish eighteen lanes, each its own
/// tool: four heads fed directly, then hubs of four, four and six lanes into
/// extruder4, extruder5 and extruder6. Lane i is slot i. Each tool in
/// @p seated_tools has its lane seated in its head.
inline ToolTopology make_hub_topology(int active_tool = 0,
                                      std::initializer_list<int> seated_tools = {}) {
    const char* extruders[] = {"extruder",  "extruder1", "extruder2", "extruder3", "extruder4",
                               "extruder4", "extruder4", "extruder4", "extruder5", "extruder5",
                               "extruder5", "extruder5", "extruder6", "extruder6", "extruder6",
                               "extruder6", "extruder6", "extruder6"};
    ToolTopology topo;
    topo.tool_count = 18;
    topo.active_tool = active_tool;
    topo.tools_are_lanes = true;
    for (int i = 0; i < 18; ++i) {
        topo.tool_to_slot.push_back(i);
        topo.tool_extruder.emplace_back(extruders[i]);
    }
    if (seated_tools.size() > 0) {
        topo.tool_seated.assign(18, false);
        for (const int t : seated_tools) {
            topo.tool_seated[static_cast<size_t>(t)] = true;
        }
    }
    return topo;
}

/// The reporter's Voron 2.4 (debug bundle LB6LEZ44): klipper-toolchanger tools
/// T0..T4 on extruder..extruder4, a sixth AFC head on extruder5 with no tool
/// object, and twelve AFC lanes whose T numbers follow AFC's lane map.
inline PrinterDiscovery make_lb6lez44_discovery() {
    nlohmann::json objects = nlohmann::json::array({"toolchanger"});
    for (int i = 0; i < 5; ++i) {
        objects.push_back("tool T" + std::to_string(i));
    }
    objects.push_back("extruder");
    for (int i = 1; i < 6; ++i) {
        objects.push_back("extruder" + std::to_string(i));
    }
    PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}

/// AFC's lanes in its own order, each with the T number it maps and the head
/// it feeds; tool Tn sources the lane mapped Tn. lane4 is Tooled in extruder2
/// (mounted), lane2 is loaded in extruder1, the extruder4 lane is Tooled.
inline ToolTopology make_lb6lez44_topology() {
    struct Lane {
        int tool;
        const char* extruder;
        bool seated;
    };
    const Lane lanes[] = {
        {5, "extruder4", true},   // extruder4
        {6, "extruder5", false},  // extruder5
        {8, "extruder", false},   // lane1
        {2, "extruder1", true},   // lane2
        {7, "extruder2", false},  // lane3, HTLF_1
        {0, "extruder2", true},   // lane4, HTLF_1
        {4, "extruder", false},   // lane5, NightOwl
        {1, "extruder", false},   // lane6, NightOwl
        {10, "extruder3", false}, // lane7, Turtle_1
        {9, "extruder3", false},  // lane8
        {3, "extruder3", false},  // lane9
        {11, "extruder3", false}, // lane10
    };
    ToolTopology topo;
    topo.tool_count = 12;
    topo.active_tool = 0;
    topo.allows_empty_carriage = true;
    topo.tools_are_lanes = true;
    topo.tool_to_slot.assign(12, -1);
    topo.tool_extruder.assign(12, "");
    topo.tool_seated.assign(12, false);
    for (int slot = 0; slot < 12; ++slot) {
        const Lane& lane = lanes[slot];
        topo.tool_to_slot[static_cast<size_t>(lane.tool)] = slot;
        topo.tool_extruder[static_cast<size_t>(lane.tool)] = lane.extruder;
        topo.tool_seated[static_cast<size_t>(lane.tool)] = lane.seated;
    }
    return topo;
}

} // namespace helix::test
