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

struct ToolChangerMock : LVGLTestFixture {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "toolchanger"};
    helix::ScopedEnv printer{"HELIX_MOCK_PRINTER", nullptr};
    StatusFrames frames;
    std::optional<MoonrakerClientMock> mock;

    ToolChangerMock() {
        mock.emplace(MoonrakerClientMock::PrinterType::VORON_24, 100.0);
        mock->register_notify_update(frames.callback());
    }
};

} // namespace

TEST_CASE("mock_hardware_backend: migrated modes run production, the screenshot "
          "topologies stay on the mock backend",
          "[ams][mock]") {
    for (const char* mode : {"toolchanger", "tool_changer", "tc", "medusahc", "indx", "cfs"}) {
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
