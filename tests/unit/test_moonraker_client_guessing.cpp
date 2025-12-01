// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "../catch_amalgamated.hpp"
#include "moonraker_client.h"
#include "moonraker_client_mock.h"

// Suppress deprecation warnings - these tests specifically validate deprecated
// MoonrakerClient methods that are being migrated to MoonrakerAPI
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// ============================================================================
// Test Fixture for Hardware Guessing
// ============================================================================

/**
 * @brief Test fixture providing custom hardware configurations
 *
 * Extends MoonrakerClient to allow direct manipulation of protected
 * hardware vectors for comprehensive edge case testing.
 */
class HardwareGuessingFixture : public MoonrakerClient {
public:
    HardwareGuessingFixture() = default;

    // Allow tests to directly set hardware lists
    void set_heaters(const std::vector<std::string>& heaters) {
        heaters_ = heaters;
    }

    void set_sensors(const std::vector<std::string>& sensors) {
        sensors_ = sensors;
    }

    void clear_hardware() {
        heaters_.clear();
        sensors_.clear();
    }

    // Expose guessing methods for testing
    using MoonrakerClient::guess_bed_heater;
    using MoonrakerClient::guess_hotend_heater;
    using MoonrakerClient::guess_bed_sensor;
    using MoonrakerClient::guess_hotend_sensor;
};

// ============================================================================
// guess_bed_heater() Tests
// ============================================================================

TEST_CASE("MoonrakerClient::guess_bed_heater", "[moonraker][hardware][guessing]") {
    HardwareGuessingFixture client;

    SECTION("Exact match: heater_bed (highest priority)") {
        client.set_heaters({"extruder", "heater_bed", "extruder1"});
        REQUIRE(client.guess_bed_heater() == "heater_bed");
    }

    SECTION("Exact match: heated_bed (second priority)") {
        client.set_heaters({"extruder", "heated_bed", "extruder1"});
        REQUIRE(client.guess_bed_heater() == "heated_bed");
    }

    SECTION("Substring match: custom_bed_heater") {
        client.set_heaters({"extruder", "custom_bed_heater", "extruder1"});
        REQUIRE(client.guess_bed_heater() == "custom_bed_heater");
    }

    SECTION("Substring match: bed_chamber") {
        client.set_heaters({"extruder", "bed_chamber"});
        REQUIRE(client.guess_bed_heater() == "bed_chamber");
    }

    SECTION("Priority: heater_bed wins over heated_bed") {
        client.set_heaters({"heated_bed", "heater_bed", "extruder"});
        REQUIRE(client.guess_bed_heater() == "heater_bed");
    }

    SECTION("Priority: heated_bed wins over substring match") {
        client.set_heaters({"extruder", "custom_bed", "heated_bed"});
        REQUIRE(client.guess_bed_heater() == "heated_bed");
    }

    SECTION("Priority: exact match wins when multiple substrings exist") {
        client.set_heaters({"bed_zone1", "bed_zone2", "heater_bed"});
        REQUIRE(client.guess_bed_heater() == "heater_bed");
    }

    SECTION("Multiple substring matches: returns first found") {
        client.set_heaters({"extruder", "bed_zone1", "bed_zone2"});
        REQUIRE(client.guess_bed_heater() == "bed_zone1");
    }

    SECTION("No match: returns empty string") {
        client.set_heaters({"extruder", "extruder1", "chamber_heater"});
        REQUIRE(client.guess_bed_heater() == "");
    }

    SECTION("Empty heaters list: returns empty string") {
        client.set_heaters({});
        REQUIRE(client.guess_bed_heater() == "");
    }

    SECTION("Case sensitivity: 'Bed' does not match 'bed'") {
        client.set_heaters({"extruder", "heater_Bed"});
        REQUIRE(client.guess_bed_heater() == "");
    }
}

// ============================================================================
// guess_hotend_heater() Tests
// ============================================================================

TEST_CASE("MoonrakerClient::guess_hotend_heater", "[moonraker][hardware][guessing]") {
    HardwareGuessingFixture client;

    SECTION("Exact match: extruder (highest priority)") {
        client.set_heaters({"heater_bed", "extruder", "extruder1"});
        REQUIRE(client.guess_hotend_heater() == "extruder");
    }

    SECTION("Exact match: extruder0 (second priority)") {
        client.set_heaters({"heater_bed", "extruder0", "extruder1"});
        REQUIRE(client.guess_hotend_heater() == "extruder0");
    }

    SECTION("Substring match: extruder1") {
        client.set_heaters({"heater_bed", "extruder1"});
        REQUIRE(client.guess_hotend_heater() == "extruder1");
    }

    SECTION("Substring match: hotend_heater") {
        client.set_heaters({"heater_bed", "hotend_heater"});
        REQUIRE(client.guess_hotend_heater() == "hotend_heater");
    }

    SECTION("Substring match: e0_heater") {
        client.set_heaters({"heater_bed", "e0_heater"});
        REQUIRE(client.guess_hotend_heater() == "e0_heater");
    }

    SECTION("Priority: extruder wins over extruder0") {
        client.set_heaters({"heater_bed", "extruder0", "extruder"});
        REQUIRE(client.guess_hotend_heater() == "extruder");
    }

    SECTION("Priority: extruder0 wins over extruder1") {
        client.set_heaters({"heater_bed", "extruder1", "extruder0"});
        REQUIRE(client.guess_hotend_heater() == "extruder0");
    }

    SECTION("Priority: extruder substring wins over hotend") {
        client.set_heaters({"heater_bed", "hotend", "extruder2"});
        REQUIRE(client.guess_hotend_heater() == "extruder2");
    }

    SECTION("Priority: hotend wins over e0") {
        client.set_heaters({"heater_bed", "e0", "hotend"});
        REQUIRE(client.guess_hotend_heater() == "hotend");
    }

    SECTION("Multiple extruder substring matches: returns first found") {
        client.set_heaters({"heater_bed", "extruder1", "extruder2"});
        REQUIRE(client.guess_hotend_heater() == "extruder1");
    }

    SECTION("No match: returns empty string") {
        client.set_heaters({"heater_bed", "chamber_heater"});
        REQUIRE(client.guess_hotend_heater() == "");
    }

    SECTION("Empty heaters list: returns empty string") {
        client.set_heaters({});
        REQUIRE(client.guess_hotend_heater() == "");
    }

    SECTION("Case sensitivity: 'Extruder' does not match 'extruder'") {
        client.set_heaters({"heater_bed", "Extruder"});
        REQUIRE(client.guess_hotend_heater() == "");
    }

    SECTION("Edge case: e0 matches as substring in 'e0'") {
        client.set_heaters({"heater_bed", "e0"});
        REQUIRE(client.guess_hotend_heater() == "e0");
    }
}

// ============================================================================
// guess_bed_sensor() Tests
// ============================================================================

TEST_CASE("MoonrakerClient::guess_bed_sensor", "[moonraker][hardware][guessing]") {
    HardwareGuessingFixture client;

    SECTION("Heater found: returns heater name (heaters have built-in thermistors)") {
        client.set_heaters({"extruder", "heater_bed"});
        client.set_sensors({"temperature_sensor chamber"});
        REQUIRE(client.guess_bed_sensor() == "heater_bed");
    }

    SECTION("Heater found: returns heated_bed") {
        client.set_heaters({"extruder", "heated_bed"});
        client.set_sensors({});
        REQUIRE(client.guess_bed_sensor() == "heated_bed");
    }

    SECTION("No heater, sensor match: temperature_sensor bed_temp") {
        client.set_heaters({"extruder"});
        client.set_sensors({"temperature_sensor chamber", "temperature_sensor bed_temp"});
        REQUIRE(client.guess_bed_sensor() == "temperature_sensor bed_temp");
    }

    SECTION("No heater, sensor substring: bed_thermistor") {
        client.set_heaters({"extruder"});
        client.set_sensors({"chamber", "bed_thermistor"});
        REQUIRE(client.guess_bed_sensor() == "bed_thermistor");
    }

    SECTION("Priority: heater wins over sensor with 'bed'") {
        client.set_heaters({"extruder", "heater_bed"});
        client.set_sensors({"temperature_sensor bed_auxiliary"});
        REQUIRE(client.guess_bed_sensor() == "heater_bed");
    }

    SECTION("Multiple sensors with 'bed': returns first found") {
        client.set_heaters({"extruder"});
        client.set_sensors({"chamber", "bed_sensor1", "bed_sensor2"});
        REQUIRE(client.guess_bed_sensor() == "bed_sensor1");
    }

    SECTION("No heater, no sensor match: returns empty string") {
        client.set_heaters({"extruder"});
        client.set_sensors({"temperature_sensor chamber", "temperature_sensor mcu"});
        REQUIRE(client.guess_bed_sensor() == "");
    }

    SECTION("Empty heaters and sensors: returns empty string") {
        client.set_heaters({});
        client.set_sensors({});
        REQUIRE(client.guess_bed_sensor() == "");
    }

    SECTION("Heater substring match: custom_bed_heater returns from heater") {
        client.set_heaters({"extruder", "custom_bed_heater"});
        client.set_sensors({"temperature_sensor bed_aux"});
        REQUIRE(client.guess_bed_sensor() == "custom_bed_heater");
    }
}

// ============================================================================
// guess_hotend_sensor() Tests
// ============================================================================

TEST_CASE("MoonrakerClient::guess_hotend_sensor", "[moonraker][hardware][guessing]") {
    HardwareGuessingFixture client;

    SECTION("Heater found: returns extruder (heaters have built-in thermistors)") {
        client.set_heaters({"heater_bed", "extruder"});
        client.set_sensors({"temperature_sensor chamber"});
        REQUIRE(client.guess_hotend_sensor() == "extruder");
    }

    SECTION("Heater found: returns extruder0") {
        client.set_heaters({"heater_bed", "extruder0"});
        client.set_sensors({});
        REQUIRE(client.guess_hotend_sensor() == "extruder0");
    }

    SECTION("No heater, sensor match: temperature_sensor extruder_aux") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"temperature_sensor chamber", "temperature_sensor extruder_aux"});
        REQUIRE(client.guess_hotend_sensor() == "temperature_sensor extruder_aux");
    }

    SECTION("No heater, sensor priority: extruder wins over hotend") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"hotend_thermistor", "extruder_aux"});
        REQUIRE(client.guess_hotend_sensor() == "extruder_aux");
    }

    SECTION("No heater, sensor priority: hotend wins over e0") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"e0_temp", "hotend_thermistor"});
        REQUIRE(client.guess_hotend_sensor() == "hotend_thermistor");
    }

    SECTION("No heater, sensor match: e0_thermistor") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"chamber", "e0_thermistor"});
        REQUIRE(client.guess_hotend_sensor() == "e0_thermistor");
    }

    SECTION("Priority: heater wins over sensor with 'extruder'") {
        client.set_heaters({"heater_bed", "extruder"});
        client.set_sensors({"temperature_sensor extruder_aux"});
        REQUIRE(client.guess_hotend_sensor() == "extruder");
    }

    SECTION("Multiple extruder sensors: returns first found") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"chamber", "extruder_sensor1", "extruder_sensor2"});
        REQUIRE(client.guess_hotend_sensor() == "extruder_sensor1");
    }

    SECTION("No heater, no sensor match: returns empty string") {
        client.set_heaters({"heater_bed"});
        client.set_sensors({"temperature_sensor chamber", "temperature_sensor mcu"});
        REQUIRE(client.guess_hotend_sensor() == "");
    }

    SECTION("Empty heaters and sensors: returns empty string") {
        client.set_heaters({});
        client.set_sensors({});
        REQUIRE(client.guess_hotend_sensor() == "");
    }

    SECTION("Heater substring match: hotend_heater returns from heater") {
        client.set_heaters({"heater_bed", "hotend_heater"});
        client.set_sensors({"temperature_sensor hotend_aux"});
        REQUIRE(client.guess_hotend_sensor() == "hotend_heater");
    }

    SECTION("Heater e0 match: e0 returns from heater") {
        client.set_heaters({"heater_bed", "e0"});
        client.set_sensors({"temperature_sensor e0_aux"});
        REQUIRE(client.guess_hotend_sensor() == "e0");
    }
}

// ============================================================================
// Real-world Mock Data Tests
// ============================================================================

TEST_CASE("MoonrakerClientMock hardware guessing", "[moonraker][hardware][guessing][mock]") {
    SECTION("VORON_24 mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.discover_printer([]() {});  // Populate hardware

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("VORON_TRIDENT mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_TRIDENT);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("CREALITY_K1 mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::CREALITY_K1);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("FLASHFORGE_AD5M mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("GENERIC_COREXY mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("GENERIC_BEDSLINGER mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }

    SECTION("MULTI_EXTRUDER mock data") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::MULTI_EXTRUDER);
        mock.discover_printer([]() {});

        REQUIRE(mock.guess_bed_heater() == "heater_bed");
        REQUIRE(mock.guess_hotend_heater() == "extruder");  // Base extruder prioritized
        REQUIRE(mock.guess_bed_sensor() == "heater_bed");
        REQUIRE(mock.guess_hotend_sensor() == "extruder");
    }
}

// ============================================================================
// Edge Cases and Complex Scenarios
// ============================================================================

TEST_CASE("Hardware guessing edge cases", "[moonraker][hardware][guessing][edge]") {
    HardwareGuessingFixture client;

    SECTION("Bed heater with unusual name: 'bed' only") {
        client.set_heaters({"extruder", "bed"});
        REQUIRE(client.guess_bed_heater() == "bed");
    }

    SECTION("Hotend heater with unusual name: 'hotend' only") {
        client.set_heaters({"heater_bed", "hotend"});
        REQUIRE(client.guess_hotend_heater() == "hotend");
    }

    SECTION("Names containing but not matching: 'extruder_bed' for bed") {
        client.set_heaters({"extruder", "extruder_bed"});
        REQUIRE(client.guess_bed_heater() == "extruder_bed");
    }

    SECTION("Names containing but not matching: 'bed_extruder' for hotend") {
        client.set_heaters({"heater_bed", "bed_extruder"});
        REQUIRE(client.guess_hotend_heater() == "bed_extruder");
    }

    SECTION("Multiple priority levels: all types present for bed") {
        client.set_heaters({"bed_custom", "heated_bed", "heater_bed", "extruder"});
        REQUIRE(client.guess_bed_heater() == "heater_bed");
    }

    SECTION("Multiple priority levels: all types present for hotend") {
        client.set_heaters({"e0_custom", "hotend", "extruder1", "extruder0", "extruder", "heater_bed"});
        REQUIRE(client.guess_hotend_heater() == "extruder");
    }

    SECTION("Sensor-only configuration: no heaters, sensors present") {
        client.set_heaters({});
        client.set_sensors({"bed_sensor", "extruder_sensor"});
        REQUIRE(client.guess_bed_sensor() == "bed_sensor");
        REQUIRE(client.guess_hotend_sensor() == "extruder_sensor");
    }

    SECTION("Mixed heater/sensor names: heater_bed_sensor") {
        client.set_heaters({"extruder", "heater_bed_sensor"});
        // Should match as bed heater (contains 'bed')
        REQUIRE(client.guess_bed_heater() == "heater_bed_sensor");
    }

    SECTION("Numeric variants: extruder10 vs extruder1") {
        client.set_heaters({"heater_bed", "extruder10", "extruder1"});
        // extruder1 appears first in iteration order
        REQUIRE(client.guess_hotend_heater() == "extruder10");
    }

    SECTION("Empty string in hardware list") {
        client.set_heaters({"", "heater_bed", "extruder"});
        REQUIRE(client.guess_bed_heater() == "heater_bed");
        REQUIRE(client.guess_hotend_heater() == "extruder");
    }

    SECTION("Very long hardware name") {
        std::string long_name = "heater_bed_with_very_long_descriptive_name_for_testing_purposes";
        client.set_heaters({"extruder", long_name});
        REQUIRE(client.guess_bed_heater() == long_name);
    }

    SECTION("Unicode/special characters: should not match (if present)") {
        client.set_heaters({"extruder", "heater_bed_™"});
        // Should still match as substring contains 'bed'
        REQUIRE(client.guess_bed_heater() == "heater_bed_™");
    }
}

// Restore warning state
#pragma GCC diagnostic pop
