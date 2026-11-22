// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lane_rig_head_commands.cpp
 * @brief On a rig whose tool numbers name lanes, every command meant for a
 *        head reaches that head: the reporter's Voron 2.4 (bundle LB6LEZ44),
 *        where lane T0 prints through extruder2, whose printer tool is T2.
 */

#include "ui_ams_device_section_detail_overlay.h"

#include "../busy_guard_fixture.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "printer_discovery.h"
#include "test_helpers/tool_topologies.h"
#include "tool_state.h"
#include "z_offset_calibration_approach.h"
#include "z_offset_utils.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

class LaneRigFixture : public BusyGuardFixture {
  public:
    LaneRigFixture() {
        state.set_hardware(test::make_lb6lez44_discovery());
        auto& ts = ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(true);
        ts.init_tools(state.get_discovery());
        ts.set_ams_topology(test::make_lb6lez44_topology());
        ui::UpdateQueue::instance().drain();
        mock_client.clear_gcode_script_history();
    }
    ~LaneRigFixture() override {
        ToolState::instance().clear_ams_topology();
        ToolState::instance().deinit_subjects();
        ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& line) const {
        const auto& history = mock_client.gcode_script_history();
        return std::any_of(history.begin(), history.end(), [&line](const std::string& script) {
            return script.find(line) != std::string::npos;
        });
    }
};

/// A lane rig on a first connect: the printer's tool offsets arrive before the
/// AFC backend's first topology.
class LaneRigFirstConnectFixture : public BusyGuardFixture {
  public:
    LaneRigFirstConnectFixture() {
        state.set_hardware(test::make_lb6lez44_discovery());
        auto& ts = ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(true);
        ts.init_tools(state.get_discovery());
        ui::UpdateQueue::instance().drain();
        mock_client.clear_gcode_script_history();
    }
    ~LaneRigFirstConnectFixture() override {
        ToolState::instance().clear_ams_topology();
        ToolState::instance().deinit_subjects();
        ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LaneRigFirstConnectFixture,
                 "A head's offset seeded before the topology arrives is still readable after it",
                 "[head-identity][tool_offsets][zoffset]") {
    auto& ts = ToolState::instance();
    REQUIRE_FALSE(ts.tools_are_lanes());

    ts.update_from_status(json{{"tool T0", {{"gcode_z_offset", -0.2}, {"extruder", "extruder"}}},
                               {"tool T2", {{"gcode_z_offset", 0.15}, {"extruder", "extruder2"}}}});
    ts.set_ams_topology(test::make_lb6lez44_topology());
    ui::UpdateQueue::instance().drain();
    REQUIRE(ts.tools_are_lanes());
    REQUIRE(ts.active_tool_index() == 0);

    const int head = ts.offset_tool_for(0);
    REQUIRE(head == 2);
    CHECK(ts.tool_offset_known(head, Axis::Z));
    CHECK(ts.tool_offset_mm(head, Axis::Z) == Catch::Approx(0.15));

    mock_client.clear_gcode_script_history();
    REQUIRE(zoffset::adjust_tool_offset(api.get(), &state, 0, 0.05));
    const auto& history = mock_client.gcode_script_history();
    CHECK(std::any_of(history.begin(), history.end(), [](const std::string& script) {
        return script.find("SET_TOOL_PARAMETER T=2 PARAMETER=gcode_z_offset VALUE=0.2") !=
               std::string::npos;
    }));
}

TEST_CASE_METHOD(LaneRigFixture,
                 "A lane's tool offset is its head's, read and written as the head's",
                 "[head-identity][tool_offsets][zoffset]") {
    auto& ts = ToolState::instance();
    REQUIRE(ts.tools_are_lanes());
    REQUIRE(lv_subject_get_int(ts.get_per_tool_axis_supported_subject(Axis::Z)) == 1);

    // The printer's tool objects: T0 on extruder, T2 on extruder2.
    ts.update_from_status(json{{"tool T0", {{"gcode_z_offset", -0.2}, {"extruder", "extruder"}}},
                               {"tool T2", {{"gcode_z_offset", 0.15}, {"extruder", "extruder2"}}}});

    // Lane T0 is mounted, in extruder2: its offset is tool T2's.
    REQUIRE(ts.active_tool_index() == 0);
    CHECK(ts.offset_tool_for(0) == 2);
    CHECK(ts.tool_offset_mm(ts.offset_tool_for(0), Axis::Z) == Catch::Approx(0.15));
    // The tool object's extruder describes tool T0, not lane T0.
    CHECK(ts.extruder_name_for_tool(0) == "extruder2");

    REQUIRE(zoffset::adjust_tool_offset(api.get(), &state, 0, 0.05));
    CHECK(sent("SET_TOOL_PARAMETER T=2 PARAMETER=gcode_z_offset VALUE=0.2"));
    CHECK_FALSE(sent("SET_TOOL_PARAMETER T=0"));
    // The save flushes tool T2.
    CHECK(ts.dirty_tool_indices() == std::vector<int>{2});

    // extruder5's lane: no printer tool, so no offset to move.
    mock_client.clear_gcode_script_history();
    CHECK(ts.offset_tool_for(6) == -1);
    CHECK_FALSE(zoffset::adjust_tool_offset(api.get(), &state, 6, 0.05));
    CHECK_FALSE(sent("SET_TOOL_PARAMETER"));
}

TEST_CASE_METHOD(LaneRigFixture,
                 "The probe's reference tool is asked about by head, not by lane number",
                 "[head-identity][zoffset]") {
    using zoffset_cal::ToolGate;
    constexpr auto kProbe = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    auto& ts = ToolState::instance();

    // Lane T0 mounted: it sits in extruder2, not on tool T0's extruder.
    REQUIRE(ts.active_tool_index() == 0);
    CHECK(zoffset_cal::mounted_reference_gate(kProbe, 0, ts) == ToolGate::ConfirmFirst);
    CHECK(ui::active_head_name_label(ts.head_identity_for_tool(0)) == "Tool 3");
    CHECK(ui::active_head_name_label(ts.head_identity_for_offset_tool(0)) == "Tool 1");

    // Lane T1 sits in extruder, tool T0's head: no question.
    auto topo = test::make_lb6lez44_topology();
    topo.active_tool = 1;
    ts.set_ams_topology(topo);
    CHECK(zoffset_cal::mounted_reference_gate(kProbe, 0, ts) == ToolGate::Proceed);
}

TEST_CASE_METHOD(LaneRigFixture, "A per-head device action names its head, not a lane",
                 "[head-identity][device-section]") {
    // AFC's per-extruder actions number extruders; T2 there is a lane.
    printer::DeviceAction action =
        printer::DeviceAction::toggle("tool_stn_T2", "Sensor to Nozzle", "toolhead", false);
    action.tool_index = 2;
    action.extruder = "extruder2";
    CHECK(ui::device_action_label(action) == "Sensor to Nozzle - Tool 3 (E3)");

    // Without an extruder the tool number names it, as on a tool changer.
    action.extruder.clear();
    CHECK(ui::device_action_label(action) == "Sensor to Nozzle - Tool 3 (T2)");
}
