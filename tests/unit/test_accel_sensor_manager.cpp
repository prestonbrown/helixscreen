// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_accel_sensor_manager.cpp
 * @brief Unit tests for AccelSensorManager
 *
 * Tests cover:
 * - Type helpers: role/type string conversion
 * - Sensor discovery from classified configfile sections
 * - Beacon onboard accelerometer rules
 * - The real discovery sequence records accelerometer configfile sections
 * - Status frames leave discovered state alone (accelerometers publish none)
 * - No roles
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "accel_sensor_manager.h"
#include "accel_sensor_types.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

// ============================================================================
// Test Fixture
// ============================================================================

namespace {

PrinterDiscovery discovery_of_config(const json& config) {
    PrinterDiscovery hw;
    hw.parse_config_keys(config);
    return hw;
}

class AccelSensorTestFixture : public LVGLTestFixture {
  public:
    AccelSensorTestFixture() {
        mgr().init_subjects();
        mgr().set_sync_mode(true);
        SensorManagerTestAccess::reconcile(mgr(), {});
    }

    ~AccelSensorTestFixture() override {
        SensorManagerTestAccess::reconcile(mgr(), {});
    }

  protected:
    AccelSensorManager& mgr() {
        return AccelSensorManager::instance();
    }

    void discover_config(const json& config) {
        mgr().discover(discovery_of_config(config));
    }

    void discover_test_sensors() {
        discover_config({{"adxl345", json::object()},
                         {"adxl345 bed", json::object()},
                         {"lis2dw hotend", json::object()}});
    }

    std::vector<std::string> names() {
        std::vector<std::string> out;
        for (const auto& e : mgr().entries()) {
            out.push_back(e.klipper_name);
        }
        return out;
    }

    AccelSensorType type_of(const std::string& klipper_name) {
        SensorEntry e;
        REQUIRE(mgr().find(klipper_name, e));
        return static_cast<AccelSensorType>(e.type);
    }
};

} // namespace

// ============================================================================
// Type Helper Tests (accel_sensor_types.h)
// ============================================================================

TEST_CASE("AccelSensorTypes - type string conversion", "[accel][types]") {
    SECTION("accel_type_to_string") {
        REQUIRE(accel_type_to_string(AccelSensorType::ADXL345) == "adxl345");
        REQUIRE(accel_type_to_string(AccelSensorType::LIS2DW) == "lis2dw");
        REQUIRE(accel_type_to_string(AccelSensorType::LIS3DH) == "lis3dh");
        REQUIRE(accel_type_to_string(AccelSensorType::MPU9250) == "mpu9250");
        REQUIRE(accel_type_to_string(AccelSensorType::ICM20948) == "icm20948");
    }

    SECTION("accel_type_from_string") {
        REQUIRE(accel_type_from_string("adxl345") == AccelSensorType::ADXL345);
        REQUIRE(accel_type_from_string("lis2dw") == AccelSensorType::LIS2DW);
        REQUIRE(accel_type_from_string("lis3dh") == AccelSensorType::LIS3DH);
        REQUIRE(accel_type_from_string("mpu9250") == AccelSensorType::MPU9250);
        REQUIRE(accel_type_from_string("icm20948") == AccelSensorType::ICM20948);
        REQUIRE(accel_type_from_string("invalid") == AccelSensorType::ADXL345);
        REQUIRE(accel_type_from_string("") == AccelSensorType::ADXL345);
    }
}

// ============================================================================
// Discovery from config sections
// ============================================================================

TEST_CASE_METHOD(AccelSensorTestFixture, "AccelSensorManager - config-based discovery",
                 "[accel][discovery][config]") {
    SECTION("Discovers ADXL345 from config keys") {
        discover_config({{"adxl345", json::object()}});

        REQUIRE(mgr().sensor_count() == 1);
        const auto entries = mgr().entries();
        REQUIRE(entries[0].klipper_name == "adxl345");
        REQUIRE(entries[0].display_name == "adxl345");
        REQUIRE(static_cast<AccelSensorType>(entries[0].type) == AccelSensorType::ADXL345);
        REQUIRE(entries[0].available);
    }

    SECTION("Discovers named ADXL345 from config keys") {
        discover_config({{"adxl345 bed", json::object()}});

        REQUIRE(mgr().sensor_count() == 1);
        const auto entries = mgr().entries();
        REQUIRE(entries[0].klipper_name == "adxl345 bed");
        REQUIRE(entries[0].display_name == "bed");
    }

    SECTION("Discovers multiple accelerometers and ignores everything else") {
        discover_config({{"adxl345", json::object()},
                         {"adxl345 bed", json::object()},
                         {"lis2dw hotend", json::object()},
                         {"resonance_tester", json::object()},
                         {"stepper_x", json::object()}});

        REQUIRE(mgr().sensor_count() == 3);
        REQUIRE(type_of("lis2dw hotend") == AccelSensorType::LIS2DW);
    }

    SECTION("Handles empty config keys") {
        discover_config(json::object());
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("Ignores non-accelerometer config keys") {
        discover_config({{"stepper_x", json::object()},
                         {"extruder", json::object()},
                         {"resonance_tester", json::object()}});
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("Discovers every chip type") {
        discover_config({{"lis3dh toolhead", json::object()},
                         {"mpu9250", json::object()},
                         {"icm20948", json::object()}});
        REQUIRE(mgr().sensor_count() == 3);
        REQUIRE(type_of("lis3dh toolhead") == AccelSensorType::LIS3DH);
        REQUIRE(type_of("mpu9250") == AccelSensorType::MPU9250);
        REQUIRE(type_of("icm20948") == AccelSensorType::ICM20948);
    }

    SECTION("Rediscovery replaces the list instead of appending") {
        discover_test_sensors();
        discover_config({{"adxl345", json::object()}});
        REQUIRE(names() == std::vector<std::string>{"adxl345"});
        REQUIRE(lv_subject_get_int(mgr().count_subject()) == 1);
    }
}

TEST_CASE_METHOD(AccelSensorTestFixture, "AccelSensorManager - Beacon onboard accelerometer",
                 "[accel][discovery][beacon]") {
    SECTION("accel_scale") {
        discover_config({{"beacon", {{"accel_scale", "16g"}, {"serial", "/dev/ttyUSB0"}}}});
        REQUIRE(names() == std::vector<std::string>{"beacon"});
        REQUIRE(type_of("beacon") == AccelSensorType::LIS2DW);
    }

    SECTION("accel_axes_map") {
        discover_config({{"beacon", {{"accel_axes_map", "x,y,z"}}}});
        REQUIRE(names() == std::vector<std::string>{"beacon"});
    }

    SECTION("Beacon without accel fields is not an accelerometer") {
        discover_config({{"beacon", {{"serial", "/dev/serial/by-id/usb-beacon"}}}});
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("resonance_tester naming beacon") {
        for (const char* field : {"accel_chip", "accel_chip_x", "accel_chip_y"}) {
            discover_config({{"resonance_tester", {{field, "beacon"}}}});
            REQUIRE(mgr().sensor_count() == 1);
            const auto entry = mgr().entries()[0];
            REQUIRE(entry.display_name == "beacon");
            REQUIRE(static_cast<AccelSensorType>(entry.type) == AccelSensorType::LIS2DW);
        }
    }

    SECTION("Both beacon rules together register one entry") {
        discover_config({{"beacon", {{"accel_scale", "16g"}}},
                         {"resonance_tester", {{"accel_chip", "beacon"}}}});
        REQUIRE(names() == std::vector<std::string>{"beacon"});
    }
}

// ============================================================================
// Status, roles and settings
// ============================================================================

TEST_CASE_METHOD(AccelSensorTestFixture, "AccelSensorManager - status frames change nothing",
                 "[accel][state]") {
    discover_test_sensors();
    const int revision = lv_subject_get_int(mgr().revision_subject());

    json status;
    status["adxl345"]["connected"] = false;
    status["unknown_sensor"]["connected"] = true;
    mgr().update_from_status(status);
    mgr().update_from_status(json::object());

    REQUIRE(mgr().sensor_count() == 3);
    REQUIRE(mgr().entries()[0].available);
    REQUIRE(lv_subject_get_int(mgr().count_subject()) == 3);
    REQUIRE(lv_subject_get_int(mgr().revision_subject()) == revision);
}

TEST_CASE_METHOD(AccelSensorTestFixture, "AccelSensorManager - has no roles", "[accel][config]") {
    discover_test_sensors();

    REQUIRE(mgr().offered_roles().empty());
    for (const auto& e : mgr().entries()) {
        REQUIRE(e.role == 0);
        REQUIRE(e.enabled);
    }
}

// ============================================================================
// Discovery wiring (#1262)
//
// Klipper accelerometers have no get_status(), so they exist only in
// configfile.config. The discovery sequence's configfile arm records those
// sections in PrinterDiscovery; discover(hw) turns them into manager entries.
// ============================================================================

namespace {

/// Runs the REAL discovery sequence: MoonrakerClientMock overrides
/// discover_printer() with a shortcut, so the base implementation is called
/// directly while send_jsonrpc() still dispatches through the mock.
class AccelDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }
};

} // namespace

TEST_CASE("Discovery sequence's configfile arm records accelerometer sections", "[sensors][1262]") {
    LVGLTestFixture fixture;
    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    client.discover_printer_real();

    const auto sections = client.hardware().sections_of({helix::hw::ObjectKind::Accelerometer});
    CHECK(std::find(sections.begin(), sections.end(), "adxl345") != sections.end());
}

TEST_CASE_METHOD(AccelSensorTestFixture,
                 "Repeated discovery does not duplicate accelerometer entries", "[sensors][1262]") {
    discover_config({{"adxl345", json::object()}, {"adxl345 bed", json::object()}});
    REQUIRE(mgr().sensor_count() == 2);

    discover_config({{"adxl345", json::object()}, {"adxl345 bed", json::object()}});
    CHECK(mgr().sensor_count() == 2);
}
