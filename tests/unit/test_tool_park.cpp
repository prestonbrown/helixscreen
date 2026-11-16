// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_park.cpp
 * @brief Docking the mounted tool is a backend capability (prestonbrown/helixscreen#1784).
 *
 * The U1 calls it "Detach Toolhead" and sends PARK_EXTRUDER; klipper-toolchanger
 * unmounts with UNSELECT_TOOL. Everything else answers unsupported, and the
 * home tool switcher offers Park only where the backend says it can.
 */

#include "../helix_test_fixture.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "core/update_queue.h"
#include "toolchanger_addon.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// Real U1 backend with its gcode captured. api_ stays null, so the print gate
/// passes (unknown print state is not blocked) and only the command is asserted.
class ParkingSnapmaker : public AmsBackendSnapmaker {
  public:
    ParkingSnapmaker() : AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    std::vector<std::string> sent;

    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }

    void mount(int tool) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.current_tool = tool;
    }
};

/// Real klipper-toolchanger backend with its gcode captured and acked at once.
class ParkingToolChanger : public AmsBackendToolChanger {
  public:
    explicit ParkingToolChanger(int tool_count) : AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
    }

    ~ParkingToolChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> sent;

    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        sent.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }
    // A dispatch that unwinds on a rejection sends through this form.
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                           bool /*silent*/) override {
        return execute_gcode(gcode, std::move(on_complete));
    }

    void mount(int tool) {
        handle_status_update(
            json{{"method", "notify_status_update"},
                 {"params",
                  json::array({json{{"toolchanger", {{"status", "ready"}, {"tool_number", tool}}}},
                               0.0})}});
    }
};

} // namespace

TEST_CASE("the U1 parks its mounted toolhead with PARK_EXTRUDER", "[ams][snapmaker][1784]") {
    HelixTestFixture fixture;
    ParkingSnapmaker backend;
    CHECK(backend.supports_tool_park());

    backend.mount(2);
    const AmsError err = backend.park_active_tool();
    CHECK(err.success());
    CHECK(backend.sent == std::vector<std::string>{"PARK_EXTRUDER"});
}

TEST_CASE("the U1 has nothing to park with no toolhead mounted", "[ams][snapmaker][1784]") {
    HelixTestFixture fixture;
    ParkingSnapmaker backend;
    backend.mount(-1);

    const AmsError err = backend.park_active_tool();
    CHECK(err.result == AmsResult::WRONG_STATE);
    CHECK(backend.sent.empty());
}

TEST_CASE("klipper-toolchanger parks with the unmount Unload sends", "[ams][toolchanger][1784]") {
    HelixTestFixture fixture;
    ParkingToolChanger backend(4);
    CHECK(backend.supports_tool_park());

    backend.mount(1);
    const AmsError err = backend.park_active_tool();
    CHECK(err.success());
    // No tool named: whatever is on the carriage goes back to its dock.
    CHECK(backend.sent == std::vector<std::string>{"UNSELECT_TOOL"});
}

TEST_CASE("a toolchanger with an empty carriage has nothing to park", "[ams][toolchanger][1784]") {
    HelixTestFixture fixture;
    ParkingToolChanger backend(4);
    backend.mount(-1);

    CHECK(backend.park_active_tool().result == AmsResult::WRONG_STATE);
    CHECK(backend.sent.empty());
}

TEST_CASE("a changer's own unmount command parks; without one, nothing can",
          "[ams][toolchanger][1784]") {
    HelixTestFixture fixture;

    SECTION("an unmount command is the park") {
        ParkingToolChanger backend(4);
        toolchanger_addon::ToolCommands commands;
        commands.present = true;
        commands.provider_name = "test";
        commands.select_prefix = "T";
        commands.unselect = "DROP_TOOL";
        backend.set_tool_commands(commands);
        backend.mount(0);

        CHECK(backend.supports_tool_park());
        CHECK(backend.park_active_tool().success());
        CHECK(backend.sent == std::vector<std::string>{"DROP_TOOL"});
    }
    SECTION("swap commands with no unmount cannot park") {
        ParkingToolChanger backend(4);
        toolchanger_addon::ToolCommands commands;
        commands.present = true;
        commands.provider_name = "test";
        commands.select_prefix = "T";
        backend.set_tool_commands(commands);
        backend.mount(0);

        CHECK_FALSE(backend.supports_tool_park());
    }
}

TEST_CASE("an AMS with one shared toolhead has no tool to park", "[ams][happy_hare][1784]") {
    HelixTestFixture fixture;
    AmsBackendHappyHare backend(nullptr, nullptr);
    CHECK_FALSE(backend.supports_tool_park());
    CHECK(backend.park_active_tool().result == AmsResult::NOT_SUPPORTED);
}

TEST_CASE("the mock parks only for the personas whose hardware can", "[ams][mock][1784]") {
    HelixTestFixture fixture;

    SECTION("tool changer: the park ends with the carriage empty") {
        AmsBackendMock backend(4);
        backend.set_tool_changer_mode(true);
        backend.set_operation_delay(0);
        REQUIRE(backend.start());
        REQUIRE(backend.get_current_tool() >= 0);
        CHECK(backend.supports_tool_park());

        CHECK(backend.park_active_tool().success());
        backend.wait_for_operation_thread();
        CHECK(backend.get_current_tool() == -1);
        // Nothing left to park.
        CHECK(backend.park_active_tool().result == AmsResult::WRONG_STATE);
        backend.stop();
    }
    SECTION("Happy Hare: unsupported") {
        AmsBackendMock backend(4);
        backend.set_operation_delay(0);
        REQUIRE(backend.start());
        CHECK_FALSE(backend.supports_tool_park());
        CHECK(backend.park_active_tool().result == AmsResult::NOT_SUPPORTED);
        backend.stop();
    }
    helix::ui::UpdateQueue::instance().drain();
}
