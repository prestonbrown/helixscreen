// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_tool_offset_calibration.cpp
 * @brief The mock's CALIBRATE_TOOL_OFFSETS simulation, against what the real
 *        macro does.
 *
 * klipper-toolchanger's example macro is one blocking gcode: the rpc answers
 * when the last tool is parked, the console narrates each tool as it goes,
 * and every measured tool's offsets are written with SET_TOOL_PARAMETER (live,
 * republished per axis) and SAVE_TOOL_PARAMETER (staged for SAVE_CONFIG). The
 * panel is built on those three facts, so the simulator must keep them.
 */

#include "../lvgl_test_fixture.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

/// The toolchanger persona is chosen by HELIX_MOCK_AMS at construction.
struct ToolchangerEnv {
    std::string prev;
    bool had = false;
    ToolchangerEnv() {
        if (const char* p = std::getenv("HELIX_MOCK_AMS")) {
            had = true;
            prev = p;
        }
        setenv("HELIX_MOCK_AMS", "toolchanger", 1);
    }
    ~ToolchangerEnv() {
        if (had) {
            setenv("HELIX_MOCK_AMS", prev.c_str(), 1);
        } else {
            unsetenv("HELIX_MOCK_AMS");
        }
    }
};

struct ToolCalFixture : public LVGLTestFixture {
    ToolchangerEnv env;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24, 100.0};
    std::vector<std::string> console;
    bool acked = false;
    bool failed = false;
    std::string error;

    ToolCalFixture() {
        client.register_method_callback("notify_gcode_response", "test_console",
                                        [this](const json& msg) {
                                            for (const auto& p : msg["params"]) {
                                                if (p.is_string()) {
                                                    console.push_back(p.get<std::string>());
                                                }
                                            }
                                        });
    }
    ~ToolCalFixture() override {
        client.unregister_method_callback("notify_gcode_response", "test_console");
    }

    void run_macro() {
        client.send_jsonrpc(
            "printer.gcode.script", json{{"script", "CALIBRATE_TOOL_OFFSETS"}},
            [this](const json&) { acked = true; },
            [this](const MoonrakerError& e) {
                failed = true;
                error = e.message;
            });
    }

    bool saw(const std::string& needle) const {
        for (const auto& l : console) {
            if (l.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(ToolCalFixture, "mock: the calibration rpc answers only when the run is over",
                 "[mock][toolchanger][tool_offset_cal]") {
    REQUIRE(client.hardware().tool_names().size() == 4);

    run_macro();
    CHECK_FALSE(acked); // blocking, like the macro

    process_lvgl(600 * 2); // T0 selected and located; T1 selected
    CHECK_FALSE(acked);
    CHECK(saw("Selected tool 0 (T0)"));
    CHECK(saw("Sensor location at"));

    process_lvgl(600 * 8); // the rest of the tools and the park
    CHECK(acked);
    CHECK_FALSE(failed);
    CHECK(saw("Selected tool 3 (T3)"));
    CHECK(saw("Tool offset is"));
}

TEST_CASE_METHOD(ToolCalFixture, "mock: every measured tool's offsets land live and staged",
                 "[mock][toolchanger][tool_offset_cal]") {
    run_macro();
    process_lvgl(600 * 10);
    REQUIRE(acked);

    // T0 is the reference and keeps its seed; the others carry the measured
    // offset on all three axes, exactly as _SAVE_TOOL_OFFSET writes them.
    CHECK(client.tool_offset(0, helix::Axis::X) == Catch::Approx(0.0));
    CHECK(client.tool_offset(1, helix::Axis::X) == Catch::Approx(0.12));
    CHECK(client.tool_offset(1, helix::Axis::Y) == Catch::Approx(-0.05));
    CHECK(client.tool_offset(1, helix::Axis::Z) == Catch::Approx(-0.03));
    CHECK(client.tool_offset(3, helix::Axis::X) == Catch::Approx(0.36));

    // Staged, not persisted: SAVE_CONFIG is the panel's job.
    CHECK(client.save_config_pending());
    const json items = client.save_config_pending_items();
    REQUIRE(items.contains("tool T2"));
    CHECK(items["tool T2"].contains("gcode_x_offset"));
    CHECK(items["tool T2"].contains("gcode_y_offset"));
    CHECK(items["tool T2"].contains("gcode_z_offset"));
    CHECK_FALSE(items.contains("tool T0"));

    // And a restart without SAVE_CONFIG throws it all away, as on the printer.
    client.send_jsonrpc(
        "printer.gcode.script", json{{"script", "RESTART"}}, [](const json&) {},
        [](const MoonrakerError&) {});
    CHECK(client.tool_offset(1, helix::Axis::X) == Catch::Approx(0.100));
}

TEST_CASE_METHOD(ToolCalFixture, "mock: HELIX_MOCK_TOOL_CAL_FAIL fails the run on that tool",
                 "[mock][toolchanger][tool_offset_cal]") {
    setenv("HELIX_MOCK_TOOL_CAL_FAIL", "2", 1);
    run_macro();
    process_lvgl(600 * 12);
    unsetenv("HELIX_MOCK_TOOL_CAL_FAIL");

    CHECK_FALSE(acked);
    CHECK(failed);
    CHECK(error.find("samples_tolerance") != std::string::npos);
    // T1 was measured before the failure; T2 and T3 were not.
    CHECK(client.tool_offset(1, helix::Axis::X) == Catch::Approx(0.12));
    CHECK(client.tool_offset(2, helix::Axis::X) == Catch::Approx(0.200));
    CHECK(saw("!! Probe samples exceed samples_tolerance"));
    CHECK_FALSE(saw("Selected tool 3 (T3)"));
}

TEST_CASE_METHOD(ToolCalFixture, "mock: printer.gcode.help describes the macro",
                 "[mock][toolchanger][tool_offset_cal]") {
    json result;
    client.send_jsonrpc(
        "printer.gcode.help", json::object(), [&result](const json& r) { result = r["result"]; },
        [](const MoonrakerError&) {});

    REQUIRE(result.contains("CALIBRATE_TOOL_OFFSETS"));
    CHECK(result["CALIBRATE_TOOL_OFFSETS"].get<std::string>().find("offset") != std::string::npos);
}
