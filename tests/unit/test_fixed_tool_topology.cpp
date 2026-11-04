// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Firmware that ships a fixed set of toolheads declares them in one table
// (helix::tool_topology); ToolState builds its tools from the answer and never
// names the vendor.

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "fixed_tool_topology.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace topology = helix::tool_topology;

namespace {

PrinterDiscovery discover(const json& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

const json kU1Objects = json::array({"extruder", "extruder1", "extruder2", "extruder3",
                                     "heater_bed", "filament_detect", "toolhead"});

struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        helix::ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        helix::ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE("fixed_toolheads gives the U1 four heads and everyone else none", "[tool]") {
    SECTION("the U1's four heads") {
        const auto heads = topology::fixed_toolheads(discover(kU1Objects));
        REQUIRE(heads.size() == 4);
        const std::vector<std::string> fans = {"fan", "fan_generic e1_fan", "fan_generic e2_fan",
                                               "fan_generic e3_fan"};
        const std::vector<std::string> extruders = {"extruder", "extruder1", "extruder2",
                                                    "extruder3"};
        for (size_t i = 0; i < heads.size(); ++i) {
            CAPTURE(i);
            CHECK(heads[i].extruder == extruders[i]);
            CHECK(heads[i].heater == extruders[i]);
            CHECK(heads[i].fan == fans[i]);
        }
        CHECK(std::string(topology::provider_name(discover(kU1Objects))) != "none");
    }

    SECTION("a klipper-toolchanger printer has none") {
        const PrinterDiscovery hw = discover(json::array(
            {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"}));
        CHECK(topology::fixed_toolheads(hw).empty());
        CHECK(std::string(topology::provider_name(hw)) == "none");
    }

    SECTION("a plain printer has none") {
        const PrinterDiscovery hw = discover(json::array({"extruder", "heater_bed", "fan"}));
        CHECK(topology::fixed_toolheads(hw).empty());
        CHECK(std::string(topology::provider_name(hw)) == "none");
    }
}

TEST_CASE_METHOD(ToolStateFixture, "ToolState builds the U1 heads from the fixed topology",
                 "[tool]") {
    auto& ts = helix::ToolState::instance();
    ts.init_tools(discover(kU1Objects));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(ts.tool_count() == 4);
    const std::vector<std::string> fans = {"fan", "fan_generic e1_fan", "fan_generic e2_fan",
                                           "fan_generic e3_fan"};
    const std::vector<std::string> extruders = {"extruder", "extruder1", "extruder2", "extruder3"};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const auto& tool = ts.tools()[static_cast<size_t>(i)];
        CHECK(tool.index == i);
        REQUIRE(tool.extruder_name.has_value());
        CHECK(*tool.extruder_name == extruders[static_cast<size_t>(i)]);
        REQUIRE(tool.heater_name.has_value());
        CHECK(*tool.heater_name == extruders[static_cast<size_t>(i)]);
        REQUIRE(tool.fan_name.has_value());
        CHECK(*tool.fan_name == fans[static_cast<size_t>(i)]);
    }
}
