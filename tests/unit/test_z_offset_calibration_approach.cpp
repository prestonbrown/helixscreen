// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"
#include "z_offset_calibration_approach.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AxisBounds;
using helix::ProbeXYOffset;
using helix::ZOffsetCalibrationStrategy;
using helix::sensors::ProbeSensorType;
using namespace helix::zoffset_cal;
using json = nlohmann::json;

namespace {

AxisBounds bounds(float x_min, float x_max, float y_min, float y_max, float z_max = 300.0f) {
    AxisBounds b;
    b.x_min = x_min;
    b.x_max = x_max;
    b.y_min = y_min;
    b.y_max = y_max;
    b.z_min = 0.0f;
    b.z_max = z_max;
    b.has_x = true;
    b.has_y = true;
    b.has_z = true;
    return b;
}

/// A 350mm bed whose travel runs past the plate on the right.
ApproachInputs plate_350(ReadingPoint point, bool homed) {
    ApproachInputs in;
    in.all_homed = homed;
    in.plate = bounds(0.0f, 350.0f, 0.0f, 350.0f);
    in.gcode_travel = bounds(-2.0f, 360.0f, -2.0f, 352.0f);
    in.point = point;
    return in;
}

} // namespace

TEST_CASE("Z-offset approach: a homed printer still moves to the plate centre",
          "[zoffset_center]") {
    const auto a = calibration_approach(plate_350(ReadingPoint::Nozzle, true));
    REQUIRE(a.nozzle.has_value());
    CHECK(*a.nozzle->x == Catch::Approx(175.0));
    CHECK(*a.nozzle->y == Catch::Approx(175.0));
    CHECK(a.gcode == "G90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\n");
}

TEST_CASE("Z-offset approach: an unhomed printer homes before the move", "[zoffset_center]") {
    const auto a = calibration_approach(plate_350(ReadingPoint::Nozzle, false));
    CHECK(a.gcode == "G28\nG90\nG1 Z5.0 F600\nG1 X175.0 Y175.0 F6000\n");
}

TEST_CASE("Z-offset approach: the plate centre is not the travel centre", "[zoffset_center]") {
    auto in = plate_350(ReadingPoint::Nozzle, true);
    in.plate = bounds(20.0f, 220.0f, 10.0f, 210.0f);
    const auto a = calibration_approach(in);
    REQUIRE(a.nozzle.has_value());
    CHECK(*a.nozzle->x == Catch::Approx(120.0));
    CHECK(*a.nozzle->y == Catch::Approx(110.0));
}

TEST_CASE("Z-offset approach: a probe reading puts the probe on centre", "[zoffset_center]") {
    auto in = plate_350(ReadingPoint::Probe, true);
    in.probe_offset = ProbeXYOffset{-40.0, -10.0};
    const auto a = calibration_approach(in);
    REQUIRE(a.nozzle.has_value());
    // probe = nozzle + offset, so the nozzle sits at centre - offset.
    CHECK(*a.nozzle->x == Catch::Approx(215.0));
    CHECK(*a.nozzle->y == Catch::Approx(185.0));
    CHECK(a.gcode == "G90\nG1 Z5.0 F600\nG1 X215.0 Y185.0 F6000\n");
}

TEST_CASE("Z-offset approach: a nozzle reading ignores the probe offset", "[zoffset_center]") {
    auto in = plate_350(ReadingPoint::Nozzle, true);
    in.probe_offset = ProbeXYOffset{-40.0, -10.0};
    const auto a = calibration_approach(in);
    REQUIRE(a.nozzle.has_value());
    CHECK(*a.nozzle->x == Catch::Approx(175.0));
    CHECK(*a.nozzle->y == Catch::Approx(175.0));
}

TEST_CASE("Z-offset approach: an unknown probe offset reads as a probe at the nozzle",
          "[zoffset_center]") {
    const auto a = calibration_approach(plate_350(ReadingPoint::Probe, true));
    REQUIRE(a.nozzle.has_value());
    CHECK(*a.nozzle->x == Catch::Approx(175.0));
    CHECK(*a.nozzle->y == Catch::Approx(175.0));
}

TEST_CASE("Z-offset approach: the nozzle target stays inside travel", "[zoffset_center]") {
    auto in = plate_350(ReadingPoint::Probe, true);
    in.plate = bounds(0.0f, 220.0f, 0.0f, 220.0f);
    in.gcode_travel = bounds(0.0f, 230.0f, 0.0f, 230.0f);
    in.probe_offset = ProbeXYOffset{-150.0, 200.0};
    const auto a = calibration_approach(in);
    REQUIRE(a.nozzle.has_value());
    CHECK(*a.nozzle->x <= 230.0);
    CHECK(*a.nozzle->x > 229.0);
    CHECK(*a.nozzle->y >= 0.0);
    CHECK(*a.nozzle->y < 1.0);
}

TEST_CASE("Z-offset approach: a short Z axis lifts no higher than its travel", "[zoffset_center]") {
    auto in = plate_350(ReadingPoint::Nozzle, true);
    in.gcode_travel = bounds(0.0f, 350.0f, 0.0f, 350.0f, 3.0f);
    const auto a = calibration_approach(in);
    CHECK(a.gcode.find("G1 Z5.0") == std::string::npos);
    CHECK(a.gcode.find("G1 Z2.9") != std::string::npos);
}

TEST_CASE("Z-offset approach: an unknown plate sends no move", "[zoffset_center]") {
    SECTION("homed: nothing at all") {
        ApproachInputs in;
        in.all_homed = true;
        const auto a = calibration_approach(in);
        CHECK_FALSE(a.nozzle.has_value());
        CHECK(a.gcode.empty());
    }
    SECTION("unhomed: only the homing") {
        ApproachInputs in;
        in.all_homed = false;
        const auto a = calibration_approach(in);
        CHECK_FALSE(a.nozzle.has_value());
        CHECK(a.gcode == "G28\n");
    }
}

TEST_CASE("Z-offset calibration command: strategy and probe pick the command and reading point",
          "[zoffset_center]") {
    using S = ZOffsetCalibrationStrategy;
    auto check = [](S s, ProbeSensorType t, const std::string& gcode, ReadingPoint point) {
        const auto c = calibration_command(s, t);
        CHECK(std::string(c.gcode) == gcode);
        CHECK(c.point == point);
    };
    check(S::PROBE_CALIBRATE, ProbeSensorType::STANDARD, "PROBE_CALIBRATE", ReadingPoint::Probe);
    check(S::PROBE_CALIBRATE, ProbeSensorType::BLTOUCH, "PROBE_CALIBRATE", ReadingPoint::Probe);
    check(S::PROBE_CALIBRATE, ProbeSensorType::CARTOGRAPHER, "CARTOGRAPHER_SCAN_CALIBRATE",
          ReadingPoint::Nozzle);
    check(S::PROBE_CALIBRATE, ProbeSensorType::BEACON, "BEACON_CALIBRATE", ReadingPoint::Nozzle);
    check(S::ENDSTOP, ProbeSensorType::BLTOUCH, "Z_ENDSTOP_CALIBRATE", ReadingPoint::Nozzle);
    check(S::FIRMWARE_MANAGED, ProbeSensorType::STANDARD, "", ReadingPoint::Nozzle);
}

TEST_CASE("PrinterDiscovery: the probe XY offset comes from the probe's config section",
          "[zoffset_center]") {
    helix::PrinterDiscovery hw;

    SECTION("[probe]") {
        hw.parse_probe_xy_offset(json{{"probe", {{"x_offset", -40.0}, {"y_offset", -10.0}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        CHECK(hw.probe_xy_offset()->x == Catch::Approx(-40.0));
        CHECK(hw.probe_xy_offset()->y == Catch::Approx(-10.0));
    }
    SECTION("[bltouch], one axis set") {
        hw.parse_probe_xy_offset(json{{"bltouch", {{"x_offset", 25.5}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        CHECK(hw.probe_xy_offset()->x == Catch::Approx(25.5));
        CHECK(hw.probe_xy_offset()->y == Catch::Approx(0.0));
    }
    SECTION("a named [probe_eddy_current]") {
        hw.parse_probe_xy_offset(json{{"probe_eddy_current btt", {{"y_offset", 18.0}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        CHECK(hw.probe_xy_offset()->y == Catch::Approx(18.0));
    }
    SECTION("Kalico's [dockable_probe]") {
        hw.parse_probe_xy_offset(json{{"dockable_probe", {{"x_offset", 0.0}, {"y_offset", 25.0}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        CHECK(hw.probe_xy_offset()->y == Catch::Approx(25.0));
    }
    SECTION("a RevH [beacon] that also carries its accelerometer") {
        hw.parse_probe_xy_offset(json{{"beacon",
                                       {{"x_offset", 0.0},
                                        {"y_offset", 20.0},
                                        {"accel_scale", "2g"},
                                        {"accel_axes_map", "x,y,z"}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        CHECK(hw.probe_xy_offset()->y == Catch::Approx(20.0));
    }
    SECTION("no probe section") {
        hw.parse_probe_xy_offset(json{{"stepper_x", {{"position_max", 235.0}}}});
        CHECK_FALSE(hw.probe_xy_offset().has_value());
    }
    SECTION("a later parse without a probe clears the offset") {
        hw.parse_probe_xy_offset(json{{"probe", {{"x_offset", -40.0}}}});
        hw.parse_probe_xy_offset(json::object());
        CHECK_FALSE(hw.probe_xy_offset().has_value());
    }
    SECTION("clear() forgets the offset, so a different printer starts unknown") {
        hw.parse_probe_xy_offset(json{{"probe", {{"x_offset", -40.0}}}});
        REQUIRE(hw.probe_xy_offset().has_value());
        hw.clear();
        CHECK_FALSE(hw.probe_xy_offset().has_value());
    }
}
