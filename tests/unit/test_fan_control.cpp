// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_gcode.h"
#include "printer_fan_state.h"

#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Fan gcode generation", "[fan][gcode]") {
    SECTION("bare fan uses M106 S<value>") {
        auto gcode = helix::fan_gcode("fan", 100.0);
        REQUIRE(gcode == "M106 S255");
    }

    SECTION("bare fan at 50% uses M106 S128") {
        auto gcode = helix::fan_gcode("fan", 50.0);
        REQUIRE(gcode == "M106 S128");
    }

    SECTION("bare fan off uses M107") {
        auto gcode = helix::fan_gcode("fan", 0.0);
        REQUIRE(gcode == "M107");
    }

    SECTION("output_pin fan0 uses M106 P0") {
        auto gcode = helix::fan_gcode("output_pin fan0", 100.0);
        REQUIRE(gcode == "M106 P0 S255");
    }

    SECTION("output_pin fan2 at 50% uses M106 P2 S128") {
        auto gcode = helix::fan_gcode("output_pin fan2", 50.0);
        REQUIRE(gcode == "M106 P2 S128");
    }

    SECTION("output_pin fan0 off uses M107 P0") {
        auto gcode = helix::fan_gcode("output_pin fan0", 0.0);
        REQUIRE(gcode == "M107 P0");
    }

    SECTION("output_pin non-fan uses SET_PIN") {
        auto gcode = helix::fan_gcode("output_pin aux_blower", 75.0);
        REQUIRE(gcode == "SET_PIN PIN=aux_blower VALUE=0.75");
    }

    SECTION("fan_generic uses SET_FAN_SPEED") {
        auto gcode = helix::fan_gcode("fan_generic aux_fan", 50.0);
        REQUIRE(gcode == "SET_FAN_SPEED FAN=aux_fan SPEED=0.50");
    }

    SECTION("heater_fan uses SET_FAN_SPEED") {
        auto gcode = helix::fan_gcode("heater_fan hotend_fan", 100.0);
        REQUIRE(gcode == "SET_FAN_SPEED FAN=hotend_fan SPEED=1.00");
    }
}

// K2's output_pin extruder_fan is driven with the hotend heater. Fan Control
// shows it like a heater_fan, with no dial, and no command is ever built for it.
TEST_CASE("A hotend-area output_pin fan is read-only and never commanded", "[fan][gcode]") {
    lv_init_safe();
    helix::PrinterFanState fans;
    fans.init_subjects(/*register_xml=*/false);
    fans.init_fans({"fan", "output_pin extruder_fan", "output_pin hotend_fan",
                    "output_pin Heatbreak_Fan", "output_pin aux_fan", "output_pin fan1"},
                   helix::FanRoleConfig{});

    auto find = [&fans](const std::string& name) -> const helix::FanInfo* {
        for (const auto& f : fans.get_fans()) {
            if (f.object_name == name) {
                return &f;
            }
        }
        return nullptr;
    };

    for (const char* name :
         {"output_pin extruder_fan", "output_pin hotend_fan", "output_pin Heatbreak_Fan"}) {
        INFO(name);
        const auto* f = find(name);
        REQUIRE(f != nullptr);
        CHECK(f->type == helix::FanType::HEATER_FAN);
        CHECK_FALSE(f->is_controllable);
        CHECK(helix::fan_gcode(name, 50.0).empty());
    }
    // Other output_pin fans stay commandable.
    REQUIRE(find("output_pin aux_fan") != nullptr);
    CHECK(find("output_pin aux_fan")->is_controllable);
    REQUIRE(find("output_pin fan1") != nullptr);
    CHECK(find("output_pin fan1")->type == helix::FanType::OUTPUT_PIN_FAN);
    CHECK(helix::fan_gcode("output_pin aux_fan", 50.0) == "SET_PIN PIN=aux_fan VALUE=0.50");
}
