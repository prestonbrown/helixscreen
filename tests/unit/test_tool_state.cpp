// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_state.cpp
 * @brief Tests for ToolInfo struct, DetectState enum, and ToolState singleton
 */

#include "../fake_moonraker_client.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/config_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "printer_cache_registry.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sys/stat.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

/// This file had no fixture, so nothing drained the UpdateQueue. The
/// request_tool_change tests register an AMS backend, and the
/// backend's events reach AmsState::on_backend_event, which defers its subject
/// writes; each test returned with that work queued and handed it to whichever
/// test drained next (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects and the test's
/// backend are still alive, before HelixTestFixture's own teardown.
struct ToolStateFixture : public HelixTestFixture {
    ~ToolStateFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        // init_subjects() keeps the tools a live ToolState holds.
        ToolState::instance().deinit_subjects();
    }
};

// ============================================================================
// ToolInfo struct tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: default construction", "[tool][tool-state]") {
    ToolInfo info;

    REQUIRE(info.index == 0);
    REQUIRE(info.name == "T0");
    REQUIRE(info.extruder_name.has_value());
    REQUIRE(info.extruder_name.value() == "extruder");
    REQUIRE_FALSE(info.heater_name.has_value());
    REQUIRE_FALSE(info.fan_name.has_value());
    for (helix::Axis axis : helix::kAllAxes) {
        REQUIRE(info.gcode_offset(axis).mm == 0.0f);
        REQUIRE_FALSE(info.gcode_offset(axis).known);
        REQUIRE_FALSE(info.gcode_offset(axis).dirty());
    }
    REQUIRE_FALSE(info.active);
    REQUIRE_FALSE(info.mounted);
    REQUIRE(info.detect_state == DetectState::UNAVAILABLE);
    REQUIRE(info.backend_index == -1);
    REQUIRE(info.backend_slot == -1);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolInfo: effective_heater is the heater, else the extruder, else \"extruder\"",
                 "[tool][tool-state]") {
    struct Row {
        std::optional<std::string> heater, extruder;
        const char* want;
    };
    const Row rows[] = {
        {"heater_generic chamber", "extruder1", "heater_generic chamber"},
        {std::nullopt, "extruder1", "extruder1"},
        {std::nullopt, std::nullopt, "extruder"},
    };
    for (const auto& row : rows) {
        INFO(row.want);
        ToolInfo info;
        info.heater_name = row.heater;
        info.extruder_name = row.extruder;
        CHECK(info.effective_heater() == row.want);
    }
}

// ============================================================================
// ToolState singleton tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: init_subjects creates subjects",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    REQUIRE(ts.get_active_tool_subject() != nullptr);
    REQUIRE(ts.get_tool_count_subject() != nullptr);
    REQUIRE(ts.get_tools_version_subject() != nullptr);

    REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 0);
    REQUIRE(lv_subject_get_int(ts.get_tool_count_subject()) == 0);
    REQUIRE(lv_subject_get_int(ts.get_tools_version_subject()) == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: double init is safe", "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.init_subjects(false); // Should be a no-op

    REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: deinit then re-init", "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // Set a value
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 1);

    // Deinit clears state
    ts.deinit_subjects();
    REQUIRE(ts.tool_count() == 0);

    // Re-init works
    ts.init_subjects(false);
    REQUIRE(lv_subject_get_int(ts.get_tool_count_subject()) == 0);
}

// ============================================================================
// init_tools tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: init_tools with no tools creates implicit tool",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "fan", "gcode_move"});
    hw.parse_objects(objects);

    ts.init_tools(hw);

    REQUIRE(ts.tool_count() == 1);
    REQUIRE(lv_subject_get_int(ts.get_tool_count_subject()) == 1);

    const auto& tools = ts.tools();
    REQUIRE(tools.size() == 1);
    REQUIRE(tools[0].name == "T0");
    REQUIRE(tools[0].extruder_name.value() == "extruder");
    REQUIRE(tools[0].fan_name.value() == "fan");
    REQUIRE(tools[0].active == true);
    REQUIRE(tools[0].index == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: init_tools with toolchanger creates N tools",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "extruder",
                               "extruder1", "extruder2", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    int version_before = lv_subject_get_int(ts.get_tools_version_subject());
    ts.init_tools(hw);
    int version_after = lv_subject_get_int(ts.get_tools_version_subject());

    REQUIRE(ts.tool_count() == 3);
    REQUIRE(version_after == version_before + 1);

    const auto& tools = ts.tools();
    REQUIRE(tools[0].name == "T0");
    REQUIRE(tools[0].extruder_name.value() == "extruder");
    REQUIRE(tools[1].name == "T1");
    REQUIRE(tools[1].extruder_name.value() == "extruder1");
    REQUIRE(tools[2].name == "T2");
    REQUIRE(tools[2].extruder_name.value() == "extruder2");
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: extruder_name_for_tool", "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "tool T2", "extruder", "extruder1", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 3);

    CHECK(ts.extruder_name_for_tool(0) == "extruder");
    CHECK(ts.extruder_name_for_tool(1) == "extruder1");
    // Three tools on two extruders: T2 names none.
    CHECK(ts.extruder_name_for_tool(2).empty());
    CHECK(ts.extruder_name_for_tool(3).empty());
    CHECK(ts.extruder_name_for_tool(-1).empty());
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: active_tool accessors", "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.active_tool() == nullptr);

    ts.update_from_status({{"toolchanger", {{"tool_number", 0}}}});
    REQUIRE(ts.active_tool_index() == 0);
    REQUIRE(ts.active_tool() != nullptr);
    REQUIRE(ts.active_tool()->name == "T0");
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: re-init with different tool count",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // First init: 1 implicit tool
    helix::PrinterDiscovery hw1;
    nlohmann::json objects1 = nlohmann::json::array({"extruder", "gcode_move"});
    hw1.parse_objects(objects1);
    ts.init_tools(hw1);

    int v1 = lv_subject_get_int(ts.get_tools_version_subject());
    REQUIRE(ts.tool_count() == 1);

    // Second init: 2 tools
    helix::PrinterDiscovery hw2;
    nlohmann::json objects2 = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"});
    hw2.parse_objects(objects2);
    ts.init_tools(hw2);

    int v2 = lv_subject_get_int(ts.get_tools_version_subject());
    REQUIRE(ts.tool_count() == 2);
    REQUIRE(v2 == v1 + 1);
}

// ============================================================================
// update_from_status tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status sets active tool",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    nlohmann::json status = {{"toolchanger", {{"tool_number", 1}}}};
    ts.update_from_status(status);

    REQUIRE(ts.active_tool_index() == 1);
    REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 1);
    REQUIRE(ts.active_tool()->name == "T1");
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status sets mounted state",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    nlohmann::json status = {{"tool T0", {{"mounted", true}, {"active", true}}},
                             {"tool T1", {{"mounted", false}, {"active", false}}}};
    ts.update_from_status(status);

    REQUIRE(ts.tools()[0].mounted == true);
    REQUIRE(ts.tools()[0].active == true);
    REQUIRE(ts.tools()[1].mounted == false);
    REQUIRE(ts.tools()[1].active == false);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status parses offsets",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    nlohmann::json status = {
        {"tool T1", {{"gcode_x_offset", 1.5}, {"gcode_y_offset", -2.3}, {"gcode_z_offset", 0.15}}}};
    ts.update_from_status(status);

    REQUIRE(ts.tools()[1].gcode_offset(helix::Axis::X).mm == Catch::Approx(1.5f));
    REQUIRE(ts.tools()[1].gcode_offset(helix::Axis::Y).mm == Catch::Approx(-2.3f));
    REQUIRE(ts.tools()[1].gcode_offset(helix::Axis::Z).mm == Catch::Approx(0.15f));
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status with no tools is safe",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // No init_tools called, tools_ is empty
    const int before = ts.active_tool_index();
    nlohmann::json status = {{"toolchanger", {{"tool_number", 1}}}};
    ts.update_from_status(status);

    // With no tools there is nothing for tool_number to select.
    REQUIRE(ts.active_tool_index() == before);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status tool_number -1 means no tool",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Set active to T1 first
    nlohmann::json status1 = {{"toolchanger", {{"tool_number", 1}}}};
    ts.update_from_status(status1);
    REQUIRE(ts.active_tool_index() == 1);
    REQUIRE(ts.active_tool() != nullptr);

    // -1 means no tool mounted
    nlohmann::json status2 = {{"toolchanger", {{"tool_number", -1}}}};
    ts.update_from_status(status2);
    REQUIRE(ts.active_tool_index() == -1);
    REQUIRE(ts.active_tool() == nullptr);
}

namespace {

// A klipper-toolchanger printer with the given tools, numbered as its first
// status frame reports them.
void init_numbered_changer(ToolState& ts, const std::vector<std::pair<int, std::string>>& tools) {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"toolchanger", "gcode_move"});
    nlohmann::json numbers = nlohmann::json::array();
    nlohmann::json names = nlohmann::json::array();
    for (size_t i = 0; i < tools.size(); ++i) {
        objects.push_back("tool " + tools[i].second);
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
        numbers.push_back(tools[i].first);
        names.push_back("tool " + tools[i].second);
    }
    hw.parse_objects(objects);
    hw.order_tools_by_number({{"tool_numbers", numbers}, {"tool_names", names}});
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == static_cast<int>(tools.size()));
}

} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: tool_number maps through klipper-toolchanger's tool numbers",
                 "[tool][tool-state][tool-number]") {
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    SECTION("numbers that skip: T0 and T2 only") {
        init_numbered_changer(ts, {{0, "T0"}, {2, "T2"}});

        ts.update_from_status({{"toolchanger", {{"tool_number", 2}}}});
        REQUIRE(ts.active_tool_index() == 1);
        REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 1);
        REQUIRE(ts.active_tool() != nullptr);
        REQUIRE(ts.active_tool()->name == "T2");

        ts.update_from_status({{"toolchanger", {{"tool_number", 0}}}});
        REQUIRE(ts.active_tool_index() == 0);
        REQUIRE(ts.active_tool()->name == "T0");

        ts.update_from_status({{"toolchanger", {{"tool_number", -1}}}});
        REQUIRE(ts.active_tool_index() == -1);
        REQUIRE(ts.active_tool() == nullptr);
    }

    SECTION("a number the changer does not list mounts no known tool") {
        init_numbered_changer(ts, {{0, "T0"}, {2, "T2"}});
        ts.update_from_status({{"toolchanger", {{"tool_number", 2}}}});
        REQUIRE(ts.active_tool_index() == 1);

        // 1 is inside the list's range but numbers no tool; 5 is past it.
        for (int unknown : {1, 5}) {
            ts.update_from_status({{"toolchanger", {{"tool_number", 2}}}});
            ts.update_from_status({{"toolchanger", {{"tool_number", unknown}}}});
            CHECK(ts.active_tool_index() == -1);
            CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == -1);
            CHECK(ts.active_tool() == nullptr);
        }
    }

    SECTION("contiguous numbering is unchanged") {
        init_numbered_changer(ts, {{0, "T0"}, {1, "T1"}, {2, "T2"}});
        for (int n : {2, 0, 1}) {
            ts.update_from_status({{"toolchanger", {{"tool_number", n}}}});
            CHECK(ts.active_tool_index() == n);
        }
    }

    SECTION("names out of number order: the number picks the tool, not the position") {
        // [tool Right] numbered 0 sorts after [tool Left] by name.
        init_numbered_changer(ts, {{0, "Right"}, {1, "Left"}});
        ts.update_from_status({{"toolchanger", {{"tool_number", 1}}}});
        REQUIRE(ts.active_tool() != nullptr);
        CHECK(ts.active_tool()->name == "Left");
    }
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: without a numbers list, tool_number is the tool's position",
                 "[tool][tool-state][tool-number]") {
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"}));
    ts.init_tools(hw);

    ts.update_from_status({{"toolchanger", {{"tool_number", 1}}}});
    REQUIRE(ts.active_tool_index() == 1);

    ts.update_from_status({{"toolchanger", {{"tool_number", 7}}}});
    CHECK(ts.active_tool_index() == -1);
    CHECK(ts.active_tool() == nullptr);
}

// ============================================================================
// Lifecycle edge case tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: update_from_status captures extruder and fan from Klipper",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Klipper sends extruder association in tool status
    nlohmann::json status = {{"tool T0", {{"extruder", "extruder"}, {"fan", "part_fan_T0"}}},
                             {"tool T1", {{"extruder", "extruder1"}, {"fan", "part_fan_T1"}}}};
    ts.update_from_status(status);

    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[0].fan_name.value() == "part_fan_T0");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[1].fan_name.value() == "part_fan_T1");

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: detect_state parsed from status",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"toolchanger", "tool T0", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // klipper-toolchanger spells DETECT_PRESENT "mounted" (klipper/extras/
    // toolchanger.py); a stock machine never sends "present".
    nlohmann::json status = {{"tool T0", {{"detect_state", "mounted"}}}};
    ts.update_from_status(status);
    REQUIRE(ts.tools()[0].detect_state == DetectState::PRESENT);

    nlohmann::json status2 = {{"tool T0", {{"detect_state", "absent"}}}};
    ts.update_from_status(status2);
    REQUIRE(ts.tools()[0].detect_state == DetectState::ABSENT);

    // DETECT_UNAVAILABLE, and any value we do not recognise, read as unavailable.
    nlohmann::json status3 = {{"tool T0", {{"detect_state", "unavailable"}}}};
    ts.update_from_status(status3);
    REQUIRE(ts.tools()[0].detect_state == DetectState::UNAVAILABLE);

    // "present" is no firmware's spelling upstream, but forks that copied the
    // older wording still send it, and it means the same thing.
    nlohmann::json status4 = {{"tool T0", {{"detect_state", "present"}}}};
    ts.update_from_status(status4);
    REQUIRE(ts.tools()[0].detect_state == DetectState::PRESENT);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: toolhead.extruder names the active tool",
                 "[tool][tool-state][active-extruder]") {
    using nlohmann::json;
    const json changer = json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    const json two_extruders = json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    struct Row {
        const char* name;
        json objects;
        json status;
        int seeded;
        int active;
    };
    const Row rows[] = {
        {"an extruder mapped to T1",
         two_extruders,
         {{"toolhead", {{"extruder", "extruder1"}}}},
         0,
         1},
        {"an extruder no tool owns",
         two_extruders,
         {{"toolhead", {{"extruder", "extruder_unknown"}}}},
         0,
         0},
        {"the implicit single tool",
         json::array({"extruder", "heater_bed", "fan", "gcode_move"}),
         {{"toolhead", {{"extruder", "extruder"}}}},
         0,
         0},
        // klipper-toolchanger's tool_number is the mount authority: toolhead.extruder
        // keeps naming the last extruder used after the tool is parked.
        {"a changer's tool_number over toolhead.extruder",
         changer,
         {{"toolchanger", {{"tool_number", 0}}}, {"toolhead", {{"extruder", "extruder1"}}}},
         -1,
         0},
        {"a changer ignores toolhead.extruder alone",
         changer,
         {{"toolhead", {{"extruder", "extruder1"}}}},
         -1,
         -1},
    };
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    for (const auto& row : rows) {
        INFO(row.name);
        ts.deinit_subjects();
        ts.init_subjects(false);
        helix::PrinterDiscovery hw;
        hw.parse_objects(row.objects);
        ts.init_tools(hw);
        REQUIRE(ts.active_tool_index() == row.seeded);

        ts.update_from_status(row.status);
        CHECK(ts.active_tool_index() == row.active);
        CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == row.active);
    }
    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: an empty klipper-toolchanger carriage mounts no tool (#2058)",
                 "[tool][tool-state][active-extruder][empty-carriage]") {
    using nlohmann::json;
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1",
                                  "heater_bed", "gcode_move"}));
    ts.init_tools(hw);

    // Nothing is known to be mounted until the first frame says so.
    REQUIRE(ts.active_tool_index() == -1);
    CHECK(ts.active_tool() == nullptr);
    CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == -1);
    for (const auto& tool : ts.tools()) {
        CHECK_FALSE(tool.active);
    }

    // Klipper names "extruder" with nothing on the carriage.
    ts.update_from_status(
        {{"toolchanger", {{"tool_number", -1}}}, {"toolhead", {{"extruder", "extruder"}}}});
    CHECK(ts.active_tool_index() == -1);
    CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == -1);

    ts.update_from_status(
        {{"toolchanger", {{"tool_number", 1}}}, {"toolhead", {{"extruder", "extruder1"}}}});
    CHECK(ts.active_tool_index() == 1);
    CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == 1);

    // A delta carrying only toolhead.extruder does not remount a tool.
    ts.update_from_status({{"toolhead", {{"extruder", "extruder"}}}});
    CHECK(ts.active_tool_index() == 1);

    ts.update_from_status(
        {{"toolchanger", {{"tool_number", -1}}}, {"toolhead", {{"extruder", "extruder"}}}});
    CHECK(ts.active_tool_index() == -1);
    CHECK(ts.active_tool() == nullptr);

    // A backend handing the carriage back leaves it empty: the changer reports
    // an empty carriage itself.
    ts.set_backend_active_tool(-1);
    ts.clear_backend_active_tool();
    CHECK(ts.active_tool_index() == -1);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: without klipper-toolchanger the head is never empty",
                 "[tool][tool-state][active-extruder][empty-carriage]") {
    using nlohmann::json;
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "extruder1", "heater_bed", "gcode_move"}));
    ts.init_tools(hw);
    REQUIRE(ts.active_tool_index() == 0);
    CHECK(ts.tools()[0].active);

    ts.update_from_status({{"toolhead", {{"extruder", "extruder1"}}}});
    CHECK(ts.active_tool_index() == 1);
    ts.update_from_status({{"toolhead", {{"extruder", "extruder"}}}});
    CHECK(ts.active_tool_index() == 0);

    ts.set_backend_active_tool(-1);
    ts.clear_backend_active_tool();
    CHECK(ts.active_tool_index() == 0);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: without a toolchanger each extruder is a tool, T0 taking the fan",
                 "[tool][tool-state][multi-extruder]") {
    const std::vector<std::vector<std::string>> extruder_sets = {
        {"extruder"}, {"extruder", "extruder1"}, {"extruder", "extruder1", "extruder2"}};
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    for (const auto& extruders : extruder_sets) {
        INFO(extruders.size() << " extruder(s)");
        ts.deinit_subjects();
        ts.init_subjects(false);
        nlohmann::json objects = nlohmann::json::array({"heater_bed", "fan", "gcode_move"});
        for (const auto& e : extruders)
            objects.push_back(e);
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        ts.init_tools(hw);

        const int n = static_cast<int>(extruders.size());
        REQUIRE(ts.tool_count() == n);
        CHECK(ts.is_multi_tool() == (n > 1));
        CHECK(lv_subject_get_int(ts.get_tool_count_subject()) == n);
        for (int i = 0; i < n; ++i) {
            const auto& tool = ts.tools()[static_cast<size_t>(i)];
            INFO("T" << i);
            CHECK(tool.name == "T" + std::to_string(i));
            CHECK(tool.extruder_name.value() == extruders[static_cast<size_t>(i)]);
            CHECK(tool.active == (i == 0));
            CHECK(tool.fan_name.has_value() == (i == 0));
        }
        CHECK(ts.tools()[0].fan_name.value() == "fan");
    }
    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: multi-extruder tracks active via toolhead.extruder",
                 "[tool][tool-state][multi-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.active_tool_index() == 0);

    // Klipper reports active extruder changed
    nlohmann::json status = {{"toolhead", {{"extruder", "extruder1"}}}};
    ts.update_from_status(status);

    REQUIRE(ts.active_tool_index() == 1);
    REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 1);
    REQUIRE(ts.nozzle_label() == "Nozzle 2");

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: request_tool_change answers without the printer when it can",
                 "[tool][tool-state][tool-change]") {
    using nlohmann::json;
    const json two_extruders = json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    const json changer = json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    struct Row {
        const char* name;
        json objects;
        int tool;
        const char* error; // substring of the error; nullptr = success
    };
    const Row rows[] = {
        {"a tool past the end", two_extruders, 5, "Invalid"},
        {"a negative tool", changer, -1, "Invalid"},
        {"the tool already active", two_extruders, 0, nullptr},
        {"another tool with no API", two_extruders, 1, "No API"},
    };
    lv_init_safe();
    ToolState& ts = ToolState::instance();
    for (const auto& row : rows) {
        INFO(row.name);
        ts.deinit_subjects();
        ts.init_subjects(false);
        helix::PrinterDiscovery hw;
        hw.parse_objects(row.objects);
        ts.init_tools(hw);

        bool succeeded = false;
        std::optional<std::string> error;
        ts.request_tool_change(
            row.tool, nullptr, [&] { succeeded = true; },
            [&](const std::string& msg) { error = msg; });
        CHECK(succeeded == (row.error == nullptr));
        if (row.error) {
            REQUIRE(error.has_value());
            CHECK(error->find(row.error) != std::string::npos);
        } else {
            CHECK_FALSE(error.has_value());
        }
    }
    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: request_tool_change delegates to AMS backend when it manages the tool",
                 "[tool][tool-state][tool-change][toolchanger]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "extruder",
                               "extruder1", "extruder2", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 3);

    // Set up mock AMS backend with tool-to-slot map (3 tools → 3 slots)
    auto mock = std::make_unique<AmsBackendMock>(3);
    mock->set_tool_changer_mode(true);
    mock->set_operation_delay(0);
    REQUIRE(mock->start());
    auto* mock_ptr = mock.get();
    AmsState::instance().deinit_subjects();
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(std::move(mock));
    REQUIRE(AmsState::instance().get_backend() != nullptr);

    SECTION("tool change to T1 goes through AMS backend") {
        // Verify T1 is not currently active
        REQUIRE(ts.active_tool_index() != 1);

        bool success_called = false;
        // Pass nullptr for API — backend handles tool change without needing MoonrakerAPI.
        // If it fell through to gcode path, we'd get "No API connection" error.
        ts.request_tool_change(
            1, nullptr, [&]() { success_called = true; },
            [](const std::string& err) { FAIL("Unexpected error: " << err); });

        CHECK(success_called);

        // change_tool() runs on a real std::thread, so the action sampled right
        // here is a race against it — and at set_operation_delay(0) that thread
        // usually finishes first, leaving IDLE. Sampling the transient made this
        // pass alone and fail under parallel load. Join, then assert the RESULT,
        // which is both deterministic and a stronger claim: it proves the backend
        // performed the change rather than merely starting something.
        mock_ptr->wait_for_operation_thread();

        const AmsSystemInfo info = mock_ptr->get_system_info();
        CHECK(info.action == AmsAction::IDLE);
        CHECK(info.current_slot == 1);
        CHECK(info.filament_loaded);
    }

    SECTION("tool change to T2 via backend works") {
        bool success_called = false;
        ts.request_tool_change(
            2, nullptr, [&]() { success_called = true; },
            [](const std::string& err) { FAIL("Unexpected error: " << err); });

        CHECK(success_called);
    }

    mock_ptr->stop();
    AmsState::instance().deinit_subjects();
    ts.deinit_subjects();
}

TEST_CASE_METHOD(
    ToolStateFixture,
    "ToolState: request_tool_change falls back to gcode when AMS backend has no tool map",
    "[tool][tool-state][tool-change][toolchanger]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 2);

    // Set up AMS backend with 0 slots (loaded but no hardware configured)
    auto mock = std::make_unique<AmsBackendMock>(0);
    mock->set_operation_delay(0);
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(std::move(mock));

    // With 0 slots, backend_manages_tool is false → falls through to gcode path.
    // Since we pass nullptr as API, the gcode path should hit the null-API check
    // and call on_error. This proves the backend was skipped.
    bool error_called = false;
    ts.request_tool_change(1, nullptr, nullptr, [&](const std::string& msg) {
        error_called = true;
        CHECK(msg.find("No API") != std::string::npos);
    });

    CHECK(error_called);

    AmsState::instance().deinit_subjects();
    ts.deinit_subjects();
}

TEST_CASE_METHOD(
    ToolStateFixture,
    "ToolState: request_tool_change for toolchanger with no AMS backend uses gcode fallback",
    "[tool][tool-state][tool-change][toolchanger]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // Clear any AMS backend from previous tests
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(nullptr);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 2);

    // No AMS backend → gcode path. Pass nullptr API to verify we reach the gcode
    // path (which will error on null API).
    bool error_called = false;
    ts.request_tool_change(1, nullptr, nullptr, [&](const std::string& msg) {
        error_called = true;
        CHECK(msg.find("No API") != std::string::npos);
    });

    CHECK(error_called);

    AmsState::instance().deinit_subjects();
    ts.deinit_subjects();
}

// ============================================================================
// Per-tool offsets: X/Y/Z bookkeeping
// ============================================================================

namespace {

/// A stock klipper-toolchanger with @p tool_count tools, plus the extra objects.
helix::PrinterDiscovery toolchanger_hw(int tool_count = 2,
                                       std::initializer_list<const char*> extra = {}) {
    nlohmann::json objects = nlohmann::json::array({"toolchanger", "gcode_move"});
    for (int i = 0; i < tool_count; ++i) {
        objects.push_back("tool T" + std::to_string(i));
        objects.push_back(i == 0 ? "extruder" : "extruder" + std::to_string(i));
    }
    for (const char* o : extra) {
        objects.push_back(o);
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

ToolState& fresh_tool_state(const helix::PrinterDiscovery& hw) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.init_tools(hw);
    return ts;
}

int subj(lv_subject_t* s) {
    return lv_subject_get_int(s);
}

} // namespace

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: a toolchanger supports all three axes",
                 "[tool][tool-state][tool-offsets]") {
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw());

    for (Axis axis : kAllAxes) {
        CAPTURE(axis_letter(axis));
        CHECK(subj(ts.get_per_tool_axis_supported_subject(axis)) == 1);
        // Nothing reported yet: not known, not dirty, reads as 0.
        CHECK_FALSE(ts.tool_offset_known(0, axis));
        CHECK(ts.tool_offset_mm(0, axis) == 0.0f);
    }
    CHECK(subj(ts.get_any_tool_offset_dirty_subject()) == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: a frame seeds every axis as persisted",
                 "[tool][tool-state][tool-offsets]") {
    // The first value seen is the config value, so a fresh connect must not
    // claim unsaved work - on any axis.
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw());

    ts.update_from_status(
        {{"tool T0",
          {{"gcode_x_offset", 1.5}, {"gcode_y_offset", -0.25}, {"gcode_z_offset", 0.1}}}});

    CHECK(ts.tool_offset_known(0, Axis::X));
    CHECK(ts.tool_offset_mm(0, Axis::X) == Catch::Approx(1.5f));
    CHECK(ts.tool_offset_mm(0, Axis::Y) == Catch::Approx(-0.25f));
    CHECK(ts.tool_offset_mm(0, Axis::Z) == Catch::Approx(0.1f));
    // Seeded, not dirty.
    CHECK(ts.dirty_tool_indices().empty());
    CHECK(subj(ts.get_any_tool_offset_dirty_subject()) == 0);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: a local X change dirties X and only X",
                 "[tool][tool-state][tool-offsets]") {
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw());
    ts.update_from_status(
        {{"tool T1", {{"gcode_x_offset", 0.0}, {"gcode_y_offset", 0.0}, {"gcode_z_offset", 0.0}}}});

    ts.set_tool_offset_local(1, Axis::X, 500);

    CHECK(ts.tool_offset_mm(1, Axis::X) == Catch::Approx(0.5f));
    CHECK(ts.dirty_tool_indices() == std::vector<int>{1});
    // Only X: the save affordance must not offer to flush axes that never
    // moved.
    CHECK(ts.tool_offset_dirty(1, Axis::X));
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::Y));
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::Z));
    CHECK(subj(ts.get_any_tool_offset_dirty_subject()) == 1);

    // Saving the tool clears every axis of it.
    ts.mark_tool_offsets_saved(1);
    CHECK(ts.dirty_tool_indices().empty());
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::X));
    CHECK(subj(ts.get_any_tool_offset_dirty_subject()) == 0);
}

TEST_CASE_METHOD(
    ToolStateFixture,
    "ToolState offsets: a tool with an unreported axis reads unknown on that axis only",
    "[tool][tool-state][tool-offsets]") {
    // Known is per axis: switching to a tool whose X was never reported must
    // read unknown on X while its Z, which was, stays known - otherwise the
    // previous tool's X would sit on screen beside the new tool's Z.
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw());
    ts.update_from_status({{"tool T0", {{"gcode_x_offset", 1.5}, {"gcode_z_offset", 0.1}}},
                           {"tool T1", {{"gcode_z_offset", -0.2}}}});
    REQUIRE(ts.tool_offset_known(0, Axis::X));

    ts.update_from_status({{"toolchanger", {{"tool_number", 1}}}});

    REQUIRE(ts.active_tool_index() == 1);
    CHECK_FALSE(ts.tool_offset_known(1, Axis::X));
    CHECK(ts.tool_offset_mm(1, Axis::X) == 0.0f);
    CHECK(ts.tool_offset_known(1, Axis::Z));
    CHECK(ts.tool_offset_mm(1, Axis::Z) == Catch::Approx(-0.2f));
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: a Z-only delta frame leaves X alone",
                 "[tool][tool-state][tool-offsets]") {
    // Moonraker republishes per field. A frame that only carries Z must not
    // reset X's value, its known flag, or its saved baseline.
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw());
    ts.update_from_status({{"tool T0", {{"gcode_x_offset", 1.5}, {"gcode_z_offset", 0.1}}}});
    ts.set_tool_offset_local(0, Axis::X, 1750);
    REQUIRE(ts.tool_offset_dirty(0, Axis::X));

    ts.update_from_status({{"tool T0", {{"gcode_z_offset", 0.15}}}});

    CHECK(ts.tool_offset_mm(0, Axis::X) == Catch::Approx(1.75f));
    CHECK(ts.tool_offset_dirty(0, Axis::X));
    CHECK(ts.tool_offset_mm(0, Axis::Z) == Catch::Approx(0.15f));
    // Z was seeded at 0.1 and moved by the printer, not by us, so it is dirty
    // too - a runtime change is a runtime change whoever made it.
    CHECK(ts.tool_offset_dirty(0, Axis::Z));
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: the TOOL_OFFSET macro owns every axis",
                 "[tool][tool-state][tool-offsets]") {
    // The MedusaHC shape: klipper-toolchanger objects plus the TOOL_OFFSET
    // macro, whose t{n}_off_{x,y,z} variables are what the controller applies.
    // The tool object still carries gcode_x_offset, and it must be ignored:
    // that store is not the authority on this machine.
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw(2, {"gcode_macro TOOL_OFFSET"}));

    CHECK(subj(ts.get_per_tool_axis_supported_subject(Axis::X)) == 1);
    CHECK(subj(ts.get_per_tool_axis_supported_subject(Axis::Y)) == 1);
    CHECK(subj(ts.get_per_tool_axis_supported_subject(Axis::Z)) == 1);

    ts.update_from_status(
        {{"gcode_macro TOOL_OFFSET", {{"t0_off_x", 0.4}, {"t0_off_y", -0.3}, {"t0_off_z", -0.2}}},
         {"tool T0", {{"gcode_x_offset", 1.5}, {"gcode_z_offset", 0.1}}}});

    CHECK(ts.tool_offset_mm(0, Axis::X) == Catch::Approx(0.4f));
    CHECK(ts.tool_offset_mm(0, Axis::Y) == Catch::Approx(-0.3f));
    CHECK(ts.tool_offset_mm(0, Axis::Z) == Catch::Approx(-0.2f));
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState offsets: a topology rebuild carries every axis",
                 "[tool][tool-state][tool-offsets]") {
    // set_ams_topology() rebuilds tools_ from the backend's view; the offsets
    // and their bookkeeping are facts about the toolhead and must survive it.
    lv_init_safe();
    ToolState& ts = fresh_tool_state(toolchanger_hw(4));
    ts.update_from_status(
        {{"tool T1",
          {{"gcode_x_offset", 1.5}, {"gcode_y_offset", -0.25}, {"gcode_z_offset", 0.1}}}});
    ts.set_tool_offset_local(1, Axis::Y, -500);

    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    ts.set_ams_topology(topo);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(ts.tools().size() == 4);
    CHECK(ts.tool_offset_mm(1, Axis::X) == Catch::Approx(1.5f));
    CHECK(ts.tool_offset_mm(1, Axis::Y) == Catch::Approx(-0.5f));
    CHECK(ts.tool_offset_dirty(1, Axis::Y));
    CHECK_FALSE(ts.tool_offset_dirty(1, Axis::X));
    CHECK(ts.tool_offset_known(1, Axis::Z));
}
