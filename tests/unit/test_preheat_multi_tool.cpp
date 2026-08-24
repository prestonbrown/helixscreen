// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preheat_multi_tool.cpp
 * @brief Tests for multi-tool preheat logic in PreheatWidget
 *
 * Verifies that when preheating on a multi-tool printer, the correct set of
 * heaters are targeted based on tool_target (-1 = all, 0..N = specific tool).
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/preheat_widget_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "preheat_widget.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: build a vector of ToolInfo for testing
// ============================================================================

static std::vector<ToolInfo> make_test_tools(int count) {
    std::vector<ToolInfo> tools;
    for (int i = 0; i < count; ++i) {
        ToolInfo t;
        t.index = i;
        t.name = "T" + std::to_string(i);
        t.extruder_name = (i == 0) ? "extruder" : ("extruder" + std::to_string(i));
        tools.push_back(t);
    }
    return tools;
}

// ============================================================================
// collect_preheat_heaters: all tools
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters returns all tool heaters when target is -1",
          "[preheat][panel_widget]") {
    auto tools = make_test_tools(3);

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 3);
    REQUIRE(heaters[0] == "extruder");
    REQUIRE(heaters[1] == "extruder1");
    REQUIRE(heaters[2] == "extruder2");
}

TEST_CASE("PreheatWidget: collect_preheat_heaters returns single tool heater for specific target",
          "[preheat][panel_widget]") {
    auto tools = make_test_tools(3);

    SECTION("tool 0") {
        auto heaters = PreheatWidget::collect_preheat_heaters(tools, 0);
        REQUIRE(heaters.size() == 1);
        REQUIRE(heaters[0] == "extruder");
    }

    SECTION("tool 1") {
        auto heaters = PreheatWidget::collect_preheat_heaters(tools, 1);
        REQUIRE(heaters.size() == 1);
        REQUIRE(heaters[0] == "extruder1");
    }

    SECTION("tool 2") {
        auto heaters = PreheatWidget::collect_preheat_heaters(tools, 2);
        REQUIRE(heaters.size() == 1);
        REQUIRE(heaters[0] == "extruder2");
    }
}

// ============================================================================
// collect_preheat_heaters: skips tools with no valid heater
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters skips tools with no heater",
          "[preheat][panel_widget]") {
    auto tools = make_test_tools(3);
    // Tool 1 has neither extruder_name nor heater_name
    tools[1].extruder_name = std::nullopt;
    tools[1].heater_name = std::nullopt;

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 2);
    REQUIRE(heaters[0] == "extruder");
    REQUIRE(heaters[1] == "extruder2");
}

// ============================================================================
// collect_preheat_heaters: prefers heater_name over extruder_name
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters uses effective_heater (heater_name priority)",
          "[preheat][panel_widget]") {
    auto tools = make_test_tools(2);
    tools[0].heater_name = "heater_generic nozzle0";

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 2);
    REQUIRE(heaters[0] == "heater_generic nozzle0");
    REQUIRE(heaters[1] == "extruder1");
}

// ============================================================================
// collect_preheat_heaters: out-of-range target returns empty
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters returns empty for out-of-range target",
          "[preheat][panel_widget]") {
    auto tools = make_test_tools(3);

    SECTION("target beyond tool count") {
        auto heaters = PreheatWidget::collect_preheat_heaters(tools, 5);
        REQUIRE(heaters.empty());
    }

    SECTION("negative target other than -1") {
        auto heaters = PreheatWidget::collect_preheat_heaters(tools, -2);
        REQUIRE(heaters.empty());
    }
}

// ============================================================================
// collect_preheat_heaters: empty tools
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters handles empty tools vector",
          "[preheat][panel_widget]") {
    std::vector<ToolInfo> tools;

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);
    REQUIRE(heaters.empty());
}

// ============================================================================
// collect_preheat_heaters: 6-tool printer (Voron Stealth Changer)
// ============================================================================

// Builds a tool whose heater names are written out rather than generated. An
// expectation produced by the same rule as the fixture passes even when the code
// under test fabricates names instead of reading the ones it was given, so the
// names here are deliberately non-sequential and the assertions are literals.
static void add_tool(std::vector<ToolInfo>& tools, const char* extruder, const char* heater) {
    ToolInfo t;
    t.index = static_cast<int>(tools.size());
    t.name = "T" + std::to_string(tools.size());
    if (extruder) {
        t.extruder_name = extruder;
    }
    if (heater) {
        t.heater_name = heater;
    }
    tools.push_back(std::move(t));
}

TEST_CASE("PreheatWidget: collect_preheat_heaters reads each tool's own heater, in tool order",
          "[preheat][panel_widget]") {
    std::vector<ToolInfo> tools;
    add_tool(tools, "extruder", nullptr);      // plain first hotend
    add_tool(tools, "extruder3", nullptr);     // gap in the numbering, on purpose
    add_tool(tools, "extruder1", "extruder7"); // heater_name outranks extruder_name
    add_tool(tools, nullptr, "extruder9");     // heater only, no extruder
    add_tool(tools, "extruder2", nullptr);

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 5);
    CHECK(heaters[0] == "extruder");
    CHECK(heaters[1] == "extruder3");
    CHECK(heaters[2] == "extruder7");
    CHECK(heaters[3] == "extruder9");
    CHECK(heaters[4] == "extruder2");
}

TEST_CASE("PreheatWidget: collect_preheat_heaters skips a tool with no heater at all",
          "[preheat][panel_widget]") {
    // effective_heater() falls back to "extruder" when both names are unset, so
    // a tool that is skipped and a tool that is kept are distinguishable only by
    // the result size and the absence of that fallback.
    std::vector<ToolInfo> tools;
    add_tool(tools, "extruder", nullptr);
    add_tool(tools, nullptr, nullptr); // no extruder, no heater
    add_tool(tools, "extruder4", nullptr);

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 2);
    CHECK(heaters[0] == "extruder");
    CHECK(heaters[1] == "extruder4");
}

// ============================================================================
// cycle_tool_target
//
// Drives the real PreheatWidget::cycle_tool_target() through
// PreheatWidgetTestAccess. The function reads nothing but ToolState and writes
// nothing but tool_target_, so it runs without an attached widget tree -
// production only reaches it from tool_target_cb(), which needs one.
//
// The count it gates on is extruder_count(), not tool_count(): set_ams_topology()
// expands ToolState's list to one entry per filament lane, and every lane on a
// single-hotend printer feeds the same nozzle. Cycling over lanes would offer
// "T0 / T1 / T2 / T3" as four names for one heater.
// ============================================================================

namespace {

/// Rebuild ToolState around `extruders` real hotends and drain, so
/// extruder_count() reports that number before the cycle runs.
/// init_tools() enumerates extruders out of the discovered Klipper objects.
void seed_extruders(int extruders) {
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < extruders; ++i) {
        arr.push_back(i == 0 ? "extruder" : "extruder" + std::to_string(i));
    }
    arr.push_back("heater_bed");
    arr.push_back("fan");
    helix::PrinterDiscovery disc;
    disc.parse_objects(arr);

    ts.init_tools(disc);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(ts.extruder_count() == extruders);
}

/// Lane topology an AMS backend pushes: `lanes` tools, one per filament slot.
ToolTopology lane_topology(int lanes) {
    ToolTopology topo;
    topo.tool_count = lanes;
    topo.active_tool = 0;
    topo.tool_to_slot.resize(static_cast<size_t>(lanes));
    for (int i = 0; i < lanes; ++i) {
        topo.tool_to_slot[static_cast<size_t>(i)] = i;
    }
    topo.tool_name_prefix = "T";
    return topo;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "PreheatWidget: cycle_tool_target walks every nozzle then wraps back to all",
                 "[preheat][panel_widget]") {
    seed_extruders(3);

    PreheatWidget widget(get_printer_state());
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);

    // all -> T0 -> T1 -> T2 -> all. Asserted step by step rather than as a set,
    // because the ordering is what the button caption promises the user.
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 0);

    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 1);

    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 2);

    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);

    // A second lap lands on the same values, so the wrap left no residue.
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "PreheatWidget: cycle_tool_target visits every nozzle in one lap",
                 "[preheat][panel_widget]") {
    // Paired with the ordering case: a cycle that skipped a nozzle, or stopped
    // one short, would leave a hotend the user cannot aim a preheat at.
    seed_extruders(6);

    PreheatWidget widget(get_printer_state());

    std::vector<int> seen;
    for (int i = 0; i < 7; ++i) {
        PreheatWidgetTestAccess::cycle(widget);
        seen.push_back(PreheatWidgetTestAccess::tool_target(widget));
    }

    REQUIRE(seen == std::vector<int>{0, 1, 2, 3, 4, 5, -1});
}

TEST_CASE_METHOD(LVGLTestFixture, "PreheatWidget: cycle_tool_target collapses to all on one nozzle",
                 "[preheat][panel_widget]") {
    seed_extruders(1);

    PreheatWidget widget(get_printer_state());

    // Seeded with a stale per-tool target, as a recycled widget instance carries
    // one across a home-panel rebuild onto a printer with fewer nozzles.
    PreheatWidgetTestAccess::set_tool_target(widget, 2);

    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);

    // Still "all" however many times it is pressed - there is nothing to cycle to.
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);
}

TEST_CASE_METHOD(LVGLTestFixture, "PreheatWidget: cycle_tool_target counts nozzles, not AMS lanes",
                 "[preheat][panel_widget]") {
    // A 4-lane AMS on a single hotend. tool_count() reports 4; every lane feeds
    // the one heater, so the cycle must offer nothing but "all".
    seed_extruders(1);
    auto& ts = ToolState::instance();
    ts.set_ams_topology(lane_topology(4));
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(ts.tool_count() == 4);
    REQUIRE(ts.extruder_count() == 1);

    PreheatWidget widget(get_printer_state());
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);

    ts.clear_ams_topology();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PreheatWidget: cycle_tool_target still cycles two hotends behind an AMS",
                 "[preheat][panel_widget]") {
    // Paired with the case above: gating on extruder_count() must not disable
    // the cycle wherever an AMS is present, only where the nozzles are one.
    seed_extruders(2);
    auto& ts = ToolState::instance();
    ts.set_ams_topology(lane_topology(8));
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(ts.tool_count() == 8);
    REQUIRE(ts.extruder_count() == 2);

    PreheatWidget widget(get_printer_state());

    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 0);
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == 1);
    // Wraps at the nozzle count, not the lane count.
    PreheatWidgetTestAccess::cycle(widget);
    REQUIRE(PreheatWidgetTestAccess::tool_target(widget) == -1);

    ts.clear_ams_topology();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

// ============================================================================
// collect_preheat_heaters: AMS lanes share a hotend
//
// set_ams_topology() expands ToolState's list to one entry per filament lane,
// and every lane on a single-hotend printer resolves to the same heater. The
// "all tools" preheat must send one target per heater, not one per lane. The
// count it returns is also what the confirmation toast reports.
// ============================================================================

TEST_CASE("PreheatWidget: collect_preheat_heaters collapses lanes sharing one heater",
          "[preheat][panel_widget]") {
    // What set_ams_topology() leaves behind on a 4-lane AMS + one extruder:
    // four tools, every one of them mapped to "extruder".
    std::vector<ToolInfo> tools;
    for (int i = 0; i < 4; ++i) {
        ToolInfo t;
        t.index = i;
        t.name = "T" + std::to_string(i);
        t.extruder_name = "extruder";
        tools.push_back(t);
    }

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 1);
    REQUIRE(heaters[0] == "extruder");
}

TEST_CASE("PreheatWidget: collect_preheat_heaters keeps every distinct heater in tool order",
          "[preheat][panel_widget]") {
    // Paired with the case above: a collapse that kept only the first heater
    // would pass that one and silently stop heating a toolchanger's other
    // hotends. Two extra lanes are hung off T0's heater to prove the dedup
    // removes duplicates rather than truncating the list.
    auto tools = make_test_tools(3);
    ToolInfo lane;
    lane.index = 3;
    lane.name = "T3";
    lane.extruder_name = "extruder";
    tools.push_back(lane);
    lane.index = 4;
    lane.name = "T4";
    lane.extruder_name = "extruder2";
    tools.push_back(lane);

    auto heaters = PreheatWidget::collect_preheat_heaters(tools, -1);

    REQUIRE(heaters.size() == 3);
    REQUIRE(heaters[0] == "extruder");
    REQUIRE(heaters[1] == "extruder1");
    REQUIRE(heaters[2] == "extruder2");
}
