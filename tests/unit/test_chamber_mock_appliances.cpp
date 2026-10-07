// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_mock_appliances.cpp
 * @brief Mock-client materialization of the appliance chamber-heater backends
 *        (issue #1290).
 *
 * Both appliance backends live here because they share this file's frame and
 * object-list helpers; a second file would redefine them.
 *
 * DragonBreath — the full trio.
 *
 * HELIX_MOCK_OBJECTS="heater_generic dragonbreath dragonbreath
 * output_pin dragonbreath_filter" must yield:
 *   1. an object list with all three objects (tokenizer: `output_pin` is a
 *      prefix; a bare `dragonbreath` diagnostics token starts a NEW object
 *      instead of appending to the completed heater),
 *   2. discovery resolving the chamber heater to `heater_generic
 *      dragonbreath` via the backend registry — including the mock's
 *      chamber-key cache, which must consult the registry, not
 *      find("chamber"),
 *   3. status frames carrying the diagnostics object (nominal payload:
 *      fault false, fan_percent 0, ptc_temp > 0) plus the heater object with
 *      temperature/target,
 *   4. a configfile answer of max_temp 75 for the heater section,
 *   5. SET_PIN round-trip on the filter pin,
 *   6. a HELIX_MOCK_DRAGONBREATH_FAULT=1 hook flipping fault + fault_reason.
 *
 * Stock Panda Breath — a heater and a status object, no filter pin. Its schema
 * carries no fault and no element temperature, so what the mock has to get
 * right is the link field and which control loop holds the heater.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// First parameter of a notify_status_update frame, or a null json when the
/// notification is not shaped like one. nlohmann's CONST operator[] asserts on
/// a missing key rather than throwing, so a malformed frame here would abort
/// the run instead of failing a check; every step of the walk is fallible.
json first_status_param(const json& notification) {
    auto params = notification.find("params");
    if (params == notification.end() || !params->is_array() || params->empty()) {
        return json{};
    }
    return (*params)[0];
}

constexpr const char* TRIO_ENV =
    "heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter";

bool object_list_contains(const json& response, const std::string& object) {
    if (!response.contains("result") || !response["result"].contains("objects")) {
        return false;
    }
    const auto& objects = response["result"]["objects"];
    return std::find(objects.begin(), objects.end(), json(object)) != objects.end();
}

json query_object_list(MoonrakerClientMock& client) {
    json response;
    client.send_jsonrpc("printer.objects.list", json::object(),
                        [&response](const json& r) { response = r; });
    return response;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "mock materializes dragonbreath trio", "[chamber][mock]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", TRIO_ENV);
    MoonrakerClientMock client;

    SECTION("object list contains exactly the three parsed objects") {
        const json response = query_object_list(client);
        CHECK(object_list_contains(response, "heater_generic dragonbreath"));
        CHECK(object_list_contains(response, "dragonbreath"));
        CHECK(object_list_contains(response, "output_pin dragonbreath_filter"));
        // The two ways the accumulator can mis-parse: appending the bare
        // diagnostics token to the heater, and flushing a bare prefix.
        CHECK_FALSE(object_list_contains(response, "heater_generic dragonbreath dragonbreath"));
        CHECK_FALSE(object_list_contains(response, "output_pin"));
    }

    SECTION("discovery resolves chamber heater via backend registry") {
        const PrinterDiscovery hw = client.hardware();
        REQUIRE(hw.has_chamber_heater());
        CHECK(hw.chamber_heater_name() == "heater_generic dragonbreath");
        CHECK(hw.chamber_heater_backend_id() == "dragonbreath");
        CHECK(hw.chamber_diagnostics_object() == "dragonbreath");
        CHECK(hw.chamber_filter_fan_pin() == "output_pin dragonbreath_filter");
        // The registry-driven override must have retired the default
        // VORON_24 "heater_generic chamber" (keyword 100 beats dragonbreath's
        // 95, so a stale entry would still WIN the discovery pick).
        const auto& heaters = hw.heaters();
        CHECK(std::find(heaters.begin(), heaters.end(), "heater_generic chamber") == heaters.end());
        // The status-key cache follows the registry, not find("chamber").
        CHECK(MoonrakerClientMockTestAccess::chamber_heater_status_key(client) ==
              "heater_generic dragonbreath");
    }

    SECTION("first status frame carries diagnostics + heater") {
        json frame;
        client.register_notify_update(
            [&frame](const json& notification) { frame = first_status_param(notification); });
        MoonrakerClientMockTestAccess::dispatch_initial_state(client);

        REQUIRE(frame.is_object());
        REQUIRE(frame.contains("dragonbreath"));
        const json& diag = frame["dragonbreath"];
        CHECK(diag.at("fault").get<bool>() == false);
        CHECK(diag.at("fan_percent").get<int>() == 0);
        CHECK(diag.at("ptc_temp").get<double>() > 0.0);
        CHECK(diag.at("inhibited").get<bool>() == false);
        CHECK(diag.at("connected").get<bool>() == true);
        CHECK(diag.at("mode").get<std::string>() == "off"); // no target set yet
        // fault_reason is JSON null in the nominal frame
        CHECK((diag.at("fault_reason").is_null() ||
               diag.at("fault_reason").get<std::string>().empty()));

        REQUIRE(frame.contains("heater_generic dragonbreath"));
        CHECK(frame["heater_generic dragonbreath"]["temperature"].get<double>() > 0.0);
        CHECK(frame["heater_generic dragonbreath"].contains("target"));

        REQUIRE(frame.contains("output_pin dragonbreath_filter"));
        CHECK(frame["output_pin dragonbreath_filter"]["value"].get<double>() == 0.0);
    }

    SECTION("configfile answers max_temp 75 for the heater section") {
        json response;
        client.send_jsonrpc(
            "printer.objects.query",
            json{{"objects", json::object({{"configfile", json::array({"config"})}})}},
            [&response](const json& r) { response = r; });
        REQUIRE(response.contains("result"));
        REQUIRE(response["result"]["status"].contains("configfile"));
        const json& config = response["result"]["status"]["configfile"]["config"];
        REQUIRE(config.contains("heater_generic dragonbreath"));
        CHECK(config.at("heater_generic dragonbreath")["max_temp"].get<double>() == 75.0);
    }

    SECTION("SET_PIN toggles the filter pin in subsequent frames") {
        json frame;
        client.register_notify_update(
            [&frame](const json& notification) { frame = first_status_param(notification); });

        REQUIRE(client.gcode_script("SET_PIN PIN=dragonbreath_filter VALUE=1") == 0);
        REQUIRE(frame.contains("output_pin dragonbreath_filter"));
        CHECK(frame["output_pin dragonbreath_filter"]["value"].get<double>() == 1.0);

        REQUIRE(client.gcode_script("SET_PIN PIN=dragonbreath_filter VALUE=0") == 0);
        CHECK(frame["output_pin dragonbreath_filter"]["value"].get<double>() == 0.0);

        // And the synthesized diagnostics frame follows the pin (fan_percent).
        frame = json();
        MoonrakerClientMockTestAccess::dispatch_initial_state(client);
        REQUIRE(frame.contains("dragonbreath"));
        CHECK(frame["dragonbreath"]["fan_percent"].get<int>() == 0);
    }

    SECTION("device heating runs the fan on its own while the pin stays 0") {
        json frame;
        client.register_notify_update(
            [&frame](const json& notification) { frame = first_status_param(notification); });

        // Measured on the rig (issue #1290): a chamber target makes the
        // firmware run the filter fan itself; our pin never moves.
        REQUIRE(client.gcode_script("SET_HEATER_TEMPERATURE HEATER=dragonbreath TARGET=45") == 0);
        frame = json();
        MoonrakerClientMockTestAccess::dispatch_initial_state(client);

        REQUIRE(frame.contains("dragonbreath"));
        CHECK(frame["dragonbreath"]["fan_percent"].get<int>() == 100);
        CHECK(frame["dragonbreath"]["fan_reason"].get<std::string>() == "heater");
        REQUIRE(frame.contains("output_pin dragonbreath_filter"));
        CHECK(frame["output_pin dragonbreath_filter"]["value"].get<double>() == 0.0);
    }

    SECTION("SET_HEATER_TEMPERATURE uses the bare backend heater name") {
        json frame;
        client.register_notify_update(
            [&frame](const json& notification) { frame = first_status_param(notification); });

        REQUIRE(client.gcode_script("SET_HEATER_TEMPERATURE HEATER=dragonbreath TARGET=45") == 0);
        REQUIRE(frame.contains("heater_generic dragonbreath"));
        CHECK(frame["heater_generic dragonbreath"]["target"].get<double>() == 45.0);

        // The diagnostics mode field tracks the target.
        frame = json();
        MoonrakerClientMockTestAccess::dispatch_initial_state(client);
        REQUIRE(frame.contains("dragonbreath"));
        CHECK(frame["dragonbreath"]["mode"].get<std::string>() == "power_on");
    }

    SECTION("set_heaters rebuild keeps backend surfaces answering") {
        // rebuild_hardware_from_lists() re-parses from the discovery lists,
        // which cannot express the bare diagnostics object or the filter pin
        // (parse_objects classifies neither). Without the rebuild preserving
        // them, chamber status emission and objects.list silently go dark.
        client.set_heaters({"heater_bed", "extruder", "heater_generic dragonbreath"});

        const json response = query_object_list(client);
        CHECK(object_list_contains(response, "heater_generic dragonbreath"));
        CHECK(object_list_contains(response, "dragonbreath"));
        CHECK(object_list_contains(response, "output_pin dragonbreath_filter"));

        json frame;
        client.register_notify_update(
            [&frame](const json& notification) { frame = first_status_param(notification); });
        MoonrakerClientMockTestAccess::dispatch_initial_state(client);
        REQUIRE(frame.contains("dragonbreath"));
        CHECK(frame["dragonbreath"]["ptc_temp"].get<double>() > 0.0);
        CHECK(frame.contains("output_pin dragonbreath_filter"));
    }
}

TEST_CASE_METHOD(HelixTestFixture, "mock dragonbreath fault hook", "[chamber][mock]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", TRIO_ENV);
    ScopedEnv fault_env("HELIX_MOCK_DRAGONBREATH_FAULT", "1");
    MoonrakerClientMock client;

    json frame;
    client.register_notify_update(
        [&frame](const json& notification) { frame = first_status_param(notification); });
    MoonrakerClientMockTestAccess::dispatch_initial_state(client);

    REQUIRE(frame.contains("dragonbreath"));
    CHECK(frame["dragonbreath"]["fault"].get<bool>() == true);
    CHECK_FALSE(frame["dragonbreath"]["fault_reason"].get<std::string>().empty());
}

// HELIX_MOCK_DRAGONBREATH_OFFLINE=1 drops the appliance off its radio link:
// every synthesized frame reports connected: false instead of true. One client
// crosses the transition through the test-access setter.
TEST_CASE_METHOD(HelixTestFixture, "mock dragonbreath offline hook", "[chamber][mock]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", TRIO_ENV);
    MoonrakerClientMock client;

    json frame;
    client.register_notify_update(
        [&frame](const json& notification) { frame = first_status_param(notification); });
    MoonrakerClientMockTestAccess::dispatch_initial_state(client);

    REQUIRE(frame.contains("dragonbreath"));
    CHECK(frame["dragonbreath"]["connected"].get<bool>() == true);

    MoonrakerClientMockTestAccess::set_dragonbreath_offline(client, true);
    frame = json{};
    MoonrakerClientMockTestAccess::dispatch_initial_state(client);

    REQUIRE(frame.contains("dragonbreath"));
    CHECK(frame["dragonbreath"]["connected"].get<bool>() == false);
}

TEST_CASE_METHOD(HelixTestFixture, "HELIX_MOCK_OBJECTS legacy shapes still parse",
                 "[chamber][mock]") {
    SECTION("temperature_fan chamber stays a single object and wins the chamber key") {
        ScopedEnv objects_env("HELIX_MOCK_OBJECTS", "temperature_fan chamber");
        MoonrakerClientMock client;

        const json response = query_object_list(client);
        CHECK(object_list_contains(response, "temperature_fan chamber"));
        // "chamber" appended to the prefix, not flushed as a bare object.
        CHECK_FALSE(object_list_contains(response, "chamber"));
        CHECK(MoonrakerClientMockTestAccess::chamber_heater_status_key(client) ==
              "temperature_fan chamber");
    }

    SECTION("default mock without env keeps heater_generic chamber") {
        MoonrakerClientMock client;
        CHECK(MoonrakerClientMockTestAccess::chamber_heater_status_key(client) ==
              "heater_generic chamber");
    }
}

// ============================================================================
// Stock Panda Breath
// ============================================================================

namespace {
constexpr const char* STOCK_PAIR_ENV = "heater_generic panda_breath panda_breath";

/// The stock diagnostics object out of the first synthesized status frame.
json first_stock_diagnostics(MoonrakerClientMock& client) {
    json frame;
    client.register_notify_update(
        [&frame](const json& notification) { frame = first_status_param(notification); });
    MoonrakerClientMockTestAccess::dispatch_initial_state(client);
    if (!frame.is_object() || !frame.contains("panda_breath")) {
        return json{};
    }
    return frame["panda_breath"];
}
} // namespace

TEST_CASE_METHOD(HelixTestFixture, "mock materializes the stock panda_breath pair",
                 "[chamber][mock][1290]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", STOCK_PAIR_ENV);
    MoonrakerClientMock client;

    SECTION("discovery resolves the heater and its diagnostics, and no filter pin") {
        const PrinterDiscovery hw = client.hardware();
        REQUIRE(hw.has_chamber_heater());
        CHECK(hw.chamber_heater_name() == "heater_generic panda_breath");
        CHECK(hw.chamber_heater_backend_id() == "panda_breath");
        CHECK(hw.chamber_diagnostics_object() == "panda_breath");
        CHECK(hw.chamber_filter_fan_pin().empty());
        CHECK(MoonrakerClientMockTestAccess::chamber_heater_status_key(client) ==
              "heater_generic panda_breath");
    }

    SECTION("the synthesized frame carries the stock schema and nothing else") {
        const json diag = first_stock_diagnostics(client);
        REQUIRE(diag.is_object());
        CHECK(diag.at("connected").get<bool>() == true);
        CHECK(diag.at("work_on").get<bool>() == false); // no target, no auto
        CHECK(diag.at("filament_drying_active").get<bool>() == false);
        CHECK(diag.at("temperature").get<double>() > 0.0);
        // Whole degrees, as the appliance reports them.
        CHECK(diag.at("temperature").get<double>() ==
              std::floor(diag.at("temperature").get<double>()));
        // Fields the stock binding does not have must not be invented here, or
        // the mock would exercise a parse path no device can reach.
        for (const char* absent : {"fault", "inhibited", "fault_reason", "ptc_temp", "fan_percent",
                                   "fan_reason", "protocol_error"}) {
            CAPTURE(absent);
            CHECK_FALSE(diag.contains(absent));
        }
    }
}

// The appliance holding its own auto target while the Klipper target reads 0 is
// the state the rig sits in at rest, and the only one that raises External.
TEST_CASE_METHOD(HelixTestFixture, "HELIX_MOCK_PANDA_BREATH_AUTO drives the device's own loop",
                 "[chamber][mock][1290]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", STOCK_PAIR_ENV);
    ScopedEnv auto_env("HELIX_MOCK_PANDA_BREATH_AUTO", "1");
    MoonrakerClientMock client;

    const json diag = first_stock_diagnostics(client);
    REQUIRE(diag.is_object());
    CHECK(diag.at("work_mode").get<int>() == 1);
    CHECK(diag.at("work_on").get<bool>() == true);
    CHECK(diag.at("auto_enabled").get<bool>() == true);
    CHECK(diag.at("device_target").get<double>() > 0.0); // the appliance's own target
    CHECK(diag.at("target").get<double>() == 0.0);       // ours is still zero
    CHECK(diag.at("connected").get<bool>() == true);
}

// The WebSocket to the appliance can drop while Klipper keeps answering for the
// heater section it owns.
TEST_CASE_METHOD(HelixTestFixture, "HELIX_MOCK_PANDA_BREATH_OFFLINE drops the link",
                 "[chamber][mock][1290]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", STOCK_PAIR_ENV);
    ScopedEnv offline_env("HELIX_MOCK_PANDA_BREATH_OFFLINE", "1");
    MoonrakerClientMock client;

    const json diag = first_stock_diagnostics(client);
    REQUIRE(diag.is_object());
    CHECK(diag.at("connected").get<bool>() == false);
    // The heater section still answers — that is what makes the banner the only
    // signal the reading is dead.
    CHECK(diag.at("temperature").get<double>() > 0.0);
}

// The stock binding's drying cycle: DRY_START runs work_mode 3 with the
// requested target and length and a countdown, DRY_STOP ends it (#1299).
TEST_CASE_METHOD(HelixTestFixture, "mock runs the stock drying cycle",
                 "[chamber][mock][dryer][1299]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", STOCK_PAIR_ENV);
    MoonrakerClientMock client;

    REQUIRE(client.gcode_script("PANDA_BREATH_DRY_START TEMP=55 HOURS=4") == 0);
    json diag = first_stock_diagnostics(client);
    REQUIRE(diag.is_object());
    CHECK(diag.at("filament_drying_active").get<bool>() == true);
    CHECK(diag.at("work_mode").get<int>() == 3);
    CHECK(diag.at("work_on").get<bool>() == true);
    CHECK(diag.at("filament_temp").get<int>() == 55);
    CHECK(diag.at("filament_timer").get<int>() == 4);
    CHECK(diag.at("remaining_seconds").get<int>() > 0);
    CHECK(diag.at("remaining_seconds").get<int>() <= 4 * 3600);

    REQUIRE(client.gcode_script("PANDA_BREATH_DRY_STOP") == 0);
    diag = first_stock_diagnostics(client);
    CHECK(diag.at("filament_drying_active").get<bool>() == false);
    CHECK(diag.at("remaining_seconds").get<int>() == 0);
}

TEST_CASE_METHOD(HelixTestFixture, "mock simulates a non-chamber heater_generic",
                 "[mock][heater_generic]") {
    ScopedEnv objects_env("HELIX_MOCK_OBJECTS", "heater_generic filament_dryer");
    MoonrakerClientMock client;

    // A dryer is not the chamber: the persona's own chamber heater keeps the slot.
    CHECK(client.hardware().chamber_heater_name() != "heater_generic filament_dryer");

    json frame;
    client.register_notify_update(
        [&frame](const json& notification) { frame = first_status_param(notification); });
    MoonrakerClientMockTestAccess::dispatch_initial_state(client);
    REQUIRE(frame.contains("heater_generic filament_dryer"));
    CHECK(frame["heater_generic filament_dryer"].at("target").get<double>() == 0.0);

    frame = json{};
    REQUIRE(client.gcode_script("SET_HEATER_TEMPERATURE HEATER=filament_dryer TARGET=55") == 0);
    REQUIRE(frame.contains("heater_generic filament_dryer"));
    CHECK(frame["heater_generic filament_dryer"].at("target").get<double>() == 55.0);
}
