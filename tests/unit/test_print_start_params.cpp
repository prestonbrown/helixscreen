// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_start_params.cpp
 * @brief The per-print tool -> slot map a print start carries on Z-Mod
 *        (prestonbrown/helixscreen#2033).
 *
 * Run with: ./build/bin/helix-tests "[print_start_params]"
 */

#include "../fake_moonraker_client.h"
#include "filament_mapper.h"
#include "moonraker_job_api.h"
#include "print_start_params.h"
#include "printer_discovery.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::PrintStartParams;
using namespace helix::print_start;

TEST_CASE("tools: index is the G-code tool, value the 1-based slot", "[print_start_params][2033]") {
    CHECK(tool_slot_params({{0, 2}, {2, 1}}) == PrintStartParams{{"tools", "2,,1"}});
    CHECK(tool_slot_params({{0, 2}, {1, 1}}) == PrintStartParams{{"tools", "2,1"}});
}

TEST_CASE("tools: a tool with no slot stays an empty entry", "[print_start_params][2033]") {
    CHECK(tool_slot_params({{1, 3}}) == PrintStartParams{{"tools", ",3"}});
    CHECK(tool_slot_params({{0, 1}, {3, 4}}) == PrintStartParams{{"tools", "1,,,4"}});
    // A slot below 1 names no slot, so that tool stays empty too.
    CHECK(tool_slot_params({{0, 0}, {1, 2}}) == PrintStartParams{{"tools", ",2"}});
}

TEST_CASE("tools: nothing mapped sends no parameter", "[print_start_params][2033]") {
    CHECK(tool_slot_params({}).empty());
    CHECK(tool_slot_params({{0, 0}, {-1, 2}}).empty());
}

TEST_CASE("a lane name's trailing digits are its slot", "[print_start_params][2033]") {
    CHECK(slot_number_of_lane("lane1") == 1);
    CHECK(slot_number_of_lane("lane4") == 4);
    CHECK(slot_number_of_lane("lane11") == 11);
    CHECK_FALSE(slot_number_of_lane("lane").has_value());
    CHECK_FALSE(slot_number_of_lane("lane0").has_value());
    CHECK_FALSE(slot_number_of_lane("").has_value());
}

TEST_CASE("only Z-Mod with its built-in AFC takes the tools map", "[print_start_params][2033]") {
    helix::PrinterDiscovery zmod;
    zmod.parse_objects(nlohmann::json::array({"AFC", "AFC_lane lane1", "zmod_color"}));
    CHECK(takes_tool_slots(zmod));

    // AFC=0, or a build older than the emulation: nothing says its Moonraker
    // reads `tools`, so it keeps its own prompt.
    helix::PrinterDiscovery plain;
    plain.parse_objects(nlohmann::json::array({"zmod_color", "zmod_ifs"}));
    CHECK_FALSE(takes_tool_slots(plain));

    helix::PrinterDiscovery afclite;
    afclite.parse_objects(
        nlohmann::json::array({"AFC", "AFC_lane E0", "zmod_afclite", "zmod_color"}));
    CHECK_FALSE(takes_tool_slots(afclite));

    helix::PrinterDiscovery afc;
    afc.parse_objects(nlohmann::json::array({"AFC", "AFC_lane lane1", "AFC_BoxTurtle Turtle_1"}));
    CHECK_FALSE(takes_tool_slots(afc));
}

namespace {
const helix::test::FakeMoonrakerClient::RpcCall*
find_start(const helix::test::FakeMoonrakerClient& client) {
    const auto it = std::find_if(client.rpc_calls.begin(), client.rpc_calls.end(),
                                 [](const auto& c) { return c.method == "printer.print.start"; });
    return it == client.rpc_calls.end() ? nullptr : &*it;
}
} // namespace

TEST_CASE("printer.print.start carries the extra parameters beside the filename",
          "[print_start_params][2033]") {
    helix::test::FakeMoonrakerClient client;
    MoonrakerJobAPI job(client);

    SECTION("with tools") {
        job.start_print("part.gcode", [] {}, [](const MoonrakerError&) {}, {{"tools", "2,,1"}});
        const auto* start = find_start(client);
        REQUIRE(start != nullptr);
        CHECK(start->params["filename"] == "part.gcode");
        CHECK(start->params["tools"] == "2,,1");
    }

    SECTION("without") {
        job.start_print("part.gcode", [] {}, [](const MoonrakerError&) {});
        const auto* start = find_start(client);
        REQUIRE(start != nullptr);
        CHECK(start->params == nlohmann::json{{"filename", "part.gcode"}});
    }
}

namespace {
helix::ToolMapping mapping(int tool, int slot, int backend = 0, bool is_auto = false) {
    helix::ToolMapping m;
    m.tool_index = tool;
    m.mapped_slot = slot;
    m.mapped_backend = backend;
    m.is_auto = is_auto;
    return m;
}
} // namespace

TEST_CASE("the full map carries every row and leaves Auto to the firmware",
          "[print_start_params][2033]") {
    // T0 picked onto slot 1, T1 a seed on slot 0, T2 Auto, T3 with no row.
    CHECK(full_tool_slots({mapping(0, 1), mapping(1, 0), mapping(2, -1, 0, true)}, 0) ==
          std::map<int, int>{{0, 1}, {1, 0}});
    // An Auto row still naming a slot is Auto all the same.
    CHECK(full_tool_slots({mapping(0, 2, 0, true)}, 0).empty());
    // A row on another backend is not this backend's slot.
    CHECK(full_tool_slots({mapping(0, 1), mapping(1, 3, 1)}, 0) == std::map<int, int>{{0, 1}});
}

TEST_CASE("the map belongs to the backend of its first mapped row", "[print_start_params][2033]") {
    CHECK(mapped_backend({}) == 0);
    CHECK(mapped_backend({mapping(0, -1, 1, true), mapping(1, 2, 1)}) == 1);
    CHECK(mapped_backend({mapping(0, 2, -1)}) == 0);
}
