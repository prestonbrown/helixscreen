// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mock_snapmaker_parity.cpp
 * @brief Pin AmsBackendMock's Snapmaker mode to AmsBackendSnapmaker's rules.
 *
 * The mock is what every `--test` run drives, so a mock that answers a question
 * more confidently than the hardware does turns local verification into a
 * rehearsal of a branch the machine never takes.
 *
 * get_tool_mapping() is the one that bit. The real U1 publishes a routing only
 * while a print task is configured and answers EMPTY otherwise — its own comment
 * calls a bare [0,1,2,3] "the identity-as-truth mistake this accessor exists to
 * end", because an idle machine holds a default table that says nothing about
 * any particular file. The mock instead returned its attachment map, and
 * FilamentMapper::effective_routing() takes any non-empty answer as the truth,
 * so the preview resolved every logical tool to its own head index — the exact
 * shape b7e649ca4 fixed on hardware, where a cancelled T0-red / T2-black print
 * rendered as a black body with a red tail.
 *
 * firmware_default_routing() is the same class of divergence one step quieter:
 * the inherited lane-per-tool identity agrees with the U1's four fixed heads for
 * T0-T3 and disagrees from T4 up, which is precisely where a file with extended
 * tools lands.
 */

#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_types.h"
#include "filament_mapper.h"
#include "firmware_routing.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The routing the app would actually colour by, given what a backend published.
/// This is AmsState::routed_tool_colors()'s own composition — the accessor, the
/// attachment map, and the tool-changer question — so the assertions below are
/// about what reaches the renderer, not about the accessor in isolation.
std::vector<int> app_routing(const AmsBackend& backend) {
    return FilamentMapper::effective_routing(
        backend.get_tool_mapping(), backend.get_system_info().tool_to_slot_map,
        /*attachment_is_routing=*/!is_tool_changer(backend.get_type()));
}

/// The backend's own lanes, in the shape AmsState hands to the colour engine.
/// Taken from the backend rather than written out here so the cases below assert
/// the ROUTING - which lane's colour a tool ends up with - and not a palette the
/// mock is free to restyle.
std::vector<AvailableSlot> lanes_of(const AmsBackend& backend) {
    std::vector<AvailableSlot> slots;
    const int total = backend.get_system_info().total_slots;
    for (int i = 0; i < total; ++i) {
        const SlotInfo info = backend.get_slot_info(i);
        AvailableSlot s{};
        s.slot_index = i;
        s.backend_index = 0;
        s.color_rgb = info.color_rgb;
        s.material = info.material;
        s.is_empty = !info.is_present();
        s.current_tool_mapping = info.mapped_tool;
        slots.push_back(std::move(s));
    }
    return slots;
}

} // namespace

// ============================================================================
// No task configured — the answer is "no opinion", on both backends
// ============================================================================

TEST_CASE("Mock Snapmaker with no print task publishes no routing", "[ams][snapmaker][mock]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);

    // The attachment map is still there and still 1:1 — that is the point. It is
    // which lane each head owns, never which head prints a given tool, and
    // returning it here is what made the mock lie.
    REQUIRE(mock.get_system_info().tool_to_slot_map.size() == 4);
    CHECK(mock.get_tool_mapping().empty());

    // Parity: an idle real U1 says the same thing.
    SnapmakerProbe real;
    CHECK(real.get_tool_mapping().empty());
}

TEST_CASE("Mock Snapmaker with no task leaves the slicer palette alone", "[ams][snapmaker][mock]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);

    // A U1 is a tool changer, so its attachment map is not allowed to stand in
    // for a routing — with nothing published the app has no routing at all and
    // the file's own tool colours survive.
    REQUIRE(app_routing(mock).empty());
    CHECK(FilamentMapper::routed_tool_colors(app_routing(mock), lanes_of(mock), helix::printer::ToolMappingOrigin::Unvouched).empty());
}

// ============================================================================
// A task configured — the routing is the task's, not the lanes'
// ============================================================================

TEST_CASE("Mock Snapmaker publishes the configured task routing", "[ams][snapmaker][mock]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);

    // A crossover: T0 prints from head 2, T2 from head 0. Identity would answer
    // [0,1,2,3] here and get both of them wrong.
    mock.set_snapmaker_print_task({2, 1, 0, 3});

    const std::vector<int> expected{2, 1, 0, 3};
    CHECK(mock.get_tool_mapping() == expected);
    CHECK(app_routing(mock) == expected);

    // And it reaches the renderer as the task's colours, not the lanes' order:
    // T0 gets head 2's filament and T2 gets head 0's. Identity would hand each
    // tool its own lane, so a crossover is what tells the two answers apart.
    const auto colors = FilamentMapper::routed_tool_colors(app_routing(mock), lanes_of(mock), helix::printer::ToolMappingOrigin::Unvouched);
    REQUIRE(colors.size() == 4);
    CHECK(colors[0] == mock.get_slot_info(2).color_rgb);
    CHECK(colors[2] == mock.get_slot_info(0).color_rgb);
    REQUIRE(mock.get_slot_info(0).color_rgb != mock.get_slot_info(2).color_rgb);

    // Clearing the task returns the machine to "no opinion" — a print ending is
    // not a reason to keep asserting how it was routed.
    mock.set_snapmaker_print_task({});
    CHECK(mock.get_tool_mapping().empty());
}

TEST_CASE("HELIX_MOCK_REMAP stages a Snapmaker task, not a lane move",
          "[ams][snapmaker][mock][remap]") {
    // On a U1 a user remap does not move the lanes: it becomes the task's
    // extruder_map_table. The knob exists to show a non-identity routing in the
    // swatches, which it can only do through the routing accessor now that the
    // attachment map no longer stands in for one.
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);
    REQUIRE(mock.get_tool_mapping().empty());

    mock.apply_remap_overrides("0:2,2:0");

    const auto routing = mock.get_tool_mapping();
    REQUIRE(routing.size() >= 3);
    CHECK(routing[0] == 2);
    CHECK(routing[2] == 0);
}

// ============================================================================
// The firmware default is four fixed heads, not lane-per-tool
// ============================================================================

TEST_CASE("Mock Snapmaker declares the four-head U1 default routing",
          "[ams][snapmaker][mock][routing]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);
    SnapmakerProbe real;

    const auto mock_routing = mock.firmware_default_routing();
    const auto real_routing = real.firmware_default_routing();

    // Compared against the real backend rather than a literal, so this cannot
    // drift if the U1's table ever changes. T4+ is where the inherited identity
    // silently disagreed.
    for (int tool = 0; tool <= 6; ++tool) {
        CAPTURE(tool);
        CHECK(mock_routing.head(tool) == real_routing.head(tool));
    }
    CHECK(mock_routing.head(5) == 0);
}

TEST_CASE("A non-Snapmaker mock stays lane-per-tool", "[ams][snapmaker][mock][routing]") {
    // The override must be scoped to the emulated machine: every other mode this
    // mock wears is a lane-per-tool system, where a four-head table would send
    // every tool above 3 to lane 0.
    AmsBackendMock mock(8);
    CHECK(mock.firmware_default_routing().head(7) == 7);

    mock.set_tool_changer_mode(true);
    CHECK(mock.firmware_default_routing().head(7) == 7);
}
