// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_air_modes.cpp
 * @brief The chamber air circulation mode capability: which printers offer
 *        which modes, and the gcode each mode sends.
 */

#include "chamber_air_modes.h"
#include "printer_discovery.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace air = helix::chamber_air;
using air::AirMode;

namespace {

PrinterDiscovery printer_with(std::initializer_list<const char*> extra) {
    json objects = json::array({"gcode_move", "toolhead", "extruder"});
    for (const char* o : extra) {
        objects.push_back(o);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A Creator 5 Pro on Z-Mod with the named air circulation macros.
PrinterDiscovery zmod_c5(std::initializer_list<const char*> macros) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "zmod", "zmod_color"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

PrinterDiscovery full_zmod_c5() {
    return zmod_c5(
        {"AIR_CIRCULATION_INTERNAL", "AIR_CIRCULATION_EXTERNAL", "AIR_CIRCULATION_STOP"});
}

} // namespace

TEST_CASE("chamber air: a Z-Mod Creator 5 offers all three modes", "[chamber_air]") {
    const PrinterDiscovery c5 = full_zmod_c5();
    CHECK(air::supported(c5));
    CHECK(air::modes(c5) ==
          std::vector<AirMode>{AirMode::Recirculate, AirMode::Exhaust, AirMode::Off});
    CHECK_FALSE(air::provider_name(c5).empty());
}

TEST_CASE("chamber air: each mode sends its own macro", "[chamber_air]") {
    const PrinterDiscovery c5 = full_zmod_c5();
    CHECK(air::mode_gcode(c5, AirMode::Recirculate) == "AIR_CIRCULATION_INTERNAL");
    CHECK(air::mode_gcode(c5, AirMode::Exhaust) == "AIR_CIRCULATION_EXTERNAL");
    CHECK(air::mode_gcode(c5, AirMode::Off) == "AIR_CIRCULATION_STOP");
}

TEST_CASE("chamber air: a mode whose macro is missing is not offered", "[chamber_air]") {
    const PrinterDiscovery c5 = zmod_c5({"AIR_CIRCULATION_STOP"});
    CHECK(air::supported(c5));
    CHECK(air::modes(c5) == std::vector<AirMode>{AirMode::Off});
    CHECK_FALSE(air::offers(c5, AirMode::Exhaust));
    CHECK(air::mode_gcode(c5, AirMode::Exhaust).empty());
}

TEST_CASE("chamber air: printers without the macros offer nothing", "[chamber_air]") {
    const PrinterDiscovery bare_c5 = zmod_c5({});
    CHECK_FALSE(air::supported(bare_c5));
    CHECK(air::modes(bare_c5).empty());

    // The macros are the capability on any printer, and only the defined
    // ones are offered.
    const PrinterDiscovery other =
        printer_with({"gcode_macro AIR_CIRCULATION_INTERNAL", "gcode_macro AIR_CIRCULATION_STOP"});
    CHECK(air::supported(other));
    CHECK(air::mode_gcode(other, AirMode::Recirculate) == "AIR_CIRCULATION_INTERNAL");
    CHECK(air::mode_gcode(other, AirMode::Exhaust).empty());

    const PrinterDiscovery bare = printer_with({});
    CHECK_FALSE(air::supported(bare));
    CHECK(air::provider_name(bare).empty());
}

TEST_CASE("chamber air: mode ids round-trip and reject anything else", "[chamber_air]") {
    for (AirMode m : {AirMode::Recirculate, AirMode::Exhaust, AirMode::Off}) {
        CHECK(air::parse_mode_id(air::mode_id(m)) == m);
    }
    CHECK(air::parse_mode_id("exhaust") == AirMode::Exhaust);
    CHECK_FALSE(air::parse_mode_id("").has_value());
    CHECK_FALSE(air::parse_mode_id("Exhaust").has_value());
}

// --- A detachable accessory with mode readback, targets and owned fans -------

namespace {

/// The object set a U1 reports around its purifier.
PrinterDiscovery u1() {
    return printer_with({"purifier", "fm175xx_reader", "tool"});
}

json purifier(json fields) {
    return json{{"purifier", std::move(fields)}};
}

} // namespace

TEST_CASE("chamber air: the U1 top cover offers all three modes and reads them back",
          "[chamber_air]") {
    const PrinterDiscovery hw = u1();
    CHECK(air::modes(hw) ==
          std::vector<AirMode>{AirMode::Recirculate, AirMode::Exhaust, AirMode::Off});
    CHECK(air::presence_is_live(hw));
    CHECK(air::reports_mode(hw));
    CHECK(air::takes_targets(hw));
    CHECK(air::guard_floor_pct(hw) == 10);
}

TEST_CASE("chamber air: a purifier object alone is not the U1", "[chamber_air]") {
    const PrinterDiscovery hw = printer_with({"purifier"});
    CHECK_FALSE(air::supported(hw));
    CHECK_FALSE(air::presence_is_live(hw));
    CHECK(air::required_status_objects(hw).empty());
    CHECK_FALSE(air::read_status(hw, purifier({{"mode", 1}})).mode.has_value());
}

TEST_CASE("chamber air: U1 mode gcode carries the targets", "[chamber_air]") {
    const PrinterDiscovery hw = u1();
    const air::ModeTargets t{38.0, 70, 50};
    CHECK(air::mode_gcode(hw, AirMode::Exhaust, t) ==
          "SET_PURIFIER_MODE MODE=1 FAN_SPEED=0.70 DESIRE_TEMP=38 ALARM_TEMP=39");
    CHECK(air::mode_gcode(hw, AirMode::Recirculate, t) ==
          "SET_PURIFIER_MODE MODE=3 FAN_SPEED=0.50");
    CHECK(air::mode_gcode(hw, AirMode::Off, t) == "SET_PURIFIER_MODE MODE=0");
    // The firmware's own defaults when nothing is stored.
    CHECK(air::mode_gcode(hw, AirMode::Exhaust) ==
          "SET_PURIFIER_MODE MODE=1 FAN_SPEED=0.60 DESIRE_TEMP=42 ALARM_TEMP=43");
}

TEST_CASE("chamber air: U1 status maps preheat and hot chamber to recirculate", "[chamber_air]") {
    const PrinterDiscovery hw = u1();
    CHECK(air::read_status(hw, purifier({{"mode", 0}})).mode == AirMode::Off);
    CHECK(air::read_status(hw, purifier({{"mode", 1}})).mode == AirMode::Exhaust);
    CHECK(air::read_status(hw, purifier({{"mode", 2}})).mode == AirMode::Recirculate);
    CHECK(air::read_status(hw, purifier({{"mode", 3}})).mode == AirMode::Recirculate);
    CHECK_FALSE(air::read_status(hw, purifier({{"mode", 7}})).mode.has_value());
    CHECK_FALSE(air::read_status(hw, purifier({{"mode", "1"}})).mode.has_value());
}

TEST_CASE("chamber air: a 2.x status frame fills every reading", "[chamber_air]") {
    const PrinterDiscovery hw = u1();
    const air::Reading r =
        air::read_status(hw, purifier({{"power_detected", true},
                                       {"mode", 1},
                                       {"critical_chamber_temp", 43.0},
                                       {"inner_fan_work_time", 7200.5},
                                       {"exhaust_fan", {{"speed", 0.6}, {"speed_threshold", 0.4}}},
                                       {"inner_fan", {{"speed", 0.0}, {"speed_threshold", 0.3}}}}));
    CHECK(r.present == true);
    CHECK(r.mode == AirMode::Exhaust);
    CHECK(r.critical_temp_c == Catch::Approx(43.0));
    CHECK(r.filter_seconds == Catch::Approx(7200.5));
    CHECK(r.exhaust_speed == Catch::Approx(0.6));
    CHECK(r.recirculate_speed == Catch::Approx(0.0));
    CHECK(r.exhaust_threshold == Catch::Approx(0.4));
    CHECK(r.recirculate_threshold == Catch::Approx(0.3));
}

TEST_CASE("chamber air: a frame without the object says nothing", "[chamber_air]") {
    const air::Reading r = air::read_status(u1(), json{{"toolhead", json::object()}});
    CHECK_FALSE(r.present.has_value());
    CHECK_FALSE(r.mode.has_value());
    CHECK_FALSE(r.exhaust_speed.has_value());
    CHECK_FALSE(r.filter_seconds.has_value());
    CHECK_FALSE(r.critical_temp_c.has_value());
}

TEST_CASE("chamber air: the 1.3 layout has no mode or filter counter", "[chamber_air]") {
    const air::Reading r =
        air::read_status(u1(), purifier({{"power_detected", true},
                                         {"exhaust_fan", {{"speed", 0.5}}},
                                         {"inner_fan", {{"speed", 0.0}, {"work", 12}}}}));
    CHECK(r.present == true);
    CHECK_FALSE(r.mode.has_value());
    CHECK_FALSE(r.filter_seconds.has_value());
    CHECK(r.exhaust_speed == Catch::Approx(0.5));
}

TEST_CASE("chamber air: U1 subscribes the fields it reads", "[chamber_air]") {
    const json subs = air::required_status_objects(u1());
    REQUIRE(subs.contains("purifier"));
    const json& fields = subs["purifier"];
    for (const char* f : {"power_detected", "mode", "critical_chamber_temp", "inner_fan_work_time",
                          "exhaust_fan", "inner_fan"}) {
        CHECK(std::find(fields.begin(), fields.end(), f) != fields.end());
    }
}

TEST_CASE("chamber air: owned fans name their republished equivalents", "[chamber_air]") {
    const auto fans = air::owned_fans(u1());
    REQUIRE(fans.size() == 2);
    CHECK(fans[0].role == air::FanRole::Exhaust);
    CHECK(std::string(fans[0].equivalent_object) == "fan_generic exhaust_fan");
    CHECK(fans[1].role == air::FanRole::Recirculate);
    CHECK(std::string(fans[1].equivalent_object) == "fan_generic circulation_fan");
    CHECK(std::string(fans[0].id) != fans[1].id);
}

TEST_CASE("chamber air: fan and filter gcode", "[chamber_air]") {
    const PrinterDiscovery hw = u1();
    CHECK(air::fan_speed_gcode(hw, air::FanRole::Recirculate, 50) ==
          "SET_PURIFIER FAN=inner SPEED=0.50");
    CHECK(air::fan_speed_gcode(hw, air::FanRole::Exhaust, 0) ==
          "SET_PURIFIER FAN=exhaust SPEED=0.00");
    CHECK(air::fan_speed_gcode(hw, air::FanRole::Exhaust, 140) ==
          "SET_PURIFIER FAN=exhaust SPEED=1.00");
    CHECK(air::filter_reset_gcode(hw) == "SET_PURIFIER FAN=inner WORK=0");
}

TEST_CASE("chamber air: Z-Mod keeps action-only modes", "[chamber_air]") {
    const PrinterDiscovery c5 = full_zmod_c5();
    CHECK_FALSE(air::presence_is_live(c5));
    CHECK_FALSE(air::reports_mode(c5));
    CHECK_FALSE(air::takes_targets(c5));
    CHECK(air::owned_fans(c5).empty());
    CHECK(air::guard_floor_pct(c5) == 0);
    CHECK(air::filter_reset_gcode(c5).empty());
    CHECK(air::fan_speed_gcode(c5, air::FanRole::Exhaust, 50).empty());
    CHECK(air::required_status_objects(c5).empty());
    // Targets are ignored by macros.
    CHECK(air::mode_gcode(c5, AirMode::Exhaust, air::ModeTargets{30.0, 10, 10}) ==
          "AIR_CIRCULATION_EXTERNAL");
}

TEST_CASE("chamber air: each mode drives one fan and keeps the other off", "[chamber_air]") {
    using air::FanRole;
    CHECK(air::driven_fan(AirMode::Exhaust) == FanRole::Exhaust);
    CHECK(air::disabled_fan(AirMode::Exhaust) == FanRole::Recirculate);
    CHECK(air::driven_fan(AirMode::Recirculate) == FanRole::Recirculate);
    CHECK(air::disabled_fan(AirMode::Recirculate) == FanRole::Exhaust);
    CHECK_FALSE(air::driven_fan(AirMode::Off).has_value());
    CHECK_FALSE(air::disabled_fan(AirMode::Off).has_value());
}

TEST_CASE("chamber air: fan guard", "[chamber_air]") {
    using air::FanGuardAction;
    using air::FanRole;
    SECTION("no mode or Off applies the request as asked") {
        auto g = air::guard_fan_change(std::nullopt, FanRole::Exhaust, 0, 60, 40, 10);
        CHECK(g.action == FanGuardAction::Apply);
        CHECK(g.pct == 0);
        g = air::guard_fan_change(AirMode::Off, FanRole::Recirculate, 5, 60, 40, 10);
        CHECK(g.action == FanGuardAction::Apply);
        CHECK(g.pct == 5);
    }
    SECTION("the fan the mode keeps off refuses any speed") {
        auto g = air::guard_fan_change(AirMode::Exhaust, FanRole::Recirculate, 30, 0, 30, 10);
        CHECK(g.action == FanGuardAction::Blocked);
        // Turning it off is never refused.
        g = air::guard_fan_change(AirMode::Exhaust, FanRole::Recirculate, 0, 0, 30, 10);
        CHECK(g.action == FanGuardAction::Apply);
    }
    SECTION("the driven fan never goes below the floor") {
        auto g = air::guard_fan_change(AirMode::Exhaust, FanRole::Exhaust, 0, 10, std::nullopt, 10);
        CHECK(g.action == FanGuardAction::Apply);
        CHECK(g.pct == 10);
    }
    SECTION("lowering the driven fan below its threshold asks first") {
        auto g = air::guard_fan_change(AirMode::Recirculate, FanRole::Recirculate, 20, 60, 30, 10);
        CHECK(g.action == FanGuardAction::Confirm);
        CHECK(g.pct == 20);
        // Raising toward the threshold, or staying at or above it, does not ask.
        g = air::guard_fan_change(AirMode::Recirculate, FanRole::Recirculate, 25, 20, 30, 10);
        CHECK(g.action == FanGuardAction::Apply);
        g = air::guard_fan_change(AirMode::Recirculate, FanRole::Recirculate, 30, 60, 30, 10);
        CHECK(g.action == FanGuardAction::Apply);
    }
}
