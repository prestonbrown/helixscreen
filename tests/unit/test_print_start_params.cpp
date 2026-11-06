// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_start_params.cpp
 * @brief The per-print tool -> slot map a print start carries on Z-Mod
 *        (prestonbrown/helixscreen#2033).
 *
 * Run with: ./build/bin/helix-tests "[print_start_params]"
 */

#include "../fake_moonraker_client.h"
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

TEST_CASE("only a printer with zmod_color takes the tools map", "[print_start_params][2033]") {
    helix::PrinterDiscovery zmod;
    zmod.parse_objects(nlohmann::json::array({"AFC", "AFC_lane lane1", "zmod_color"}));
    CHECK(takes_tool_slots(zmod));

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
