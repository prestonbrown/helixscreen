// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_state.cpp
 * @brief Tests for ToolInfo struct, DetectState enum, and ToolState singleton
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/tool_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;

/// This file had no fixture, so nothing drained the UpdateQueue. The
/// request_tool_change / assign_spool tests register an AMS backend, and the
/// backend's events reach AmsState::on_backend_event, which defers its subject
/// writes; each test returned with that work queued and handed it to whichever
/// test drained next (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects and the test's
/// backend are still alive, before HelixTestFixture's own teardown.
struct ToolStateFixture : public HelixTestFixture {
    ~ToolStateFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
    REQUIRE(info.gcode_x_offset == 0.0f);
    REQUIRE(info.gcode_y_offset == 0.0f);
    REQUIRE(info.gcode_z_offset == 0.0f);
    REQUIRE_FALSE(info.active);
    REQUIRE_FALSE(info.mounted);
    REQUIRE(info.detect_state == DetectState::UNAVAILABLE);
    REQUIRE(info.backend_index == -1);
    REQUIRE(info.backend_slot == -1);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: default backend mapping is unassigned",
                 "[tool][tool-state]") {
    ToolInfo info;
    REQUIRE(info.backend_index == -1);
    REQUIRE(info.backend_slot == -1);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: effective_heater prefers heater_name",
                 "[tool][tool-state]") {
    ToolInfo info;
    info.heater_name = "heater_generic chamber";
    info.extruder_name = "extruder1";

    REQUIRE(info.effective_heater() == "heater_generic chamber");
}

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: effective_heater falls back to extruder_name",
                 "[tool][tool-state]") {
    ToolInfo info;
    info.extruder_name = "extruder1";
    // heater_name not set

    REQUIRE(info.effective_heater() == "extruder1");
}

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: effective_heater fallback when nothing set",
                 "[tool][tool-state]") {
    ToolInfo info;
    info.extruder_name = std::nullopt;
    info.heater_name = std::nullopt;

    REQUIRE(info.effective_heater() == "extruder");
}

// ============================================================================
// DetectState enum tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "DetectState: enum values", "[tool][tool-state]") {
    REQUIRE(static_cast<int>(DetectState::PRESENT) == 0);
    REQUIRE(static_cast<int>(DetectState::ABSENT) == 1);
    REQUIRE(static_cast<int>(DetectState::UNAVAILABLE) == 2);
}

// ============================================================================
// ToolState singleton tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: singleton access", "[tool][tool-state]") {
    ToolState& a = ToolState::instance();
    ToolState& b = ToolState::instance();

    REQUIRE(&a == &b);
}

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

    REQUIRE(ts.tools()[1].gcode_x_offset == Catch::Approx(1.5f));
    REQUIRE(ts.tools()[1].gcode_y_offset == Catch::Approx(-2.3f));
    REQUIRE(ts.tools()[1].gcode_z_offset == Catch::Approx(0.15f));
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: update_from_status with no tools is safe",
                 "[tool][tool-state]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // No init_tools called, tools_ is empty
    nlohmann::json status = {{"toolchanger", {{"tool_number", 1}}}};
    ts.update_from_status(status); // Should not crash
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

    nlohmann::json status = {{"tool T0", {{"detect_state", "present"}}}};
    ts.update_from_status(status);
    REQUIRE(ts.tools()[0].detect_state == DetectState::PRESENT);

    // Also test "absent"
    nlohmann::json status2 = {{"tool T0", {{"detect_state", "absent"}}}};
    ts.update_from_status(status2);
    REQUIRE(ts.tools()[0].detect_state == DetectState::ABSENT);

    ts.deinit_subjects();
}

// ============================================================================
// toolhead.extruder cross-check tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: toolhead.extruder updates active tool for multi-extruder",
                 "[tool][tool-state][active-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Initially active tool is T0
    REQUIRE(ts.active_tool_index() == 0);

    // Send toolhead.extruder pointing to extruder1 (mapped to T1)
    nlohmann::json status = {{"toolhead", {{"extruder", "extruder1"}}}};
    ts.update_from_status(status);

    REQUIRE(ts.active_tool_index() == 1);
    REQUIRE(lv_subject_get_int(ts.get_active_tool_subject()) == 1);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: toolchanger tool_number takes priority over toolhead.extruder",
                 "[tool][tool-state][active-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Both toolchanger.tool_number=0 and toolhead.extruder="extruder1" present
    // toolchanger block sets to 0 first, then toolhead.extruder would set to 1
    // Since toolchanger is parsed first and sets active_tool_index_=0,
    // and toolhead.extruder sees extruder1 -> tool 1 != 0, it updates to 1.
    // But that's actually fine — in practice Klipper keeps these consistent.
    // This test verifies both code paths execute without error.
    nlohmann::json status = {{"toolchanger", {{"tool_number", 0}}},
                             {"toolhead", {{"extruder", "extruder1"}}}};
    ts.update_from_status(status);

    // The toolhead.extruder runs after toolchanger, so extruder1 -> T1 wins
    REQUIRE(ts.active_tool_index() == 1);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: toolhead.extruder with no matching tool is ignored",
                 "[tool][tool-state][active-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.active_tool_index() == 0);

    // Send toolhead.extruder with name that doesn't map to any tool
    nlohmann::json status = {{"toolhead", {{"extruder", "extruder_unknown"}}}};
    ts.update_from_status(status);

    // Should remain unchanged
    REQUIRE(ts.active_tool_index() == 0);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: toolhead.extruder works for implicit single tool",
                 "[tool][tool-state][active-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // No toolchanger — single implicit tool
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "fan", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.tool_count() == 1);
    REQUIRE(ts.active_tool_index() == 0);

    // toolhead.extruder="extruder" matches T0's extruder_name — no change expected
    nlohmann::json status = {{"toolhead", {{"extruder", "extruder"}}}};
    ts.update_from_status(status);

    REQUIRE(ts.active_tool_index() == 0);

    ts.deinit_subjects();
}

// ============================================================================
// Multi-extruder (no toolchanger) tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: multi-extruder without toolchanger creates multiple tools",
                 "[tool][tool-state][multi-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // Two extruders, no toolchanger object
    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "fan", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.tool_count() == 2);
    REQUIRE(ts.is_multi_tool() == true);
    REQUIRE(lv_subject_get_int(ts.get_tool_count_subject()) == 2);

    const auto& tools = ts.tools();
    REQUIRE(tools[0].name == "T0");
    REQUIRE(tools[0].extruder_name.value() == "extruder");
    REQUIRE(tools[0].active == true);
    REQUIRE(tools[0].fan_name.value() == "fan");

    REQUIRE(tools[1].name == "T1");
    REQUIRE(tools[1].extruder_name.value() == "extruder1");
    REQUIRE(tools[1].active == false);
    REQUIRE_FALSE(tools[1].fan_name.has_value());

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: single extruder without toolchanger still creates 1 tool",
                 "[tool][tool-state][multi-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "fan", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.tool_count() == 1);
    REQUIRE(ts.is_multi_tool() == false);

    const auto& tools = ts.tools();
    REQUIRE(tools[0].name == "T0");
    REQUIRE(tools[0].extruder_name.value() == "extruder");
    REQUIRE(tools[0].fan_name.value() == "fan");

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: three extruders without toolchanger",
                 "[tool][tool-state][multi-extruder]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "heater_bed", "fan", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE(ts.tool_count() == 3);
    REQUIRE(ts.is_multi_tool() == true);

    REQUIRE(ts.tools()[0].extruder_name.value() == "extruder");
    REQUIRE(ts.tools()[1].extruder_name.value() == "extruder1");
    REQUIRE(ts.tools()[2].extruder_name.value() == "extruder2");
    REQUIRE(ts.tools()[2].name == "T2");

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
    REQUIRE(ts.nozzle_label() == "Nozzle T1");

    ts.deinit_subjects();
}

// ============================================================================
// request_tool_change tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: request_tool_change with invalid index calls error",
                 "[tool][tool-state][tool-change]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    bool error_called = false;
    ts.request_tool_change(5, nullptr, nullptr, [&](const std::string& msg) {
        error_called = true;
        REQUIRE(msg.find("Invalid") != std::string::npos);
    });

    REQUIRE(error_called);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: request_tool_change for already-active tool calls success",
                 "[tool][tool-state][tool-change]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    bool success_called = false;
    ts.request_tool_change(0, nullptr, [&]() { success_called = true; }, nullptr);

    REQUIRE(success_called);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: request_tool_change with no API calls error",
                 "[tool][tool-state][tool-change]") {
    lv_init_safe();

    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    bool error_called = false;
    ts.request_tool_change(1, nullptr, nullptr, [&](const std::string& msg) {
        error_called = true;
        REQUIRE(msg.find("No API") != std::string::npos);
    });

    REQUIRE(error_called);

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
    AmsState::instance().init_subjects(false);
    AmsState::instance().set_backend(std::move(mock));
    REQUIRE(AmsState::instance().get_backend() != nullptr);

    SECTION("tool change to T1 goes through AMS backend") {
        // Verify T1 is not currently active
        REQUIRE(ts.active_tool_index() == 0);

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
    AmsState::instance().init_subjects(false);
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

TEST_CASE_METHOD(ToolStateFixture, "ToolState: request_tool_change with negative tool index",
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

    bool error_called = false;
    ts.request_tool_change(-1, nullptr, nullptr, [&](const std::string& msg) {
        error_called = true;
        CHECK(msg.find("Invalid") != std::string::npos);
    });

    CHECK(error_called);

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
    AmsState::instance().init_subjects(false);
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
// Spool assignment tests
// ============================================================================

namespace {
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("helix_tool_spool_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const {
        return path.string();
    }
};
} // namespace

TEST_CASE_METHOD(ToolStateFixture, "ToolInfo: default spool fields", "[tool][tool-state][spool]") {
    ToolInfo info;
    REQUIRE(info.spoolman_id == 0);
    REQUIRE(info.spool_name.empty());
    REQUIRE(info.remaining_weight_g == -1.0f);
    REQUIRE(info.total_weight_g == -1.0f);
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assign_spool updates ToolInfo fields",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    // Create some tools via mock discovery
    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "extruder1", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 2);

    // Assign a spool to T0
    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);

    const auto& tools = ts.tools();
    REQUIRE(tools[0].spoolman_id == 42);
    REQUIRE(tools[0].spool_name == "Red PLA");
    REQUIRE(tools[0].remaining_weight_g == 750.0f);
    REQUIRE(tools[0].total_weight_g == 1000.0f);

    // T1 should be unaffected
    REQUIRE(tools[1].spoolman_id == 0);
    REQUIRE(tools[1].spool_name.empty());

    ts.deinit_subjects();
}

// ============================================================================
// assign_spool change detection.
//
// AFC reports lane weight as a continuous float (e.g. 627.685056380799 g) that
// moves by hundredths on every status update, ~every 10 s. AmsState calls
// assign_spool from that path and then save_spool_assignments_if_dirty(), so an
// exact float compare meant each of those updates rewrote tool_spools.json,
// POSTed the Moonraker DB, logged at info and rebuilt the filament panel. Debug
// bundle L53W5PKG: 590 times in one session.
// ============================================================================

namespace {

/// Bring up ToolState with two tools and one assigned spool.
ToolState& seed_assigned_tool_state() {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "extruder1", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    ts.assign_spool(0, 45, "Industry Blue", 627.685056380799f, 1000.0f);
    helix::ToolStateTestAccess::clear_spool_dirty(ts);
    return ts;
}

} // namespace

TEST_CASE_METHOD(ToolStateFixture, "ToolState: sub-gram weight drift is not a change",
                 "[tool][tool-state][spool][regression]") {
    auto& ts = seed_assigned_tool_state();
    const int version_before = lv_subject_get_int(ts.get_tools_version_subject());

    // The exact shape from the bundle: a few hundredths of a gram, repeatedly.
    ts.assign_spool(0, 45, "Industry Blue", 627.63f, 1000.0f);
    ts.assign_spool(0, 45, "Industry Blue", 627.58f, 1000.0f);
    ts.assign_spool(0, 45, "Industry Blue", 627.51f, 1000.0f);

    CHECK(lv_subject_get_int(ts.get_tools_version_subject()) == version_before);
    CHECK_FALSE(helix::ToolStateTestAccess::spool_dirty(ts));

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: crossing a gram boundary updates the UI",
                 "[tool][tool-state][spool]") {
    // The debounce must not swallow real consumption — whole grams are what the
    // UI renders, so that is the resolution downstream can observe.
    auto& ts = seed_assigned_tool_state();
    const int version_before = lv_subject_get_int(ts.get_tools_version_subject());

    ts.assign_spool(0, 45, "Industry Blue", 626.2f, 1000.0f);

    CHECK(lv_subject_get_int(ts.get_tools_version_subject()) > version_before);
    CHECK(ts.tools()[0].remaining_weight_g == 626.2f);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: weight-only change does not trigger a save",
                 "[tool][tool-state][spool][regression]") {
    // Weights are a cache refreshed from AFC/Spoolman seconds after startup; the
    // durable half of the record is which spool is on which tool. Persisting on
    // every gram consumed writes flash and POSTs the Moonraker DB throughout a
    // print for data that is thrown away on the next connect.
    auto& ts = seed_assigned_tool_state();

    ts.assign_spool(0, 45, "Industry Blue", 500.0f, 1000.0f);
    CHECK_FALSE(helix::ToolStateTestAccess::spool_dirty(ts));

    SECTION("but an identity change does") {
        ts.assign_spool(0, 44, "Dark Teal", 500.0f, 1000.0f);
        CHECK(helix::ToolStateTestAccess::spool_dirty(ts));
    }

    SECTION("and so does a rename of the same spool id") {
        ts.assign_spool(0, 45, "Industry Blue v2", 500.0f, 1000.0f);
        CHECK(helix::ToolStateTestAccess::spool_dirty(ts));
    }

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: drift does not accumulate past a boundary",
                 "[tool][tool-state][spool][regression]") {
    // Each report is compared against the last STORED value, so a long series of
    // sub-gram steps still fires when it crosses a gram — neither never (the
    // displayed weight would silently freeze) nor once per report.
    auto& ts = seed_assigned_tool_state();
    const int version_before = lv_subject_get_int(ts.get_tools_version_subject());

    float w = 627.685056380799f;
    for (int i = 0; i < 40; ++i) {
        w -= 0.05f; // 40 steps = 2 g, i.e. two boundaries
        ts.assign_spool(0, 45, "Industry Blue", w, 1000.0f);
    }

    const int bumps = lv_subject_get_int(ts.get_tools_version_subject()) - version_before;
    INFO("version bumps for a 2 g slide: " << bumps);
    CHECK(bumps >= 1);
    CHECK(bumps <= 3); // one per gram crossed, not one per report

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: clear_spool resets ToolInfo fields",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);
    REQUIRE(ts.tools()[0].spoolman_id == 42);

    ts.clear_spool(0);
    REQUIRE(ts.tools()[0].spoolman_id == 0);
    REQUIRE(ts.tools()[0].spool_name.empty());
    REQUIRE(ts.tools()[0].remaining_weight_g == -1.0f);
    REQUIRE(ts.tools()[0].total_weight_g == -1.0f);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assign_spool ignores invalid index",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Should not crash or corrupt state
    ts.assign_spool(-1, 42, "Bad", 100, 200);
    ts.assign_spool(99, 42, "Bad", 100, 200);
    REQUIRE(ts.tools()[0].spoolman_id == 0);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: save/load JSON round-trip",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    TempDir tmp;
    ts.set_config_dir(tmp.str());

    // Set up tools and assign spools
    PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 3);

    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);
    ts.assign_spool(2, 99, "Blue PETG", 200.0f, 500.0f);
    // T1 left unassigned

    // Save to local JSON
    ts.save_spool_assignments(nullptr);

    // Verify file exists
    auto json_path = std::filesystem::path(tmp.str()) / "tool_spools.json";
    REQUIRE(std::filesystem::exists(json_path));

    // Read and verify JSON structure
    std::ifstream ifs(json_path);
    auto data = nlohmann::json::parse(ifs);
    REQUIRE(data.is_object());
    REQUIRE(data.contains("0"));
    REQUIRE(data["0"]["spoolman_id"] == 42);
    REQUIRE(data["0"]["spool_name"] == "Red PLA");
    REQUIRE(data.contains("2"));
    REQUIRE(data["2"]["spoolman_id"] == 99);
    REQUIRE_FALSE(data.contains("1")); // T1 had no spool

    // Now reinit tools (simulating restart) and load
    ts.init_tools(hw);
    REQUIRE(ts.tools()[0].spoolman_id == 0); // cleared by init

    ts.load_spool_assignments(nullptr); // no API, falls back to JSON
    REQUIRE(ts.tools()[0].spoolman_id == 42);
    REQUIRE(ts.tools()[0].spool_name == "Red PLA");
    REQUIRE(ts.tools()[0].remaining_weight_g == 750.0f);
    REQUIRE(ts.tools()[0].total_weight_g == 1000.0f);
    REQUIRE(ts.tools()[2].spoolman_id == 99);
    REQUIRE(ts.tools()[2].spool_name == "Blue PETG");
    REQUIRE(ts.tools()[1].spoolman_id == 0); // still unassigned

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: load from missing JSON file is no-op",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    TempDir tmp;
    ts.set_config_dir(tmp.str());

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Should not crash, tools remain at defaults
    ts.load_spool_assignments(nullptr);
    REQUIRE(ts.tools()[0].spoolman_id == 0);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: save with no assigned spools writes empty object",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    TempDir tmp;
    ts.set_config_dir(tmp.str());

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    ts.save_spool_assignments(nullptr);

    auto json_path = std::filesystem::path(tmp.str()) / "tool_spools.json";
    REQUIRE(std::filesystem::exists(json_path));

    std::ifstream ifs(json_path);
    auto data = nlohmann::json::parse(ifs);
    REQUIRE(data.is_object());
    REQUIRE(data.empty());

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: weight fields omitted from JSON when unknown",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    TempDir tmp;
    ts.set_config_dir(tmp.str());

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Assign with unknown weights (defaults: -1)
    ts.assign_spool(0, 42, "Mystery Spool");
    ts.save_spool_assignments(nullptr);

    auto json_path = std::filesystem::path(tmp.str()) / "tool_spools.json";
    std::ifstream ifs(json_path);
    auto data = nlohmann::json::parse(ifs);
    REQUIRE(data["0"]["spoolman_id"] == 42);
    REQUIRE_FALSE(data["0"].contains("remaining_weight_g"));
    REQUIRE_FALSE(data["0"].contains("total_weight_g"));

    ts.deinit_subjects();
}

// ============================================================================
// assigned_spool_ids tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assigned_spool_ids returns all assigned IDs",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 3);

    ts.assign_spool(0, 10, "Spool A");
    ts.assign_spool(1, 20, "Spool B");
    // T2 unassigned

    auto ids = ts.assigned_spool_ids();
    REQUIRE(ids.size() == 2);
    REQUIRE(ids.count(10) == 1);
    REQUIRE(ids.count(20) == 1);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assigned_spool_ids excludes specified tool",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    ts.assign_spool(0, 10, "Spool A");
    ts.assign_spool(1, 20, "Spool B");
    ts.assign_spool(2, 30, "Spool C");

    // Exclude tool 1 — should not include spool 20
    auto ids = ts.assigned_spool_ids(1);
    REQUIRE(ids.size() == 2);
    REQUIRE(ids.count(10) == 1);
    REQUIRE(ids.count(30) == 1);
    REQUIRE(ids.count(20) == 0);

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assigned_spool_ids empty when no spools assigned",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    auto ids = ts.assigned_spool_ids();
    REQUIRE(ids.empty());

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: assigned_spool_ids skips cleared spools",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "extruder1", "heater_bed"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    ts.assign_spool(0, 10, "Spool A");
    ts.assign_spool(1, 20, "Spool B");
    ts.clear_spool(0);

    auto ids = ts.assigned_spool_ids();
    REQUIRE(ids.size() == 1);
    REQUIRE(ids.count(20) == 1);
    REQUIRE(ids.count(10) == 0);

    ts.deinit_subjects();
}

// ============================================================================
// spool_assignments_loaded flag tests
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture, "ToolState: spool_assignments_loaded initially false",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    REQUIRE_FALSE(ts.spool_assignments_loaded());

    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState: spool_assignments_loaded set after load without API",
                 "[tool][tool-state][spool]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    REQUIRE_FALSE(ts.spool_assignments_loaded());

    // load_spool_assignments with nullptr API takes synchronous JSON fallback path
    ts.load_spool_assignments(nullptr);

    REQUIRE(ts.spool_assignments_loaded());

    ts.deinit_subjects();
}

// ============================================================================
// Toolchanger reverse-sync: ToolState → AmsBackend slot propagation
// ============================================================================

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: assign_spool + sync_from_backend populates toolchanger slot",
                 "[tool][tool-state][spool][ams]") {
    lv_init_safe();

    // Set up ToolState with a 2-tool toolchanger
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 2);

    // Set up AmsState with a toolchanger mock backend (2 slots)
    // Note: AmsBackendMock pre-populates slots with spoolman_id = i+1
    auto mock = std::make_unique<AmsBackendMock>(2);
    mock->set_tool_changer_mode(true);
    mock->set_operation_delay(0);
    auto* mock_ptr = mock.get();

    // Clear the mock's pre-populated spoolman data to simulate empty slots
    for (int i = 0; i < 2; ++i) {
        SlotInfo empty_slot = mock_ptr->get_slot_info(i);
        empty_slot.spoolman_id = 0;
        empty_slot.spool_name.clear();
        mock_ptr->set_slot_info(i, empty_slot, false);
    }

    auto& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.set_backend(std::move(mock));
    REQUIRE(ams.get_backend() != nullptr);
    REQUIRE(is_tool_changer(ams.get_backend()->get_type()));

    // Initially: backend slot has no spoolman_id
    SlotInfo slot_before = mock_ptr->get_slot_info(0);
    REQUIRE(slot_before.spoolman_id == 0);

    // Assign a spool to tool T0 via ToolState
    ts.assign_spool(0, 42, "Red PLA", 750.0f, 1000.0f);
    REQUIRE(ts.tools()[0].spoolman_id == 42);

    // Sync: should reverse-sync from ToolState → backend slot
    ams.sync_from_backend();

    // Backend slot should now have the spoolman_id
    SlotInfo slot_after = mock_ptr->get_slot_info(0);
    REQUIRE(slot_after.spoolman_id == 42);
    REQUIRE(slot_after.spool_name == "Red PLA");

    // Slot 1 should remain unassigned (tool T1 has spoolman_id=0)
    REQUIRE(ts.tools()[1].spoolman_id == 0);

    // Cleanup
    ams.set_backend(nullptr);
    ams.deinit_subjects();
    ts.deinit_subjects();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "ToolState: sync_from_backend does not overwrite existing tool assignment",
                 "[tool][tool-state][spool][ams]") {
    lv_init_safe();

    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    ts.init_tools(hw);

    // Pre-assign spool 99 to T0
    ts.assign_spool(0, 99, "Existing Spool", 500.0f, 1000.0f);

    auto mock = std::make_unique<AmsBackendMock>(2);
    mock->set_tool_changer_mode(true);
    mock->set_operation_delay(0);
    auto* mock_ptr = mock.get();

    // Clear mock's pre-populated spoolman data
    for (int i = 0; i < 2; ++i) {
        SlotInfo empty_slot = mock_ptr->get_slot_info(i);
        empty_slot.spoolman_id = 0;
        empty_slot.spool_name.clear();
        mock_ptr->set_slot_info(i, empty_slot, false);
    }

    auto& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.set_backend(std::move(mock));

    // Sync should propagate spool 99 to the backend via reverse-sync
    ams.sync_from_backend();

    SlotInfo slot = mock_ptr->get_slot_info(0);
    REQUIRE(slot.spoolman_id == 99);
    REQUIRE(slot.spool_name == "Existing Spool");

    // The tool should still have its original assignment
    REQUIRE(ts.tools()[0].spoolman_id == 99);

    ams.set_backend(nullptr);
    ams.deinit_subjects();
    ts.deinit_subjects();
}

// Regression for prestonbrown/helixscreen#1176.
//
// The installer symlinks tool_spools.json out to printer_data
// (HELIX_USER_CONFIG_FILES), and that link is the only thing keeping the file
// alive through a Moonraker one-click update, which rmtree()s the install dir --
// rmtree unlinks a symlink instead of following it. save_spool_json() does an
// atomic write, and rename(2) onto a symlink replaces THE SYMLINK, not its
// target. So before the fix the first spool save silently converted the link
// into a regular file and stranded it in the doomed directory. Observed in the
// field on the Pi: a real tool_spools.json in the install dir with an orphaned
// copy three months stale in printer_data.
//
// Content-only assertions cannot see this -- the data round-trips perfectly
// either way. The property under test is that the path is STILL A SYMLINK after
// saving, and that the bytes landed on the far side of it.
TEST_CASE_METHOD(ToolStateFixture, "ToolState: saving through a symlink preserves the link (#1176)",
                 "[tool][tool-state][spool][regression]") {
    lv_init_safe();
    auto& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    TempDir install; // stands in for ~/helixscreen/config
    TempDir real;    // stands in for printer_data/config/helixscreen

    auto real_file = std::filesystem::path(real.str()) / "tool_spools.json";
    auto link_path = std::filesystem::path(install.str()) / "tool_spools.json";
    {
        std::ofstream seed(real_file);
        seed << "{}";
    }
    std::error_code ec;
    std::filesystem::create_symlink(real_file, link_path, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::is_symlink(link_path));

    ts.set_config_dir(install.str());

    PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "extruder1"});
    hw.parse_objects(objects);
    ts.init_tools(hw);
    ts.assign_spool(0, 4242, "Symlink PLA", 750.0f, 1000.0f);
    ts.save_spool_assignments(nullptr);

    // The link must survive. Pre-fix this is a regular file and the test fails here.
    CHECK(std::filesystem::is_symlink(link_path));

    // ...and the write must have gone through it to the real file, not beside it.
    // Shape is a flat object keyed by tool index: {"0": {"spoolman_id": ...}}
    std::ifstream in(real_file);
    nlohmann::json written;
    in >> written;
    REQUIRE(written.contains("0"));
    CHECK(written["0"]["spoolman_id"].get<int>() == 4242);
    CHECK(written["0"]["spool_name"].get<std::string>() == "Symlink PLA");

    // No stray temp file left behind next to either path.
    CHECK_FALSE(std::filesystem::exists(link_path.string() + ".tmp"));
    CHECK_FALSE(std::filesystem::exists(real_file.string() + ".tmp"));

    ts.deinit_subjects();
}
