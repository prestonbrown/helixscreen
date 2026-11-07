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

TEST_CASE("server.info's print_start_tools flag is read from zmod.features",
          "[print_start_params][2033]") {
    using nlohmann::json;
    CHECK(server_takes_tool_slots(
        json{{"zmod", {{"features", json::array({"print_start_tools"})}}}}));
    CHECK(server_takes_tool_slots(
        json{{"zmod", {{"features", json::array({"other", "print_start_tools"})}}}}));

    CHECK_FALSE(server_takes_tool_slots(json::object()));
    CHECK_FALSE(server_takes_tool_slots(json{{"zmod", json::object()}}));
    CHECK_FALSE(server_takes_tool_slots(json{{"zmod", {{"features", json::array()}}}}));
    CHECK_FALSE(server_takes_tool_slots(json{{"zmod", {{"features", "print_start_tools"}}}}));
    CHECK_FALSE(server_takes_tool_slots(json{{"zmod", {{"features", json::array({1, 2})}}}}));
    CHECK_FALSE(server_takes_tool_slots(json{{"zmod", "print_start_tools"}}));
    CHECK_FALSE(server_takes_tool_slots(json::array()));
    // Only Z-Mod's own member counts.
    CHECK_FALSE(server_takes_tool_slots(json{{"features", json::array({"print_start_tools"})}}));
}

TEST_CASE("a Moonraker advertising print_start_tools takes the map with AFC off",
          "[print_start_params][2033]") {
    helix::PrinterDiscovery ifs;
    ifs.set_server_takes_print_start_tools(true);
    // Discovery reads server.info before the object list; parsing keeps the flag.
    ifs.parse_objects(nlohmann::json::array({"zmod_color", "zmod_ifs"}));
    REQUIRE_FALSE(ifs.has_zmod_afc());
    CHECK(takes_tool_slots(ifs));

    helix::PrinterDiscovery quiet;
    quiet.parse_objects(nlohmann::json::array({"zmod_color", "zmod_ifs"}));
    CHECK_FALSE(quiet.server_takes_print_start_tools());
    CHECK_FALSE(takes_tool_slots(quiet));
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

TEST_CASE("the start request names a plate only for a printer-started .3mf plate",
          "[print_start_params][native_3mf]") {
    helix::print_3mf::Route route;
    route.kind = helix::print_3mf::RouteKind::NativePlate;
    route.plate = 5;
    CHECK(start_request_params(route, {}) == PrintStartParams{{"plateindex", "5"}});
    CHECK(start_request_params(route, {{"tools", "2,,1"}}) ==
          PrintStartParams{{"plateindex", "5"}, {"tools", "2,,1"}});
    // An extracted plate starts as G-code: the tool map alone.
    route.kind = helix::print_3mf::RouteKind::ExtractPlate;
    CHECK(start_request_params(route, {{"tools", "1"}}) == PrintStartParams{{"tools", "1"}});
    CHECK(start_request_params({}, {}).empty());
}
