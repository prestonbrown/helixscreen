// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_tool_text.h"

#include "ams_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_tool_topology.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "lvgl_test_fixture.h"
#include "printer_discovery.h"
#include "test_helpers/ams_backend_probes.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/snapmaker_test_access.h"
#include "test_helpers/tool_topologies.h"
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

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 15);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    REQUIRE(ToolState::instance().tools()[14].name == "T14");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] set_ams_topology keeps gcode identity and physical "
                 "label apart",
                 "[tool-state][ams][ams-topology][numbering]") {
    // name is what you type into a console; display_label is what is written on
    // the machine. A widget picking the wrong one is the bug this split exists
    // to make visible at the call site.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 4);
    const auto& tools = ToolState::instance().tools();
    // display_label names the printing end, not the filament lane, so it is
    // built from active_tool_noun() even where the two happen to coincide.
    const auto noun = helix::ui::active_tool_noun();

    CHECK(tools[0].name == "T0");
    CHECK(tools[0].display_label == helix::ui::lane_label(noun, 0));
    CHECK(tools[3].name == "T3");
    CHECK(tools[3].display_label == helix::ui::lane_label(noun, 3));
    // The two fields must never collapse onto the same string — that would
    // silently undo the split this test exists to pin.
    CHECK(tools[0].display_label != tools[0].name);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] active tool updates without rebuilding tools list",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
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
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().tool_count() == 6);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();

    // After clear, tools_ is empty (callers must invoke init_tools again to repopulate)
    REQUIRE(ToolState::instance().tool_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "[ToolState][ams-topology] clear_ams_topology before init is a silent no-op",
                 "[tool-state][ams][ams-topology]") {
    // Shutdown clears AMS backends unconditionally, including on paths such as
    // --version that exit before any subject is initialized.
    ToolState::instance().deinit_subjects();
    REQUIRE_FALSE(ToolState::instance().ams_topology_active());

    helix::TextLogCapture capture;
    ToolState::instance().clear_ams_topology();

    CHECK_FALSE(capture.contains("clear_ams_topology() before init_subjects()"));
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] AFC mock with 4 lanes drives ToolState",
                 "[tool-state][ams][afc][ams-topology]") {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(/*register_xml=*/false);

    auto mock = helix::AmsBackend::create_mock(4);
    REQUIRE(mock != nullptr);
    // A REQUIRE, not a silent skip: the plain mock stands in for a lane
    // multiplexer and owns a table by construction. Returning early here would
    // make the whole case vanish if that ever stopped being true, which is
    // exactly the regression the case exists to catch.
    REQUIRE(mock->owns_tool_mapping_table());

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
    // owns_tool_mapping_table() true (AmsBackendToolChanger does):
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
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ts.ams_topology_active());

    // Feed status that includes per-tool keys ("tool T0", "tool T1", ...).
    // The toolchanger.tool_number block MUST be ignored (topology owns active),
    // but the per-tool loop MUST update .mounted, and the offsets pass the X/Y offsets.
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
    REQUIRE(ts.tools()[1].gcode_offset(helix::Axis::X).mm == Catch::Approx(12.5));
    REQUIRE(ts.tools()[1].gcode_offset(helix::Axis::Y).mm == Catch::Approx(-3.25));
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
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.extruder_count() == 4);
    REQUIRE(ts.has_multiple_extruders());

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

// =============================================================================
// nozzle_label(): "Nozzle" vs "Nozzle <n>"
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
    // one, so a machine with real hotends must still get its number.
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
    // 1-based, matching what PrinterTemperatureState, the temp graph and the
    // print status widget spell for this same nozzle. The gcode identity "T2"
    // names the same hotend one lower and belongs in a console, not here.
    REQUIRE(ts.nozzle_label() == "Nozzle 3");
}

namespace {
helix::PrinterDiscovery make_named_toolchanger_discovery() {
    nlohmann::json objects = {"toolchanger", "tool Left", "tool Right", "extruder", "extruder1"};
    helix::PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}
} // namespace

TEST_CASE_METHOD(ToolStateFixture, "nozzle_label keeps a tool name its owner configured",
                 "[tool-state][ams][ams-topology][nozzle-label]") {
    // A viesturz [tool Right] is what is written on the machine; renumbering it
    // would name a hotend something its owner never sees.
    auto& ts = helix::ToolState::instance();
    auto disc = make_named_toolchanger_discovery();

    ts.init_tools(disc);
    UpdateQueue::instance().drain();
    REQUIRE(ts.extruder_count() == 2);
    REQUIRE(ts.tools()[1].name == "Right");

    nlohmann::json status = {{"toolhead", {{"extruder", "extruder1"}}}};
    ts.update_from_status(status);
    UpdateQueue::instance().drain();
    REQUIRE(ts.active_tool_index() == 1);

    REQUIRE(ts.nozzle_label() == "Nozzle Right");
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
        helix::AmsState::instance().init_subjects(/*register_xml=*/false);
        ToolState::instance().init_subjects(/*register_xml=*/false);
        helix::ui::init_ams_tool_text_observers();
    }
    ~ToolBadgeFixture() override {
        StaticSubjectRegistry::instance().deinit_one("AmsToolTextObservers");
        helix::AmsState::instance().deinit_subjects();
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
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(badge_shown() == 1);
    REQUIRE(badge_text() == "1"); // 1-based: storage index 0 displays as "1"

    const int version_before = lv_subject_get_int(ts.get_tools_version_subject());

    // Same shape, different active tool — the AMS-driven toolchange.
    topo.active_tool = 2;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // Guard the premise: if this ever bumps, the test stops covering the bug
    // because the tools_version_ observer would mask the missing active_tool one.
    REQUIRE(lv_subject_get_int(ts.get_tools_version_subject()) == version_before);
    REQUIRE(ts.active_tool_index() == 2);
    REQUIRE(badge_text() == "3");
    REQUIRE(badge_shown() == 1);

    // And back down, so a fix that only ever counts upward still fails.
    topo.active_tool = 1;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(badge_text() == "2");
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

// ============================================================================
// Init order — the AMS push must not land on subjects that do not exist yet
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "[ToolState][ams-topology] topology before init_subjects is refused whole",
                 "[tool-state][ams][ams-topology][regression]") {
    // AmsState::init_subjects() creates its backend and syncs it straight
    // through build_ams_topology() into set_ams_topology(). Reached before
    // ToolState::init_subjects() had run, every lv_subject_set_int() below
    // landed on a zeroed lv_subject_t (LV_SUBJECT_TYPE_INVALID): LVGL dropped
    // the write and warned, while tools_ was rebuilt anyway. The vector then
    // said 4 tools and tool_count/tools_version said 0 - a divergence nothing
    // republished until PrinterDiscovery's init_tools() happened to reset the
    // override. Applying nothing is the only answer that cannot diverge.
    auto& ts = helix::ToolState::instance();
    ts.deinit_subjects();

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};

    ts.set_ams_topology(topo);

    REQUIRE_FALSE(ts.ams_topology_active());
    REQUIRE(ts.tool_count() == 0);
    REQUIRE(ts.tools().empty());

    // Same contract on the way back out: nothing to clear, nothing published.
    ts.clear_ams_topology();
    REQUIRE_FALSE(ts.ams_topology_active());
}

// A topology rebuild fires whenever the tool count, the tool->slot table or the
// name prefix moves. Which spool is mounted is durable user data (see "Spool
// assignment: identity is durable, weight is cache" in the architecture guide),
// so it has to survive that rebuild the way the per-tool hardware mappings
// already do — but only for a tool that still sources the same lane, because it
// is a fact about the lane and not about the tool index.
//
// It survived nothing at all: every assignment was zeroed the first time a
// table-owning backend published its topology. Nothing caught it because the
// only backend these tests reached through was the mock in tool-changer mode,
// which declared it owned no tool table while the real AmsBackendToolChanger
// declares it does — so the rebuild never ran here.
TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] first topology keeps a spool assigned to the tool",
                 "[tool-state][ams][ams-topology][spool]") {
    auto& ts = ToolState::instance();

    // No topology yet, so backend_slot is -1: the user assigned this against the
    // TOOL, before any lane was known. That record is the tool's own.
    ToolTopology topo;
    topo.tool_count = 2;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1};
    ts.set_ams_topology(topo);
    REQUIRE(ts.tools().size() == 2);
    REQUIRE(ts.tools()[0].backend_slot == 0);

    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);

    // A rebuild that does not move this tool's lane — the tool count grows.
    topo.tool_count = 3;
    topo.tool_to_slot = {0, 1, 2};
    ts.set_ams_topology(topo);

    REQUIRE(ts.tools().size() == 3);
    CHECK(ts.tools()[0].spoolman_id == 42);
    CHECK(ts.tools()[0].spool_name == "Red PLA");
    CHECK(ts.tools()[0].remaining_weight_g == Catch::Approx(750.0f));
    CHECK(ts.tools()[0].total_weight_g == Catch::Approx(1000.0f));

    ts.clear_ams_topology();
}

// The other half, and the one that makes carrying by tool index wrong. A remap
// evicts both losing sides (assign_tool_slot in ams_tool_map_sync.h), so it
// routinely leaves a tool mapped to no lane — and sync_from_backend()'s
// slot->tool bridge skips slots whose mapped_tool is < 0. A record carried onto
// such a tool is never corrected by anything: it claims a spool that is now
// driving a different head, and on a backend without firmware spool persistence
// it is written to tool_spools.json and survives a restart.
TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] a rebuild drops the spool of a tool that lost its lane",
                 "[tool-state][ams][ams-topology][spool]") {
    auto& ts = ToolState::instance();

    ToolTopology topo;
    topo.tool_count = 2;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1};
    ts.set_ams_topology(topo);
    REQUIRE(ts.tools().size() == 2);

    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);
    ts.assign_spool(1, 99, "Blue PETG", 500.0f, 1000.0f);

    // The user remaps T0 onto lane 1. The eviction leaves T1 pointing at nothing.
    topo.tool_to_slot = {1, -1};
    ts.set_ams_topology(topo);

    // T0 moved lanes, so its old record is not its own any more; the bridge in
    // sync_from_backend() re-derives it from lane 1.
    CHECK(ts.tools()[0].spoolman_id == 0);
    CHECK(ts.tools()[0].spool_name.empty());

    // T1 sources no lane at all. Nothing will ever correct it, so it must not
    // be left holding the spool that is now driving T0.
    CHECK(ts.tools()[1].spoolman_id == 0);
    CHECK(ts.tools()[1].spool_name.empty());

    // The rebuild really did happen — otherwise both checks pass for the wrong
    // reason, because nothing moved.
    CHECK(ts.tools()[0].backend_slot == 1);
    CHECK(ts.tools()[1].backend_slot == -1);

    ts.clear_ams_topology();
}

// ============================================================================
// Empty carriage — on a tool changer, "no tool on the carriage" is a real
// state, not "nothing loaded". Lane-based topologies keep -1 -> T0: the one
// hotend is still T0 regardless of which lane feeds it.
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "An empty carriage stays empty on a tool changer topology",
                 "[tool-state][ams-topology][empty-carriage]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.allows_empty_carriage = true;

    topo.active_tool = 2;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    // Unmounting parks the head: nothing is on the carriage.
    topo.active_tool = -1;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == -1);
    CHECK(ToolState::instance().active_tool() == nullptr);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "A lane topology with nothing loaded still reports T0",
                 "[tool-state][ams-topology][empty-carriage]") {
    // Lanes feeding one extruder: -1 means no lane loaded, and the one hotend
    // is still T0.
    ToolTopology topo;
    topo.tool_count = 4;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.active_tool = -1;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == 0);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE("A tool changer's topology allows an empty carriage",
          "[tool-state][ams-topology][empty-carriage]") {
    helix::AmsBackendToolChanger tc(nullptr, nullptr);
    tc.set_discovered_tools({"T0", "T1"});
    auto topo = helix::build_ams_topology(&tc, 0);
    REQUIRE(topo.has_value());
    CHECK(topo->allows_empty_carriage);
    CHECK(topo->active_tool == -1);
}

// ============================================================================
// A backend-owned active tool. A backend whose carriage can be empty but owns
// no tool table still decides which head is mounted, because toolhead.extruder
// names the last head used rather than a mounted one
// (prestonbrown/helixscreen#1784).
// ============================================================================

namespace {

struct BackendActiveToolFixture : public ToolStateFixture {
    BackendActiveToolFixture() {
        helix::AmsState::instance().init_subjects(/*register_xml=*/false);
    }
    ~BackendActiveToolFixture() override {
        UpdateQueue::instance().drain();
        helix::AmsState::instance().clear_backends();
        helix::AmsState::instance().deinit_subjects();
    }

    static void init_tools_from(std::initializer_list<const char*> objects) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json(objects));
        ToolState::instance().init_tools(hw);
    }

    // Production delivers one status frame to ToolState and to the backend,
    // and AmsState syncs from the backend afterwards.
    static void deliver(helix::AmsBackendSnapmaker& backend, const nlohmann::json& frame) {
        ToolState::instance().update_from_status(frame);
        helix::SnapmakerTestAccess::handle_status(backend, frame);
        UpdateQueue::instance().drain();
        helix::AmsState::instance().sync_from_backend();
        UpdateQueue::instance().drain();
    }
};

nlohmann::json u1_all_parked_frame() {
    return {{"extruder", {{"state", "PARKED"}}},
            {"extruder1", {{"state", "PARKED"}}},
            {"extruder2", {{"state", "PARKED"}}},
            {"extruder3", {{"state", "PARKED"}}},
            {"toolhead", {{"extruder", "extruder2"}}}};
}

} // namespace

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "An empty carriage reported by a table-less backend reaches ToolState",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                     "filament_detect", "toolhead", "heater_bed", "print_task_config"});
    REQUIRE(ToolState::instance().tool_count() == 4);

    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend(nullptr, nullptr);
    REQUIRE_FALSE(backend->owns_tool_mapping_table());

    deliver(*backend, nlohmann::json{{"toolhead", {{"extruder", "extruder2"}}}});
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    deliver(*backend, u1_all_parked_frame());
    CHECK(ToolState::instance().active_tool_index() == -1);
    CHECK(lv_subject_get_int(ToolState::instance().get_active_tool_subject()) == -1);
    CHECK(ToolState::instance().active_tool() == nullptr);

    // toolhead.extruder arriving again on its own does not remount T2.
    ToolState::instance().update_from_status(
        nlohmann::json{{"toolhead", {{"extruder", "extruder2"}}}});
    CHECK(ToolState::instance().active_tool_index() == -1);

    deliver(*backend, nlohmann::json{{"extruder1", {{"state", "ACTIVE"}, {"active_pin", true}}},
                                     {"toolhead", {{"extruder", "extruder1"}}}});
    CHECK(ToolState::instance().active_tool_index() == 1);
}

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "Parking the mounted head on its own empties the carriage in ToolState",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                     "filament_detect", "toolhead", "heater_bed", "print_task_config"});
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend(nullptr, nullptr);

    deliver(*backend, nlohmann::json{{"toolhead", {{"extruder", "extruder2"}}}});
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    deliver(*backend, nlohmann::json{{"extruder2", {{"state", "PARKED"}}}});
    CHECK(ToolState::instance().active_tool_index() == -1);
}

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "Dropping the backend hands the active tool back to toolhead.extruder",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                     "filament_detect", "toolhead", "heater_bed", "print_task_config"});
    {
        helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend(nullptr, nullptr);
        deliver(*backend, u1_all_parked_frame());
        REQUIRE(ToolState::instance().active_tool_index() == -1);
    }
    // No backend reports an empty carriage now, so T0 holds the head until a
    // frame names one.
    CHECK_FALSE(ToolState::instance().backend_owns_active_tool());
    CHECK(ToolState::instance().active_tool_index() == 0);

    ToolState::instance().update_from_status(
        nlohmann::json{{"toolhead", {{"extruder", "extruder3"}}}});
    CHECK(ToolState::instance().active_tool_index() == 3);
}

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "A table-less backend whose carriage cannot be empty leaves toolhead.extruder "
                 "in charge",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "toolhead", "heater_bed"});
    REQUIRE(ToolState::instance().tool_count() == 2);

    helix::test::RegisteredBackend<helix::AmsBackendAce> backend(nullptr, nullptr);
    REQUIRE_FALSE(backend->owns_tool_mapping_table());
    REQUIRE_FALSE(backend->carriage_can_be_empty());
    REQUIRE(backend->get_current_tool() == -1);

    ToolState::instance().update_from_status(
        nlohmann::json{{"toolhead", {{"extruder", "extruder1"}}}});
    helix::AmsState::instance().sync_from_backend();
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == 1);
}

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "A backend whose carriage cannot be empty reporting no tool leaves T0 mounted",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "toolhead", "heater_bed"});
    helix::test::RegisteredBackend<helix::AmsBackendAce> backend(nullptr, nullptr);
    REQUIRE_FALSE(backend->carriage_can_be_empty());
    REQUIRE(backend->get_current_tool() == -1);

    helix::AmsState::instance().sync_from_backend();
    UpdateQueue::instance().drain();
    CHECK(ToolState::instance().active_tool_index() == 0);
    CHECK(lv_subject_get_int(ToolState::instance().get_active_tool_subject()) == 0);
    CHECK_FALSE(ToolState::instance().backend_owns_active_tool());
}

TEST_CASE_METHOD(BackendActiveToolFixture,
                 "A park split across two frames empties the carriage in ToolState",
                 "[tool-state][tool_switcher][1784]") {
    init_tools_from({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                     "filament_detect", "toolhead", "heater_bed", "print_task_config"});
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend(nullptr, nullptr);

    deliver(*backend, nlohmann::json{{"extruder2", {{"state", "ACTIVE"}, {"active_pin", true}}},
                                     {"toolhead", {{"extruder", "extruder2"}}}});
    REQUIRE(ToolState::instance().active_tool_index() == 2);

    deliver(*backend, nlohmann::json{{"extruder2", {{"state", "PARKED"}, {"active_pin", true}}}});
    REQUIRE(ToolState::instance().active_tool_index() == 2);
    deliver(*backend, nlohmann::json{{"extruder2", {{"active_pin", false}}}});
    CHECK(ToolState::instance().active_tool_index() == -1);
}

TEST_CASE("A lane backend's topology does not allow an empty carriage",
          "[tool-state][ams-topology][tool_switcher][1784]") {
    helix::AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(afc.owns_tool_mapping_table());
    auto topo = helix::build_ams_topology(&afc, 0);
    REQUIRE(topo.has_value());
    CHECK_FALSE(topo->allows_empty_carriage);
}

TEST_CASE_METHOD(ToolStateFixture, "multiple toolheads means a tool changer, never AMS slots",
                 "[tool-state][ams][ams-topology]") {
    auto& ts = helix::ToolState::instance();

    SECTION("one hotend") {
        ts.init_tools(make_single_extruder_discovery());
        UpdateQueue::instance().drain();
        CHECK_FALSE(ts.has_multiple_toolheads());
    }
    SECTION("a 4-slot AMS feeding one hotend") {
        ts.init_tools(make_single_extruder_discovery());
        helix::ToolTopology topo;
        topo.tool_count = 4;
        topo.active_tool = 0;
        topo.tool_to_slot = {0, 1, 2, 3};
        ts.set_ams_topology(topo);
        UpdateQueue::instance().drain();
        REQUIRE(ts.is_multi_tool());
        CHECK_FALSE(ts.has_multiple_toolheads());
        ts.clear_ams_topology();
        UpdateQueue::instance().drain();
    }
    SECTION("a 4-tool changer") {
        ts.init_tools(make_toolchanger_discovery());
        UpdateQueue::instance().drain();
        CHECK(ts.has_multiple_toolheads());
    }
}

// =============================================================================
// Physical heads on a hub-fed rig. A StealthChanger whose AFC units feed some
// heads through hubs publishes one tool per lane, so the tool list is longer
// than the head list, and each lane tool must keep the extruder it prints
// through.
// =============================================================================

namespace {

/// Seven heads, as klipper-toolchanger enumerates them.
helix::PrinterDiscovery make_seven_head_discovery() {
    return helix::test::make_head_discovery(7);
}

} // namespace

TEST_CASE_METHOD(ToolStateFixture, "A hub-fed lane tool keeps the extruder its lane feeds",
                 "[tool-state][ams-topology][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    REQUIRE(ts.tool_count() == 7);

    ts.set_ams_topology(helix::test::make_hub_topology());
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 18);
    const auto& tools = ts.tools();
    CHECK(tools[5].extruder_name == std::optional<std::string>("extruder4"));
    CHECK(tools[8].extruder_name == std::optional<std::string>("extruder5"));
    CHECK(tools[12].extruder_name == std::optional<std::string>("extruder6"));
    CHECK(tools[17].extruder_name == std::optional<std::string>("extruder6"));
    // A lane tool takes its head's fan, not the fan of the tool its index
    // happened to land on.
    CHECK(tools[5].fan_name == tools[4].fan_name);
    CHECK(ts.extruder_count() == 7);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "physical_heads collapses hub lanes onto their head",
                 "[tool-state][ams-topology][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    ts.set_ams_topology(helix::test::make_hub_topology());
    UpdateQueue::instance().drain();

    const auto heads = ts.physical_heads();
    REQUIRE(heads.size() == 7);
    CHECK(heads[0].tool_indices == std::vector<int>{0});
    CHECK(heads[4].extruder_name == std::optional<std::string>("extruder4"));
    CHECK(heads[4].tool_indices == std::vector<int>{4, 5, 6, 7});
    CHECK(heads[5].tool_indices == std::vector<int>{8, 9, 10, 11});
    CHECK(heads[6].extruder_name == std::optional<std::string>("extruder6"));
    CHECK(heads[6].tool_indices == std::vector<int>{12, 13, 14, 15, 16, 17});

    // A head stands for its mounted lane while mounted, else its lowest.
    CHECK(heads[5].representative_tool(9) == 9);
    CHECK(heads[5].representative_tool(0) == 8);
    CHECK(heads[5].representative_tool(-1) == 8);
    CHECK(helix::head_index_for_tool(heads, 14) == 6);
    CHECK(helix::head_index_for_tool(heads, 18) == -1);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "a parked hub head stands for the lane seated in it",
                 "[tool-state][ams-topology][physical-heads][seated-lane]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());

    SECTION("the seated lane, not the lowest") {
        ts.set_ams_topology(helix::test::make_hub_topology(0, {0, 10, 14}));
        const auto& heads = ts.physical_heads();
        REQUIRE(heads.size() == 7);
        CHECK(heads[5].seated_tool == 10);
        CHECK(heads[5].representative_tool(0) == 10);
        CHECK(heads[5].representative_tool(-1) == 10);
        CHECK(heads[6].representative_tool(0) == 14);
        // The mounted head keeps the lane it prints through.
        CHECK(heads[5].representative_tool(9) == 9);
        // A head with nothing seated falls back to its lowest lane.
        CHECK(heads[4].seated_tool == -1);
        CHECK(heads[4].representative_tool(0) == 4);
    }

    SECTION("two lanes claiming one head decide nothing") {
        ts.set_ams_topology(helix::test::make_hub_topology(0, {9, 11}));
        const auto& heads = ts.physical_heads();
        CHECK(heads[5].seated_tool == -1);
        CHECK(heads[5].representative_tool(0) == 8);
    }

    SECTION("a seat moving without the shape changing reaches the heads") {
        ts.set_ams_topology(helix::test::make_hub_topology(0, {10}));
        REQUIRE(ts.physical_heads()[5].representative_tool(0) == 10);
        const int version = lv_subject_get_int(ts.get_tools_version_subject());

        ts.set_ams_topology(helix::test::make_hub_topology(0, {11}));
        CHECK(ts.physical_heads()[5].representative_tool(0) == 11);
        CHECK(ts.tools()[11].lane_seated);
        CHECK_FALSE(ts.tools()[10].lane_seated);

        // A backend that stops saying leaves no lane seated.
        ts.set_ams_topology(helix::test::make_hub_topology(0));
        CHECK(ts.physical_heads()[5].representative_tool(0) == 8);
        CHECK(lv_subject_get_int(ts.get_tools_version_subject()) == version);
    }

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE("physical_heads keeps a tool naming no extruder as a head of its own",
          "[tool-state][physical-heads]") {
    std::vector<helix::ToolInfo> tools(3);
    tools[0].extruder_name = "extruder";
    tools[1].extruder_name = std::nullopt;
    tools[2].extruder_name = std::nullopt;
    const auto heads = helix::physical_heads(tools);
    REQUIRE(heads.size() == 3);
    CHECK_FALSE(heads[1].extruder_name.has_value());
    CHECK(heads[2].tool_indices == std::vector<int>{2});
}

TEST_CASE("build_ams_topology carries each lane's extruder",
          "[tool-state][ams-topology][physical-heads]") {
    helix::AmsBackendMock mock(4);
    mock.set_stealth_mode(true);
    auto topo = helix::build_ams_topology(&mock, 0);
    REQUIRE(topo.has_value());
    REQUIRE(topo->tool_count == 18);
    REQUIRE(topo->tool_extruder.size() == 18);
    CHECK(topo->tool_extruder[0] == "extruder");
    CHECK(topo->tool_extruder[7] == "extruder4");
    CHECK(topo->tool_extruder[12] == "extruder6");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "A lane naming an extruder no tool carries keeps its tool's own extruder",
                 "[tool-state][ams-topology][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    REQUIRE(ts.tools()[1].extruder_name == std::optional<std::string>("extruder1"));

    // A backend reporting its own section name rather than the Klipper
    // extruder's: nothing heats under that name.
    auto topo = helix::test::make_head_topology(7);
    topo.tool_extruder[1] = "AFC_extruder_left";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 7);
    CHECK(ts.tools()[1].extruder_name == std::optional<std::string>("extruder1"));
    CHECK(ts.tools()[1].effective_heater() == "extruder1");
    CHECK(ts.extruder_count() == 7);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "physical_heads follows a tool's extruder changing in status",
                 "[tool-state][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    REQUIRE(ts.physical_heads().size() == 7);

    // T1 reports that it prints through T0's extruder: one head fewer.
    ts.update_from_status({{"tool T1", {{"extruder", "extruder"}}}});
    CHECK(ts.physical_heads().size() == 6);
    CHECK(ts.physical_heads()[0].tool_indices == std::vector<int>{0, 1});
    CHECK(ts.heated_heads().size() == 6);

    // And a tool that reports none is a head of its own, with no heater.
    ts.update_from_status({{"tool T1", {{"extruder", ""}}}});
    CHECK(ts.physical_heads().size() == 7);
    CHECK(ts.heated_heads().size() == 6);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "A topology that moves a lane to another head rebuilds the tools",
                 "[tool-state][ams-topology][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    ts.set_ams_topology(helix::test::make_head_topology(7));
    UpdateQueue::instance().drain();
    REQUIRE(ts.physical_heads().size() == 7);
    REQUIRE(ts.tools()[1].extruder_name == std::optional<std::string>("extruder1"));

    // The same tools on the same lanes, but T1's lane now feeds T0's head.
    auto topo = helix::test::make_head_topology(7);
    topo.tool_extruder[1] = "extruder";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();

    CHECK(ts.tools()[1].extruder_name == std::optional<std::string>("extruder"));
    CHECK(ts.tools()[1].effective_heater() == "extruder");
    REQUIRE(ts.physical_heads().size() == 6);
    CHECK(ts.physical_heads()[0].tool_indices == std::vector<int>{0, 1});

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture, "clear_ams_topology leaves no heads behind",
                 "[tool-state][ams-topology][physical-heads]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_seven_head_discovery());
    ts.set_ams_topology(helix::test::make_hub_topology());
    UpdateQueue::instance().drain();
    REQUIRE(ts.physical_heads().size() == 7);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 0);
    CHECK(ts.physical_heads().empty());
    CHECK(ts.extruder_count() == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "A changer listing an extruder twice maps each tool to its own",
                 "[tool-state][toolchanger][physical-heads]") {
    helix::PrinterDiscovery disc;
    disc.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2",
                                              "extruder", "extruder", "extruder1", "extruder2"}));
    auto& ts = helix::ToolState::instance();
    ts.init_tools(disc);

    REQUIRE(ts.tool_count() == 3);
    CHECK(ts.tools()[0].extruder_name == std::optional<std::string>("extruder"));
    CHECK(ts.tools()[1].extruder_name == std::optional<std::string>("extruder1"));
    CHECK(ts.tools()[2].extruder_name == std::optional<std::string>("extruder2"));
    CHECK(ts.physical_heads().size() == 3);
}

TEST_CASE("build_ams_topology's 1:1 fallback counts the slots in the snapshot it is given",
          "[tool-state][ams-topology][physical-heads]") {
    // An AFC with no lanes yet: it owns the table but has no mapping to give.
    AfcProbe afc;
    REQUIRE(afc.get_tool_mapping().empty());
    REQUIRE(afc.get_system_info().total_slots == 0);

    helix::AmsSystemInfo info;
    helix::AmsUnit unit;
    for (int i = 0; i < 3; ++i) {
        helix::SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.extruder_name = i == 2 ? "extruder1" : "extruder";
        unit.slots.push_back(slot);
    }
    unit.slot_count = 3;
    info.units.push_back(unit);
    info.total_slots = 3;

    auto topo = helix::build_ams_topology(&afc, 0, info);
    REQUIRE(topo.has_value());
    CHECK(topo->tool_count == 3);
    CHECK(topo->tool_to_slot == std::vector<int>{0, 1, 2});
    CHECK(topo->tool_extruder == std::vector<std::string>{"extruder", "extruder", "extruder1"});
}

TEST_CASE_METHOD(ToolBadgeFixture, "tool badge names the mounted nozzle on a shared heater",
                 "[tool-state][ams][tool-badge][indx]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(make_single_extruder_discovery());
    UpdateQueue::instance().drain();

    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.allows_empty_carriage = true;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_extruder = {"extruder", "extruder", "extruder", "extruder"};
    topo.heads_share_heater = true;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(badge_shown() == 1);
    CHECK(badge_text() == "3");

    topo.active_tool = -1;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    CHECK(badge_shown() == 0);

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}
