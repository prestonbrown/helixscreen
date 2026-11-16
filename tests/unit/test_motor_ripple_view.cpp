// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "motor_ripple_view.h"

#include <cmath>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration::ripple;

TEST_CASE("only CoreXY and Cartesian printers with an accelerometer get the check",
          "[ripple][view]") {
    CHECK(ripple_kinematics("corexy") == Kinematics::CoreXY);
    CHECK(ripple_kinematics("limited_corexy") == Kinematics::CoreXY);
    CHECK(ripple_kinematics("cartesian") == Kinematics::Cartesian);
    CHECK(ripple_kinematics("delta") == Kinematics::Unsupported);
    CHECK(ripple_check_supported("corexy", true, true));
    CHECK_FALSE(ripple_check_supported("corexy", false, true));
    CHECK_FALSE(ripple_check_supported("corexy", true, false));
    CHECK_FALSE(ripple_check_supported("delta", true, true));
}

TEST_CASE_METHOD(LVGLTestFixture, "the band list names each band, its ripple and its frequency",
                 "[ripple][view]") {
    const std::vector<RippleBand> bands = {
        {MotorId::StepperX, 77.0f, 96.95f, 2, 86.3f, 3011.0f, 215.75f},
        {MotorId::StepperY, 153.9f, 179.4f, 1, 172.5f, 1413.0f, 215.6f}};
    const std::string text = ripple_band_summary(bands);
    CHECK(text.find("77-97 mm/s") != std::string::npos);
    CHECK(text.find("154-179 mm/s") != std::string::npos);
    CHECK(text.find("216 Hz") != std::string::npos);
    CHECK(ripple_band_summary({}).find("No speed") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "the comparison gives each ripple's worst speed before and after",
                 "[ripple][view]") {
    SweepResult r;
    MotorSweep off;
    MotorSweep on;
    SpeedResult a;
    a.speed_mm_s = 86.3f;
    a.ripple = {150.0f, 3000.0f, std::nullopt};
    SpeedResult b = a;
    b.ripple = {120.0f, 600.0f, std::nullopt};
    off.speeds = {a};
    on.speeds = {b};
    r.sweeps = {off};
    r.compensated_sweeps = {on};
    const std::string calibrated = ripple_reduction_summary(r, false);
    CHECK(calibrated.find("80%") != std::string::npos); // 2x: 3000 -> 600
    CHECK(calibrated.find("as your printer applies it") == std::string::npos);
    CHECK(ripple_reduction_summary(r, true).find("as your printer applies it") !=
          std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "progress names the motor, the speed and the count",
                 "[ripple][view]") {
    const std::string text = ripple_progress_text(MotorId::StepperY, 86.3f, 22, 74, false);
    CHECK(text.find("Y") != std::string::npos);
    CHECK(text.find("86 mm/s") != std::string::npos);
    CHECK(text.find("23 of 74") != std::string::npos);
    CHECK(ripple_estimate_minutes(2, 37, false) >= 4);
    CHECK(ripple_estimate_minutes(2, 37, true) >= 2 * ripple_estimate_minutes(2, 37, false) - 1);
}

TEST_CASE("a geometric sweep is resampled onto the chart's even speed grid", "[ripple][view]") {
    const std::vector<float> xs = {20.0f, 40.0f, 80.0f};
    const std::vector<float> ys = {100.0f, 300.0f, NAN};
    const auto grid = ripple_resample_uniform(xs, ys, 0.0f, 100.0f, 11); // 0, 10, ... 100
    REQUIRE(grid.size() == 11);
    CHECK(std::isnan(grid[1]));              // 10 mm/s: before the first speed
    CHECK(grid[2] == Catch::Approx(100.0f)); // 20 mm/s
    CHECK(grid[3] == Catch::Approx(200.0f)); // 30 mm/s, halfway to 40
    CHECK(grid[4] == Catch::Approx(300.0f)); // 40 mm/s
    CHECK(std::isnan(grid[6]));              // 60 mm/s: its right neighbour was not measured
    CHECK(std::isnan(grid[10]));             // past the last speed
}
