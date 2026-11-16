// SPDX-License-Identifier: GPL-3.0-or-later

// HELIX_MOCK_AMS modes that are mock HARDWARE: MoonrakerClientMock publishes what
// the firmware does and answers its G-code, so --test runs the production backend.

#include "../lvgl_test_fixture.h"
#include "happy_hare_status_parse.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Every value a key took across the notify_status_update frames, in order.
class StatusFrames {
  public:
    std::function<void(const json&)> callback() {
        return [this](const json& n) {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.push_back(n);
        };
    }

    std::vector<json> values(const std::string& object, const std::string& field) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<json> out;
        for (const auto& f : frames_) {
            const auto& params = f["params"];
            if (params.is_array() && !params.empty() && params[0].contains(object) &&
                params[0][object].contains(field)) {
                out.push_back(params[0][object][field]);
            }
        }
        return out;
    }

  private:
    std::mutex mutex_;
    std::vector<json> frames_;
};

/// A mock client built under one HELIX_MOCK_AMS value, recording its frames.
struct MockHardware : LVGLTestFixture {
    helix::ScopedEnv ams;
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
    StatusFrames frames;
    std::optional<MoonrakerClientMock> mock;

    explicit MockHardware(const char* mode) : ams("HELIX_MOCK_AMS", mode) {
        mock.emplace(MoonrakerClientMock::PrinterType::VORON_24, 100.0);
        mock->register_notify_update(frames.callback());
    }
};

struct ToolChangerMock : MockHardware {
    ToolChangerMock() : MockHardware("toolchanger") {}
};

struct AfcMock : MockHardware {
    AfcMock() : MockHardware("afc") {}

    json lane(int n) {
        return mock->afc_boxturtle_status_json()["AFC_stepper lane" + std::to_string(n)];
    }
};

} // namespace

TEST_CASE("mock_hardware_backend: migrated modes run production, the screenshot "
          "topologies stay on the mock backend",
          "[ams][mock]") {
    for (const char* mode : {"toolchanger", "tool_changer", "tc", "afc", "box_turtle", "boxturtle",
                             "ifs", "ad5x", "ad5x_ifs", "snapmaker", "snapswap", "u1", "medusahc",
                             "indx", "cfs", "happy_hare", "hh"}) {
        CAPTURE(mode);
        CHECK(helix::mock::mock_hardware_backend(mode, nullptr) != nullptr);
    }
    for (const char* mode :
         {"mixed", "multi", "torture", "stealth", "vivid", "htlf_toolchanger", "none"}) {
        CAPTURE(mode);
        CHECK(helix::mock::mock_hardware_backend(mode, nullptr) == nullptr);
    }
    // With no mode: a hardware persona, else Happy Hare where the persona publishes `mmu`.
    CHECK(helix::mock::mock_hardware_backend("", "creator5_zmod") != nullptr);
    CHECK(std::string(helix::mock::mock_hardware_backend("", "voron_24")) == "Happy Hare");
    CHECK(std::string(helix::mock::mock_hardware_backend("", nullptr)) == "Happy Hare");
    CHECK(helix::mock::mock_hardware_backend("", "cc1") == nullptr);
    CHECK(helix::mock::mock_hardware_backend("hh", "cc1") == nullptr);
    // A scenario only the mock backend serves keeps the default there; idle is no scenario.
    {
        helix::ScopedEnv state{"HELIX_MOCK_AMS_STATE", "grade"};
        CHECK(helix::mock::mock_hardware_backend("", nullptr) == nullptr);
        CHECK(helix::mock::mock_hardware_backend("happy_hare", nullptr) != nullptr);
    }
    helix::ScopedEnv idle{"HELIX_MOCK_AMS_STATE", "idle"};
    CHECK(helix::mock::mock_hardware_backend("", nullptr) != nullptr);
}

namespace {
/// The default persona: Happy Hare 4 on a four-gate ERCF.
struct HappyHareMock : MockHardware {
    HappyHareMock() : MockHardware(nullptr) {}

    json mmu() {
        return mock->happy_hare_mmu_status();
    }
    std::vector<json> actions() {
        return frames.values("mmu", "action");
    }
};
} // namespace

TEST_CASE_METHOD(HappyHareMock,
                 "Happy Hare mock: the default persona's ERCF, as the parser reads it",
                 "[ams][mock][happy_hare]") {
    const helix::PrinterDiscovery hw = mock->hardware();
    CHECK(hw.mmu_type() == helix::AmsType::HAPPY_HARE);
    const json status = mock->happy_hare_status_json();
    CHECK(status["mmu_machine"]["happy_hare_version"] == "4.1.0");
    CHECK(status["mmu_machine"]["unit_0"]["environment_sensor"] == "temperature_sensor dryer");

    const auto d = helix::happy_hare::parse_mmu_status(mmu());
    CHECK(d.core.gate == 0);
    CHECK(d.core.tool == 0);
    CHECK(d.core.filament_loaded == true);
    CHECK(d.core.filament_pos == 10);
    REQUIRE(d.identity.gate_status);
    std::vector<int> gate_status;
    for (const auto& g : *d.identity.gate_status) {
        gate_status.push_back(g.value_or(-9));
    }
    CHECK(gate_status == std::vector<int>{1, 1, 1, 0});
    // Spools 1-4 of the mock Spoolman, the colours the mock backend showed.
    CHECK((*d.identity.color_rgb)[0] == 0x1A1A2Eu);
    CHECK((*d.identity.color_rgb)[2] == 0x00AEFFu);
    CHECK((*d.identity.material)[1] == "Silk PLA");
    CHECK((*d.identity.vendor)[2] == "Elegoo");
    CHECK((*d.identity.spool_id)[3] == 4);
    CHECK(d.topology.ttg_map == std::vector<int>{0, 1, 2, 3});
    CHECK(mock->happy_hare_config_sections()["mmu_parameters"]["form_tip_macro"] ==
          "_MMU_FORM_TIP");
}

TEST_CASE_METHOD(HappyHareMock, "Happy Hare mock: unload, load and T<n> step through the path",
                 "[ams][mock][happy_hare]") {
    REQUIRE(mock->gcode_script("MMU_UNLOAD") == 0);
    // A move refuses a second one until it ends.
    CHECK(mock->gcode_script("MMU_LOAD GATE=1") != 0);
    process_lvgl(3000);
    CHECK(mmu()["filament"] == "Unloaded");
    CHECK(mmu()["filament_pos"] == 0);
    CHECK(actions().front() == "Forming Tip");
    CHECK(actions().back() == "Idle");

    CHECK(mock->gcode_script("MMU_LOAD GATE=3") != 0); // gate 3 is empty
    REQUIRE(mock->gcode_script("MMU_LOAD GATE=2") == 0);
    process_lvgl(3000);
    CHECK(mmu()["gate"] == 2);
    CHECK(mmu()["tool"] == 2);
    CHECK(mmu()["filament"] == "Loaded");
    const auto positions = frames.values("mmu", "filament_pos");
    CHECK(std::count(positions.begin(), positions.end(), json(5)) == 1);
    CHECK(mock->gcode_script("MMU_LOAD GATE=1") != 0); // already loaded

    // T1 unloads gate 2 and loads gate 1 through the tool map.
    REQUIRE(mock->gcode_script("MMU_TTG_MAP TOOL=1 GATE=0") == 0);
    REQUIRE(mock->gcode_script("T1") == 0);
    process_lvgl(6000);
    CHECK(mmu()["gate"] == 0);
    CHECK(mmu()["tool"] == 1);
    CHECK(mmu()["filament_pos"] == 10);
    CHECK(mock->gcode_script("T9") != 0);
}

TEST_CASE_METHOD(HappyHareMock, "Happy Hare mock: gate map, eject, select and endless spool edits",
                 "[ams][mock][happy_hare]") {
    REQUIRE(mock->gcode_script("MMU_GATE_MAP GATE=1 MATERIAL=PETG COLOR=00FF00 NAME=Green "
                               "VENDOR=Prusament SPOOLID=-1 QUIET=1") == 0);
    json m = mmu();
    CHECK(m["gate_material"][1] == "PETG");
    CHECK(m["gate_color_rgb"][1] == 0x00FF00);
    CHECK(m["gate_color"][1] == "00FF00");
    CHECK(m["gate_name"][1] == "Green");
    CHECK(m["gate_vendor"][1] == "Prusament");
    CHECK(m["gate_spool_id"][1] == -1);
    CHECK(frames.values("mmu", "gate_material").back()[1] == "PETG");
    REQUIRE(mock->gcode_script("MMU_GATE_MAP GATE=1 MATERIAL= COLOR=") == 0);
    CHECK(mmu()["gate_material"][1] == "");

    REQUIRE(mock->gcode_script("MMU_ENDLESS_SPOOL QUIET=1 GROUPS=2,1,2,3") == 0);
    REQUIRE(mock->gcode_script("MMU_ENDLESS_SPOOL ENABLE=0 QUIET=1") == 0);
    CHECK(mmu()["endless_spool_groups"] == json::array({2, 1, 2, 3}));
    CHECK(mmu()["endless_spool_enabled"] == false);
    CHECK(mock->gcode_script("MMU_ENDLESS_SPOOL GROUPS=1,2") != 0);

    // Selecting needs an empty toolhead; eject unloads and leaves the gate empty.
    CHECK(mock->gcode_script("MMU_SELECT GATE=2") != 0);
    REQUIRE(mock->gcode_script("MMU_EJECT GATE=0") == 0);
    process_lvgl(3000);
    CHECK(mmu()["gate_status"][0] == 0);
    CHECK(mmu()["filament"] == "Unloaded");
    REQUIRE(mock->gcode_script("MMU_PRELOAD GATE=0") == 0);
    CHECK(mmu()["gate_status"][0] == 1);
    REQUIRE(mock->gcode_script("MMU_SELECT_BYPASS") == 0);
    process_lvgl(1500);
    CHECK(mmu()["gate"] == -2);
    REQUIRE(mock->gcode_script("MMU_SELECT GATE=2") == 0);
    process_lvgl(1500);
    CHECK(mmu()["gate"] == 2);

    REQUIRE(mock->gcode_script("MMU_SYNC_FEEDBACK ENABLE=0") == 0);
    REQUIRE(mock->gcode_script("MMU_FAN FAN_FORCED=1") == 0);
    CHECK(mmu()["sync_feedback_enabled"] == false);
    CHECK(mmu()["fans"][0]["modes"][0] == 1);
    helix::ScopedEnv grade{"HELIX_MOCK_AMS_STATE", "grade"}; // AmsBackendMock runs: generic T<n>
    CHECK(mock->gcode_script("T9") == 0);
}

TEST_CASE_METHOD(ToolChangerMock, "toolchanger mock: discovery claims the changer, not Happy Hare",
                 "[ams][mock][toolchanger]") {
    const helix::PrinterDiscovery hw = mock->hardware();
    CHECK(hw.has_tool_changer());
    CHECK(hw.mmu_type() != helix::AmsType::HAPPY_HARE);
    CHECK(hw.tool_names().size() == 4);
    // Identity lives in the T<n> lane records, mirroring Spoolman spools 1-4.
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const json rec = mock->mock_db_get("lane_data", "T" + std::to_string(i));
        REQUIRE(rec.is_object());
        CHECK(rec["spoolman_id"] == i + 1);
    }
    CHECK(mock->mock_db_get("lane_data", "T0")["color"] == "#1A1A2E");
}

TEST_CASE_METHOD(ToolChangerMock, "toolchanger mock: SELECT_TOOL swaps, UNSELECT_TOOL parks",
                 "[ams][mock][toolchanger]") {
    REQUIRE(mock->toolchanger_tool() == 0);

    REQUIRE(mock->gcode_script("SELECT_TOOL TOOL=\"tool T2\"") == 0);
    process_lvgl(2000);
    CHECK(mock->toolchanger_tool() == 2);
    const auto statuses = frames.values("toolchanger", "status");
    REQUIRE(statuses.size() >= 2);
    CHECK(statuses.front() == "changing");
    CHECK(statuses.back() == "ready");
    CHECK(frames.values("toolchanger", "tool_number").back() == 2);

    REQUIRE(mock->gcode_script("UNSELECT_TOOL") == 0);
    process_lvgl(2000);
    CHECK(mock->toolchanger_tool() == -1);
    CHECK(frames.values("toolchanger", "tool_number").back() == -1);

    // A tool the changer does not have is refused, and nothing moves.
    CHECK(mock->gcode_script("SELECT_TOOL T=7") != 0);
    CHECK(mock->toolchanger_tool() == -1);
}

TEST_CASE_METHOD(AfcMock, "AFC mock: discovery claims AFC on one Box Turtle, lane 1 tooled",
                 "[ams][mock][afc]") {
    const helix::PrinterDiscovery hw = mock->hardware();
    CHECK(hw.mmu_type() == helix::AmsType::AFC);
    const json status = mock->afc_boxturtle_status_json();
    CHECK(status["AFC_BoxTurtle Turtle_1"]["lanes"].size() == 4);
    CHECK(status["AFC"]["current_load"] == "lane1");
    CHECK(lane(1)["status"] == "Tooled");
    CHECK(lane(1)["color"] == "#1A1A2E");
    CHECK(lane(1)["spool_id"] == 1);
    // Lane 4 is the unlinked, weightless lane.
    CHECK(lane(4)["spool_id"].is_null());
    CHECK(lane(4)["weight"].is_null());
    CHECK(lane(2)["status"] == "Loaded");
}

TEST_CASE_METHOD(AfcMock, "AFC mock: load, T<n> and unload move the toolhead lane",
                 "[ams][mock][afc]") {
    REQUIRE(mock->gcode_script("CHANGE_TOOL LANE=lane3") == 0);
    CHECK(frames.values("AFC", "current_state").back() == "Loading");
    process_lvgl(2000);
    CHECK(mock->afc_boxturtle_status_json()["AFC"]["current_load"] == "lane3");
    CHECK(lane(3)["tool_loaded"] == true);
    CHECK(lane(1)["tool_loaded"] == false);
    CHECK(frames.values("AFC", "current_state").back() == "Idle");

    // T1 resolves through the lane map: lane2 carries T1.
    REQUIRE(mock->gcode_script("T1") == 0);
    process_lvgl(2000);
    CHECK(mock->afc_boxturtle_status_json()["AFC_extruder extruder"]["lane_loaded"] == "lane2");

    REQUIRE(mock->gcode_script("TOOL_UNLOAD") == 0);
    process_lvgl(2000);
    CHECK(mock->afc_boxturtle_status_json()["AFC"]["current_load"].is_null());

    CHECK(mock->gcode_script("CHANGE_TOOL LANE=lane9") != 0);
    CHECK(mock->gcode_script("T7") != 0);
}

TEST_CASE_METHOD(AfcMock, "AFC mock: lane edits and eject republish the lane", "[ams][mock][afc]") {
    REQUIRE(mock->gcode_script("SET_COLOR LANE=lane2 COLOR=00FF00") == 0);
    REQUIRE(mock->gcode_script("SET_MATERIAL LANE=lane2 MATERIAL=PETG") == 0);
    REQUIRE(mock->gcode_script("SET_MAP LANE=lane2 MAP=T3") == 0);
    CHECK(lane(2)["color"] == "#00FF00");
    CHECK(lane(2)["material"] == "PETG");
    CHECK(lane(2)["map"] == "T3");
    CHECK(frames.values("AFC_stepper lane2", "color").back() == "#00FF00");

    REQUIRE(mock->gcode_script("LANE_UNLOAD LANE=lane4") == 0);
    CHECK(lane(4)["status"] == "None");
    CHECK(lane(4)["prep"] == false);
}

TEST_CASE("IFS mock: the stock AD5X is native Z-Mod's four-port IFS, port 1 in the head",
          "[ams][mock][ad5x]") {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "ifs"};
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_AD5X);
    CHECK(mock.hardware().mmu_type() == helix::AmsType::AD5X_IFS);

    const json color = mock.zmod_color_status();
    REQUIRE(color["slots"].size() == 4);
    CHECK(color["slots"][0]["HEX"] == "000000");
    CHECK(color["slots"][2]["HEX"] == "8000FF");
    CHECK(color["active_tool_id"] == 0);
    const json ifs = mock.zmod_ifs_status();
    CHECK(ifs["color_limit"] == 4);
    CHECK(ifs["Chan"] == 1);
    CHECK(ifs["Ports"] == json::array({true, true, true, true}));

    REQUIRE(mock.gcode_script("_IFS_REMOVE_CURRENT_PRUTOK") == 0);
    CHECK(mock.zmod_color_status()["active_tool_id"] == -1);
    REQUIRE(mock.gcode_script("INSERT_PRUTOK_IFS PRUTOK=3") == 0);
    CHECK(mock.zmod_ifs_status()["Chan"] == 3);
    CHECK(mock.zmod_color_status()["extruder_sensor"] == true);
}

TEST_CASE("SnapSwap mock: a stock U1's four heads, swap, unload and re-tag",
          "[ams][mock][snapmaker]") {
    // Built under the persona whose default this is.
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    helix::ScopedEnv u1{"HELIX_MOCK_PRINTER", "snapmaker_u1"};
    MoonrakerClientMock u1_mock(MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0);
    CHECK(u1_mock.hardware().mmu_type() == helix::AmsType::SNAPMAKER);

    json status = u1_mock.u1_snapswap_status_json();
    CHECK(status["print_task_config"]["filament_color_rgba"][0] == "1E9E4AFF");
    CHECK(status["filament_detect"]["info"][3]["ARGB_COLOR"] == 0xFFF08000u);
    CHECK(status["extruder"]["active_pin"] == true);
    CHECK(status["extruder2"]["park_pin"] == true);

    u1_mock.gcode_script("T2");
    status = u1_mock.u1_snapswap_status_json();
    CHECK(status["extruder2"]["active_pin"] == true);
    CHECK(status["extruder"]["park_pin"] == true);

    REQUIRE(u1_mock.gcode_script("AUTO_FEEDING EXTRUDER=3 UNLOAD=1") == 0);
    CHECK(u1_mock.u1_snapswap_status_json()["print_task_config"]["filament_exist"][3] == false);

    REQUIRE(u1_mock.gcode_script("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                                 "FILAMENT_TYPE=PETG FILAMENT_COLOR_RGBA=112233FF") == 0);
    const json ptc = u1_mock.u1_snapswap_status_json()["print_task_config"];
    CHECK(ptc["filament_type"][1] == "PETG");
    CHECK(ptc["filament_vendor"][1] == "Generic");
    CHECK(ptc["filament_color_rgba"][1] == "112233FF");
}
