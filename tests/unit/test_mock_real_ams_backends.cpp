// SPDX-License-Identifier: GPL-3.0-or-later

// HELIX_MOCK_AMS modes that are mock HARDWARE: MoonrakerClientMock publishes what
// the firmware does and answers its G-code, so --test runs the production backend.

#include "../lvgl_test_fixture.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

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
                             "medusahc", "indx", "cfs"}) {
        CAPTURE(mode);
        CHECK(helix::mock::mock_hardware_backend(mode, nullptr) != nullptr);
    }
    for (const char* mode :
         {"mixed", "multi", "torture", "stealth", "vivid", "htlf_toolchanger", "none"}) {
        CAPTURE(mode);
        CHECK(helix::mock::mock_hardware_backend(mode, nullptr) == nullptr);
    }
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
