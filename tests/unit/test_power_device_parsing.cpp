// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_types.h"
#include "../../include/power_device_parse.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

// The parser MoonrakerAPI::get_power_devices() runs on every device_power reply
// (src/api/moonraker_api_power.cpp). Called directly rather than restated here,
// so a change to the shape rules cannot pass this file by.
using helix::parse_power_devices;

// ============================================================================
// Power Device Parsing Tests (prestonbrown/helixscreen#466)
// ============================================================================

TEST_CASE("Power device parsing uses device name from array elements",
          "[power][parsing][moonraker]") {
    // Moonraker returns devices as an array, not an object.
    // Each element has a "device" field with the device name.
    // Bug #466: code was using .items() which returns array indices as keys.
    json response = json::parse(R"({
        "result": {
            "devices": [
                {
                    "device": "Printer",
                    "status": "on",
                    "type": "homeassistant",
                    "locked_while_printing": true
                },
                {
                    "device": "Automatic Power Off",
                    "status": "off",
                    "type": "klipper_device",
                    "locked_while_printing": false
                },
                {
                    "device": "Cooldown after Print",
                    "status": "on",
                    "type": "klipper_device",
                    "locked_while_printing": false
                },
                {
                    "device": "Unload Filament after Print",
                    "status": "off",
                    "type": "klipper_device",
                    "locked_while_printing": false
                }
            ]
        }
    })");

    auto devices = parse_power_devices(response);

    REQUIRE(devices.size() == 4);

    // Device names must be the actual names, not array indices
    CHECK(devices[0].device == "Printer");
    CHECK(devices[0].type == "homeassistant");
    CHECK(devices[0].status == "on");
    CHECK(devices[0].locked_while_printing == true);

    CHECK(devices[1].device == "Automatic Power Off");
    CHECK(devices[1].type == "klipper_device");
    CHECK(devices[1].status == "off");
    CHECK(devices[1].locked_while_printing == false);

    CHECK(devices[2].device == "Cooldown after Print");
    CHECK(devices[3].device == "Unload Filament after Print");
}

TEST_CASE("Power device parsing handles empty device list", "[power][parsing][moonraker]") {
    json response = json::parse(R"({"result": {"devices": []}})");
    auto devices = parse_power_devices(response);
    REQUIRE(devices.empty());
}

TEST_CASE("Power device parsing skips entries with empty device name",
          "[power][parsing][moonraker]") {
    json response = json::parse(R"({
        "result": {
            "devices": [
                {"device": "valid_device", "status": "on", "type": "gpio"},
                {"status": "off", "type": "gpio"},
                {"device": "", "status": "off", "type": "gpio"}
            ]
        }
    })");

    auto devices = parse_power_devices(response);
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].device == "valid_device");
}

TEST_CASE("Power device parsing handles missing result key", "[power][parsing][moonraker]") {
    json response = json::parse(R"({"error": "not found"})");
    auto devices = parse_power_devices(response);
    REQUIRE(devices.empty());
}

// The three rules the file's old private copy of the parser did not have, and
// so could never have caught drifting away from.

TEST_CASE("Power device parsing survives a JSON null field", "[power][parsing][moonraker]") {
    // nlohmann's .value("type", "unknown") throws type_error.302 on a null, which
    // would abort the loop and lose every device in the reply, not just this one.
    json response = json::parse(R"({
        "result": {
            "devices": [
                {"device": "printer", "status": null, "type": null,
                 "locked_while_printing": null},
                {"device": "lamp", "status": "on", "type": "gpio"}
            ]
        }
    })");

    auto devices = parse_power_devices(response);
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].device == "printer");
    CHECK(devices[0].type == "unknown");
    CHECK(devices[0].status == "off");
    CHECK(devices[0].locked_while_printing == false);
    CHECK(devices[1].device == "lamp");
}

TEST_CASE("Power device parsing rejects an object-shaped device list",
          "[power][parsing][moonraker]") {
    // #466 in its original form: iterating an object hands back keys, so the
    // devices would come back named after their JSON keys instead of "device".
    json response = json::parse(R"({
        "result": {"devices": {"printer": {"status": "on", "type": "gpio"}}}
    })");
    CHECK(parse_power_devices(response).empty());
}

TEST_CASE("Power device parsing skips a non-object array element", "[power][parsing][moonraker]") {
    json response = json::parse(R"({
        "result": {"devices": ["printer", 42, null,
                               {"device": "lamp", "status": "on", "type": "gpio"}]}
    })");

    auto devices = parse_power_devices(response);
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].device == "lamp");
}
