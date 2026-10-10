// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfs_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("CFS status parse classifies a box frame by the keys it carries", "[cfs][status_parse]") {
    const auto bare = cfs::classify_box_frame(json{{"measuring_wheel", 3}});
    CHECK_FALSE(bare.has_top_level);
    CHECK_FALSE(bare.has_unit_data);

    const auto top = cfs::classify_box_frame(json{{"map", json::object()}});
    CHECK(top.has_top_level);
    CHECK_FALSE(top.has_unit_data);

    // The box resends the auto-refill bit alone when it changes.
    const auto refill = cfs::classify_box_frame(json{{"auto_refill", 0}});
    CHECK(refill.has_top_level);
    CHECK_FALSE(refill.has_unit_data);

    const auto unit = cfs::classify_box_frame(json{{"T3", json::object()}});
    CHECK_FALSE(unit.has_top_level);
    CHECK(unit.has_unit_data);
}

TEST_CASE("CFS status parse reads the sibling objects only when present", "[cfs][status_parse]") {
    const json none = json::object();
    CHECK_FALSE(cfs::parse_filament_sensor(none));
    CHECK_FALSE(cfs::parse_extruder(none));
    CHECK_FALSE(cfs::parse_motor_control(none));

    const auto sensor = cfs::parse_filament_sensor(
        json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", true}}}});
    REQUIRE(sensor);
    CHECK(sensor->filament_detected == true);

    // The object is there but the sensor has taken no reading: present, empty.
    const auto null_sensor = cfs::parse_filament_sensor(
        json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", nullptr}}}});
    REQUIRE(null_sensor);
    CHECK_FALSE(null_sensor->filament_detected);

    const auto extruder = cfs::parse_extruder(json{{"extruder", json{{"target", 220}}}});
    REQUIRE(extruder);
    CHECK(extruder->target_c == Catch::Approx(220.0));
    CHECK_FALSE(extruder->temperature_c);

    const auto motor =
        cfs::parse_motor_control(json{{"motor_control", json{{"motor_ready", false}}}});
    REQUIRE(motor);
    CHECK(motor->motor_ready == false);
    CHECK_FALSE(cfs::parse_motor_control(json{{"motor_control", json{{"motor_ready", "yes"}}}})
                    ->motor_ready);
}
