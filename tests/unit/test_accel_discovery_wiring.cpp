// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_accel_discovery_wiring.cpp
 * @brief Regression guard for prestonbrown/helixscreen#1262
 *
 * AccelSensorManager's sensor list is only ever filled by
 * discover_from_config(). Its intended caller — SensorRegistry::discover_all()
 * — has no production callers, so the manager stayed permanently empty on real
 * printers: Settings > Sensors showed an empty accelerometer list with a 0
 * badge, telemetry reported accel:0, and detect_belt_hardware() sourced
 * has_adxl from the same empty manager while claiming to be the single source
 * of truth.
 *
 * These tests drive the REAL discovery sequence (not the mock override) against
 * a mock transport whose configfile.config carries [adxl345], and assert the
 * manager and its downstream consumers see the sensor.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "accel_sensor_manager.h"
#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/**
 * @brief Mock transport that runs the REAL discovery sequence
 *
 * MoonrakerClientMock overrides discover_printer() with its own shortcut.
 * Calling the base implementation exercises MoonrakerDiscoverySequence while
 * still dispatching send_jsonrpc() through the mock handler registry — the
 * same technique as TestDiscoveryClient in test_discovery_klippy_gate.cpp.
 */
class AccelDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }
};

/// Clear the singleton so a previous test's discovery can't mask the result.
/// discover_from_config() drops sensors_ and prunes states_, so an empty
/// object is a full reset through the public API.
void reset_accel_manager() {
    // Drain first: a callback another test queued would otherwise repopulate
    // the singleton on our own drain, after the reset.
    helix::ui::UpdateQueue::instance().drain();
    helix::sensors::AccelSensorManager::instance().discover_from_config(json::object());
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE("Discovery sequence populates AccelSensorManager from configfile", "[sensors][1262]") {
    LVGLTestFixture fixture;
    reset_accel_manager();

    auto& mgr = helix::sensors::AccelSensorManager::instance();
    REQUIRE(mgr.sensor_count() == 0); // precondition: nothing carried over

    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.discover_printer_real();

    // discover_from_config() runs through ui_queue_update() — it sets LVGL
    // subjects, so the sequence must not call it on the WebSocket thread.
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mgr.has_sensors());
    CHECK(mgr.sensor_count() >= 1); // the Settings > Sensors badge reads this

    bool found_adxl = false;
    for (const auto& sensor : mgr.get_sensors()) {
        if (sensor.klipper_name == "adxl345") {
            found_adxl = true;
            CHECK(sensor.type == helix::sensors::AccelSensorType::ADXL345);
        }
    }
    CHECK(found_adxl);
}

TEST_CASE("Discovery sequence leaves AccelSensorManager empty without an accel section",
          "[sensors][1262]") {
    // Mutation guard: the wiring must read the payload, not unconditionally
    // invent a sensor. A config with no accelerometer section must discover
    // nothing even though the identical code path ran.
    LVGLTestFixture fixture;
    reset_accel_manager();

    auto& mgr = helix::sensors::AccelSensorManager::instance();
    json cfg = {{"printer", {{"kinematics", "corexy"}}}, {"heater_bed", json::object()}};
    mgr.discover_from_config(cfg);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(mgr.has_sensors());
    CHECK(mgr.sensor_count() == 0);
}

TEST_CASE("Repeated discovery does not duplicate accelerometer entries", "[sensors][1262]") {
    // Discovery re-runs on reconnect and on FIRMWARE_RESTART. Entries must not
    // accumulate across runs.
    LVGLTestFixture fixture;
    reset_accel_manager();

    auto& mgr = helix::sensors::AccelSensorManager::instance();

    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain();
    const size_t after_first = mgr.sensor_count();
    REQUIRE(after_first >= 1);

    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(mgr.sensor_count() == after_first);
}

TEST_CASE("detect_belt_hardware reports has_adxl once discovery has run", "[sensors][1262]") {
    // moonraker_advanced_api.cpp calls AccelSensorManager::has_sensors() and
    // documents it as the single source of truth for accelerometer presence.
    // With the manager never populated that flag was false on every printer,
    // which downgraded belt tension to strobe-only.
    LVGLTestFixture fixture;

    helix::PrinterState state;
    state.init_subjects(false);
    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    // Reset after construction so nothing the mock queued on the way up is
    // mistaken for the discovery under test.
    reset_accel_manager();

    // Before discovery the manager is empty, so the flag must be false.
    helix::calibration::BeltTensionHardware before;
    bool before_done = false;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            before = hw;
            before_done = true;
        },
        [](const MoonrakerError&) {});
    REQUIRE(before_done);
    CHECK_FALSE(before.has_adxl);

    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.discover_printer_real();
    helix::ui::UpdateQueue::instance().drain();

    helix::calibration::BeltTensionHardware after;
    bool after_done = false;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            after = hw;
            after_done = true;
        },
        [](const MoonrakerError&) {});
    REQUIRE(after_done);
    CHECK(after.has_adxl);
}
