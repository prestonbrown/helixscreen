// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fan_tachometers.cpp
 * @brief Which fan a separate tachometer module's slot measures.
 *
 * The settings below are the [fan_feedback] and fan sections of Creality's
 * shipped configs (K2_Series_Klipper config/F008*, K1_Series_Klipper
 * config/K1C_*), in the shape Klipper's configfile.settings reports them.
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "fan_tachometers.h"
#include "printer_fan_state.h"
#include "printer_state.h"

#include <algorithm>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;
using helix::fan_tach::TachMap;

namespace {

json k2_plus_settings() {
    return {
        {"fan_feedback",
         {{"fan0_pin", json::array({"PC6"})},
          {"fan1_pin", "nozzle_mcu:PA12"},
          {"fan2_pin", "nozzle_mcu:PC13"}}},
        {"heater_fan hotend_fan", {{"pin", "multi_pin:heater_fans"}, {"heater", {"extruder"}}}},
        {"heater_fan chamber_fan", {{"pin", "!PB14"}, {"heater", {"chamber_heater"}}}},
        {"heater_generic chamber_heater", {{"heater_pin", "PC12"}}},
        {"output_pin fan0", {{"pin", "!nozzle_mcu:PB15"}}},
        {"output_pin fan1", {{"pin", "PA0"}}},
        {"output_pin fan2", {{"pin", "multi_pin:heater_fan2"}}},
        {"temperature_fan chamber_fan", {{"pin", "PA0"}}},
    };
}

json k1c_settings() {
    return {
        {"fan_feedback", {{"fan0_pin", "nozzle_mcu:PB4"}, {"fan1_pin", "PC6"}}},
        {"heater_fan hotend_fan", {{"pin", "multi_pin:heater_fans"}, {"heater", {"extruder"}}}},
        {"output_pin fan0", {{"pin", "!nozzle_mcu: PB8"}}},
        {"output_pin fan1", {{"pin", "PC0"}}},
        {"output_pin fan2", {{"pin", "PB1"}}},
        {"temperature_fan chamber_fan", {{"pin", "PC0"}}},
    };
}

std::optional<std::string> fan_for(const TachMap& map, const std::string& field) {
    auto it = std::find_if(map.begin(), map.end(), [&](const auto& s) { return s.field == field; });
    if (it == map.end())
        return std::nullopt;
    return it->fan;
}

} // namespace

TEST_CASE("Fan tachometers: K2 series fixed slots", "[fan][fan_feedback][fan_tach]") {
    const TachMap map = fan_tach::map_from_config(k2_plus_settings());

    CHECK(fan_for(map, "fan2_speed") == std::optional<std::string>("output_pin fan0"));
    CHECK(fan_for(map, "fan1_speed") == std::optional<std::string>("heater_fan hotend_fan"));
    CHECK(fan_for(map, "fan0_speed") == std::optional<std::string>("heater_fan chamber_fan"));
    CHECK_FALSE(fan_for(map, "fan3_speed"));
    CHECK_FALSE(fan_for(map, "fan4_speed"));
    for (const auto& src : map) {
        CHECK(src.object == "fan_feedback");
        CHECK(src.fan != "output_pin fan2");
    }
}

TEST_CASE("Fan tachometers: K1 series maps the throat fan and leaves the backplane fan",
          "[fan][fan_feedback][fan_tach]") {
    const TachMap map = fan_tach::map_from_config(k1c_settings());

    CHECK(fan_for(map, "fan0_speed") == std::optional<std::string>("heater_fan hotend_fan"));
    CHECK_FALSE(fan_for(map, "fan1_speed"));
    CHECK(map.size() == 1);
}

TEST_CASE("Fan tachometers: K1 model-fan tach maps to the part fan",
          "[fan][fan_feedback][fan_tach]") {
    json s = k1c_settings();
    s["fan_feedback"] = {{"fan1_pin", "^nozzle_mcu: PB3"}};

    const TachMap map = fan_tach::map_from_config(s);
    // The first configured pin reports as fan0_speed whatever its option index.
    CHECK(fan_for(map, "fan0_speed") == std::optional<std::string>("output_pin fan0"));
    CHECK(map.size() == 1);
}

TEST_CASE("Fan tachometers: a K1 with a third tach pin is not read as a K2",
          "[fan][fan_feedback][fan_tach]") {
    json s = k1c_settings();
    s["fan_feedback"]["fan2_pin"] = "nozzle_mcu:PB3";
    // Creality's K1 module reports PB4 and PC6 as fan0/fan1 and drops the third pin;
    // the K2 slots would put the backplane fan's RPM on the hotend fan.
    CHECK(fan_tach::map_from_config(s).empty());
}

TEST_CASE("Fan tachometers: blank pins are not configured slots", "[fan][fan_feedback][fan_tach]") {
    SECTION("a blank fan2_pin leaves the K1 layout") {
        json s = k1c_settings();
        s["fan_feedback"]["fan2_pin"] = " ";
        const TachMap map = fan_tach::map_from_config(s);
        CHECK(fan_for(map, "fan0_speed") == std::optional<std::string>("heater_fan hotend_fan"));
        CHECK(map.size() == 1);
    }
    SECTION("a blank K1 slot does not shift the later pins") {
        json s = k1c_settings();
        s["fan_feedback"] = {{"fan0_pin", ""}, {"fan1_pin", "nozzle_mcu:PB4"}, {"fan3_pin", "PC6"}};
        const TachMap map = fan_tach::map_from_config(s);
        CHECK(fan_for(map, "fan0_speed") == std::optional<std::string>("heater_fan hotend_fan"));
        CHECK_FALSE(fan_for(map, "fan1_speed"));
    }
}

TEST_CASE("Fan tachometers: two candidate fans for one slot leave it unmapped",
          "[fan][fan_feedback][fan_tach]") {
    json s = k2_plus_settings();
    s["heater_fan aux"] = {{"pin", "PA1"}, {"heater", {"extruder"}}};
    const TachMap map = fan_tach::map_from_config(s);
    CHECK_FALSE(fan_for(map, "fan1_speed"));
    CHECK(fan_for(map, "fan2_speed") == std::optional<std::string>("output_pin fan0"));
}

TEST_CASE("Fan tachometers: a slot whose fan is missing stays unmapped",
          "[fan][fan_feedback][fan_tach]") {
    SECTION("no fan_feedback section") {
        CHECK(fan_tach::map_from_config({{"heater_fan hotend_fan", json::object()}}).empty());
    }
    SECTION("K2 layout without a chamber heater fan") {
        json s = k2_plus_settings();
        s.erase("heater_fan chamber_fan");
        const TachMap map = fan_tach::map_from_config(s);
        CHECK_FALSE(fan_for(map, "fan0_speed"));
        CHECK(fan_for(map, "fan2_speed") == std::optional<std::string>("output_pin fan0"));
    }
    SECTION("part fan falls back to [fan] when no output_pin fan0") {
        json s = k2_plus_settings();
        s.erase("output_pin fan0");
        s["fan"] = {{"pin", "PB15"}};
        CHECK(fan_for(fan_tach::map_from_config(s), "fan2_speed") ==
              std::optional<std::string>("fan"));
    }
    SECTION("settings that are not an object") {
        CHECK(fan_tach::map_from_config(json()).empty());
    }
}

TEST_CASE("Fan tachometers: readings follow the map", "[fan][fan_feedback][fan_tach]") {
    const TachMap map = fan_tach::map_from_config(k2_plus_settings());

    SECTION("numeric fields read, rounded") {
        auto r = fan_tach::readings(
            map,
            {{"fan_feedback", {{"fan0_speed", 0}, {"fan2_speed", 6857.6}, {"fan3_speed", 9}}}});
        REQUIRE(r.size() == 2);
        CHECK(std::find(r.begin(), r.end(), std::make_pair(std::string("output_pin fan0"), 6858)) !=
              r.end());
        CHECK(std::find(r.begin(), r.end(),
                        std::make_pair(std::string("heater_fan chamber_fan"), 0)) != r.end());
    }
    SECTION("absent or non-numeric fields yield nothing") {
        CHECK(fan_tach::readings(map, {{"fan_feedback", {{"fan2_speed", nullptr}}}}).empty());
        CHECK(fan_tach::readings(map, {{"fan", {{"speed", 0.5}}}}).empty());
    }
}

TEST_CASE("Fan tachometers: K2 Plus part fan RPM lands on the part fan (#2071)",
          "[fan][fan_feedback][fan_tach][rpm]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto& fs = state.fan_state();
    fs.init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2", "heater_fan hotend_fan",
                  "heater_fan chamber_fan"});
    fs.set_tachometers(fan_tach::map_from_config(k2_plus_settings()));

    // Measured on a K2 Plus: part fan only, at 46%.
    state.update_from_status({{"output_pin fan0", {{"value", 0.459}}},
                              {"fan_feedback",
                               {{"fan0_speed", 0},
                                {"fan1_speed", 0},
                                {"fan2_speed", 6857},
                                {"fan3_speed", 0},
                                {"fan4_speed", 0}}}});

    auto rpm_of = [&](const std::string& name) -> std::optional<int> {
        for (const auto& f : fs.get_fans())
            if (f.object_name == name)
                return f.rpm;
        FAIL("no fan " << name);
        return std::nullopt;
    };
    CHECK(rpm_of("output_pin fan0") == std::optional<int>(6857));
    CHECK_FALSE(rpm_of("output_pin fan1"));
    CHECK_FALSE(rpm_of("output_pin fan2"));
    CHECK(rpm_of("heater_fan hotend_fan") == std::optional<int>(0));
    CHECK(rpm_of("heater_fan chamber_fan") == std::optional<int>(0));

    SECTION("without a tachometer map nothing is mapped") {
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);
        state.fan_state().init_fans({"output_pin fan0", "output_pin fan2"});
        state.update_from_status({{"fan_feedback", {{"fan0_speed", 16000}, {"fan2_speed", 6857}}}});
        for (const auto& f : state.fan_state().get_fans())
            CHECK_FALSE(f.rpm.has_value());
    }
}
