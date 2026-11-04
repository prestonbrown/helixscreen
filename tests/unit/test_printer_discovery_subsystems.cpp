// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_discovery_subsystems.cpp
 * @brief init_subsystems_from_hardware() is the one place discovery reaches the
 * managers, on desktop and on the ESP32 alike. These cases pin the parts a
 * caller cannot see from the UI until something is pressed: the per-tool offset
 * seed, the standard-macro slots, and a printer switch that must not inherit
 * the previous printer's sensors.
 */

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_ace.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "filament_sensor_manager.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "printer_discovery.h"
#include "standard_macros.h"
#include "tool_state.h"
#include "width_sensor_manager.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::PrinterDiscovery;

namespace {

PrinterDiscovery discovery_of(std::initializer_list<const char*> objects) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json(std::vector<std::string>(objects.begin(), objects.end())));
    return hw;
}

struct DiscoverySubsystemsFixture : public LVGLTestFixture {
    helix::test::FakeMoonrakerClient client;

    DiscoverySubsystemsFixture() {
        auto& ts = helix::ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(false);
        helix::FilamentSensorManager::instance().init_subjects();
        helix::sensors::WidthSensorManager::instance().init_subjects();
        StandardMacros::instance().reset();
    }

    ~DiscoverySubsystemsFixture() override {
        // An empty printer is the clean slate: it clears every manager this
        // touched, through the same path the cases drive.
        helix::init_subsystems_from_hardware(PrinterDiscovery{}, nullptr, nullptr);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        helix::led::LedAutoState::instance().deinit();
        helix::led::LedController::instance().deinit();
        StandardMacros::instance().reset();
    }

    bool queried(const std::string& object) const {
        return std::any_of(client.rpc_calls.begin(), client.rpc_calls.end(), [&](const auto& c) {
            return c.method == "printer.objects.query" && c.params.contains("objects") &&
                   c.params["objects"].contains(object);
        });
    }
};

} // namespace

TEST_CASE_METHOD(DiscoverySubsystemsFixture, "discovery seeds per-tool offsets from the client",
                 "[discovery]") {
    helix::init_subsystems_from_hardware(
        discovery_of({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "gcode_move"}),
        nullptr, &client);

    REQUIRE(queried("tool T0"));
    REQUIRE(queried("tool T1"));
}

TEST_CASE_METHOD(DiscoverySubsystemsFixture, "discovery resolves the standard macro slots",
                 "[discovery]") {
    REQUIRE_FALSE(StandardMacros::instance().is_initialized());

    helix::init_subsystems_from_hardware(
        discovery_of({"extruder", "gcode_macro PAUSE", "gcode_macro RESUME"}), nullptr, &client);

    REQUIRE(StandardMacros::instance().is_initialized());
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::Pause).get_macro() == "PAUSE");
}

TEST_CASE_METHOD(DiscoverySubsystemsFixture,
                 "a printer without sensors does not keep the previous printer's", "[discovery]") {
    auto& fsm = helix::FilamentSensorManager::instance();
    auto& wsm = helix::sensors::WidthSensorManager::instance();

    helix::init_subsystems_from_hardware(
        discovery_of({"extruder", "filament_switch_sensor runout", "hall_filament_width_sensor"}),
        nullptr, &client);
    REQUIRE(fsm.sensor_count() == 1);
    REQUIRE(wsm.sensor_count() == 1);

    helix::init_subsystems_from_hardware(discovery_of({"extruder"}), nullptr, &client);
    REQUIRE(fsm.sensor_count() == 0);
    REQUIRE(wsm.sensor_count() == 0);
}

// init_tools() rebuilds the tools with T0 mounted, but a backend that already
// reported an empty carriage owns the answer and sends no frame to restate it
// (prestonbrown/helixscreen#1784).
TEST_CASE_METHOD(DiscoverySubsystemsFixture,
                 "rediscovery keeps the empty carriage a backend already reported",
                 "[discovery][tool-state][tool_switcher][1784]") {
    auto& ams = helix::AmsState::instance();
    auto& ts = helix::ToolState::instance();
    ams.init_subjects(/*register_xml=*/false);
    const PrinterDiscovery u1 =
        discovery_of({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                      "filament_detect", "toolhead", "heater_bed", "print_task_config"});
    {
        helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> backend(nullptr, nullptr);
        ts.init_tools(u1);
        helix::SnapmakerTestAccess::handle_status(
            *backend, nlohmann::json{{"extruder", {{"state", "PARKED"}}},
                                     {"extruder1", {{"state", "PARKED"}}},
                                     {"extruder2", {{"state", "PARKED"}}},
                                     {"extruder3", {{"state", "PARKED"}}},
                                     {"toolhead", {{"extruder", "extruder2"}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
        ams.sync_from_backend();
        REQUIRE(ts.active_tool_index() == -1);

        helix::init_subsystems_from_hardware(u1, nullptr, &client);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(ts.active_tool_index() == -1);
        CHECK(lv_subject_get_int(ts.get_active_tool_subject()) == -1);
    }
    ams.deinit_subjects();
}

TEST_CASE_METHOD(DiscoverySubsystemsFixture,
                 "rediscovery leaves T0 mounted where the carriage cannot be empty",
                 "[discovery][tool-state][tool_switcher][1784]") {
    auto& ams = helix::AmsState::instance();
    auto& ts = helix::ToolState::instance();
    ams.init_subjects(/*register_xml=*/false);
    const PrinterDiscovery hw = discovery_of({"extruder", "extruder1", "toolhead", "heater_bed"});
    {
        helix::test::RegisteredBackend<helix::AmsBackendAce> backend(nullptr, nullptr);
        REQUIRE_FALSE(backend->carriage_can_be_empty());
        REQUIRE(backend->get_current_tool() == -1);

        helix::init_subsystems_from_hardware(hw, nullptr, &client);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(ts.active_tool_index() == 0);
        CHECK_FALSE(ts.backend_owns_active_tool());
    }
    ams.deinit_subjects();
}
