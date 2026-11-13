// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_helpers/script_lines.h"
#include "motor_ripple_plan.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration::ripple;
using Catch::Approx;

namespace {

SweepPlanInputs voron(Kinematics k = Kinematics::CoreXY) {
    SweepPlanInputs in;
    in.kinematics = k;
    in.x_min = 0.0f;
    in.x_max = 300.0f;
    in.y_min = 0.0f;
    in.y_max = 305.0f;
    in.has_xy_limits = true;
    in.probe_point = std::array<float, 3>{150.0f, 150.0f, 20.0f};
    in.max_velocity_mm_s = 500.0f;
    in.max_accel_mm_s2 = 30000.0f;
    in.current_z = 5.0f;
    in.max_z = 280.0f;
    in.minimum_cruise_ratio = 0.5f;
    return in;
}

struct Move {
    float x, y, f;
};

// The pass moves of a step: G1 lines after the first (the approach), up to M400.
std::vector<Move> passes_of(const std::string& gcode) {
    std::vector<Move> out;
    bool approach_seen = false;
    for (const auto& line : helix::test::script_lines(gcode)) {
        Move m{};
        if (std::sscanf(line.c_str(), "G1 X%f Y%f F%f", &m.x, &m.y, &m.f) == 3) {
            if (approach_seen) {
                out.push_back(m);
            }
            approach_seen = true;
        }
    }
    return out;
}

} // namespace

TEST_CASE("electrical cycle and ripple frequency follow the stepper geometry", "[ripple][plan]") {
    CHECK(electrical_cycle_mm({40.0f, 1.0f, 200}) == Approx(0.8f));
    CHECK(electrical_cycle_mm({38.82f, 1.0f, 200}) == Approx(0.7764f));
    CHECK(electrical_cycle_mm({40.0f, 3.0f, 200}) == Approx(0.8f / 3.0f));
    CHECK(ripple_freq_hz(86.3f, 2, 0.8f) == Approx(215.75f));
}

TEST_CASE("sweep speeds climb 8% at a time from 20 to 300 mm/s or 90% of max velocity",
          "[ripple][plan]") {
    const auto sp = sweep_speeds(500.0f);
    REQUIRE(sp.size() == 37);
    CHECK(sp.front() == Approx(20.0f));
    CHECK(sp.back() == Approx(300.0f));
    for (size_t i = 1; i < sp.size(); ++i) {
        CHECK(sp[i] / sp[i - 1] <= 1.08f + 5e-3f); // speeds are rounded to 0.1 mm/s
    }
    CHECK(sweep_speeds(200.0f).back() == Approx(180.0f));
    CHECK(sweep_speeds(0.0f).empty());
}

TEST_CASE("CoreXY steps move along one motor's diagonal at speed over root two", "[ripple][plan]") {
    const auto in = voron();
    for (MotorId m : {MotorId::StepperX, MotorId::StepperY}) {
        const auto plan = plan_step(in, m, 120.0f);
        REQUIRE(plan.refusal == SweepRefusal::None);
        const auto moves = passes_of(plan.gcode);
        REQUIRE(moves.size() == static_cast<size_t>(plan.passes));
        const float dx = moves[1].x - moves[0].x;
        const float dy = moves[1].y - moves[0].y;
        CHECK(std::fabs(dx) > 1.0f);
        CHECK(dy == Approx(m == MotorId::StepperX ? dx : -dx).margin(0.2f));
        CHECK(moves[0].f == Approx(120.0f / std::sqrt(2.0f) * 60.0f).margin(1.0f));
    }

    const auto slow = plan_step(in, MotorId::StepperX, 20.0f);
    CHECK(slow.pass_mm == Approx(30.0f));
    CHECK(slow.passes == 2);

    const auto fast = plan_step(in, MotorId::StepperX, 300.0f);
    const float diag = std::hypot(280.0f, 285.0f);
    CHECK(fast.pass_mm <= 0.85f * diag);
    CHECK(fast.passes % 2 == 0);
    CHECK(fast.passes * fast.pass_mm / (300.0f / std::sqrt(2.0f)) >= 2.0f);
}

TEST_CASE("Cartesian steps move X alone and skip Y without a Y accelerometer", "[ripple][plan]") {
    auto in = voron(Kinematics::Cartesian);
    const auto moves = passes_of(plan_step(in, MotorId::StepperX, 100.0f).gcode);
    REQUIRE(moves.size() >= 2);
    CHECK(moves[1].y == Approx(moves[0].y));
    CHECK(moves[0].f == Approx(6000.0f));
    // A 255 mm bed caps the pass at 200 mm, so 2 s of cruise at 250 mm/s needs
    // 2.5 passes, rounded up to an even 4 to end where it began.
    in.x_max = 255.0f;
    in.probe_point = std::array<float, 3>{127.5f, 150.0f, 20.0f};
    const auto capped = plan_step(in, MotorId::StepperX, 250.0f);
    CHECK(capped.pass_mm == Approx(200.0f).margin(0.5f));
    CHECK(capped.passes == 4);
    CHECK(motors_to_measure(in).size() == 2);
    in.y_motor_measurable = false;
    CHECK(motors_to_measure(in) == std::vector<MotorId>{MotorId::StepperX});
}

TEST_CASE("plans refuse what they cannot move safely", "[ripple][plan]") {
    auto in = voron(Kinematics::Unsupported);
    CHECK(plan_step(in, MotorId::StepperX, 50.0f).refusal == SweepRefusal::UnsupportedKinematics);
    CHECK(motors_to_measure(in).empty());
    in = voron();
    in.has_xy_limits = false;
    CHECK(plan_step(in, MotorId::StepperX, 50.0f).refusal == SweepRefusal::NoAxisLimits);
    in = voron();
    in.max_velocity_mm_s = 0.0f;
    CHECK(plan_step(in, MotorId::StepperX, 50.0f).refusal == SweepRefusal::NoVelocity);
    in = voron();
    in.x_max = 40.0f;
    in.y_max = 40.0f;
    in.probe_point = std::array<float, 3>{20.0f, 20.0f, 10.0f};
    const auto tiny = plan_step(in, MotorId::StepperX, 50.0f);
    CHECK(tiny.refusal == SweepRefusal::SpanTooSmall);
    CHECK(tiny.gcode.empty());
}

TEST_CASE("begin and restore bracket the run with the printer's own limits", "[ripple][plan]") {
    auto in = voron();
    const auto begin = begin_gcode(in);
    CHECK(begin.find("SAVE_GCODE_STATE NAME=helix_ripple_sweep") != std::string::npos);
    CHECK(begin.find("SET_VELOCITY_LIMIT ACCEL=10000 MINIMUM_CRUISE_RATIO=0") != std::string::npos);
    CHECK(begin.find("G1 Z20.0") != std::string::npos);

    in.current_z = 40.0f;
    CHECK(begin_gcode(in).find("G1 Z40.0") != std::string::npos);
    in.max_z = 15.0f;
    in.current_z = 5.0f;
    CHECK(begin_gcode(in).find("G1 Z15.0") != std::string::npos);

    in = voron();
    in.max_accel_mm_s2 = 5000.0f;
    CHECK(begin_gcode(in).find("ACCEL=5000 ") != std::string::npos);

    in = voron();
    in.minimum_cruise_ratio.reset();
    in.max_accel_to_decel = 15000.0f;
    CHECK(begin_gcode(in).find("ACCEL_TO_DECEL=10000") != std::string::npos);
    CHECK(restore_gcode(in).find("ACCEL_TO_DECEL=15000") != std::string::npos);

    in = voron();
    const auto lines = helix::test::script_lines(restore_gcode(in));
    REQUIRE(!lines.empty());
    CHECK(lines.front() == "SET_VELOCITY_LIMIT ACCEL=30000 MINIMUM_CRUISE_RATIO=0.5");
    CHECK(lines.back() == "RESTORE_GCODE_STATE NAME=helix_ripple_sweep");
}
