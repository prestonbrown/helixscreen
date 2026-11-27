// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_ifs_module_env.cpp
 * @brief The ifs-module mock stages the unknown-lane states a toolhead unload
 *        has to find its lane from: lane presence, the ifs_at_hub record and the
 *        toolhead switch, and keeps the record in step with its macros (#1324).
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using TA = MoonrakerClientMockTestAccess;

namespace {

std::vector<int> lanes(const MoonrakerClientMock& c) {
    return TA::ifs_module_status(c)["loaded_channels"].get<std::vector<int>>();
}

int at_hub(const MoonrakerClientMock& c) {
    return TA::ifs_module_vars(c)["variables"]["ifs_at_hub"].get<int>();
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "ifs-module mock stages lanes, the at-hub record and the head",
                 "[mock][ifs_module][1324]") {
    ScopedEnv ams("HELIX_MOCK_AMS", "ifs-module");
    ScopedEnv unaccounted("HELIX_MOCK_IFS_UNACCOUNTED", "1");

    SECTION("the env vars set presence, the record and the head switch") {
        ScopedEnv lane_env("HELIX_MOCK_IFS_LANES", "1,3");
        ScopedEnv hub_env("HELIX_MOCK_IFS_AT_HUB", "2");
        MoonrakerClientMock client;
        CHECK(lanes(client) == std::vector<int>{1, 3});
        CHECK(at_hub(client) == 2);
        CHECK(TA::ifs_module_vars(client)["variables"]["ifs_loaded"].get<int>() == 0);
        CHECK(TA::ifs_module_head_loaded(client));
    }
    SECTION("none empties every lane; an out-of-range hub lane is ignored") {
        ScopedEnv lane_env("HELIX_MOCK_IFS_LANES", "none");
        ScopedEnv hub_env("HELIX_MOCK_IFS_AT_HUB", "7");
        MoonrakerClientMock client;
        CHECK(lanes(client).empty());
        CHECK(at_hub(client) == 0);
    }
    SECTION("unset, all four lanes hold filament and no lane holds the hub") {
        MoonrakerClientMock client;
        CHECK(lanes(client) == std::vector<int>{1, 2, 3, 4});
        CHECK(at_hub(client) == 0);
    }
    SECTION("the extruder clear empties the head and keeps the record") {
        ScopedEnv hub_env("HELIX_MOCK_IFS_AT_HUB", "2");
        MoonrakerClientMock client;
        client.gcode_script("_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220");
        CHECK_FALSE(TA::ifs_module_head_loaded(client));
        CHECK(at_hub(client) == 2);
    }
    SECTION("loads claim the hub; unload and eject of that lane release it") {
        MoonrakerClientMock client;
        client.gcode_script("IFS_LOAD SLOT=3");
        CHECK(at_hub(client) == 3);
        client.gcode_script("IFS_UNLOAD SLOT=3");
        CHECK(at_hub(client) == 0);
        client.gcode_script("T1");
        CHECK(at_hub(client) == 2);
        client.gcode_script("IFS_EJECT SLOT=4");
        CHECK(at_hub(client) == 2);
        client.gcode_script("IFS_EJECT SLOT=2");
        CHECK(at_hub(client) == 0);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "ifs-module mock states each slot's identity from plugin API 1.12",
                 "[mock][ifs_module][lane_writer]") {
    ScopedEnv ams("HELIX_MOCK_AMS", "ifs-module");
    const char* edit = "IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3 SPOOL_ID=42 "
                       "VENDOR=\"Polymaker\" NAME=\"Matte PLA\" FILAMENT_ID= BED_TEMP=60 TEMP=";

    SECTION("1.12 states the edit's identity, null for what it cleared or never had") {
        ScopedEnv api("HELIX_MOCK_IFS_PLUGIN_API", "1.12");
        MoonrakerClientMock client;
        const nlohmann::json before = TA::ifs_module_materials(client)["slots"]["2"];
        CHECK(before.contains("spool_id"));
        CHECK(before["spool_id"].is_null());

        client.gcode_script(edit);
        const nlohmann::json slots = TA::ifs_module_materials(client)["slots"];
        CHECK(slots["2"]["spool_id"] == 42);
        CHECK(slots["2"]["vendor"] == "Polymaker");
        CHECK(slots["2"]["name"] == "Matte PLA");
        CHECK(slots["2"]["filament_id"].is_null());
        CHECK(slots["2"]["bed_temp"] == 60);
        CHECK(slots["1"]["vendor"].is_null());

        // A parameter left out keeps the field.
        client.gcode_script("IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3 SPOOL_ID=43");
        const nlohmann::json kept = TA::ifs_module_materials(client)["slots"]["2"];
        CHECK(kept["spool_id"] == 43);
        CHECK(kept["bed_temp"] == 60);
    }
    SECTION("an older plugin API states type, color and temperature only") {
        ScopedEnv api("HELIX_MOCK_IFS_PLUGIN_API", "1.11");
        MoonrakerClientMock client;
        client.gcode_script(edit);
        CHECK_FALSE(TA::ifs_module_materials(client)["slots"]["2"].contains("spool_id"));
    }
}
