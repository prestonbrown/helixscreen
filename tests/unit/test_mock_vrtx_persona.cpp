// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_vrtx_persona.cpp
 * @brief The toolchanger mock's tool count and the VRTX (A4T-C) hotend-swapper persona.
 *
 * VRTX (Slimohan/VRTX) rides on klipper-toolchanger: heater-only [extruderN]
 * sections, one shared [extruder_stepper], the carriage's single [fan], T<n>
 * macros over _VRTX_T, and VRTX_CALIBRATE instead of CALIBRATE_TOOL_OFFSETS.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

struct VrtxMockFixture : public LVGLTestFixture {
    helix::ScopedEnv ams_env{"HELIX_MOCK_AMS"};
    helix::ScopedEnv tools_env{"HELIX_MOCK_TOOLCHANGER_TOOLS"};
    std::optional<MoonrakerClientMock> client;

    void start(const char* ams, const char* tools = nullptr) {
        setenv("HELIX_MOCK_AMS", ams, 1);
        if (tools) {
            setenv("HELIX_MOCK_TOOLCHANGER_TOOLS", tools, 1);
        } else {
            unsetenv("HELIX_MOCK_TOOLCHANGER_TOOLS");
        }
        client.emplace(MoonrakerClientMock::PrinterType::VORON_24, 100.0);
    }

    void run(const std::string& script) {
        client->send_jsonrpc("printer.gcode.script", json{{"script", script}}, nullptr, nullptr);
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE_METHOD(VrtxMockFixture,
                 "mock toolchanger: tool count follows HELIX_MOCK_TOOLCHANGER_TOOLS",
                 "[mock][toolchanger][vrtx]") {
    SECTION("default stays four") {
        start("toolchanger");
        CHECK(client->hardware().tool_names().size() == 4);
    }
    SECTION("an explicit count within range") {
        start("toolchanger", "7");
        CHECK(client->hardware().tool_names().size() == 7);
        CHECK(client->hardware().tool_names().back() == "T6");
    }
    SECTION("out of range falls back") {
        start("toolchanger", "99");
        CHECK(client->hardware().tool_names().size() == 4);
    }
}

TEST_CASE_METHOD(VrtxMockFixture, "mock vrtx: eight heater-only tools on a shared motor",
                 "[mock][toolchanger][vrtx]") {
    start("vrtx");
    const auto& hw = client->hardware();

    REQUIRE(hw.tool_names().size() == 8);
    CHECK(hw.has_tool_changer());
    CHECK(hw.has_macro("_VRTX_T"));
    CHECK(hw.has_macro("T7"));
    CHECK(hw.has_macro("VRTX_CALIBRATE"));
    // VRTX defines no CALIBRATE_TOOL_OFFSETS: the stock offsets screen must not be offered.
    CHECK_FALSE(hw.has_macro("CALIBRATE_TOOL_OFFSETS"));

    // Eight heaters; the [extruder_stepper] is a motor, not a ninth tool.
    int heaters = 0;
    for (const auto& h : hw.heaters()) {
        heaters += h.rfind("extruder", 0) == 0 ? 1 : 0;
    }
    CHECK(heaters == 8);

    // One carriage [fan]; no per-tool part fans.
    CHECK(has(hw.fans(), "fan"));
    for (const auto& f : hw.fans()) {
        CHECK(f.find("_part_fan") == std::string::npos);
    }
}

TEST_CASE_METHOD(VrtxMockFixture, "mock vrtx: T<n> mounts the tool and the count can shrink",
                 "[mock][toolchanger][vrtx]") {
    start("vrtx", "6");
    REQUIRE(client->hardware().tool_names().size() == 6);

    run("T5");
    process_lvgl(2000);
    CHECK(client->toolchanger_tool() == 5);

    run("T7"); // not a tool of this changer
    process_lvgl(2000);
    CHECK(client->toolchanger_tool() == 5);
}
