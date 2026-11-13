// SPDX-License-Identifier: GPL-3.0-or-later

// The creator5_afc persona: Reforge master on a Creator 5 Pro, AFC over
// ff_toolchange (Klipper4FlashForge/firmware eda86c1, ff-afc.cfg).

#include "ams_types.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "start_macro_injection.h"
#include "test_helpers/scoped_env.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// The persona's own objects, not whatever another test in this shard left in
/// the mock-topology env vars.
struct MockTopologyGuards {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
};

} // namespace

TEST_CASE("The Reforge AFC persona runs the AFC backend over the changer",
          "[mock][creator5][reforge]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_AFC);
    const helix::PrinterDiscovery hw = mock.hardware();

    REQUIRE(hw.has_mmu());
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    CHECK(systems[0].type == helix::AmsType::AFC);
    CHECK(hw.has_tool_changer());
    CHECK(hw.tool_names().size() == 4);
    CHECK(hw.afc_lane_names() == std::vector<std::string>{"e0", "e1", "e2", "e3"});
    CHECK(hw.afc_extruder_names() == std::vector<std::string>{"e0", "e1", "e2", "e3"});

    const json status = mock.reforge_afc_status_json();
    CHECK(status["AFC"]["units"] == json::array({"Toolchanger Tools"}));
    CHECK(status["AFC_lane e2"]["extruder"] == "e2");
    CHECK(status["AFC_lane e2"]["map"] == "T2");

    const json sections = mock.reforge_afc_config_sections();
    CHECK(sections["afc_extruder e0"]["extruder_name"] == "extruder");
    CHECK(sections["afc_extruder e3"]["extruder_name"] == "extruder3");
    CHECK(helix::firmware_runs_start_macro_before_file(sections));
}

TEST_CASE("The Reforge AFC persona mounts heads through AFC's map", "[mock][creator5][reforge]") {
    MockTopologyGuards guards;

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_AFC);

    REQUIRE(mock.gcode_script("AFC_SELECT_TOOL TOOL=e1") == 0);
    CHECK(mock.reforge_afc_status_json()["AFC"]["current_load"] == "e1");
    CHECK(mock.reforge_afc_status_json()["AFC_extruder e1"]["on_shuttle"] == true);

    REQUIRE(mock.gcode_script("SET_MAP LANE=e2 MAP=T0") == 0);
    CHECK(mock.reforge_afc_status_json()["AFC_lane e2"]["map"] == "T0");
    REQUIRE(mock.gcode_script("T0") == 0);
    CHECK(mock.reforge_afc_status_json()["AFC"]["current_load"] == "e2");

    CHECK(mock.gcode_script("AFC_SELECT_TOOL TOOL=e7") != 0);
    CHECK(mock.reforge_afc_status_json()["AFC"]["current_load"] == "e2");
}
