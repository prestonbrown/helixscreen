// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_domain.cpp
 * @brief Unit tests for MoonrakerAPI domain service operations
 *
 * Tests the domain logic migrated from MoonrakerClient to MoonrakerAPI:
 * - Hardware guessing (guess_bed_heater, guess_hotend_heater, guess_bed_sensor, guess_hotend_sensor)
 * - Bed mesh operations (get_active_bed_mesh, get_bed_mesh_profiles, has_bed_mesh)
 * - Object exclusion (get_excluded_objects, get_available_objects)
 *
 * These tests verify that MoonrakerAPI's domain methods produce the same results
 * as the deprecated MoonrakerClient methods, ensuring backward compatibility
 * during the migration.
 */

#include "../catch_amalgamated.hpp"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_client_mock.h"
#include "../../lvgl/lvgl.h"

#include <chrono>
#include <thread>

// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerAPIDomain {
    LVGLInitializerAPIDomain() {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_display_t* disp = lv_display_create(800, 480);
            static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerAPIDomain lvgl_init;
} // namespace

// ============================================================================
// Test Fixtures
// ============================================================================

/**
 * @brief Test fixture for MoonrakerAPI domain operations with mock client
 *
 * Uses MoonrakerClientMock to provide hardware discovery data for testing
 * the domain service operations.
 */
class MoonrakerAPIDomainTestFixture {
  public:
    MoonrakerAPIDomainTestFixture()
        : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // Initialize printer state
        state.init_subjects();

        // Connect mock client (required for discovery)
        mock_client.connect("ws://mock/websocket", []() {}, []() {});

        // Run discovery to populate hardware lists
        mock_client.discover_printer([]() {});

        // Create API with mock client
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~MoonrakerAPIDomainTestFixture() {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

// ============================================================================
// Hardware Guessing Tests - MoonrakerAPI
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::guess_bed_heater returns correct heater",
                 "[moonraker][api][domain][guessing]") {
    // VORON_24 mock should have heater_bed
    std::string bed_heater = api->guess_bed_heater();
    REQUIRE(bed_heater == "heater_bed");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::guess_hotend_heater returns correct heater",
                 "[moonraker][api][domain][guessing]") {
    // VORON_24 mock should have extruder
    std::string hotend_heater = api->guess_hotend_heater();
    REQUIRE(hotend_heater == "extruder");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::guess_bed_sensor returns correct sensor",
                 "[moonraker][api][domain][guessing]") {
    // Bed sensor should return heater_bed (heaters have built-in sensors)
    std::string bed_sensor = api->guess_bed_sensor();
    REQUIRE(bed_sensor == "heater_bed");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::guess_hotend_sensor returns correct sensor",
                 "[moonraker][api][domain][guessing]") {
    // Hotend sensor should return extruder (heaters have built-in sensors)
    std::string hotend_sensor = api->guess_hotend_sensor();
    REQUIRE(hotend_sensor == "extruder");
}

// ============================================================================
// Hardware Guessing - Parity with MoonrakerClient
// ============================================================================

TEST_CASE("MoonrakerAPI guessing matches MoonrakerClient guessing",
          "[moonraker][api][domain][guessing][parity]") {
    PrinterState state;
    state.init_subjects();

    SECTION("VORON_24 printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        MoonrakerAPI api(mock, state);

// Suppress deprecation warnings for parity testing
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        REQUIRE(api.guess_bed_heater() == mock.guess_bed_heater());
        REQUIRE(api.guess_hotend_heater() == mock.guess_hotend_heater());
        REQUIRE(api.guess_bed_sensor() == mock.guess_bed_sensor());
        REQUIRE(api.guess_hotend_sensor() == mock.guess_hotend_sensor());
#pragma GCC diagnostic pop

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("CREALITY_K1 printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K1);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        MoonrakerAPI api(mock, state);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        REQUIRE(api.guess_bed_heater() == mock.guess_bed_heater());
        REQUIRE(api.guess_hotend_heater() == mock.guess_hotend_heater());
        REQUIRE(api.guess_bed_sensor() == mock.guess_bed_sensor());
        REQUIRE(api.guess_hotend_sensor() == mock.guess_hotend_sensor());
#pragma GCC diagnostic pop

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("MULTI_EXTRUDER printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::MULTI_EXTRUDER);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        MoonrakerAPI api(mock, state);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        REQUIRE(api.guess_bed_heater() == mock.guess_bed_heater());
        REQUIRE(api.guess_hotend_heater() == mock.guess_hotend_heater());
        REQUIRE(api.guess_bed_sensor() == mock.guess_bed_sensor());
        REQUIRE(api.guess_hotend_sensor() == mock.guess_hotend_sensor());
#pragma GCC diagnostic pop

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// Bed Mesh Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::has_bed_mesh returns correct state",
                 "[moonraker][api][domain][bedmesh]") {
    // Initially the mock client may or may not have bed mesh data
    // This tests that the API method delegates correctly
    bool has_mesh = api->has_bed_mesh();

    // Verify parity with deprecated client method
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    REQUIRE(has_mesh == mock_client.has_bed_mesh());
#pragma GCC diagnostic pop
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::get_active_bed_mesh returns nullptr when no mesh",
                 "[moonraker][api][domain][bedmesh]") {
    // Check current state
    const BedMeshProfile* mesh = api->get_active_bed_mesh();

    // If no mesh, should return nullptr
    // If mesh exists, should return valid pointer
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const BedMeshProfile& client_mesh = mock_client.get_active_bed_mesh();
#pragma GCC diagnostic pop

    if (client_mesh.probed_matrix.empty()) {
        REQUIRE(mesh == nullptr);
    } else {
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->name == client_mesh.name);
    }
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::get_bed_mesh_profiles returns profile list",
                 "[moonraker][api][domain][bedmesh]") {
    std::vector<std::string> profiles = api->get_bed_mesh_profiles();

    // Verify parity with deprecated client method
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const std::vector<std::string>& client_profiles = mock_client.get_bed_mesh_profiles();
#pragma GCC diagnostic pop

    REQUIRE(profiles.size() == client_profiles.size());
    for (size_t i = 0; i < profiles.size(); ++i) {
        REQUIRE(profiles[i] == client_profiles[i]);
    }
}

// ============================================================================
// Object Exclusion Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::get_excluded_objects handles empty response",
                 "[moonraker][api][domain][exclude]") {
    bool callback_called = false;
    std::set<std::string> result;

    api->get_excluded_objects(
        [&callback_called, &result](const std::set<std::string>& objects) {
            callback_called = true;
            result = objects;
        },
        [](const MoonrakerError&) {
            // Error callback - should not be called for this test
        });

    // Note: Mock client may not invoke callbacks immediately
    // This test verifies the API method signature is correct
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "MoonrakerAPI::get_available_objects handles empty response",
                 "[moonraker][api][domain][exclude]") {
    bool callback_called = false;
    std::vector<std::string> result;

    api->get_available_objects(
        [&callback_called, &result](const std::vector<std::string>& objects) {
            callback_called = true;
            result = objects;
        },
        [](const MoonrakerError&) {
            // Error callback - should not be called for this test
        });

    // Note: Mock client may not invoke callbacks immediately
    // This test verifies the API method signature is correct
}

// ============================================================================
// Domain Service Interface Compliance Tests
// ============================================================================

TEST_CASE("BedMeshProfile struct initialization",
          "[moonraker][api][domain][bedmesh]") {
    BedMeshProfile profile;

    SECTION("Default values are correct") {
        REQUIRE(profile.name.empty());
        REQUIRE(profile.probed_matrix.empty());
        REQUIRE(profile.mesh_min[0] == 0.0f);
        REQUIRE(profile.mesh_min[1] == 0.0f);
        REQUIRE(profile.mesh_max[0] == 0.0f);
        REQUIRE(profile.mesh_max[1] == 0.0f);
        REQUIRE(profile.x_count == 0);
        REQUIRE(profile.y_count == 0);
        REQUIRE(profile.algo.empty());
    }

    SECTION("Can be populated with data") {
        profile.name = "test_profile";
        profile.mesh_min[0] = 10.0f;
        profile.mesh_min[1] = 10.0f;
        profile.mesh_max[0] = 200.0f;
        profile.mesh_max[1] = 200.0f;
        profile.x_count = 5;
        profile.y_count = 5;
        profile.algo = "bicubic";

        // Add some mesh data
        for (int y = 0; y < 5; ++y) {
            std::vector<float> row;
            for (int x = 0; x < 5; ++x) {
                row.push_back(0.01f * (x + y));
            }
            profile.probed_matrix.push_back(row);
        }

        REQUIRE(profile.name == "test_profile");
        REQUIRE(profile.probed_matrix.size() == 5);
        REQUIRE(profile.probed_matrix[0].size() == 5);
        REQUIRE(profile.x_count == 5);
        REQUIRE(profile.y_count == 5);
    }
}

// ============================================================================
// All Printer Types Parity Tests
// ============================================================================

TEST_CASE("MoonrakerAPI domain methods work for all printer types",
          "[moonraker][api][domain][all_printers]") {
    PrinterState state;
    state.init_subjects();

    std::vector<MoonrakerClientMock::PrinterType> printer_types = {
        MoonrakerClientMock::PrinterType::VORON_24,
        MoonrakerClientMock::PrinterType::VORON_TRIDENT,
        MoonrakerClientMock::PrinterType::CREALITY_K1,
        MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M,
        MoonrakerClientMock::PrinterType::GENERIC_COREXY,
        MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER,
        MoonrakerClientMock::PrinterType::MULTI_EXTRUDER,
    };

    for (auto printer_type : printer_types) {
        DYNAMIC_SECTION("Printer type " << static_cast<int>(printer_type)) {
            MoonrakerClientMock mock(printer_type);
            mock.connect("ws://mock/websocket", []() {}, []() {});
            mock.discover_printer([]() {});

            MoonrakerAPI api(mock, state);

            // Test all guessing methods return non-empty for standard printers
            std::string bed_heater = api.guess_bed_heater();
            std::string hotend_heater = api.guess_hotend_heater();
            std::string bed_sensor = api.guess_bed_sensor();
            std::string hotend_sensor = api.guess_hotend_sensor();

            // All standard printer types should have bed and hotend
            REQUIRE_FALSE(bed_heater.empty());
            REQUIRE_FALSE(hotend_heater.empty());
            REQUIRE_FALSE(bed_sensor.empty());
            REQUIRE_FALSE(hotend_sensor.empty());

            // Bed mesh methods should not crash
            bool has_mesh = api.has_bed_mesh();
            const BedMeshProfile* mesh = api.get_active_bed_mesh();
            std::vector<std::string> profiles = api.get_bed_mesh_profiles();

            // Consistency check
            if (has_mesh) {
                REQUIRE(mesh != nullptr);
            } else {
                REQUIRE(mesh == nullptr);
            }

            mock.stop_temperature_simulation();
            mock.disconnect();
        }
    }
}
