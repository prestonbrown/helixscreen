// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pins the whole subscription map MoonrakerDiscoverySequence::build_subscription_objects
// produces for each filament system, keys and field lists, against files under
// tests/fixtures/subscription_parity/. A narrowed field list that silently becomes
// `null` makes Klipper serialize every internal field on every tick, so values are
// compared, not just keys.
//
// HELIX_WRITE_GOLDENS=1 rewrites the files from the current builder output.

#include "../helix_test_fixture.h"
#include "../test_helpers/golden_json.h"
#include "ams_backend.h"
#include "hardware_object_table.h"
#include "moonraker_client_mock.h"
#include "moonraker_discovery_sequence.h"
#include "printer_discovery.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::MoonrakerDiscoverySequence;
using helix::PrinterDiscovery;
using helix::test::check_against_golden;
using nlohmann::json;

namespace {

/// One printer's object list.
struct Printer {
    json all_objects = json::array({"extruder", "heater_bed", "fan"});
    json status_claim;

    Printer& obj(const std::string& name) {
        all_objects.push_back(name);
        return *this;
    }
    Printer& afc(const std::string& name) {
        return obj(name);
    }
    Printer& sensor(const std::string& name) {
        return obj(name);
    }

    json build() const {
        PrinterDiscovery hw;
        hw.parse_objects(all_objects);
        if (!status_claim.is_null()) {
            hw.settle_status_claims(status_claim);
        }
        return MoonrakerDiscoverySequence::build_subscription_objects(hw);
    }
};

} // namespace

TEST_CASE("subscription objects are unchanged for every backend fixture", "[subscription_parity]") {
    SECTION("plain printer") {
        Printer p;
        check_against_golden("subscription_parity", "plain", p.build());
    }

    SECTION("Happy Hare") {
        Printer p;
        p.obj("mmu");
        check_against_golden("subscription_parity", "happy_hare", p.build());
    }

    SECTION("AFC BoxTurtle") {
        Printer p;
        p.afc("AFC")
            .afc("AFC_BoxTurtle Turtle_1")
            .afc("AFC_stepper lane1")
            .afc("AFC_stepper lane2")
            .afc("AFC_hub Turtle_1")
            .afc("AFC_extruder extruder")
            .afc("AFC_buffer Turtle_1");
        check_against_golden("subscription_parity", "afc_boxturtle", p.build());
    }

    SECTION("ACE with named instances") {
        Printer p;
        p.obj("ace_instance_0").obj("ace_instance_1");
        check_against_golden("subscription_parity", "ace_named", p.build());
    }

    SECTION("ACE published as the plain ace object") {
        Printer p;
        p.obj("ace");
        check_against_golden("subscription_parity", "ace_plain", p.build());
    }

    SECTION("ACE published as filament_hub") {
        Printer p;
        p.obj("filament_hub");
        check_against_golden("subscription_parity", "ace_filament_hub", p.build());
    }

    SECTION("CFS") {
        Printer p;
        p.obj("box");
        check_against_golden("subscription_parity", "cfs", p.build());
    }

    SECTION("Snapmaker with the batch macro") {
        Printer p;
        p.obj("filament_detect").obj("gcode_macro Auto_Feeding_Batch");
        check_against_golden("subscription_parity", "snapmaker_batch", p.build());
    }

    SECTION("Snapmaker without the batch macro") {
        Printer p;
        p.obj("filament_detect");
        check_against_golden("subscription_parity", "snapmaker_no_batch", p.build());
    }

    SECTION("QIDI Box") {
        Printer p;
        p.obj("box_stepper slot0").obj("box_stepper slot1").obj("box_extras");
        check_against_golden("subscription_parity", "qidi_box", p.build());
    }

#if HELIX_HAS_IFS
    SECTION("AD5X IFS on the module firmware") {
        Printer p;
        p.obj("ifs")
            .obj("ifs_materials")
            .obj("save_variables")
            .sensor("filament_switch_sensor lane1");
        check_against_golden("subscription_parity", "ad5x_ifs_module", p.build());
    }

    SECTION("AD5X IFS on native Z-Mod") {
        Printer p;
        p.sensor("filament_motion_sensor ifs_motion_sensor")
            .sensor("zmod_ifs_switch_sensor head_switch_sensor")
            .sensor("zmod_ifs_motion_sensor ifs_motion_sensor")
            .obj("zmod_ifs")
            .obj("zmod_color")
            .obj("gcode_macro END_CHANGE_FILAMENT")
            .obj("save_variables");
        check_against_golden("subscription_parity", "ad5x_ifs_zmod", p.build());
    }
#endif // HELIX_HAS_IFS

    SECTION("OpenAMS") {
        Printer p;
        p.obj("oams_manager");
        {
            // Until its status claim settles, an oams_manager may be someone
            // else's object: no backend subscribes it.
            PrinterDiscovery unclaimed;
            unclaimed.parse_objects(p.all_objects);
            CHECK(helix::AmsBackend::required_status_objects(unclaimed).empty());
        }
        p.status_claim =
            json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}};
        check_against_golden("subscription_parity", "openams", p.build());
    }

    SECTION("klipper-toolchanger") {
        Printer p;
        p.obj("toolchanger").obj("tool T0").obj("tool T1").obj("extruder1");
        check_against_golden("subscription_parity", "toolchanger", p.build());
    }

    SECTION("Bondtech INDX") {
        Printer p;
        p.obj("indx")
            .obj("save_variables")
            .obj("gcode_macro TOOL_POSITIONS")
            .obj("gcode_macro _TOOLCHANGE_VARS")
            .obj("gcode_macro CHANGE_TOOL")
            .obj("gcode_macro PARK_TOOL")
            .obj("gcode_macro LOAD_FILAMENT")
            .obj("gcode_macro T0")
            .obj("gcode_macro T1");
        check_against_golden("subscription_parity", "indx", p.build());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "backend sensor prefixes subscribe as filament sensors",
                 "[subscription_parity]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerDiscoverySequence discovery(mock);
    discovery.parse_objects(
        json::array({"extruder", "filament_switch_sensor runout", "filament_motion_sensor encoder",
                     "zmod_ifs_switch_sensor head", "zmod_ifs_motion_sensor ifs_motion_sensor",
                     "zmod_ifs", "zmod_ifs_status"}));

    const std::vector<std::string> sensors{
        "filament_switch_sensor runout", "filament_motion_sensor encoder",
        "zmod_ifs_switch_sensor head", "zmod_ifs_motion_sensor ifs_motion_sensor"};

    const json fields = json::array({"filament_detected", "enabled", "detection_count"});
    const json subs = MoonrakerDiscoverySequence::build_subscription_objects(discovery.hardware());
    for (const auto& name : sensors) {
        INFO(name);
        CHECK(subs.value(name, json()) == fields);
    }
    CHECK_FALSE(subs.contains("zmod_ifs_status"));
}

TEST_CASE_METHOD(HelixTestFixture, "discovery and the hardware snapshot share one heater rule",
                 "[subscription_parity][printer_discovery]") {
    const json objects = json::array(
        {"heater_bed", "extruder", "extruder1", "extruder_stepper belt", "heater_generic chamber",
         "heater_genericx", "heater_fan hotend", "temperature_sensor mcu", "extruderx",
         "extruder_offset_calibration", "heater_bedx", "fan"});
    std::vector<std::string> expected;
    for (const auto& o : objects) {
        if (helix::hw::is_heater(helix::hw::classify_object(o.get<std::string>()).kind)) {
            expected.push_back(o.get<std::string>());
        }
    }
    REQUIRE(expected == std::vector<std::string>{"heater_bed", "extruder", "extruder1",
                                                 "heater_generic chamber"});

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerDiscoverySequence sequence(mock);
    sequence.parse_objects(objects);
    CHECK(sequence.hardware().heaters() == expected);

    PrinterDiscovery hw;
    hw.parse_objects(objects);
    CHECK(hw.heaters() == expected);
}
