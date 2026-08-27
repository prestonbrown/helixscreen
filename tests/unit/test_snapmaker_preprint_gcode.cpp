// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for AmsBackendSnapmaker::build_preprint_gcode — the pure builder
// that produces the firmware-native print_task_config command sequence
// (SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS) emitted before
// PRINT_START on the Snapmaker U1.
//
// The U1 is a true toolchanger with 4 identical physical heads (0-3); logical
// gcode tools span 0-31. Firmware's default map is [0,1,2,3,0,0,...], so any
// extended tool (4-31) without an explicit user remap falls to physical head 0.
//
// The function is intentionally network-free, so we construct the backend with
// a nullptr api/client probe — mirroring SnapmakerProbe in test_remap_strategy.cpp.

#include "ams_backend_snapmaker.h"
#include "filament_mapper.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

// Minimal probe — constructed with nullptr api/client so no Moonraker
// connection is required. build_preprint_gcode is a pure const method that
// never touches api_, so a default/probe instance is sufficient.
class SnapmakerProbe : public AmsBackendSnapmaker {
  public:
    SnapmakerProbe() : AmsBackendSnapmaker(nullptr, nullptr) {}
};

} // namespace

TEST_CASE("Snapmaker build_preprint_gcode writes every used tool explicitly",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    // Tools landing on their firmware-default head still get a SET_PRINT_EXTRUDER_MAP
    // line. The command sets one entry and resets nothing, so emitting only the
    // genuine remaps left every other entry at whatever the PREVIOUS print wrote —
    // and a job that had remapped T0->head2 then made the next job print T0 from
    // head 2. Writing each used entry says what THIS print means.

    SECTION("two non-contiguous tools, no remap") {
        REQUIRE(sm.build_preprint_gcode({0, 2}, {}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2");
    }

    SECTION("all four heads, no remap") {
        REQUIRE(sm.build_preprint_gcode({0, 1, 2, 3}, {}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=1\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=3 MAP_EXTRUDER=3\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,1,2,3");
    }

    SECTION("single tool, no remap") {
        REQUIRE(sm.build_preprint_gcode({1}, {}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=1\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1");
    }
}

TEST_CASE("Snapmaker build_preprint_gcode emits extruder map lines for remaps",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    SECTION("single remapped tool") {
        REQUIRE(sm.build_preprint_gcode({1}, {{1, 3}}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=3\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=3");
    }

    SECTION("two remapped tools emit in ascending key order") {
        REQUIRE(sm.build_preprint_gcode({0, 2}, {{0, 1}, {2, 3}}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,3");
    }
}

TEST_CASE("Snapmaker build_preprint_gcode dedups colliding heads", "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    // Tool 0 -> head 0 (identity), tool 1 remapped -> head 0. Heads {0,0}
    // collapse to {0}.
    REQUIRE(sm.build_preprint_gcode({0, 1}, {{1, 0}}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0");
}

TEST_CASE("Snapmaker build_preprint_gcode empty tools_used yields empty string",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    REQUIRE(sm.build_preprint_gcode({}, {}) == "");
}

TEST_CASE("Snapmaker build_preprint_gcode extended tool defaults to head 0",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    // default_head(5) == 0 (firmware default map [0,1,2,3,0,0,...]).
    REQUIRE(sm.build_preprint_gcode({5}, {}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=5 MAP_EXTRUDER=0\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0");
}

// End-to-end scenario for the Batch 2 native-remap UI: a 2-color body using
// logical tools {0,2}, with the user remapping tool 0 → head 1 in the picker.
// get_effective_remap() identity-filters, so only tool 0 yields an entry
// {0,1}; tool 2 stays identity (head 2). build_preprint_gcode then emits one
// SET_PRINT_EXTRUDER_MAP line plus the recomputed used-heads {1,2}.
TEST_CASE("Snapmaker build_preprint_gcode 2-color remap tool0->head1", "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    REQUIRE(sm.build_preprint_gcode({0, 2}, {{0, 1}}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=2\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,2");
}

// Focused unit test of the IDENTITY-FILTER / conversion logic that
// PrintSelectDetailView::get_effective_remap() performs on the card's stored
// mappings vector. Full UI modal simulation is impractical (XML/LVGL fixture
// heavy), so we replicate the exact filter inline as a lambda and assert the
// produced map<int,int> matches get_effective_remap()'s contract:
//   * mapped_slot < 0 (auto/unmapped)          -> dropped
//   * mapped_slot == default_head(tool_index)  -> dropped (firmware identity)
//   * mapped_slot >= 0 && != default_head      -> kept as a true remap
// This test FAILS if the identity-filter logic regresses.
TEST_CASE("get_effective_remap identity-filter drops identity + auto entries",
          "[snapmaker][remap]") {
    // The production identity filter itself. get_effective_remap() is nothing but
    // effective_mappings() piped through this, so a change to the filter shows up
    // in these expectations instead of leaving a stale copy green.
    auto effective_remap = &helix::FilamentMapper::identity_filtered_remap;

    auto mk = [](int tool, int slot) {
        helix::ToolMapping m;
        m.tool_index = tool;
        m.mapped_slot = slot;
        return m;
    };

    SECTION("mix of identity, auto, and true remaps keeps only true remaps") {
        std::vector<helix::ToolMapping> mappings = {
            mk(0, 1),  // true remap: head 1 != default_head(0)=0  -> KEEP
            mk(1, 1),  // identity: head 1 == default_head(1)=1     -> DROP
            mk(2, -1), // auto/unmapped: mapped_slot < 0            -> DROP
            mk(3, 0),  // true remap: head 0 != default_head(3)=3   -> KEEP
        };
        std::map<int, int> expected = {{0, 1}, {3, 0}};
        REQUIRE(effective_remap(mappings) == expected);
    }

    SECTION("all-identity map yields empty remap") {
        std::vector<helix::ToolMapping> mappings = {mk(0, 0), mk(1, 1), mk(2, 2), mk(3, 3)};
        REQUIRE(effective_remap(mappings).empty());
    }

    SECTION("extended tool (>3) remapped off head 0 is kept") {
        // default_head(5) == 0, so a remap to head 2 is a true remap.
        std::vector<helix::ToolMapping> mappings = {mk(5, 2)};
        std::map<int, int> expected = {{5, 2}};
        REQUIRE(effective_remap(mappings) == expected);
    }

    SECTION("extended tool (>3) mapped to head 0 is dropped as identity") {
        // default_head(5) == 0, so mapping 5 -> 0 is the firmware identity.
        std::vector<helix::ToolMapping> mappings = {mk(5, 0)};
        REQUIRE(effective_remap(mappings).empty());
    }
}

// End-to-end of the U1 routing emit: an AUTO color match (what
// PrintSelectDetailView::get_effective_remap sources from effective_mappings on
// the U1, where the inline card is hidden and the user has left auto-color on)
// → identity-filter → build_preprint_gcode.
// Proves the emitted SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS route each
// logical tool to the physical head holding its matched filament — not identity.
//
// Inputs are the EXACT live values captured from the reporter's U1
// (192.168.30.103, 2026-07-16) for 4_COLOR_RING_PLA_10m59s.gcode:
//   sliced filament_colour = #E2DEDB;#080A0D;#F4C032;#E72F1D  (T0..T3)
//   print_task_config: filament_exist=[T,F,T,T],
//     filament_color_rgba=[080A0D, E2DEDB, E72F1D, F4C032] (heads 0..3), all PLA.
// i.e. black/red/yellow loaded in heads 0/2/3; head 1 (white) is EMPTY (stale).
TEST_CASE("Snapmaker routing: real U1 4-color-ring auto match drives the extruder map",
          "[snapmaker][preprint][remap]") {
    // get_effective_remap()'s identity filter, called rather than restated: it is
    // FilamentMapper::identity_filtered_remap(), the one the detail view applies
    // to effective_mappings() before handing the map to build_preprint_gcode().
    auto effective_remap = &helix::FilamentMapper::identity_filtered_remap;

    // Sliced per-tool colors (T0=white, T1=black, T2=yellow, T3=red), all PLA.
    std::vector<helix::GcodeToolInfo> tools = {
        {0, 0xE2DEDB, "PLA"}, {1, 0x080A0D, "PLA"}, {2, 0xF4C032, "PLA"}, {3, 0xE72F1D, "PLA"}};

    // Physical heads as the firmware reports them: 0=black, 2=red, 3=yellow LOADED;
    // 1=white EMPTY (still reports its stale 0xE2DEDB color).
    std::vector<helix::AvailableSlot> slots = {
        {0, 0, 0x080A0D, "PLA", false, -1}, // head 0: black loaded
        {1, 0, 0xE2DEDB, "PLA", true, -1},  // head 1: EMPTY (stale white)
        {2, 0, 0xE72F1D, "PLA", false, -1}, // head 2: red loaded
        {3, 0, 0xF4C032, "PLA", false, -1}, // head 3: yellow loaded
    };

    // Auto match: what effective_mappings() runs when the auto-color toggle is
    // on. It is passed explicitly here — AmsState::effective_auto_match() decides
    // it in production, and this case is about the emit, not that decision.
    auto mappings = helix::FilamentMapper::effective_mappings(tools, slots, /*auto=*/true);
    auto remap = effective_remap(mappings);

    // T1(black)->head0, T2(yellow)->head3, T3(red)->head2 are true remaps.
    // T0(white) has no loaded white lane; it substitutes to head0 (its identity
    // head, black), so it is NOT a remap entry. The empty white head 1 must never
    // be matched.
    std::map<int, int> expected_remap = {{1, 0}, {2, 3}, {3, 2}};
    REQUIRE(remap == expected_remap);

    SnapmakerProbe sm;
    std::string gcode = sm.build_preprint_gcode({0, 1, 2, 3}, remap);
    REQUIRE(gcode == "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
                     "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
                     "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\n"
                     "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=3 MAP_EXTRUDER=2\n"
                     "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2,3");
}

// The staleness case in isolation: print N remaps T0 away from its default head,
// print N+1 needs T0 back on its default. Emitting only genuine remaps sent
// NOTHING for T0 the second time, so extruder_map_table[0] kept print N's value
// and print N+1 fed from the wrong head — and SET_PRINT_USED_EXTRUDERS, which
// assumes the default applied, named the wrong head too. This is also what makes
// the table readable back as the render's routing authority.
TEST_CASE("Snapmaker build_preprint_gcode overwrites a previous print's remap",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    // Print N: T0 remapped to head 2.
    REQUIRE(sm.build_preprint_gcode({0}, {{0, 2}}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=2");

    // Print N+1: same tool, no remap. It must still SAY head 0 rather than
    // relying on the firmware still holding the default.
    const std::string next = sm.build_preprint_gcode({0}, {});
    REQUIRE(next == "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=0\n"
                    "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0");
    REQUIRE(next.find("MAP_EXTRUDER=2") == std::string::npos);
}
