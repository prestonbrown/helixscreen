// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_tool_map_symmetry.cpp
 * @brief Both directions of a backend's tool<->slot mapping must agree.
 *
 * AmsSystemInfo states the mapping twice:
 *   - SlotInfo::mapped_tool          (slot -> tool) — the AMS panel's lane badge
 *   - AmsSystemInfo::tool_to_slot_map (tool -> slot) — helix::ui::resolve_op_button_slot,
 *     which picks the lane the filament panel's Load/Unload/Purge buttons gate on
 *
 * QIDI Box published only the reverse direction: save_variables states the
 * mapping one way (`value_t<N> = "slot<M>"` — tool N prints from slot M) and the
 * read-path stamped that onto mapped_tool alone. With tool_to_slot_map left
 * empty the resolver fell through to `tool_count > 1 ? selected_tool
 * : current_slot`, so on a Box with T0 remapped to slot 2 the panel drew the T0
 * badge on lane 2 while the buttons gated and operated on lane 0.
 *
 * These tests pin BOTH directions after every mapping write, so a future
 * one-sided write fails here rather than in the field.
 *
 * ACE is the control case: it has no per-slot tool mapping at all (single
 * extruder fed by any lane — set_tool_mapping is NOT_SUPPORTED, capabilities
 * report unsupported), so the right answer there is "no map", and the resolver's
 * current_slot fallback is what makes that correct.
 */

#include "ams_backend_ace.h"
#include "ams_backend_qidi.h"
#include "ams_remap.h"
#include "ams_types.h"
#include "filament_op_slot_resolver.h"
#include "test_helpers/qidi_box_test_access.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::ui::resolve_op_button_slot;

namespace {

// Slot -> tool as the AMS panel reads it (SlotInfo::mapped_tool), looked up by
// global index so a multi-box QIDI system is addressed the same way as a
// single-box one.
int mapped_tool_of(const AmsSystemInfo& info, int global_index) {
    const auto* slot = info.get_slot_global(global_index);
    return slot ? slot->mapped_tool : -99; // -99: slot absent, distinct from "unmapped"
}

// Tool -> slot as resolve_op_button_slot reads it.
int slot_of_tool(const AmsSystemInfo& info, int tool) {
    if (tool < 0 || tool >= static_cast<int>(info.tool_to_slot_map.size())) {
        return -99; // no entry at all — the shape that caused the bug
    }
    return info.tool_to_slot_map[static_cast<size_t>(tool)];
}

} // namespace

// =====================================================================
// QIDI Box — value_t<N> remap must publish both directions
// =====================================================================

TEST_CASE("QIDI Box default mapping is identity in both directions",
          "[ams][qidi][qidi_box][tool_map]") {
    AmsBackendQidi backend(nullptr, nullptr);
    auto info = backend.get_system_info();

    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
        CHECK(resolve_op_button_slot(info, /*selected_tool=*/i, /*tool_count=*/4) == i);
    }
}

TEST_CASE("QIDI Box value_t remap publishes tool_to_slot_map, not just mapped_tool",
          "[ams][qidi][qidi_box][tool_map]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // The reported field case: T0 remapped onto lane 2.
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});
    auto info = backend.get_system_info();

    // Reverse direction (AMS panel badge) — this half always worked.
    CHECK(mapped_tool_of(info, 2) == 0);
    // Slot 0 no longer serves any tool: T0 moved away and nothing replaced it.
    CHECK(mapped_tool_of(info, 0) == -1);

    // Forward direction (filament panel op-button lane) — the half that was missing.
    REQUIRE(info.tool_to_slot_map.size() > 0);
    CHECK(slot_of_tool(info, 0) == 2);

    // The user-visible consequence: the buttons must act on lane 2, not lane 0.
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
}

TEST_CASE("QIDI Box exposes the mapping through get_tool_mapping()",
          "[ams][qidi][qidi_box][tool_map]") {
    // AmsState::build_ams_topology, the print-start remap snapshot/restore, and
    // the AMS context menu all read the backend through get_tool_mapping().
    // The base default returns {}, which made AmsState synthesise a 1:1
    // topology that contradicts the Box's own remap.
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});

    REQUIRE(backend.owns_tool_mapping_table());
    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping.size() > 0);
    CHECK(mapping[0] == 2);
    CHECK(mapping == backend.get_system_info().tool_to_slot_map);
}

TEST_CASE("QIDI Box swapping two tools keeps both directions in lockstep",
          "[ams][qidi][qidi_box][tool_map]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot2"},
                                               {"value_t2", "slot0"},
                                           });
    auto info = backend.get_system_info();

    CHECK(mapped_tool_of(info, 2) == 0);
    CHECK(mapped_tool_of(info, 0) == 2);
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(slot_of_tool(info, 2) == 0);
    // Untouched tools stay identity.
    CHECK(mapped_tool_of(info, 1) == 1);
    CHECK(slot_of_tool(info, 1) == 1);
    CHECK(mapped_tool_of(info, 3) == 3);
    CHECK(slot_of_tool(info, 3) == 3);

    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/2, /*tool_count=*/4) == 0);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/1, /*tool_count=*/4) == 1);
}

TEST_CASE("QIDI Box tool map follows a box_count resize", "[ams][qidi][qidi_box][tool_map]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Two chained boxes = eight global slots; the forward map has to grow with
    // them or every tool above T3 resolves through the fallback.
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});
    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.tool_to_slot_map.size() == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }

    // Remap a tool onto a lane in the second box.
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot5"}});
    info = backend.get_system_info();
    CHECK(mapped_tool_of(info, 5) == 0);
    CHECK(mapped_tool_of(info, 0) == -1);
    CHECK(slot_of_tool(info, 0) == 5);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/8) == 5);
}

TEST_CASE("QIDI Box mapping survives a payload carrying only slot state",
          "[ams][qidi][qidi_box][tool_map]") {
    // Moonraker pushes deltas: a frame that repeats slot<N> without any
    // value_t<N> must not wipe the mapping established by an earlier frame.
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});
    QidiBoxTestAccess::parse_vars(backend, json{{"slot2", 2}, {"last_load_slot", "slot2"}});

    auto info = backend.get_system_info();
    CHECK(mapped_tool_of(info, 2) == 0);
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
    // The seated-slot aggregate is derived from the same mapped_tool, so it
    // must name the remapped tool rather than the lane index.
    CHECK(info.current_slot == 2);
    CHECK(info.current_tool == 0);
}

// =====================================================================
// ACE — no per-slot tool mapping exists, and that is the correct answer
// =====================================================================

namespace {

// Distinct from AmsBackendAceTestHelper in test_ams_backend_ace.cpp — same
// idiom (subclass to reach the protected REST parse hooks), different name so
// the two translation units don't collide.
class AceToolMapProbe : public AmsBackendAce {
  public:
    AceToolMapProbe() : AmsBackendAce(nullptr, nullptr) {}
    void feed_info(const json& data) {
        parse_info_response(data);
    }
    void feed_slots(const json& data) {
        parse_slots_response(data);
    }
    void feed_status(const json& data) {
        parse_status_response(data);
    }
};

} // namespace

TEST_CASE("ACE publishes no tool mapping in either direction", "[ams][ace][tool_map]") {
    // ACE Pro is a hub: four lanes converge on one extruder. There is no tool
    // number per lane — set_tool_mapping is NOT_SUPPORTED and the capability
    // flags say so — so an invented map would be worse than none, and both
    // directions must stay empty/unmapped.
    AceToolMapProbe ace;
    ace.feed_info(json{{"slot_count", 4}});
    ace.feed_slots(json{{"slots",
                         {{{"index", 0}, {"color", "#FF0000"}, {"material", "PLA"}},
                          {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}},
                          {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}},
                          {{"index", 3}, {"color", "#FFFFFF"}, {"material", ""}}}}});

    CHECK_FALSE(ace.owns_tool_mapping_table());
    CHECK_FALSE(helix::printer::can_remap(ace));
    CHECK(ace.get_tool_mapping().empty());
    CHECK(ace.set_tool_mapping(0, 2).result == AmsResult::NOT_SUPPORTED);

    auto info = ace.get_system_info();
    REQUIRE(info.total_slots == 4);
    CHECK(info.tool_to_slot_map.empty());
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == -1);
    }
}

TEST_CASE("ACE op-button lane comes from the loaded slot, not a tool index",
          "[ams][ace][tool_map]") {
    // With no map published, resolve_op_button_slot's single-tool branch is
    // what makes ACE correct: the lane the buttons act on is the seated one.
    AceToolMapProbe ace;
    ace.feed_info(json{{"slot_count", 4}});
    ace.feed_status(json{{"loaded_slot", 2}, {"action", "idle"}});

    auto info = ace.get_system_info();
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.tool_to_slot_map.empty());
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/1) == 2);
}
