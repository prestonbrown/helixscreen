// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_tool_map_symmetry_backends.cpp
 * @brief Every backend must state its tool map the same both ways.
 *
 * QIDI Box, CFS, ToolChanger and Mock each publish the mapping twice:
 *
 *   - SlotInfo::mapped_tool           (slot -> tool) — the AMS panel's lane badge
 *   - AmsSystemInfo::tool_to_slot_map (tool -> slot) — helix::ui::resolve_op_button_slot,
 *     which picks the lane the filament panel's Load/Unload/Purge buttons act on
 *
 * QIDI shipped the reverse direction alone. CFS shipped the mirror image: it
 * parsed `box.map` into tool_to_slot_map and then stamped `mapped_tool =
 * global_index` unconditionally on every slot, so after a BOX_MODIFY_TN remap
 * the op buttons were right and the badge was wrong. ToolChanger's
 * set_tool_mapping() and reset_tool_mappings() wrote the forward map only. The
 * mock wrote mapped_tool straight onto its registry entries in three places and
 * left the registry's forward map empty, so any test built on those modes was
 * asserting against a state no real backend can produce.
 *
 * ToolChanger carries an extra rule these tests also pin: a tool NUMBER and a
 * slot INDEX are deliberately not interchangeable there. change_tool() takes a
 * slot and emits `SELECT_TOOL TOOL="tool <name>"`, naming the physical tool, to
 * bypass ASSIGN_TOOL remapping — a tap on lane 2 mounts lane 2's toolhead — while
 * `toolchanger.tool_number` reports the ASSIGNED number of whatever is on the
 * carriage. Anything crossing between the two must go through the map.
 *
 * ACE is the control case: it has no per-slot tool mapping at all (one extruder
 * fed by any lane, set_tool_mapping NOT_SUPPORTED), so the right answer there is
 * "no map", and the resolver's current_slot fallback is what makes that correct.
 */

#include "ui_update_queue.h"

#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/qidi_box_test_access.h"
#include "ams_backend_ace.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_backend_toolchanger.h"
#include "ams_remap.h"
#include "ams_types.h"
#include "config.h"
#include "filament_op_slot_resolver.h"
#include "toolchanger_addon.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::printer::AmsBackendCfs;
using helix::ui::resolve_op_button_slot;

namespace {

// Slot -> tool as the AMS panel reads it, by global index.
int mapped_tool_of(const helix::AmsSystemInfo& info, int global_index) {
    const auto* slot = info.get_slot_global(global_index);
    return slot ? slot->mapped_tool : -99; // -99: slot absent, distinct from "unmapped"
}

// Tool -> slot as resolve_op_button_slot reads it.
int slot_of_tool(const helix::AmsSystemInfo& info, int tool) {
    if (tool < 0 || tool >= static_cast<int>(info.tool_to_slot_map.size())) {
        return -99; // no entry at all — the shape that caused the bug
    }
    return info.tool_to_slot_map[static_cast<size_t>(tool)];
}

/// Assert the two directions are inverses of each other across the whole system.
///
/// The generic half of every case below: whatever the mapping is, a lane that
/// claims tool T must be the lane tool T resolves to, and vice versa. A
/// one-sided write fails here even if the case's own expectations are updated.
void require_symmetric(const helix::AmsSystemInfo& info) {
    for (int i = 0; i < info.total_slots; ++i) {
        int tool = mapped_tool_of(info, i);
        if (tool >= 0) {
            INFO("slot " << i << " claims T" << tool);
            CHECK(slot_of_tool(info, tool) == i);
        }
    }
    for (int t = 0; t < static_cast<int>(info.tool_to_slot_map.size()); ++t) {
        int slot = info.tool_to_slot_map[static_cast<size_t>(t)];
        if (slot >= 0) {
            INFO("T" << t << " resolves to slot " << slot);
            CHECK(mapped_tool_of(info, slot) == t);
        }
    }
}

// --- CFS ------------------------------------------------------------------

/// Minimal single-unit stock box payload with a caller-supplied `map`.
///
/// Occupancy fields are held constant (all four bays present) so the mapping is
/// the only thing under test; see make_single_unit_box in test_ams_backend_cfs.cpp
/// for the vender/remain_len presence rules being satisfied here.
json cfs_box_with_map(const json& map_obj) {
    json box = json::parse(R"({
        "state": "connect",
        "filament": 0,
        "auto_refill": 1,
        "enable": 1,
        "filament_useup": 0,
        "T1": {
            "state": "connect",
            "filament": "None",
            "temperature": "27",
            "dry_and_humidity": "48",
            "version": "1.1.3",
            "sn": "SERIAL",
            "material_type": ["101001", "101001", "101001", "101001"],
            "color_value": ["0FF0000", "000FF00", "00000FF", "0FFFFFF"],
            "change_color_num": ["-1", "-1", "-1", "-1"],
            "vender": ["unknown", "unknown", "unknown", "unknown"],
            "remain_len": ["52", "52", "52", "52"]
        }
    })");
    if (!map_obj.is_null()) {
        box["map"] = map_obj;
    }
    return box;
}

/// Drives the real CFS backend's protected status handler and captures gcode.
///
/// Deliberately NOT named CfsTestAccess / CfsRemapHelper — those live at file
/// scope in test_ams_backend_cfs.cpp, and a second definition of either name in
/// this translation unit would be an ODR violation.
class CfsToolMapProbe : public AmsBackendCfs {
  public:
    CfsToolMapProbe() : AmsBackendCfs(nullptr, nullptr) {}
    ~CfsToolMapProbe() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed_box(const json& box) {
        handle_status_update(json{{"method", "notify_status_update"},
                                  {"params", json::array({json{{"box", box}}, 0})}});
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }

    std::vector<std::string> captured;
};

// --- ToolChanger ----------------------------------------------------------

/// Drives the real toolchanger backend's protected status handler.
class ToolChangerMapProbe : public helix::AmsBackendToolChanger {
  public:
    explicit ToolChangerMapProbe(int tool_count) : helix::AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(tool_count));
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        // check_preconditions() refuses everything while the backend is stopped.
        running_ = true;
    }
    ~ToolChangerMapProbe() override {
        // change_tool()'s dispatch is resolved by a gcode ack that never comes
        // here; draining keeps scripts/check_update_queue_leaks.py quiet.
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    // client_ is null, so ensure_homed_then() routes straight to execute_gcode();
    // both overloads are captured because dispatch paths use the 2-arg form.
    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }
    // A dispatch that unwinds on a rejection sends through this form.
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        return execute_gcode(gcode, std::move(on_complete));
    }

    std::vector<std::string> captured;
};

} // namespace

// =====================================================================
// CFS — box.map is the one source; mapped_tool is derived from it
// =====================================================================

TEST_CASE("CFS with an identity box.map is identity in both directions", "[ams][cfs][tool_map]") {
    auto info = AmsBackendCfs::parse_box_status(
        cfs_box_with_map(json{{"T1A", "T1A"}, {"T1B", "T1B"}, {"T1C", "T1C"}, {"T1D", "T1D"}}));

    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() >= 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
        CHECK(resolve_op_button_slot(info, /*selected_tool=*/i, /*tool_count=*/4) == i);
    }
    require_symmetric(info);
}

TEST_CASE("CFS box with no map at all still publishes both directions", "[ams][cfs][tool_map]") {
    // The `map` key is optional on the wire. Before the fix the identity default
    // was written onto mapped_tool only, so a box that had never been remapped
    // shipped a lane badge with an EMPTY forward map beside it — and
    // resolve_op_button_slot fell through to its tool_count>1 guess.
    auto info = AmsBackendCfs::parse_box_status(cfs_box_with_map(json(nullptr)));

    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    require_symmetric(info);
}

TEST_CASE("CFS box.map remap moves the lane badge, not just the op-button lane",
          "[ams][cfs][tool_map]") {
    // The reported field case, inverted from QIDI's: T0 (T1A) now sources
    // physical bay T1C (slot 2). The forward half always worked; the badge kept
    // reading identity because mapped_tool was stamped from global_index.
    auto info = AmsBackendCfs::parse_box_status(cfs_box_with_map(json{{"T1A", "T1C"}}));

    // Forward direction (filament panel op-button lane) — the half that worked.
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);

    // Reverse direction (AMS panel badge) — the half that was missing.
    CHECK(mapped_tool_of(info, 2) == 0);
    // Lane 0 no longer serves any tool: T0 moved away and nothing replaced it.
    CHECK(mapped_tool_of(info, 0) == -1);
    // T2 does NOT get lane 2 back by identity fallback — lane 2 is spoken for.
    CHECK(slot_of_tool(info, 2) == -1);
    // Untouched lanes keep the 1:1 default in both directions.
    CHECK(mapped_tool_of(info, 1) == 1);
    CHECK(slot_of_tool(info, 1) == 1);
    CHECK(mapped_tool_of(info, 3) == 3);
    CHECK(slot_of_tool(info, 3) == 3);

    require_symmetric(info);
}

TEST_CASE("CFS box.map swap keeps both directions in lockstep", "[ams][cfs][tool_map]") {
    auto info =
        AmsBackendCfs::parse_box_status(cfs_box_with_map(json{{"T1A", "T1C"}, {"T1C", "T1A"}}));

    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(slot_of_tool(info, 2) == 0);
    CHECK(mapped_tool_of(info, 2) == 0);
    CHECK(mapped_tool_of(info, 0) == 2);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/2, /*tool_count=*/4) == 0);

    require_symmetric(info);
}

TEST_CASE("CFS current_tool names the tool routing through the seated lane",
          "[ams][cfs][tool_map]") {
    // T1.filament = "C" seats bay 2. With T0 remapped onto that bay, the tool
    // printing from it is T0 — the print-status color dot labels current_tool.
    json box = cfs_box_with_map(json{{"T1A", "T1C"}});
    box["T1"]["filament"] = "C";
    auto info = AmsBackendCfs::parse_box_status(box);

    CHECK(info.current_slot == 2);
    CHECK(info.current_tool == 0);
    CHECK(mapped_tool_of(info, info.current_slot) == info.current_tool);
}

TEST_CASE("CFS seated lane that no tool maps to falls back to the lane index",
          "[ams][cfs][tool_map]") {
    // Reachable from a PARTIAL box.map: with only `T1A -> T1C` stated, lane 2
    // belongs to T0 and identity_fallback refuses to hand it back to T2 — which
    // leaves lane 0 (T0's old home) claimed by nobody. Seat lane 0 and there is
    // genuinely no G-code tool routing through what is loaded.
    //
    // current_tool reports the lane index there rather than -1. -1 is a valid
    // value of this field, but only alongside "nothing loaded": every consumer
    // reads it as "no tool". Paired with a seated lane it misleads twice over —
    // ams_current_tool.xml hides the indicator on `< 0`, taking the still-valid
    // filament colour swatch with it, and ToolState::set_ams_topology() clamps
    // a negative active_tool to 0, lighting T0 in the tool switcher and the
    // nozzle label while a different lane is seated.
    json box = cfs_box_with_map(json{{"T1A", "T1C"}});
    box["T1"]["filament"] = "A";
    auto info = AmsBackendCfs::parse_box_status(box);

    REQUIRE(info.current_slot == 0);
    CHECK(mapped_tool_of(info, 0) == -1); // no tool routes through the seated lane
    CHECK(info.current_tool == 0);        // ...so fall back, do not publish -1
    require_symmetric(info);
}

TEST_CASE("CFS flat schema publishes both directions", "[ams][cfs][tool_map]") {
    // Community Kalico box.py reimplementation: a `slots` array and no `map`.
    // Same identity default, and it has to reach both fields.
    json box = json::parse(R"({
        "fluidd_widget_version": 3,
        "loaded_slot": 1,
        "slots": [
            {"index": 0, "present": true, "loaded": false, "material": "PLA", "color": "#FF0000"},
            {"index": 1, "present": true, "loaded": true,  "material": "PETG", "color": "#00FF00"},
            {"index": 2, "present": true, "loaded": false, "material": "ABS", "color": "#0000FF"},
            {"index": 3, "present": false, "loaded": false, "material": "", "color": ""}
        ]
    })");
    auto info = AmsBackendCfs::parse_box_status(box);

    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    require_symmetric(info);
}

TEST_CASE("CFS set_tool_mapping optimistic update moves both directions", "[ams][cfs][tool_map]") {
    // BOX_MODIFY_TN is fire-and-forget: the UI reads the local copy until the
    // next box frame confirms it — and on K1 firmware, where the command is
    // known to no-op, no confirming frame ever arrives. So the optimistic write
    // is the whole answer on that path, and a one-sided one leaves the badge on
    // the lane the tool was moved AWAY from, permanently.
    CfsToolMapProbe backend;
    backend.feed_box(
        cfs_box_with_map(json{{"T1A", "T1A"}, {"T1B", "T1B"}, {"T1C", "T1C"}, {"T1D", "T1D"}}));
    REQUIRE(backend.get_system_info().total_slots == 4);

    REQUIRE(backend.set_tool_mapping(/*tool_number=*/0, /*slot_index=*/2).success());

    auto info = backend.get_system_info();
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(mapped_tool_of(info, 2) == 0);
    // Both losing sides gave up their claim: lane 0 serves nothing, and T2 no
    // longer resolves to the lane T0 just took.
    CHECK(mapped_tool_of(info, 0) == -1);
    CHECK(slot_of_tool(info, 2) == -1);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
    require_symmetric(info);

    // get_tool_mapping() (print-start remap snapshot, AMS context menu) sees the
    // same forward map get_system_info() publishes.
    CHECK(backend.get_tool_mapping() == info.tool_to_slot_map);

    // And the gcode still went out in TNN notation.
    REQUIRE_FALSE(backend.captured.empty());
    CHECK(backend.captured.back() == "BOX_MODIFY_TN T1A=T1C");
}

TEST_CASE("CFS firmware frame overrides an optimistic remap in both directions",
          "[ams][cfs][tool_map]") {
    // Firmware stays the source of truth: if Klipper rejected the remap, the
    // next box frame must put BOTH halves back, not just the forward one.
    CfsToolMapProbe backend;
    backend.feed_box(
        cfs_box_with_map(json{{"T1A", "T1A"}, {"T1B", "T1B"}, {"T1C", "T1C"}, {"T1D", "T1D"}}));
    REQUIRE(backend.set_tool_mapping(0, 2).success());
    REQUIRE(mapped_tool_of(backend.get_system_info(), 2) == 0);

    backend.feed_box(
        cfs_box_with_map(json{{"T1A", "T1A"}, {"T1B", "T1B"}, {"T1C", "T1C"}, {"T1D", "T1D"}}));

    auto info = backend.get_system_info();
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    require_symmetric(info);
}

// =====================================================================
// ToolChanger — ASSIGN_TOOL remap, and tool number != slot index
// =====================================================================

TEST_CASE("ToolChanger default mapping is identity in both directions",
          "[ams][toolchanger][tool_map]") {
    ToolChangerMapProbe backend(4);
    auto info = backend.get_system_info();

    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
        CHECK(resolve_op_button_slot(info, /*selected_tool=*/i, /*tool_count=*/4) == i);
    }
    require_symmetric(info);
}

TEST_CASE("ToolChanger set_tool_mapping moves the lane badge with the map",
          "[ams][toolchanger][tool_map]") {
    ToolChangerMapProbe backend(4);
    REQUIRE(backend.set_tool_mapping(/*tool_number=*/0, /*slot_index=*/2).success());

    auto info = backend.get_system_info();
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(mapped_tool_of(info, 2) == 0); // the half that was never written
    CHECK(mapped_tool_of(info, 0) == -1);
    CHECK(slot_of_tool(info, 2) == -1);
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
    require_symmetric(info);

    REQUIRE_FALSE(backend.captured.empty());
    CHECK(backend.captured.back() == "ASSIGN_TOOL TOOL=\"tool T2\" N=0");
}

TEST_CASE("ToolChanger apply_user_edit remap moves the lane badge with the map",
          "[ams][toolchanger][tool_map]") {
    // The AMS slot-edit modal's path into the same remap. It wrote both fields
    // by hand and evicted neither, so a swap ended with two lanes claiming one
    // tool number.
    ToolChangerMapProbe backend(4);

    helix::SlotInfo edit = backend.get_slot_info(1);
    edit.mapped_tool = 3;
    REQUIRE(helix::test::apply_edit(backend, 1, edit).success());

    auto info = backend.get_system_info();
    CHECK(mapped_tool_of(info, 1) == 3);
    CHECK(slot_of_tool(info, 3) == 1);
    // Lane 3 lost T3, and T1 lost lane 1.
    CHECK(mapped_tool_of(info, 3) == -1);
    CHECK(slot_of_tool(info, 1) == -1);
    require_symmetric(info);

    REQUIRE_FALSE(backend.captured.empty());
    CHECK(backend.captured.back() == "ASSIGN_TOOL TOOL=\"tool T1\" N=3");
}

TEST_CASE("ToolChanger apply_user_edit ignores the default mapped_tool (-1)",
          "[ams][backend][slot-edit-remap]") {
    // A Spoolman sync builds a default SlotInfo: no mapping is stated, so the
    // live map stays and nothing is sent.
    ToolChangerMapProbe backend(4);
    REQUIRE(backend.get_slot_info(1).mapped_tool == 1);

    helix::SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";
    REQUIRE(helix::test::apply_edit(backend, 1, info).success());

    for (const auto& g : backend.captured) {
        CHECK(g.rfind("ASSIGN_TOOL ", 0) != 0);
    }
    CHECK(backend.get_slot_info(1).mapped_tool == 1);
    require_symmetric(backend.get_system_info());
}

TEST_CASE("ToolChanger reset_tool_mappings restores identity in both directions",
          "[ams][toolchanger][tool_map]") {
    ToolChangerMapProbe backend(4);
    REQUIRE(backend.set_tool_mapping(0, 2).success());
    REQUIRE(mapped_tool_of(backend.get_system_info(), 2) == 0);

    REQUIRE(backend.reset_tool_mappings().success());

    auto info = backend.get_system_info();
    for (int i = 0; i < 4; ++i) {
        INFO("lane " << i);
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    require_symmetric(info);
}

TEST_CASE("ToolChanger without klipper-toolchanger has no remap route",
          "[ams][toolchanger][tool_map][medusahc]") {
    // A changer driven by its own T<n> macros (MedusaHC without
    // klipper-toolchanger) has no ASSIGN_TOOL and no tool-number indirection:
    // its T<n> macros name the physical tool outright. Klipper answers an
    // unknown command by logging it and carrying on, so the refusal has to
    // happen here - sending it reads as a successful remap, and the print runs
    // on the firmware's own mapping with nothing to show for the user's pick.
    ToolChangerMapProbe backend(6);
    helix::toolchanger_addon::ToolCommands own_commands;
    own_commands.present = true;
    own_commands.provider_name = "MedusaHC";
    own_commands.select_prefix = "T";
    backend.set_tool_commands(own_commands);

    // The file's own tool numbers are the only thing left to change, so the
    // route is a rewrite of the job. It carries the user's pick, so can_remap()
    // is true - but it writes no table, so the AMS edit overlay's inline tool
    // dropdown stays hidden and print-start takes no generic send.
    CHECK(backend.get_remap_strategy() == helix::AmsBackend::RemapStrategy::GcodeRewrite);
    CHECK(helix::printer::can_remap(backend));
    CHECK_FALSE(helix::printer::can_write_mapping_table(backend));

    backend.captured.clear();
    CHECK_FALSE(backend.set_tool_mapping(/*tool_number=*/2, /*slot_index=*/4).success());
    CHECK(backend.captured.empty());

    // The restore path is the same command and must refuse for the same reason.
    backend.captured.clear();
    backend.reset_tool_mappings();
    CHECK(backend.captured.empty());
}

TEST_CASE("ToolChanger with klipper-toolchanger keeps its remap route",
          "[ams][toolchanger][tool_map]") {
    // resolve_tool_commands() returns an ABSENT capability when [toolchanger]
    // is present, because SELECT_TOOL and ASSIGN_TOOL are then the machine's
    // own answer. The default-constructed probe is that machine.
    ToolChangerMapProbe backend(6);

    CHECK(backend.get_remap_strategy() == helix::AmsBackend::RemapStrategy::Native);
    CHECK(helix::printer::can_write_mapping_table(backend));

    backend.captured.clear();
    REQUIRE(backend.set_tool_mapping(/*tool_number=*/2, /*slot_index=*/4).success());
    CHECK(backend.captured.back() == "ASSIGN_TOOL TOOL=\"tool T4\" N=2");
}

TEST_CASE("ToolChanger change_tool takes a SLOT and bypasses the remap",
          "[ams][toolchanger][tool_map]") {
    // The distinction this backend depends on. After T0 is assigned to physical
    // tool 2, tapping lane 2 must still mount lane 2 — SELECT_TOOL resolves
    // through the toolchanger's own list, deliberately ignoring ASSIGN_TOOL. If
    // a future change ever "helpfully" routes change_tool through the map, this
    // emits T=0 and the wrong toolhead comes off the dock.
    ToolChangerMapProbe backend(4);
    backend.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", -1}}}});
    REQUIRE(backend.set_tool_mapping(0, 2).success());
    backend.captured.clear();

    REQUIRE(backend.change_tool(2).success());
    REQUIRE_FALSE(backend.captured.empty());
    CHECK(backend.captured.back() == "SELECT_TOOL TOOL=\"tool T2\"");
}

TEST_CASE("ToolChanger resolves the carriage lane through the map",
          "[ams][toolchanger][tool_map]") {
    // toolchanger.tool_number is the ASSIGNED number, not the slot index.
    // Treating it as an index stamped LOADED on the lane that merely shares an
    // index with the number, and offered Unload there.
    ToolChangerMapProbe backend(4);
    REQUIRE(backend.set_tool_mapping(0, 2).success());
    backend.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    auto info = backend.get_system_info();
    CHECK(info.current_tool == 0); // the G-code number klipper reported
    CHECK(info.current_slot == 2); // the physical toolhead it names
    CHECK(backend.get_current_slot() == 2);

    const auto* seated = info.get_slot_global(2);
    REQUIRE(seated != nullptr);
    CHECK(seated->status == helix::SlotStatus::LOADED);
    const auto* lane0 = info.get_slot_global(0);
    REQUIRE(lane0 != nullptr);
    CHECK(lane0->status == helix::SlotStatus::AVAILABLE);

    CHECK(backend.can_unload_from_toolhead(2));
    CHECK_FALSE(backend.can_unload_from_toolhead(0));
}

TEST_CASE("ToolChanger identity mapping still reads tool_number as the lane",
          "[ams][toolchanger][tool_map]") {
    // Guard on the fallback: with no remap in effect the pre-existing behaviour
    // (and test_ams_toolchanger_per_slot_loaded.cpp) must be unchanged.
    ToolChangerMapProbe backend(4);
    backend.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});

    CHECK(backend.get_current_slot() == 2);
    CHECK(backend.is_filament_loaded());
    CHECK(backend.slot_is_actively_loaded(2));
    CHECK_FALSE(backend.slot_is_actively_loaded(0));
}

// =====================================================================
// Mock — the test double must not model an impossible state
// =====================================================================

TEST_CASE("Mock IFS mode publishes both directions", "[ams][mock]") {
    // set_ifs_mode wrote mapped_tool onto the registry entries and the forward
    // map onto system_info_ — which get_system_info() does not read. The mode
    // therefore shipped a reverse map with no forward map at all.
    helix::AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_ifs_mode(true);

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 4);
    // 16-wide with -1 padding, matching what the real AmsBackendAd5xIfs publishes.
    REQUIRE(info.tool_to_slot_map.size() == 16);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    for (int t = 4; t < 16; ++t) {
        CHECK(slot_of_tool(info, t) == -1);
    }
    CHECK(backend.get_tool_mapping() == info.tool_to_slot_map);
    require_symmetric(info);
}

TEST_CASE("Mock Snapmaker mode publishes both directions", "[ams][mock]") {
    helix::AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_snapmaker_mode(true);

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.tool_to_slot_map.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }
    require_symmetric(info);
}

TEST_CASE("Mock HELIX_MOCK_REMAP override publishes both directions", "[ams][mock]") {
    // The knob exists to stage a remapped printer. It wrote mapped_tool only,
    // so it staged a system whose two halves disagreed — a shape no backend can
    // produce, and therefore a useless thing to test a UI against.
    helix::AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_snapmaker_mode(true);
    backend.apply_remap_overrides("0:2,2:0");

    auto info = backend.get_system_info();
    CHECK(slot_of_tool(info, 0) == 2);
    CHECK(slot_of_tool(info, 2) == 0);
    CHECK(mapped_tool_of(info, 2) == 0);
    CHECK(mapped_tool_of(info, 0) == 2);
    // Tools not named by the CSV are unmapped, and so are their lanes — the
    // documented "partial CSV is deterministic" contract.
    CHECK(slot_of_tool(info, 1) == -1);
    CHECK(mapped_tool_of(info, 1) == -1);
    require_symmetric(info);
}

TEST_CASE("Mock remap override clears a previous override", "[ams][mock]") {
    helix::AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_snapmaker_mode(true);
    backend.apply_remap_overrides("0:3");
    REQUIRE(mapped_tool_of(backend.get_system_info(), 3) == 0);

    backend.apply_remap_overrides("1:2");

    auto info = backend.get_system_info();
    CHECK(mapped_tool_of(info, 2) == 1);
    CHECK(slot_of_tool(info, 1) == 2);
    CHECK(mapped_tool_of(info, 3) == -1); // the previous override is gone
    CHECK(slot_of_tool(info, 0) == -1);
    require_symmetric(info);
}

TEST_CASE("Mock apply_user_edit does not change slot status", "[ams][mock]") {
    // Same contract as every real backend: status is firmware-derived, so this
    // path carries filament metadata only. It used to drop `status` in silence,
    // which cost a debugging cycle — a test helper wrote status through here,
    // nothing took effect, and the resulting failures read as implementation
    // bugs. force_slot_status() is the path that works.
    helix::AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.force_slot_status(1, helix::SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(1).status == helix::SlotStatus::EMPTY);

    helix::SlotInfo edit = backend.get_slot_info(1);
    edit.status = helix::SlotStatus::LOADED; // ignored
    edit.material = "PETG";                  // applied
    REQUIRE(helix::test::apply_edit(backend, 1, edit).success());

    auto after = backend.get_slot_info(1);
    CHECK(after.status == helix::SlotStatus::EMPTY);
    CHECK(after.material == "PETG");

    // And the documented path does take effect.
    backend.force_slot_status(1, helix::SlotStatus::AVAILABLE);
    CHECK(backend.get_slot_info(1).status == helix::SlotStatus::AVAILABLE);
}

// =====================================================================
// QIDI Box — value_t<N> remap must publish both directions
// =====================================================================

TEST_CASE("QIDI Box default mapping is identity in both directions",
          "[ams][qidi][qidi_box][tool_map]") {
    helix::AmsBackendQidi backend(nullptr, nullptr);
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
    helix::AmsBackendQidi backend(nullptr, nullptr);

    // The reported field case: T0 remapped onto lane 2.
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});
    auto info = backend.get_system_info();

    // Reverse direction (AMS panel badge).
    CHECK(mapped_tool_of(info, 2) == 0);
    // Slot 0 serves no tool: T0 moved away and nothing replaced it.
    CHECK(mapped_tool_of(info, 0) == -1);

    // Forward direction (filament panel op-button lane).
    REQUIRE(info.tool_to_slot_map.size() > 0);
    CHECK(slot_of_tool(info, 0) == 2);

    // The user-visible consequence: the buttons must act on lane 2, not lane 0.
    CHECK(resolve_op_button_slot(info, /*selected_tool=*/0, /*tool_count=*/4) == 2);
}

TEST_CASE("QIDI Box exposes the mapping through get_tool_mapping()",
          "[ams][qidi][qidi_box][tool_map]") {
    // AmsState::build_ams_topology, the print-start remap snapshot/restore, and
    // the AMS context menu all read the backend through get_tool_mapping().
    // The base default returns {}, which makes AmsState synthesise a 1:1
    // topology that contradicts the Box's own remap.
    helix::AmsBackendQidi backend(nullptr, nullptr);
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});

    REQUIRE(backend.owns_tool_mapping_table());
    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping.size() > 0);
    CHECK(mapping[0] == 2);
    CHECK(mapping == backend.get_system_info().tool_to_slot_map);
}

TEST_CASE("QIDI Box swapping two tools keeps both directions in lockstep",
          "[ams][qidi][qidi_box][tool_map]") {
    helix::AmsBackendQidi backend(nullptr, nullptr);

    helix::QidiBoxTestAccess::parse_vars(backend, json{
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
    helix::AmsBackendQidi backend(nullptr, nullptr);

    // Two chained boxes = eight global slots; the forward map has to grow with
    // them or every tool above T3 resolves through the fallback.
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});
    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.tool_to_slot_map.size() == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(mapped_tool_of(info, i) == i);
        CHECK(slot_of_tool(info, i) == i);
    }

    // Remap a tool onto a lane in the second box.
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot5"}});
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
    helix::AmsBackendQidi backend(nullptr, nullptr);
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot2"}});
    helix::QidiBoxTestAccess::parse_vars(backend, json{{"slot2", 2}, {"last_load_slot", "slot2"}});

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
class AceToolMapProbe : public helix::AmsBackendAce {
  public:
    AceToolMapProbe() : helix::AmsBackendAce(nullptr, nullptr) {}
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
    CHECK(ace.set_tool_mapping(0, 2).result == helix::AmsResult::NOT_SUPPORTED);

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
