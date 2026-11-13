// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using Catch::Approx;
using helix::PrinterDiscovery;
using helix::calibration::ripple::MotorId;

TEST_CASE("discovery reads each XY stepper's ripple geometry from configfile settings",
          "[discovery][ripple]") {
    PrinterDiscovery hw;
    CHECK_FALSE(hw.motor_geometry(MotorId::StepperX).has_value());

    hw.parse_stepper_geometry(json::parse(R"({
        "stepper_x": {"rotation_distance": 40.0, "full_steps_per_rotation": 200},
        "stepper_y": {"rotation_distance": 40.0, "gear_ratio": [[3.0, 1.0]]}
    })"));
    const auto x = hw.motor_geometry(MotorId::StepperX);
    REQUIRE(x.has_value());
    CHECK(x->rotation_distance_mm == Approx(40.0f));
    CHECK(x->gear_ratio == Approx(1.0f));
    CHECK(x->full_steps_per_rotation == 200);
    const auto y = hw.motor_geometry(MotorId::StepperY);
    REQUIRE(y.has_value());
    CHECK(y->gear_ratio == Approx(3.0f));
    CHECK(y->full_steps_per_rotation == 200); // Klipper's default when the key is absent

    hw.parse_stepper_geometry(json::parse(R"({"stepper_x": {"rotation_distance": 38.82}})"));
    CHECK(hw.motor_geometry(MotorId::StepperX)->rotation_distance_mm == Approx(38.82f));
    CHECK_FALSE(hw.motor_geometry(MotorId::StepperY).has_value());

    hw.parse_stepper_geometry(json::parse(R"({"stepper_x": {"rotation_distance": "x"}})"));
    CHECK_FALSE(hw.motor_geometry(MotorId::StepperX).has_value());
}
