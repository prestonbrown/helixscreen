// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/flashforge_pa_sweep.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::flashforge::pa_sweep;
using Catch::Approx;

namespace {

Snapshot homed_snapshot() {
    Snapshot s;
    s.homed = true;
    s.max_accel = 20000.f;
    s.square_corner_velocity = 9.f;
    s.pressure_advance = 0.03f;
    s.smooth_time = 0.04f;
    s.gcode_z = 2.f;
    return s;
}

/// Walk a fresh sweep up to its first Line, the way the driver does.
Sweep sweep_at_first_line(int tool = 0, Snapshot snap = homed_snapshot()) {
    Sweep s(Config{}, tool, "extruder", snap);
    Step st = s.next();
    if (st.phase == Phase::Home) {
        s.step_done();
        st = s.next();
    }
    REQUIRE(st.phase == Phase::Prologue);
    s.step_done();
    REQUIRE(s.next().phase == Phase::Locate);
    s.on_located(300.f, 250.f);
    REQUIRE(s.next().phase == Phase::Latch);
    s.step_done();
    return s;
}

/// Feed one whole sweep of verdicts, one per candidate in candidate order.
void run_sweep(Sweep& s, const std::vector<int>& verdicts) {
    for (int v : verdicts) {
        const Step st = s.next();
        REQUIRE(st.phase == Phase::Line);
        s.on_verdict(v);
    }
}

size_t pos_of(const std::string& hay, const std::string& needle) {
    return hay.find(needle);
}

} // namespace

TEST_CASE("PA sweep homes first only when the printer is not homed", "[pa_sweep]") {
    Snapshot snap = homed_snapshot();
    snap.homed = false;
    Sweep s(Config{}, 0, "extruder", snap);
    const Step home = s.next();
    REQUIRE(home.phase == Phase::Home);
    REQUIRE(home.script == "G28");
    s.step_done();
    REQUIRE(s.next().phase == Phase::Prologue);

    Sweep homed(Config{}, 0, "extruder", homed_snapshot());
    REQUIRE(homed.next().phase == Phase::Prologue);
}

TEST_CASE("PA sweep parks before it latches the X/Y drivers", "[pa_sweep]") {
    Sweep s(Config{}, 0, "extruder", homed_snapshot());
    const Step pro = s.next();
    REQUIRE(pro.phase == Phase::Prologue);
    // Every real move is in the prologue; the latch is not.
    REQUIRE(pos_of(pro.script, "SET_PIN") == std::string::npos);
    REQUIRE(pos_of(pro.script, "SAVE_GCODE_STATE NAME=helix_pa_sweep") != std::string::npos);
    REQUIRE(pos_of(pro.script, "SET_VELOCITY_LIMIT ACCEL=5000") != std::string::npos);
    REQUIRE(pos_of(pro.script, "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=9") != std::string::npos);
    // Raised from Z2 to the line height, then the chute in three moves.
    REQUIRE(pos_of(pro.script, "G1 Z8.000 F3000") != std::string::npos);
    const size_t stage = pos_of(pro.script, "G1 X250.000 F6000");
    const size_t square = pos_of(pro.script, "G1 Y254.000 F24000");
    const size_t in = pos_of(pro.script, "G1 X275.000 F6000");
    REQUIRE(stage != std::string::npos);
    REQUIRE(square > stage);
    REQUIRE(in > square);
    REQUIRE(pro.script.substr(pro.script.size() - 4) == "M400");
    REQUIRE_FALSE(s.latched());

    s.step_done();
    REQUIRE(s.next().phase == Phase::Locate);
    s.on_located(300.f, 250.f);
    const Step latch = s.next();
    REQUIRE(latch.phase == Phase::Latch);
    REQUIRE(pos_of(latch.script, "SET_PIN PIN=enable_pin_tmc_x VALUE=1.00") != std::string::npos);
    REQUIRE(pos_of(latch.script, "SET_PIN PIN=enable_pin_tmc_y VALUE=1.00") != std::string::npos);
    REQUIRE(s.latched());
}

TEST_CASE("PA sweep never lowers Z to the line height", "[pa_sweep]") {
    Snapshot snap = homed_snapshot();
    snap.gcode_z = 20.f;
    Sweep s(Config{}, 0, "extruder", snap);
    REQUIRE(pos_of(s.next().script, "G1 Z") == std::string::npos);
}

TEST_CASE("PA sweep parks each tool at its own chute offset", "[pa_sweep]") {
    Sweep t3(Config{}, 3, "extruder3", homed_snapshot());
    const std::string pro = t3.next().script;
    REQUIRE(pos_of(pro, "G1 X248.000 F6000") != std::string::npos);  // 275 - 2 - 25
    REQUIRE(pos_of(pro, "G1 Y246.000 F24000") != std::string::npos); // 254 - 8
    REQUIRE(pos_of(pro, "G1 X273.000 F6000") != std::string::npos);

    Sweep t2(Config{}, 2, "extruder2", homed_snapshot());
    REQUIRE(pos_of(t2.next().script, "G1 Y255.500 F24000") != std::string::npos);
}

TEST_CASE("PA sweep lines follow the stock candidate order and Y ladder", "[pa_sweep]") {
    Sweep s = sweep_at_first_line();
    const float expected_k[] = {0.0100f, 0.0200f, 0.0150f, 0.0350f, 0.0250f, 0.0300f, 0.0400f};
    for (int i = 0; i < 7; ++i) {
        const Step st = s.next();
        REQUIRE(st.phase == Phase::Line);
        REQUIRE(st.candidate == i);
        REQUIRE(s.candidate_k(i) == Approx(expected_k[i]));
        char k[64];
        std::snprintf(k, sizeof k, "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=%.4f",
                      expected_k[i]);
        REQUIRE(pos_of(st.script, k) != std::string::npos);
        char y[64];
        std::snprintf(y, sizeof y, "G1 X40.000 Y%.3f F30000", 50.f + 5.f * i);
        REQUIRE(pos_of(st.script, y) != std::string::npos);
        s.on_verdict(0);
    }
}

TEST_CASE("PA sweep line has no M400 between the last move and the capture stop", "[pa_sweep]") {
    Sweep s = sweep_at_first_line();
    const std::string line = s.next().script;
    REQUIRE(line.rfind("PA_ACTION ACTION=11 PC=666", 0) == 0);
    const size_t last_move = pos_of(line, "G1 X200.000 E1.13573");
    const size_t stop = pos_of(line, "PA_ACTION ACTION=0 PC=666");
    REQUIRE(last_move != std::string::npos);
    REQUIRE(stop > last_move);
    REQUIRE(line.substr(last_move, stop - last_move).find("M400") == std::string::npos);
    // The barrier the scorer expects comes after the stop, then the query.
    REQUIRE(line.substr(stop).find("M400\nPA_GET") != std::string::npos);
    REQUIRE(line.substr(line.size() - 6) == "PA_GET");
}

TEST_CASE("PA sweep takes the smallest pass per sweep and the mean of three", "[pa_sweep]") {
    // candidates: 0.010 0.020 0.015 0.035 0.025 0.030 0.040
    Sweep s = sweep_at_first_line();
    run_sweep(s, {0, 9, 0, 9, 9, 0, 0}); // passes 0.020 0.035 0.025 -> 0.020
    run_sweep(s, {0, 0, 9, 0, 9, 0, 0}); // 0.015 0.025 -> 0.015
    REQUIRE_FALSE(s.result().has_value());
    run_sweep(s, {0, 0, 0, 0, 9, 9, 0}); // 0.025 0.030 -> 0.025
    // Three winners end the run without a fourth sweep.
    const Step restore = s.next();
    REQUIRE(restore.phase == Phase::Restore);
    REQUIRE(s.result().has_value());
    REQUIRE(*s.result() == Approx((0.020f + 0.015f + 0.025f) / 3.f));
    REQUIRE(s.lines_done() == 21);
}

TEST_CASE("PA sweep skips a sweep with no pass and keeps going", "[pa_sweep]") {
    Sweep s = sweep_at_first_line();
    run_sweep(s, {0, 0, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    REQUIRE(s.next().phase == Phase::Restore);
    REQUIRE(*s.result() == Approx(0.020f));
    REQUIRE(s.lines_done() == 28);
}

TEST_CASE("PA sweep fails after five sweeps with fewer than three winners", "[pa_sweep]") {
    Sweep s = sweep_at_first_line();
    run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 0, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 0, 0, 0, 0, 0, 0});
    run_sweep(s, {0, 0, 0, 3, 0, 0, 0});
    run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    REQUIRE(s.next().phase == Phase::Restore);
    REQUIRE_FALSE(s.result().has_value());
    REQUIRE(s.failure() == Failure::TooFewWinners);
}

TEST_CASE("PA sweep refuses a run where every line got the same verdict", "[pa_sweep]") {
    SECTION("every line passes") {
        Sweep s = sweep_at_first_line();
        for (int i = 0; i < 3; ++i) {
            run_sweep(s, {9, 9, 9, 9, 9, 9, 9});
        }
        REQUIRE(s.next().phase == Phase::Restore);
        REQUIRE_FALSE(s.result().has_value());
        REQUIRE(s.failure() == Failure::NoDiscrimination);
    }
    SECTION("no line ever passes") {
        Sweep s = sweep_at_first_line();
        for (int i = 0; i < 5; ++i) {
            run_sweep(s, {0, 0, 0, 0, 0, 0, 0});
        }
        REQUIRE(s.next().phase == Phase::Restore);
        REQUIRE(s.failure() == Failure::NoDiscrimination);
    }
}

TEST_CASE("PA sweep restore releases the latch before replaying the position", "[pa_sweep]") {
    Sweep s = sweep_at_first_line();
    (void)s.next(); // a line in flight
    s.stop();
    s.on_verdict(9); // the reply still arrives; it is ignored
    const Step r = s.next();
    REQUIRE(r.phase == Phase::Restore);
    REQUIRE_FALSE(s.latched());
    const size_t disarm = pos_of(r.script, "PA_ACTION ACTION=0 PC=666");
    const size_t pin_x = pos_of(r.script, "SET_PIN PIN=enable_pin_tmc_x VALUE=0.00");
    const size_t pin_y = pos_of(r.script, "SET_PIN PIN=enable_pin_tmc_y VALUE=0.00");
    const size_t limits =
        pos_of(r.script, "SET_VELOCITY_LIMIT ACCEL=20000 SQUARE_CORNER_VELOCITY=9");
    const size_t pa = pos_of(
        r.script, "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.030000 SMOOTH_TIME=0.040000");
    const size_t state = pos_of(r.script, "RESTORE_GCODE_STATE NAME=helix_pa_sweep");
    const size_t where = pos_of(r.script, "SET_KINEMATIC_POSITION X=300.000 Y=250.000");
    REQUIRE(disarm == 0);
    REQUIRE(pin_x != std::string::npos);
    REQUIRE(pin_y != std::string::npos);
    REQUIRE(limits > pin_y);
    REQUIRE(pa != std::string::npos);
    REQUIRE(state > pa);
    REQUIRE(where > state);
    // Z never moved, so it is not replayed.
    REQUIRE(pos_of(r.script, "SET_KINEMATIC_POSITION X=300.000 Y=250.000 Z") == std::string::npos);

    s.step_done();
    REQUIRE(s.next().phase == Phase::Done);
    REQUIRE(s.failure() == Failure::Stopped);
    REQUIRE_FALSE(s.result().has_value());
}

TEST_CASE("PA sweep applies the result for the session after restoring", "[pa_sweep]") {
    Sweep s = sweep_at_first_line(1, homed_snapshot());
    for (int i = 0; i < 3; ++i) {
        run_sweep(s, {0, 9, 0, 0, 0, 0, 0});
    }
    REQUIRE(s.next().phase == Phase::Restore);
    s.step_done();
    const Step apply = s.next();
    REQUIRE(apply.phase == Phase::Apply);
    REQUIRE(apply.script == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.020000");
    s.step_done();
    REQUIRE(s.finished());
    REQUIRE(s.failure() == Failure::None);
}

TEST_CASE("PA sweep stopped before it moved anything has nothing to restore", "[pa_sweep]") {
    Sweep s(Config{}, 0, "extruder", homed_snapshot());
    s.stop();
    REQUIRE(s.next().phase == Phase::Done);
    REQUIRE(s.failure() == Failure::Stopped);
}

TEST_CASE("PA sweep failed after the prologue still restores", "[pa_sweep]") {
    Sweep s(Config{}, 0, "extruder", homed_snapshot());
    (void)s.next();
    s.fail("SET_VELOCITY_LIMIT failed");
    const Step r = s.next();
    REQUIRE(r.phase == Phase::Restore);
    // Never located: no position to replay.
    REQUIRE(pos_of(r.script, "SET_KINEMATIC_POSITION") == std::string::npos);
    REQUIRE(pos_of(r.script, "SET_PIN PIN=enable_pin_tmc_x VALUE=0.00") != std::string::npos);
    s.step_done();
    REQUIRE(s.next().phase == Phase::Done);
    REQUIRE(s.failure() == Failure::Error);
    REQUIRE(s.error() == "SET_VELOCITY_LIMIT failed");
}

TEST_CASE("PA sweep refuses a tool it has no chute offset for", "[pa_sweep]") {
    Sweep s(Config{}, 4, "extruder4", homed_snapshot());
    REQUIRE(s.next().phase == Phase::Done);
    REQUIRE(s.failure() == Failure::Error);
}

TEST_CASE("PA_GET reply parsing", "[pa_sweep]") {
    REQUIRE(parse_pa_get("// Result is value=9") == 9);
    REQUIRE(parse_pa_get("Result is value=0") == 0);
    REQUIRE(parse_pa_get("// Result is value=10") == 10);
    REQUIRE_FALSE(parse_pa_get("// pressure_advance: 0.020000").has_value());
    REQUIRE_FALSE(parse_pa_get("// Result is value=").has_value());
    REQUIRE_FALSE(parse_pa_get("// Result is value=x").has_value());
    REQUIRE_FALSE(parse_pa_get("").has_value());
}

TEST_CASE("PA sweep selection and discrimination rules", "[pa_sweep]") {
    const Config cfg;
    REQUIRE_FALSE(select({}, cfg).has_value());
    REQUIRE(*select({{0, 9, 0, 0, 0, 0, 9}, {9, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 9, 0}}, cfg) ==
            Approx((0.020f + 0.010f + 0.030f) / 3.f));
    // Only the first three winners count.
    REQUIRE(*select({{9, 0, 0, 0, 0, 0, 0},
                     {9, 0, 0, 0, 0, 0, 0},
                     {9, 0, 0, 0, 0, 0, 0},
                     {0, 0, 0, 0, 0, 0, 9}},
                    cfg) == Approx(0.010f));

    REQUIRE(discriminates({{0, 9}}));
    REQUIRE_FALSE(discriminates({{9, 9}, {9}}));
    REQUIRE_FALSE(discriminates({}));
}

TEST_CASE("PA sweep latch recovery releases and unhomes only when latched", "[pa_sweep]") {
    REQUIRE(latch_recovery_gcode(0.f, 0.f).empty());
    const std::string fix = latch_recovery_gcode(1.f, 0.f);
    const size_t x = fix.find("SET_PIN PIN=enable_pin_tmc_x VALUE=0.00");
    const size_t y = fix.find("SET_PIN PIN=enable_pin_tmc_y VALUE=0.00");
    const size_t m84 = fix.find("M84");
    REQUIRE(x != std::string::npos);
    REQUIRE(y != std::string::npos);
    REQUIRE(m84 > y);
    REQUIRE_FALSE(latch_recovery_gcode(0.f, 1.f).empty());
}

TEST_CASE("PA sweep reports a typical run as three sweeps of seven", "[pa_sweep]") {
    Sweep s(Config{}, 0, "extruder", homed_snapshot());
    REQUIRE(s.expected_lines() == 21);
}
