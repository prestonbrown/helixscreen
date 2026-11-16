// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_discovery_info.cpp
 * @brief PrinterDiscovery::apply_printer_info, shared by the desktop and ESP32 discovery
 *
 * The fixture is a Kalico Voron's printer.info result, trimmed to the fields read here.
 */

#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

TEST_CASE("apply_printer_info takes the Klipper version and Kalico marker",
          "[printer_discovery][versions]") {
    const auto result = nlohmann::json::parse(R"({
        "app": "Kalico", "state": "ready", "hostname": "bigtreetech-cb1",
        "software_version": "v2026.07.00-0-g30c65e09-dirty"
    })");

    PrinterDiscovery hw;
    hw.apply_printer_info(result);

    CHECK(hw.software_version() == "v2026.07.00-0-g30c65e09-dirty");
    CHECK(hw.is_kalico());
    CHECK(std::string(hw.firmware_name()) == "Kalico");
}

TEST_CASE("apply_printer_info leaves a Klipper printer unmarked", "[printer_discovery][versions]") {
    const auto result =
        nlohmann::json::parse(R"({"app": "Klipper", "software_version": "v0.13.0-100-gabcdef12"})");

    PrinterDiscovery hw;
    hw.apply_printer_info(result);

    CHECK(hw.software_version() == "v0.13.0-100-gabcdef12");
    CHECK_FALSE(hw.is_kalico());
}

TEST_CASE("apply_printer_info reads a missing or malformed version as unknown",
          "[printer_discovery][versions]") {
    PrinterDiscovery hw;
    hw.set_software_version("stale");

    hw.apply_printer_info(nlohmann::json::parse(R"({"software_version": 42, "app": 7})"));
    CHECK(hw.software_version() == "unknown");
    CHECK_FALSE(hw.is_kalico());

    hw.set_software_version("stale");
    hw.apply_printer_info(nlohmann::json::array());
    CHECK(hw.software_version() == "unknown");
}

TEST_CASE("apply_printer_info survives parse_objects only when applied after it",
          "[printer_discovery][versions]") {
    PrinterDiscovery hw;
    hw.apply_printer_info(nlohmann::json::parse(R"({"app": "Kalico", "software_version": "v1"})"));
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    CHECK(hw.software_version().empty());
    CHECK_FALSE(hw.is_kalico());

    hw.apply_printer_info(nlohmann::json::parse(R"({"app": "Kalico", "software_version": "v1"})"));
    CHECK(hw.software_version() == "v1");
    CHECK(hw.is_kalico());
}
