// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// tests/unit/test_jog_coalescer.cpp
#include "jog_coalescer.h"

#include "../catch_amalgamated.hpp"

using helix::AxisMove;
using helix::JogCoalescer;

TEST_CASE("JogCoalescer: first tap sends immediately", "[jog_coalescer]") {
    JogCoalescer c;
    auto send = c.on_tap({1.0, 0.0, 0.0});
    REQUIRE(send.has_value());
    CHECK(send->dx == 1.0);
    CHECK(c.in_flight());
    CHECK(c.predicted_x(0.0) == 1.0); // in-flight counts as uncommitted
}

TEST_CASE("JogCoalescer: taps while in flight accumulate, ack flushes once", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK(c.predicted_x(0.0) == 3.0); // 1 in flight + 2 pending

    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    CHECK(std::get<AxisMove>(*flush).dx == 2.0); // both pending taps in ONE move
    CHECK(c.in_flight());

    auto done = c.on_ack();
    CHECK_FALSE(done.has_value()); // nothing pending -> idle
    CHECK_FALSE(c.in_flight());
    CHECK(c.predicted_x(0.0) == 0.0);
}

TEST_CASE("JogCoalescer: reversal cancels pending algebraically", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({-1.0, 0.0, 0.0});
    auto flush = c.on_ack();
    CHECK_FALSE(flush.has_value()); // +1 -1 pending nets to zero -> nothing to send
    CHECK_FALSE(c.in_flight());
}

TEST_CASE("JogCoalescer: multi-axis pending flushes as one move", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({0.0, -2.0, 0.0});
    c.on_tap({0.0, 0.0, 0.5});
    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    CHECK(std::get<AxisMove>(*flush).dx == 0.0);
    CHECK(std::get<AxisMove>(*flush).dy == -2.0);
    CHECK(std::get<AxisMove>(*flush).dz == 0.5);
}

TEST_CASE("JogCoalescer: error drops pending and goes idle", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    c.on_tap({5.0, 0.0, 0.0});
    c.on_error();
    CHECK_FALSE(c.in_flight());
    CHECK(c.predicted_x(0.0) == 0.0);
    // Next tap sends immediately again
    CHECK(c.on_tap({1.0, 0.0, 0.0}).has_value());
}

TEST_CASE("JogCoalescer: reset clears everything", "[jog_coalescer]") {
    JogCoalescer c;
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({2.0, 0.0, 0.0});
    c.reset();
    CHECK_FALSE(c.in_flight());
    CHECK(c.predicted_x(0.0) == 0.0);
}

TEST_CASE("JogCoalescer: float-residue reversal nets to idle, no residual flush",
          "[jog_coalescer]") {
    // 0.1 + 1.0 - 1.0 - 0.1 leaves ~1e-17 residue in double arithmetic. An exact
    // != 0.0 check would flush a near-null move (serialized in scientific notation);
    // the epsilon threshold in AxisMove::any() treats it as zero and goes idle.
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value()); // establish in-flight
    c.on_tap({0.1, 0.0, 0.0});
    c.on_tap({1.0, 0.0, 0.0});
    c.on_tap({-1.0, 0.0, 0.0});
    c.on_tap({-0.1, 0.0, 0.0});
    auto flush = c.on_ack();
    CHECK_FALSE(flush.has_value()); // residue below epsilon -> nothing to send
    CHECK_FALSE(c.in_flight());
}

TEST_CASE("clamp_jog_delta: clamps target to envelope", "[jog_coalescer]") {
    // current=195, nothing uncommitted, +10 would hit 205 with max 200 -> +5
    CHECK_THAT(helix::clamp_jog_delta(195.0, 0.0, 10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(5.0, 1e-9));
    // Accounts for uncommitted travel: current=190, 5 uncommitted, +10 -> +5
    CHECK_THAT(helix::clamp_jog_delta(190.0, 5.0, 10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(5.0, 1e-9));
    // Fully at edge -> 0
    CHECK(helix::clamp_jog_delta(200.0, 0.0, 1.0, 0.0, 200.0) == 0.0);
    // Never reverses direction even if predicted overshoots the envelope
    CHECK(helix::clamp_jog_delta(205.0, 0.0, 1.0, 0.0, 200.0) == 0.0);
    // Moves away from the edge pass through untouched
    CHECK_THAT(helix::clamp_jog_delta(200.0, 0.0, -1.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-1.0, 1e-9));
}

TEST_CASE("clamp_jog_delta: a head parked past the max keeps its step toward the bed",
          "[jog_coalescer]") {
    // A K2 parked at the brush: Y 370 past a 352 print-area limit. A small
    // step back toward the bed moves exactly that step, not out to the edge.
    CHECK_THAT(helix::clamp_jog_delta(370.0, 0.0, -0.1, -6.2, 352.0),
               Catch::Matchers::WithinAbs(-0.1, 1e-9));
    CHECK_THAT(helix::clamp_jog_delta(370.0, 0.0, -10.0, -6.2, 352.0),
               Catch::Matchers::WithinAbs(-10.0, 1e-9));
    // A step further out stays refused.
    CHECK(helix::clamp_jog_delta(370.0, 0.0, 1.0, -6.2, 352.0) == 0.0);
    // The min edge still binds a long step from there.
    CHECK_THAT(helix::clamp_jog_delta(370.0, 0.0, -400.0, -6.2, 352.0),
               Catch::Matchers::WithinAbs(-376.2, 1e-9));
}

TEST_CASE("clamp_jog_delta: clamps target to the min edge", "[jog_coalescer]") {
    // Mirror of the max-edge case above. Every assertion there passes min=0.0,
    // which never binds, so the min branch of std::clamp went unguarded.
    // current=5, nothing uncommitted, -10 would hit -5 with min 0 -> -5
    CHECK_THAT(helix::clamp_jog_delta(5.0, 0.0, -10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Accounts for uncommitted travel: current=10, -5 uncommitted, -10 -> -5
    CHECK_THAT(helix::clamp_jog_delta(10.0, -5.0, -10.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Fully at edge -> 0
    CHECK(helix::clamp_jog_delta(0.0, 0.0, -1.0, 0.0, 200.0) == 0.0);
    // Never reverses direction even if predicted undershoots the envelope
    CHECK(helix::clamp_jog_delta(-5.0, 0.0, -1.0, 0.0, 200.0) == 0.0);
    // Moves away from the edge pass through untouched
    CHECK_THAT(helix::clamp_jog_delta(0.0, 0.0, 1.0, 0.0, 200.0),
               Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("clamp_jog_delta: honours a negative min envelope", "[jog_coalescer]") {
    // A min of 0.0 is indistinguishable from "no lower bound" for most sign
    // errors. Y axes routinely carry a negative soft limit (position_min: -5),
    // so the lower bound must be respected as an arbitrary value, not as zero.
    // predicted=0, -10 would hit -10 with min -5 -> -5
    CHECK_THAT(helix::clamp_jog_delta(0.0, 0.0, -10.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(-5.0, 1e-9));
    // Uncommitted travel already ate part of the envelope: predicted=-4, -1 left
    CHECK_THAT(helix::clamp_jog_delta(-3.0, -1.0, -10.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(-1.0, 1e-9));
    // Sitting exactly on the negative limit -> 0
    CHECK(helix::clamp_jog_delta(-5.0, 0.0, -1.0, -5.0, 200.0) == 0.0);
    // Past the negative limit -> 0, never a direction-reversing correction
    CHECK(helix::clamp_jog_delta(-7.0, 0.0, -1.0, -5.0, 200.0) == 0.0);
    // Moving back inside from the negative limit passes through untouched
    CHECK_THAT(helix::clamp_jog_delta(-5.0, 0.0, 2.0, -5.0, 200.0),
               Catch::Matchers::WithinAbs(2.0, 1e-9));
}

TEST_CASE("clamp_jog_with_warn: clamps at the axis maximum without warning", "[jog_coalescer]") {
    // At Z=248 on a 250mm axis, a +10 request may only travel 2. Partial travel
    // is not a block, so nothing is warned and the latch clears.
    const auto r = helix::clamp_jog_with_warn(248.0, 0.0, 10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(2.0));
    REQUIRE_FALSE(r.warn);
    REQUIRE_FALSE(r.latch);
}

TEST_CASE("clamp_jog_with_warn: clamps at the axis minimum", "[jog_coalescer]") {
    const auto r = helix::clamp_jog_with_warn(1.0, 0.0, -10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(-1.0));
    REQUIRE_FALSE(r.warn);
}

TEST_CASE("clamp_jog_with_warn: a fully blocked jog warns exactly once", "[jog_coalescer]") {
    // First attempt at the limit warns and sets the latch.
    const auto first = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, false);
    REQUIRE(first.allowed == 0.0);
    REQUIRE(first.warn);
    REQUIRE(first.latch);

    // Second attempt, latch already set: still blocked, but silent. This is the
    // case hold-to-repeat turns into a toast flood without the latch.
    const auto second = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, true);
    REQUIRE(second.allowed == 0.0);
    REQUIRE_FALSE(second.warn);
    REQUIRE(second.latch);
}

TEST_CASE("clamp_jog_with_warn: retreating clears the latch so the next approach warns",
          "[jog_coalescer]") {
    const auto away = helix::clamp_jog_with_warn(100.0, 0.0, -10.0, 0.0, 250.0, true);
    REQUIRE(away.allowed == Catch::Approx(-10.0));
    REQUIRE_FALSE(away.latch);

    const auto again = helix::clamp_jog_with_warn(250.0, 0.0, 10.0, 0.0, 250.0, away.latch);
    REQUIRE(again.warn);
}

TEST_CASE("clamp_jog_with_warn: accounts for an uncommitted pending move", "[jog_coalescer]") {
    // 240 now, 8mm already queued, so only 2 of a further 10 may go.
    const auto r = helix::clamp_jog_with_warn(240.0, 8.0, 10.0, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(2.0));
}

TEST_CASE("clamp_jog_with_warn: a bed-moves printer clamps the gcode-space delta",
          "[jog_coalescer]") {
    // On a bed-moves printer the UP button produces gcode Z-. At Z=1 that must
    // clamp to -1. Passing the pre-inversion +10 would wrongly allow +2, which
    // is why MotionPanel clamps AFTER applying bed_moves_.
    const double gcode_delta = -10.0;
    const auto r = helix::clamp_jog_with_warn(1.0, 0.0, gcode_delta, 0.0, 250.0, false);
    REQUIRE(r.allowed == Catch::Approx(-1.0));
}

TEST_CASE("effective_jog_speed_mm_min: within limits passes through", "[jog_coalescer]") {
    CHECK(helix::effective_jog_speed_mm_min(6000, 0.0, 30000.0) == 6000);
    CHECK(helix::effective_jog_speed_mm_min(600, 0.0, 30000.0) == 600);
}

TEST_CASE("effective_jog_speed_mm_min: stored above the ceiling clamps to the ceiling",
          "[jog_coalescer]") {
    // A ceiling stored before the printer's configfile reply lowered it.
    CHECK(helix::effective_jog_speed_mm_min(42000, 0.0, 30000.0) == 30000);
    CHECK(helix::effective_jog_speed_mm_min(50000, 0.0, 30000.0) == 30000);
}

TEST_CASE("clamp_target_to_bounds: set axes clamp into range, unset axes untouched",
          "[jog_coalescer]") {
    using helix::AxisTarget;
    AxisTarget t;
    t.x = 100.0; // inside [0, 200]
    t.z = 15.0;  // inside [0, 20]
    const auto r = helix::clamp_target_to_bounds(t, 0.0, 200.0, 0.0, 180.0, {{0.0, 20.0}});
    REQUIRE(r.x.has_value());
    CHECK(*r.x == Catch::Approx(100.0));
    REQUIRE(r.z.has_value());
    CHECK(*r.z == Catch::Approx(15.0));
    CHECK_FALSE(r.y.has_value());
}

TEST_CASE("clamp_target_to_bounds: out-of-range targets clamp to the near edge",
          "[jog_coalescer]") {
    using helix::AxisTarget;
    AxisTarget t;
    t.x = 250.0; // above the max
    t.y = -30.0; // below the min
    t.z = 99.0;  // above the z max
    const auto r = helix::clamp_target_to_bounds(t, 0.0, 200.0, -10.0, 180.0, {{0.0, 20.0}});
    CHECK(*r.x == Catch::Approx(200.0));
    CHECK(*r.y == Catch::Approx(-10.0));
    CHECK(*r.z == Catch::Approx(20.0));
}

TEST_CASE("clamp_target_to_bounds: a set z passes through unclamped with no z range",
          "[jog_coalescer]") {
    using helix::AxisTarget;
    AxisTarget t;
    t.x = 100.0;
    t.z = 99.0; // far outside any plausible envelope; nothing to clamp against
    const auto r = helix::clamp_target_to_bounds(t, 0.0, 200.0, 0.0, 180.0, std::nullopt);
    CHECK(*r.x == Catch::Approx(100.0));
    CHECK(*r.z == Catch::Approx(99.0));
}

TEST_CASE("effective_jog_speed_mm_min: stored below the floor clamps to the floor",
          "[jog_coalescer]") {
    CHECK(helix::effective_jog_speed_mm_min(30, 60.0, 30000.0) == 60);
}

TEST_CASE("effective_jog_speed_mm_min: inverted bounds resolve to the maximum", "[jog_coalescer]") {
    // A caller that supplies min > max gets a defined answer rather than UB.
    // This cannot redden on libstdc++, whose std::clamp already lowers to
    // min(max(v, lo), hi): a mutation run shows the revert surviving.
    CHECK(helix::effective_jog_speed_mm_min(6000, 30000.0, 600.0) == 600);
}

TEST_CASE("jog_refused_for_unknown_position: refuses unless envelope and position are known",
          "[jog_coalescer]") {
    CHECK(helix::jog_refused_for_unknown_position(true, true) == false);
    CHECK(helix::jog_refused_for_unknown_position(true, false) == true);
    CHECK(helix::jog_refused_for_unknown_position(false, true) == true);
    CHECK(helix::jog_refused_for_unknown_position(false, false) == true);
}

// ============================================================================
// Absolute targets: latest target wins
// ============================================================================

using helix::AxisTarget;

namespace {

AxisTarget target(double x, double y, double z) {
    AxisTarget t;
    t.x = x;
    t.y = y;
    t.z = z;
    return t;
}

} // namespace

TEST_CASE("JogCoalescer: first target sends immediately", "[jog_coalescer]") {
    JogCoalescer c;
    auto send = c.on_target(target(10.0, 20.0, 5.0));
    REQUIRE(send.has_value());
    CHECK(*send->x == 10.0);
    CHECK(*send->y == 20.0);
    CHECK(*send->z == 5.0);
    CHECK(c.in_flight());
}

TEST_CASE("JogCoalescer: a pending target is replaced wholesale", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_target(target(10.0, 20.0, 5.0)).has_value());
    // Second target overwrites the first entirely, never merges per-axis.
    AxisTarget x_only;
    x_only.x = 30.0;
    CHECK_FALSE(c.on_target(x_only).has_value());
    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    const auto& t = std::get<AxisTarget>(*flush);
    REQUIRE(t.x.has_value());
    REQUIRE_FALSE(t.y.has_value()); // the first target's y=20 is gone
    REQUIRE_FALSE(t.z.has_value());
    CHECK(*t.x == 30.0);
    CHECK(c.in_flight());
}

TEST_CASE("JogCoalescer: a delta arriving while a target is pending discards it",
          "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_target(target(10.0, 20.0, 5.0)).has_value());
    CHECK_FALSE(c.on_target(target(99.0, 99.0, 99.0)).has_value()); // pending target
    CHECK_FALSE(c.on_tap({1.0, 0.0, 0.0}).has_value());             // delta wins pending

    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    const auto& m = std::get<AxisMove>(*flush);
    CHECK(m.dx == 1.0);
    CHECK(m.dy == 0.0);
    CHECK(m.dz == 0.0);
}

TEST_CASE("JogCoalescer: a target arriving while a delta is pending discards it",
          "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_tap({2.0, 0.0, 0.0}).has_value()); // pending delta sum 3
    CHECK_FALSE(c.on_target(target(7.0, 8.0, 9.0)).has_value());

    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    const auto& t = std::get<AxisTarget>(*flush);
    CHECK(*t.x == 7.0);
    CHECK(*t.y == 8.0);
    CHECK(*t.z == 9.0);
}

TEST_CASE("JogCoalescer: pending target survives across an ack as the flush", "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_tap({1.0, 0.0, 0.0}).has_value());
    CHECK_FALSE(c.on_target(target(50.0, 60.0, 70.0)).has_value());

    auto flush = c.on_ack();
    REQUIRE(flush.has_value());
    REQUIRE(std::holds_alternative<AxisTarget>(*flush));

    // The flushed target is now in flight; going idle needs a second ack.
    auto done = c.on_ack();
    CHECK_FALSE(done.has_value());
    CHECK_FALSE(c.in_flight());
}

TEST_CASE("JogCoalescer: predicted position with a target in flight and a delta pending",
          "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_target(target(100.0, 200.0, 30.0)).has_value()); // in flight
    CHECK_FALSE(c.on_tap({2.0, 0.0, -1.0}).has_value());          // pending delta

    CHECK(c.predicted_x(90.0) == 102.0); // target lands, then delta adds
    CHECK(c.predicted_y(180.0) == 200.0);
    CHECK(c.predicted_z(25.0) == 29.0);
}

TEST_CASE("JogCoalescer: unset target axes pass the current position through", "[jog_coalescer]") {
    JogCoalescer c;
    AxisTarget z_only;
    z_only.z = 15.0;
    REQUIRE(c.on_target(z_only).has_value());

    CHECK(c.predicted_x(42.0) == 42.0); // x untouched by a z-only target
    CHECK(c.predicted_y(-3.0) == -3.0);
    CHECK(c.predicted_z(0.0) == 15.0);
}

TEST_CASE("JogCoalescer: a pending target feeds the prediction, a later delta revises it",
          "[jog_coalescer]") {
    JogCoalescer c;
    REQUIRE(c.on_target(target(10.0, 0.0, 0.0)).has_value());     // in flight
    CHECK_FALSE(c.on_target(target(80.0, 0.0, 0.0)).has_value()); // pending
    CHECK(c.predicted_x(0.0) == 80.0);                            // latest target wins

    CHECK_FALSE(c.on_tap({5.0, 0.0, 0.0}).has_value()); // delta replaces pending target
    CHECK(c.predicted_x(0.0) == 15.0);                  // in-flight target 10 + delta 5
}

TEST_CASE("JogCoalescer: a new target starts where the in-flight move ends, not the pending one",
          "[jog_coalescer]") {
    JogCoalescer c;
    CHECK(c.target_start_z(3.0) == 3.0); // idle: the current position

    REQUIRE(c.on_target(target(0.0, 0.0, 10.0)).has_value());     // in flight to Z 10
    CHECK_FALSE(c.on_target(target(0.0, 0.0, 50.0)).has_value()); // pending, about to be replaced
    CHECK(c.predicted_z(3.0) == 50.0);
    CHECK(c.target_start_z(3.0) == 10.0);

    JogCoalescer d;
    REQUIRE(d.on_tap({0.0, 0.0, 2.0}).has_value());     // delta in flight
    CHECK_FALSE(d.on_tap({0.0, 0.0, 7.0}).has_value()); // pending delta, discarded by a target
    CHECK(d.target_start_z(3.0) == 5.0);
}
