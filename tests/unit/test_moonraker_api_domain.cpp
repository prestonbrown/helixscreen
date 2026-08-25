// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_domain.cpp
 * @brief Unit tests for MoonrakerAPI domain service operations and PrinterHardware guessing
 *
 * Tests the domain logic:
 * - PrinterHardware guessing (guess_bed_heater, guess_hotend_heater, guess_bed_sensor,
 *   guess_hotend_sensor, guess_part_cooling_fan, guess_main_led_strip)
 * - Hardware discovery reaching MoonrakerAPI::hardware()
 */

#include "ui_update_queue.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_hardware.h"
#include "../../lvgl/lvgl.h"
#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"

#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerAPIDomain {
    LVGLInitializerAPIDomain() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
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
    MoonrakerAPIDomainTestFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // Initialize printer state
        state.init_subjects(false);

        // Connect mock client (required for discovery)
        mock_client.connect("ws://mock/websocket", []() {}, []() {});

        // Create API with mock client BEFORE discovery
        // (API registers hardware discovered callback in constructor)
        api = std::make_unique<MoonrakerAPI>(mock_client, state);

        // Run discovery to populate hardware lists (triggers API callback)
        mock_client.discover_printer([]() {});
    }

    ~MoonrakerAPIDomainTestFixture() {
        // Drain while `state` is still alive — discover_printer() in the ctor
        // leaves PrinterCapabilitiesState's deferred setters queued (#1166).
        helix::ui::UpdateQueue::instance().drain();

        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

// ============================================================================
// Hardware Guessing Tests - PrinterHardware
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "PrinterHardware::guess_bed_heater returns correct heater",
                 "[printer][guessing]") {
    // VORON_24 mock should have heater_bed
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    std::string bed_heater = hw.guess_bed_heater();
    REQUIRE(bed_heater == "heater_bed");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "PrinterHardware::guess_hotend_heater returns correct heater",
                 "[printer][guessing]") {
    // VORON_24 mock should have extruder
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    std::string hotend_heater = hw.guess_hotend_heater();
    REQUIRE(hotend_heater == "extruder");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "PrinterHardware::guess_bed_sensor returns correct sensor",
                 "[printer][guessing]") {
    // Bed sensor should return heater_bed (heaters have built-in sensors)
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    std::string bed_sensor = hw.guess_bed_sensor();
    REQUIRE(bed_sensor == "heater_bed");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "PrinterHardware::guess_hotend_sensor returns correct sensor",
                 "[printer][guessing]") {
    // Hotend sensor should return extruder (heaters have built-in sensors)
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    std::string hotend_sensor = hw.guess_hotend_sensor();
    REQUIRE(hotend_sensor == "extruder");
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture,
                 "PrinterHardware::guess_part_cooling_fan returns correct fan",
                 "[printer][guessing]") {
    // VORON_24 should have canonical "fan" for part cooling
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    std::string fan = hw.guess_part_cooling_fan();
    // The canonical [fan] section should be prioritized if it exists
    REQUIRE_FALSE(fan.empty());
}

TEST_CASE_METHOD(MoonrakerAPIDomainTestFixture, "PrinterHardware::guess_main_led_strip returns LED",
                 "[printer][guessing]") {
    // VORON_24 seeds four LEDs (moonraker_client_mock.cpp): "neopixel chamber_light",
    // "neopixel status_led", "led caselight", "output_pin Enclosure_LEDs". Priority 1
    // walks its keyword list {"case", "chamber", ...} in order and returns the first LED
    // matching the CURRENT keyword, so "case" resolves before "chamber" is ever tried —
    // keyword order wins over LED order. Both room-lighting candidates must beat
    // "neopixel status_led", which is a machine-status indicator, not room lighting.
    PrinterHardware hw(api->hardware().heaters(), api->hardware().sensors(), api->hardware().fans(),
                       api->hardware().leds());
    REQUIRE(hw.guess_main_led_strip() == "led caselight");
}

// ============================================================================
// Hardware Guessing - Multiple Printer Types
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "PrinterHardware guessing works for multiple printer types",
                 "[printer][guessing][printers]") {
    PrinterState state;
    state.init_subjects(false);

    SECTION("VORON_24 printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
        REQUIRE(hw.guess_bed_sensor() == "heater_bed");
        REQUIRE(hw.guess_hotend_sensor() == "extruder");

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("CREALITY_K1 printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K1);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        // Just verify these return something sensible
        REQUIRE_FALSE(hw.guess_bed_heater().empty());
        REQUIRE_FALSE(hw.guess_hotend_heater().empty());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("MULTI_EXTRUDER printer type") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::MULTI_EXTRUDER);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        mock.discover_printer([]() {});

        PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                           mock.hardware().fans(), mock.hardware().leds());

        // Multi-extruder should still find bed and primary extruder
        REQUIRE_FALSE(hw.guess_bed_heater().empty());
        REQUIRE_FALSE(hw.guess_hotend_heater().empty());

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

// ============================================================================
// All Printer Types Tests
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrinterHardware and MoonrakerAPI domain methods work for all printer types",
                 "[printer][api][domain][all_printers]") {
    PrinterState state;
    state.init_subjects(false);

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

            // Test PrinterHardware guessing
            PrinterHardware hw(mock.hardware().heaters(), mock.hardware().sensors(),
                               mock.hardware().fans(), mock.hardware().leds());

            std::string bed_heater = hw.guess_bed_heater();
            std::string hotend_heater = hw.guess_hotend_heater();
            std::string bed_sensor = hw.guess_bed_sensor();
            std::string hotend_sensor = hw.guess_hotend_sensor();

            // All standard printer types should have bed and hotend
            REQUIRE_FALSE(bed_heater.empty());
            REQUIRE_FALSE(hotend_heater.empty());
            REQUIRE_FALSE(bed_sensor.empty());
            REQUIRE_FALSE(hotend_sensor.empty());

            // No bed-mesh assertions here: has_bed_mesh() and get_active_bed_mesh()
            // are the same expression over active_bed_mesh_.probed_matrix
            // (moonraker_advanced_api.cpp), so cross-checking them can never fail.

            mock.stop_temperature_simulation();
            mock.disconnect();
        }
    }
}

// ============================================================================
// Hardware Discovery Access via MoonrakerAPI Tests
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerAPI hardware() returns discovery data after discovery completes",
                 "[api][hardware]") {
    PrinterState state;
    state.init_subjects(false);

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    // Create API before discovery so callbacks are registered
    MoonrakerAPI api(mock, state);

    // Run discovery - this fires callbacks that populate api.hardware_
    mock.discover_printer([]() {});

    // Verify hardware data is accessible through API
    // After discovery, the API should have hardware data populated
    const auto& hw = api.hardware();

    // VORON_24 should have hostname populated from mock
    // Note: Mock sets hostname during discovery
    REQUIRE_FALSE(hw.hostname().empty());

    // Should have expected hardware for VORON_24
    REQUIRE_FALSE(hw.heaters().empty());
    REQUIRE_FALSE(hw.fans().empty());

    // Check capabilities that VORON_24 should have
    REQUIRE(hw.has_heater_bed() == true);
    REQUIRE(hw.has_qgl() == true); // Voron 2.4 has QGL

    mock.stop_temperature_simulation();
    mock.disconnect();
}
