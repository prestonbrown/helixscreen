// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_tool_text.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl_test_fixture.h"
#include "static_subject_registry.h"
#include "tool_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ToolState;
using helix::ToolTopology;
using helix::ui::UpdateQueue;

namespace {

// LVGLTestFixture brings up LVGL so ToolState::init_subjects() can register
// subjects (init uses lv_subject_init_*). HelixTestFixture alone does not.
struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(ToolStateFixture, "[ToolState][ams-topology] set_ams_topology populates 15 tools",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 15;
    topo.active_tool = 0;
    topo.tool_to_slot.resize(15);
    for (int i = 0; i < 15; ++i)
        topo.tool_to_slot[i] = i; // 1:1 default
    topo.tool_name_prefix = "T";

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 15);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    REQUIRE(ToolState::instance().tools()[14].name == "T14");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] active tool updates without rebuilding tools list",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    int initial_version = lv_subject_get_int(ToolState::instance().get_tools_version_subject());

    topo.active_tool = 2;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().active_tool_index() == 2);
    REQUIRE(lv_subject_get_int(ToolState::instance().get_tools_version_subject()) ==
            initial_version); // No rebuild
    // Pin list shape + content so a future "bump version but silently rebuild"
    // regression also fails this test.
    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().tools()[0].name == "T0");
}

TEST_CASE_METHOD(ToolStateFixture, "[ToolState][ams-topology] clear_ams_topology releases override",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 6;
    topo.active_tool = 3;
    topo.tool_to_slot = {0, 1, 2, 3, 4, 5};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().tool_count() == 6);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();

    // After clear, tools_ is empty (callers must invoke init_tools again to repopulate)
    REQUIRE(ToolState::instance().tool_count() == 0);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] AFC mock with 4 lanes drives ToolState",
                 "[tool-state][ams][afc][ams-topology]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(/*register_xml=*/false);

    auto mock = AmsBackend::create_mock(4);
    REQUIRE(mock != nullptr);
    auto caps = mock->get_tool_mapping_capabilities();
    if (!caps.supported)
        return; // Mock lacks tool multiplexing; skipped.

    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().ams_topology_active());

    // Defensive cleanup: drop the override + backend so later tests (which share
    // the AmsState / ToolState singletons) don't inherit a stale topology.
    ToolState::instance().clear_ams_topology();
    ams.clear_backends();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(
    ToolStateFixture,
    "[ToolState][ams-topology] update_from_status is ignored when AMS owns active tool",
    "[tool-state][ams][ams-topology]") {
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.tool_to_slot = {0, 1, 2, 3};
    helix::ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // Simulate Klipper telling us "toolchanger says T0, toolhead is on extruder0".
    // Under AMS topology, ToolState should ignore both and stay on T2.
    nlohmann::json status = {{"toolchanger", {{"tool_number", 0}}},
                             {"toolhead", {{"extruder", "extruder"}}}};
    helix::ToolState::instance().update_from_status(status);
    UpdateQueue::instance().drain();

    REQUIRE(helix::ToolState::instance().active_tool_index() == 2);

    // Cleanup so the override doesn't leak to later tests in this TU.
    helix::ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

// =============================================================================
// REGRESSION TESTS — final code review found two ToolChanger-printer regressions
// in the original #956 series. These tests fail without commit 1e9e4a69d's fixes
// (extruder_name preservation in set_ams_topology + narrowed update_from_status
// guard). Cite [L065] friend pattern: no public *_for_testing accessors;
// PrinterDiscovery's public parse_objects API is the injection point.
// =============================================================================

#include "printer_discovery.h"

namespace {
// Build a real PrinterDiscovery that looks like a 4-tool ToolChanger printer
// (4 named tools T0..T3, 4 extruder heaters). Goes through the same
// parse_objects path Klipper hits in production — no friends required.
helix::PrinterDiscovery make_toolchanger_discovery() {
    nlohmann::json objects = {"toolchanger", "tool T0",   "tool T1",   "tool T2",  "tool T3",
                              "extruder",    "extruder1", "extruder2", "extruder3"};
    helix::PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}
} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "set_ams_topology preserves ToolChanger per-tool extruder_name "
                 "across rebuild (regression for ToolChanger backend)",
                 "[tool-state][ams][ams-topology][regression]") {
    // Production sequence for a ToolChanger printer that also advertises
    // supports_tool_mapping=true (AmsBackendToolChanger does):
    //   1. init_tools(discovery) → tools_[i].extruder_name = "extruder", "extruder1", ...
    //   2. sync_from_backend() → build_ams_topology() → set_ams_topology() rebuilds tools_
    //
    // Pre-fix bug: rebuild constructed fresh ToolInfo whose default extruder_name
    // is "extruder" (struct default initializer) — wiping the per-tool mapping
    // and routing all heater/fan control to extruder0.
    auto& ts = helix::ToolState::instance();
    auto disc = make_toolchanger_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.tools()[0].extruder_name.has_value());
    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[2].extruder_name.value() == "extruder2");
    REQUIRE(ts.tools()[3].extruder_name.value() == "extruder3");

    // Apply a same-shape topology — this hits needs_rebuild=true because
    // ams_topology_active_ was false. The rebuild path must preserve names.
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[2].extruder_name.value() == "extruder2");
    REQUIRE(ts.tools()[3].extruder_name.value() == "extruder3");

    // Force another rebuild via shape change (tool_count differs → needs_rebuild).
    // The first 3 entries must still carry their per-tool extruder names.
    topo.tool_count = 3;
    topo.tool_to_slot = {0, 1, 2};
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 3);
    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[2].extruder_name.value() == "extruder2");

    // Grow back to 5 tools — first 3 preserve, extras keep ToolInfo default.
    topo.tool_count = 5;
    topo.tool_to_slot = {0, 1, 2, 3, 4};
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 5);
    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[2].extruder_name.value() == "extruder2");
    // Indices 3, 4: no prior entry → default "extruder" (acceptable for AFC;
    // ToolChanger printers wouldn't have a 5th tool here).
    REQUIRE(ts.tools()[3].extruder_name.has_value());
    REQUIRE(ts.tools()[4].extruder_name.has_value());

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "update_from_status processes per-tool status while topology is active "
                 "(regression for ToolChanger backend)",
                 "[tool-state][ams][ams-topology][regression]") {
    // Pre-fix bug: update_from_status had a top-of-function early-return when
    // ams_topology_active_=true. This blocked the toolchanger.tool_number and
    // toolhead.extruder parsing (correct) BUT also blocked the per-tool loop
    // that updates tool.active / .mounted / .detect_state / .gcode_offset from
    // "tool {name}" Klipper objects (wrong — ToolChanger users lost live tool
    // status). The narrowed guard wraps only the two blocks it's meant to
    // skip; the per-tool loop runs unconditionally.
    auto& ts = helix::ToolState::instance();
    auto disc = make_toolchanger_discovery();
    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 4);

    // Install topology to set ams_topology_active_=true.
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ts.ams_topology_active());

    // Feed status that includes per-tool keys ("tool T0", "tool T1", ...).
    // The toolchanger.tool_number block MUST be ignored (topology owns active),
    // but the per-tool loop MUST update .mounted / .gcode_x_offset.
    nlohmann::json status = {
        {"toolchanger", {{"tool_number", 99}}}, // Should be ignored — out-of-range proves it
        {"tool T0",
         {{"active", false},
          {"mounted", true},
          {"gcode_x_offset", 0.0},
          {"gcode_y_offset", 0.0},
          {"gcode_z_offset", 0.0}}},
        {"tool T1",
         {{"active", false},
          {"mounted", true},
          {"gcode_x_offset", 12.5},
          {"gcode_y_offset", -3.25},
          {"gcode_z_offset", 0.0}}},
        {"tool T2",
         {{"active", false},
          {"mounted", false},
          {"gcode_x_offset", 0.0},
          {"gcode_y_offset", 0.0},
          {"gcode_z_offset", 0.0}}}};
    ts.update_from_status(status);
    UpdateQueue::instance().drain();

    // Per-tool loop must have updated mounted + offsets.
    REQUIRE(ts.tools()[0].mounted == true);
    REQUIRE(ts.tools()[1].mounted == true);
    REQUIRE(ts.tools()[1].gcode_x_offset == Catch::Approx(12.5));
    REQUIRE(ts.tools()[1].gcode_y_offset == Catch::Approx(-3.25));
    REQUIRE(ts.tools()[2].mounted == false);

    // Active-tool guard must still hold: toolchanger.tool_number=99 was ignored.
    // active_tool_index_ should still be 0 (set by set_ams_topology), not 99.
    REQUIRE(ts.active_tool_index() == 0);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

// =============================================================================
// extruder_count() vs tool_count(): the nozzle tool badge is gated on physical
// hotends, so an AMS feeding one extruder must not make the nozzle icon claim
// to be "tool 0".
// =============================================================================

namespace {
/// Single-extruder printer — one hotend, no toolchanger.
helix::PrinterDiscovery make_single_extruder_discovery() {
    nlohmann::json objects = {"extruder", "heater_bed", "fan"};
    helix::PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}
} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "A 4-slot AMS on a single-extruder printer reports 4 tools but 1 extruder",
                 "[tool-state][ams][ams-topology][tool-badge]") {
    auto& ts = helix::ToolState::instance();
    auto disc = make_single_extruder_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 1);
    REQUIRE(ts.extruder_count() == 1);
    REQUIRE_FALSE(ts.has_multiple_extruders());

    // AMS advertises 4 filament slots — all feeding the one hotend.
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // is_multi_tool() flips — which is why it is the wrong badge predicate.
    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.is_multi_tool());

    // ...but there is still exactly one hotend, so no tool badge.
    REQUIRE(ts.extruder_count() == 1);
    REQUIRE_FALSE(ts.has_multiple_extruders());

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "A real 4-extruder ToolChanger does report multiple extruders",
                 "[tool-state][ams][ams-topology][tool-badge]") {
    // Paired with the AMS case above: if has_multiple_extruders() were hardcoded
    // false, or counted nothing, this fails — so the badge can still appear where
    // it is meaningful.
    auto& ts = helix::ToolState::instance();
    auto disc = make_toolchanger_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.extruder_count() == 4);
    REQUIRE(ts.has_multiple_extruders());

    // Surviving the AMS rebuild matters — that is where the mapping was lost once.
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.extruder_count() == 4);
    REQUIRE(ts.has_multiple_extruders());

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

// =============================================================================
// nozzle_label(): "Nozzle" vs "Nozzle T<n>"
//
// The label sits directly beside a nozzle temperature readout in both the
// controls panel and the filament panel, so it answers "which nozzle am I
// looking at". An AMS's lanes all feed the same hotend, so naming that hotend
// after the loaded lane says nothing, the same reason the nozzle_icon badge
// is gated on extruder count.
// =============================================================================

TEST_CASE_METHOD(ToolStateFixture, "nozzle_label stays plain when AMS lanes share one hotend",
                 "[tool-state][ams][ams-topology][nozzle-label]") {
    auto& ts = helix::ToolState::instance();
    auto disc = make_single_extruder_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.nozzle_label() == "Nozzle");

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // The lane really is selected; this is the value the label used to append.
    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.active_tool_index() == 2);
    REQUIRE(ts.extruder_count() == 1);

    REQUIRE(ts.nozzle_label() == "Nozzle");

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "nozzle_label still names the tool on a real toolchanger",
                 "[tool-state][ams][ams-topology][nozzle-label]") {
    // Paired with the case above: a label hardcoded to "Nozzle" would pass that
    // one, so a machine with real hotends must still get its T<n>.
    auto& ts = helix::ToolState::instance();
    auto disc = make_toolchanger_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.extruder_count() == 4);

    // Klipper reports the active extruder moved to the third toolhead.
    nlohmann::json status = {{"toolhead", {{"extruder", "extruder2"}}}};
    ts.update_from_status(status);
    UpdateQueue::instance().drain();
    REQUIRE(ts.active_tool_index() == 2);
    REQUIRE(ts.nozzle_label() == "Nozzle T2");
}

// ============================================================================
// Tool badge staleness — the badge index must follow an AMS-driven toolchange
// ============================================================================

namespace {

// Brings up the real badge observers from src/ui/ui_ams_tool_text.cpp over the
// real ToolState subjects, so nothing here reimplements the badge's gate or its
// format string. Teardown mirrors AfcToolchangeRenderFixture in
// test_afc_toolchange.cpp: deinit_one() releases the observers before the
// subjects they watch die AND clears the file-static "already initialized"
// latch, without disturbing registry entries other fixtures own.
struct ToolBadgeFixture : public LVGLTestFixture {
    ToolBadgeFixture() {
        AmsState::instance().init_subjects(/*register_xml=*/false);
        ToolState::instance().init_subjects(/*register_xml=*/false);
        helix::ui::init_ams_tool_text_observers();
    }
    ~ToolBadgeFixture() override {
        StaticSubjectRegistry::instance().deinit_one("AmsToolTextObservers");
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    static std::string badge_text() {
        return lv_subject_get_string(ToolState::instance().get_tool_badge_text_subject());
    }
    static int badge_shown() {
        return lv_subject_get_int(ToolState::instance().get_show_tool_badge_subject());
    }
};

} // namespace

TEST_CASE_METHOD(ToolBadgeFixture,
                 "tool badge index follows an AMS toolchange that does not rebuild the tool list",
                 "[tool-state][ams][ams-topology][tool-badge][regression]") {
    // set_ams_topology() bumps tools_version_ only when the topology SHAPE
    // changes; a plain active-tool move publishes active_tool_ alone. A badge
    // observer watching only tools_version_ therefore never re-ran, leaving the
    // index frozen at whatever the last rebuild produced.
    auto& ts = helix::ToolState::instance();
    auto disc = make_toolchanger_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.extruder_count() == 4); // Badge only shows on real multi-nozzle hardware

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(badge_shown() == 1);
    REQUIRE(badge_text() == "0");

    const int version_before = lv_subject_get_int(ts.get_tools_version_subject());

    // Same shape, different active tool — the AMS-driven toolchange.
    topo.active_tool = 2;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // Guard the premise: if this ever bumps, the test stops covering the bug
    // because the tools_version_ observer would mask the missing active_tool one.
    REQUIRE(lv_subject_get_int(ts.get_tools_version_subject()) == version_before);
    REQUIRE(ts.active_tool_index() == 2);
    REQUIRE(badge_text() == "2");
    REQUIRE(badge_shown() == 1);

    // And back down, so a fix that only ever counts upward still fails.
    topo.active_tool = 1;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(badge_text() == "1");
}

TEST_CASE_METHOD(ToolBadgeFixture, "tool badge stays hidden and empty on a single-hotend AMS",
                 "[tool-state][ams][ams-topology][tool-badge]") {
    // The AFC shape: many lanes, one nozzle. Expanding tools_ to one entry per
    // lane must not make the badge appear — annotating the single hotend those
    // lanes share with an index says nothing. This is the case that rendered an
    // empty disc over the nozzle glyph on the temp_stack widget.
    auto& ts = helix::ToolState::instance();

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 4);
    REQUIRE(badge_shown() == 0);
    REQUIRE(badge_text().empty());

    // A lane change on that single hotend must not light it up either — the new
    // active_tool_ observer runs the same gate, so this pins that it re-checks
    // has_multiple_extruders() rather than blindly writing the index.
    topo.active_tool = 3;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(badge_shown() == 0);
    REQUIRE(badge_text().empty());
}
