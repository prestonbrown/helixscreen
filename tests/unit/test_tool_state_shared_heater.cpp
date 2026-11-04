// SPDX-License-Identifier: GPL-3.0-or-later

// Several nozzles taking turns on one extruder and heater (Bondtech INDX):
// separate heads to every tool surface, one heated head to every heater
// surface, and one name for the nozzle the readout shows.

#include "core/update_queue.h"
#include "lvgl_test_fixture.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ToolState;
using helix::ToolTopology;
using helix::ui::UpdateQueue;

namespace {

struct SharedHeaterFixture : public LVGLTestFixture {
    SharedHeaterFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
        helix::PrinterDiscovery disc;
        disc.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "toolhead"}));
        ToolState::instance().init_tools(disc);
        UpdateQueue::instance().drain();
    }
    ~SharedHeaterFixture() override {
        ToolState::instance().clear_ams_topology();
        UpdateQueue::instance().drain();
        ToolState::instance().deinit_subjects();
    }

    /// @p count nozzles on the one `extruder`, @p active mounted (-1 none).
    static ToolTopology topology(int count, int active, bool share) {
        ToolTopology topo;
        topo.tool_count = count;
        topo.active_tool = active;
        topo.allows_empty_carriage = true;
        for (int i = 0; i < count; ++i) {
            topo.tool_to_slot.push_back(i);
            topo.tool_extruder.emplace_back("extruder");
        }
        topo.heads_share_heater = share;
        return topo;
    }

    static void apply(const ToolTopology& topo) {
        ToolState::instance().set_ams_topology(topo);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(SharedHeaterFixture, "Shared heater: each nozzle is a head, one heated head",
                 "[tool_state][shared_heater]") {
    apply(topology(6, 2, /*share=*/true));
    auto& ts = ToolState::instance();
    REQUIRE(ts.tool_count() == 6);
    CHECK(ts.physical_heads().size() == 6);
    CHECK(ts.has_multiple_toolheads());
    CHECK(ts.extruder_count() == 1);
    CHECK_FALSE(ts.has_multiple_extruders());
    const auto heated = ts.heated_heads();
    REQUIRE(heated.size() == 1);
    CHECK(heated[0].holds_tool(2));
    CHECK(heated[0].extruder_name == std::optional<std::string>("extruder"));
}

TEST_CASE_METHOD(SharedHeaterFixture, "Without the flag, tools on one extruder stay one hub head",
                 "[tool_state][shared_heater]") {
    apply(topology(4, 1, /*share=*/false));
    auto& ts = ToolState::instance();
    CHECK(ts.physical_heads().size() == 1);
    CHECK_FALSE(ts.has_multiple_toolheads());
    CHECK_FALSE(ts.names_mounted_nozzle());
    CHECK(ts.nozzle_label() == "Nozzle");
}

TEST_CASE_METHOD(SharedHeaterFixture, "Shared heater: the label names the mounted nozzle",
                 "[tool_state][nozzle_label]") {
    apply(topology(6, 2, /*share=*/true));
    auto& ts = ToolState::instance();
    CHECK(ts.names_mounted_nozzle());
    CHECK(ts.nozzle_label() == "Nozzle 3");
    CHECK(ts.display_label_for_extruder("extruder") == ts.tools()[2].display_label);
    CHECK(ts.tool_name_for_extruder("extruder") == "T2");

    apply(topology(6, -1, /*share=*/true));
    CHECK(ts.active_tool_index() == -1);
    CHECK(ts.nozzle_label() == "Nozzle");
    CHECK(ts.display_label_for_extruder("extruder").empty());
    CHECK(ts.tool_name_for_extruder("extruder").empty());
}

TEST_CASE_METHOD(SharedHeaterFixture, "A lane AMS on one hotend still names no nozzle",
                 "[tool_state][nozzle_label]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.tool_to_slot = {0, 1, 2, 3};
    apply(topo);
    auto& ts = ToolState::instance();
    CHECK_FALSE(ts.names_mounted_nozzle());
    CHECK(ts.nozzle_label() == "Nozzle");
}

TEST_CASE_METHOD(SharedHeaterFixture, "Shared heater: only the mounted nozzle reads the heater",
                 "[tool_state][shared_heater]") {
    apply(topology(4, 2, /*share=*/true));
    auto& ts = ToolState::instance();
    CHECK(ts.head_reads_heater(2));
    CHECK_FALSE(ts.head_reads_heater(0));
    CHECK_FALSE(ts.head_reads_heater(3));

    // Nothing mounted: no nozzle is hot, but the heater still has a stand-in.
    apply(topology(4, -1, /*share=*/true));
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(ts.head_reads_heater(i));
    }
    REQUIRE(ts.heated_heads().size() == 1);
    CHECK(ts.heated_heads()[0].holds_tool(0));
}
