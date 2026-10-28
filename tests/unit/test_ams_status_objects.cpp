// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each filament backend names the Klipper status objects it needs pushed, and
// AmsBackend::required_status_objects merges them. A backend answers only for
// its own printer: a lost gate subscribes one vendor's objects on another's
// machine.

#include "ams_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_openams.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "printer_discovery.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsBackend;
using helix::AmsBackendAce;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::AmsBackendOpenAms;
using helix::AmsBackendQidi;
using helix::AmsBackendSnapmaker;
using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

PrinterDiscovery discover(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

std::set<std::string> keys(const json& objects) {
    std::set<std::string> out;
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        out.insert(it.key());
    }
    return out;
}

} // namespace

TEST_CASE("each backend's required_status_objects matches its own printer and is empty off it",
          "[ams][ams_status_objects]") {
    const PrinterDiscovery plain = discover({"extruder", "heater_bed", "fan"});

    SECTION("a plain printer gets nothing from any backend") {
        CHECK(AmsBackendHappyHare::required_status_objects(plain).empty());
        CHECK(AmsBackendAce::required_status_objects(plain).empty());
        CHECK(helix::printer::AmsBackendCfs::required_status_objects(plain).empty());
        CHECK(AmsBackendSnapmaker::required_status_objects(plain).empty());
        CHECK(AmsBackendQidi::required_status_objects(plain).empty());
        CHECK(AmsBackendAfc::required_status_objects(plain).empty());
        CHECK(AmsBackendOpenAms::required_status_objects(plain).empty());
        CHECK(AmsBackend::required_status_objects(plain).empty());
    }

    SECTION("Happy Hare subscribes the mmu field allowlist") {
        const json objects = AmsBackendHappyHare::required_status_objects(discover({"mmu"}));
        REQUIRE(keys(objects) == std::set<std::string>{"mmu"});
        REQUIRE(objects["mmu"].is_array());
        CHECK(objects["mmu"].size() == 43);
        CHECK(std::find(objects["mmu"].begin(), objects["mmu"].end(), "leds") !=
              objects["mmu"].end());
    }

    SECTION("CFS subscribes box and motor_control") {
        const json objects =
            helix::printer::AmsBackendCfs::required_status_objects(discover({"box"}));
        CHECK(keys(objects) == std::set<std::string>{"box", "motor_control"});
    }

    SECTION("QIDI subscribes box_extras, save_variables and the controller's box field") {
        const json objects =
            AmsBackendQidi::required_status_objects(discover({"box_stepper slot0", "box_extras"}));
        CHECK(keys(objects) ==
              std::set<std::string>{"box_extras", "multi_color_controller", "save_variables"});
        CHECK(objects["multi_color_controller"] == json::array({"box"}));
    }

    SECTION("ACE subscribes every detected object name") {
        const json objects =
            AmsBackendAce::required_status_objects(discover({"ace_instance_0", "ace_instance_1"}));
        CHECK(keys(objects) == std::set<std::string>{"ace_instance_0", "ace_instance_1"});
        const json single = AmsBackendAce::required_status_objects(discover({"filament_hub"}));
        CHECK(keys(single) == std::set<std::string>{"filament_hub"});
    }

    SECTION("Snapmaker adds the batch macro only when the firmware has it") {
        const json without =
            AmsBackendSnapmaker::required_status_objects(discover({"filament_detect"}));
        CHECK(without.size() == 9);
        CHECK(without.contains("filament_feed left"));
        CHECK(without.contains("filament_motion_sensor e3_filament"));

        const json with = AmsBackendSnapmaker::required_status_objects(
            discover({"filament_detect", "gcode_macro Auto_Feeding_Batch"}));
        REQUIRE(with.size() == 10);
        CHECK(with.contains("gcode_macro Auto_Feeding_Batch"));
    }

    SECTION("AFC finds its own objects in the discovery") {
        const PrinterDiscovery hw =
            discover({"AFC", "AFC_stepper lane1", "AFC_hub Turtle_1", "mmu_unrelated", "extruder"});
        const json objects = AmsBackendAfc::required_status_objects(hw);
        CHECK(keys(objects) ==
              std::set<std::string>{"AFC", "AFC_stepper lane1", "AFC_hub Turtle_1"});
        CHECK(objects == AmsBackendAfc::required_status_objects(std::vector<std::string>{
                             "AFC", "AFC_stepper lane1", "AFC_hub Turtle_1"}));
    }
}

TEST_CASE("the dispatcher merges every backend", "[ams][ams_status_objects]") {
    SECTION("Happy Hare printer") {
        const PrinterDiscovery hw = discover({"mmu", "extruder"});
        CHECK(AmsBackend::required_status_objects(hw) ==
              AmsBackendHappyHare::required_status_objects(hw));
    }

    SECTION("Snapmaker printer") {
        const PrinterDiscovery hw = discover({"filament_detect", "extruder"});
        CHECK(AmsBackend::required_status_objects(hw) ==
              AmsBackendSnapmaker::required_status_objects(hw));
    }

    SECTION("AD5X IFS subscribes by name with no field narrowing") {
        const PrinterDiscovery hw =
            discover({"ifs", "ifs_materials", "zmod_ifs", "gcode_macro END_CHANGE_FILAMENT"});
        const json objects = AmsBackend::required_status_objects(hw);
        CHECK(keys(objects) == std::set<std::string>{"save_variables", "ifs", "ifs_materials",
                                                     "zmod_ifs",
                                                     "gcode_macro END_CHANGE_FILAMENT"});
        CHECK(objects["ifs"].is_null());
    }

    SECTION("OpenAMS needs its status claim settled") {
        PrinterDiscovery hw = discover({"oams_manager"});
        CHECK(AmsBackend::required_status_objects(hw).empty());
        hw.settle_status_claims(
            json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}});
        CHECK(keys(AmsBackend::required_status_objects(hw)) ==
              std::set<std::string>{"oams_manager"});
    }

    SECTION("CFS, QIDI and ACE printers") {
        CHECK(keys(AmsBackend::required_status_objects(discover({"box"}))) ==
              std::set<std::string>{"box", "motor_control"});
        CHECK(keys(AmsBackend::required_status_objects(discover({"box_stepper slot0"}))) ==
              std::set<std::string>{"box_extras", "multi_color_controller", "save_variables"});
        CHECK(keys(AmsBackend::required_status_objects(discover({"ace"}))) ==
              std::set<std::string>{"ace"});
    }
}
