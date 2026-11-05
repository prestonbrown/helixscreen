// SPDX-License-Identifier: GPL-3.0-or-later
#include "chute_jog_guard.h"

#include "../catch_amalgamated.hpp"

using helix::ChuteJogGuard;
using Outcome = helix::ChuteJogGuard::Outcome;
using Catch::Approx;

namespace {

constexpr double kParkX = 184.5;
constexpr double kParkY = 291.5;
constexpr double kXMin = -5.0;
constexpr double kXMax = 305.0;
constexpr double kYMin = -0.5;
constexpr double kYMax = 307.5;
constexpr uint32_t kSettled = ChuteJogGuard::kSettleMs;

/// Armed at t=0; the first jog at kSettled latches the park position.
ChuteJogGuard settled_guard() {
    ChuteJogGuard guard;
    guard.arm(0);
    return guard;
}

ChuteJogGuard::Decision jog_x(ChuteJogGuard& g, double delta, double live_x = kParkX,
                              double live_y = kParkY) {
    return g.jog_x(delta, false, kSettled, live_x, live_y, kXMin, kXMax);
}

ChuteJogGuard::Decision jog_y(ChuteJogGuard& g, double delta, double live_x = kParkX,
                              double live_y = kParkY) {
    return g.jog_y(delta, kSettled, live_x, live_y, kYMin, kYMax);
}

} // namespace

TEST_CASE("Chute jog sends nothing until armed and settled", "[chute_jog]") {
    ChuteJogGuard guard;
    CHECK(jog_x(guard, 1.0).outcome == Outcome::PositionUnknown);
    CHECK(jog_y(guard, 1.0).outcome == Outcome::PositionUnknown);

    guard.arm(100);
    CHECK(guard.jog_x(1.0, false, 100 + kSettled - 1, kParkX, kParkY, kXMin, kXMax).outcome ==
          Outcome::PositionUnknown);
    CHECK(guard.jog_y(1.0, 100 + kSettled - 1, kParkX, kParkY, kYMin, kYMax).delta == 0.0);
    CHECK(guard.jog_x(1.0, false, 100 + kSettled, kParkX, kParkY, kXMin, kXMax).outcome ==
          Outcome::Move);

    guard.disarm();
    CHECK(jog_x(guard, 1.0).outcome == Outcome::PositionUnknown);
}

TEST_CASE("Chute X stays within its reach of park", "[chute_jog]") {
    auto guard = settled_guard();

    SECTION("a partial step stops at the edge, then warns once") {
        CHECK(jog_x(guard, 10.0).delta == Approx(10.0));
        const auto partial = jog_x(guard, 10.0, kParkX + 10.0);
        CHECK(partial.outcome == Outcome::Move);
        CHECK(partial.delta == Approx(ChuteJogGuard::kXReachMm - 10.0));

        const auto blocked = jog_x(guard, 10.0, kParkX + ChuteJogGuard::kXReachMm);
        CHECK(blocked.outcome == Outcome::AxisLimit);
        CHECK(blocked.delta == 0.0);
        CHECK(blocked.warn);
        CHECK_FALSE(jog_x(guard, 10.0, kParkX + ChuteJogGuard::kXReachMm).warn);
    }

    SECTION("the low side has the same reach") {
        CHECK(jog_x(guard, -10.0).delta == Approx(-10.0));
        CHECK(jog_x(guard, -10.0, kParkX - 10.0).delta == Approx(-5.0));
        CHECK(jog_x(guard, -10.0, kParkX - 15.0).outcome == Outcome::AxisLimit);
    }

    SECTION("fast taps on a lagging position cannot overshoot") {
        double total = 0.0;
        for (int i = 0; i < 6; ++i) {
            total += jog_x(guard, 10.0).delta;
        }
        CHECK(total == Approx(ChuteJogGuard::kXReachMm));
    }

    SECTION("a retreat on a lagging position cannot pass the far edge") {
        CHECK(jog_x(guard, 10.0).delta == Approx(10.0));
        double total = 10.0;
        for (int i = 0; i < 4; ++i) {
            total += jog_x(guard, -10.0).delta;
        }
        CHECK(total >= -ChuteJogGuard::kXReachMm - 1e-9);
    }

    SECTION("a head already outside the reach retreats only by the step asked for") {
        const auto retreat = jog_x(guard, -1.0, kParkX);
        CHECK(retreat.delta == Approx(-1.0));
        const auto outside = jog_x(guard, -1.0, kParkX + 40.0);
        CHECK(outside.delta == Approx(-1.0));
        CHECK(jog_x(guard, 1.0, kParkX + 40.0).outcome == Outcome::AxisLimit);
    }
}

TEST_CASE("Chute X reach never passes the machine limits", "[chute_jog]") {
    ChuteJogGuard guard;
    guard.arm(0);
    constexpr double kNearEnd = 300.0;
    CHECK(guard.jog_x(10.0, false, kSettled, kNearEnd, kParkY, kXMin, kXMax).delta == Approx(5.0));
    CHECK(guard.jog_x(10.0, false, kSettled, kNearEnd + 5.0, kParkY, kXMin, kXMax).outcome ==
          Outcome::AxisLimit);
}

TEST_CASE("Chute X past park takes only single fine steps near park X", "[chute_jog]") {
    auto guard = settled_guard();
    constexpr double kFine = ChuteJogGuard::kFineStepMm;
    constexpr double kReach = ChuteJogGuard::kFineReachMm;
    constexpr double kPastPark = kParkY + 10.0;

    SECTION("at park and on the bed side of it, a coarse X step moves") {
        CHECK(jog_x(guard, 10.0).outcome == Outcome::Move);
        CHECK(jog_y(guard, -10.0).outcome == Outcome::Move);
        CHECK(jog_x(guard, 1.0, kParkX + 10.0, kParkY - 10.0).outcome == Outcome::Move);
    }

    SECTION("a commanded Y past park refuses a coarse X before the live Y shows it") {
        REQUIRE(jog_y(guard, 10.0).delta == Approx(10.0));
        const auto refused = jog_x(guard, 1.0);
        CHECK(refused.outcome == Outcome::CoarsePastPark);
        CHECK(refused.delta == 0.0);
        CHECK(refused.warn);
        CHECK(guard.park_y() == Approx(kParkY));
    }

    SECTION("a live Y past park refuses a coarse X with no jog commanded") {
        REQUIRE(jog_x(guard, kFine).outcome == Outcome::Move);
        CHECK(jog_x(guard, 0.5, kParkX, kPastPark).outcome == Outcome::CoarsePastPark);
    }

    SECTION("a fine step moves, on either side of park X") {
        REQUIRE(jog_y(guard, 10.0).outcome == Outcome::Move);
        CHECK(jog_x(guard, kFine, kParkX, kPastPark).delta == Approx(kFine));
        CHECK(jog_x(guard, -kFine, kParkX, kPastPark).delta == Approx(-kFine));
        // The pad's distances arrive as float.
        CHECK(jog_x(guard, static_cast<float>(kFine), kParkX, kPastPark).outcome == Outcome::Move);
    }

    SECTION("fine steps on a lagging position stop at the reach") {
        REQUIRE(jog_y(guard, 10.0).outcome == Outcome::Move);
        double total = 0.0;
        for (int i = 0; i < 40; ++i) {
            total += jog_x(guard, kFine, kParkX, kPastPark).delta;
        }
        CHECK(total == Approx(kReach));
        const auto blocked = jog_x(guard, kFine, kParkX, kPastPark);
        CHECK(blocked.outcome == Outcome::AxisLimit);
        CHECK(blocked.delta == 0.0);
    }

    SECTION("a live X past the reach is judged by its worst reading") {
        REQUIRE(jog_y(guard, 10.0).outcome == Outcome::Move);
        CHECK(jog_x(guard, kFine, kParkX + kReach, kPastPark).outcome == Outcome::AxisLimit);
        CHECK(jog_x(guard, -kFine, kParkX + kReach, kPastPark).delta == Approx(-kFine));
    }

    SECTION("a hold repeat is refused past park and allowed at park") {
        CHECK(guard.jog_x(kFine, true, kSettled, kParkX, kParkY, kXMin, kXMax).outcome ==
              Outcome::Move);
        REQUIRE(jog_y(guard, 10.0).outcome == Outcome::Move);
        const auto refused = guard.jog_x(kFine, true, kSettled, kParkX, kPastPark, kXMin, kXMax);
        CHECK(refused.outcome == Outcome::RepeatPastPark);
        CHECK(refused.delta == 0.0);
        CHECK_FALSE(refused.warn);
        CHECK(guard.jog_x(kFine, false, kSettled, kParkX, kPastPark, kXMin, kXMax).outcome ==
              Outcome::Move);
    }

    SECTION("backing Y out to park restores the coarse X window") {
        REQUIRE(jog_y(guard, 10.0).outcome == Outcome::Move);
        REQUIRE(jog_y(guard, -10.0, kParkX, kPastPark).outcome == Outcome::Move);
        CHECK(jog_x(guard, 10.0).delta == Approx(10.0));
    }

    SECTION("a Y jog first still latches the park it started from") {
        CHECK(jog_y(guard, 1.0).outcome == Outcome::Move);
        CHECK(guard.park_y() == Approx(kParkY));
    }
}

TEST_CASE("Chute Y passes park only with X near its park", "[chute_jog]") {
    auto guard = settled_guard();
    constexpr double kReach = ChuteJogGuard::kFineReachMm;

    SECTION("X at the reach may still enter") {
        REQUIRE(jog_x(guard, kReach).outcome == Outcome::Move);
        CHECK(jog_y(guard, 10.0).delta == Approx(10.0));
    }

    SECTION("X past the reach stops Y at park") {
        REQUIRE(jog_x(guard, 2.5).outcome == Outcome::Move);
        const auto refused = jog_y(guard, 10.0);
        CHECK(refused.outcome == Outcome::XDisplaced);
        CHECK(refused.delta == 0.0);
        CHECK(refused.warn);
        CHECK(guard.park_x() == Approx(kParkX));
    }

    SECTION("a live X past the reach counts before the commanded one") {
        REQUIRE(jog_x(guard, 0.1).outcome == Outcome::Move);
        CHECK(jog_y(guard, 10.0, kParkX + 2.5, kParkY).outcome == Outcome::XDisplaced);
    }

    SECTION("on the bed side of park Y moves, and stops at park") {
        REQUIRE(jog_x(guard, 15.0).outcome == Outcome::Move);
        CHECK(jog_y(guard, -5.0).delta == Approx(-5.0));
        const auto partial = jog_y(guard, 10.0, kParkX + 15.0, kParkY - 5.0);
        CHECK(partial.outcome == Outcome::Move);
        CHECK(partial.delta == Approx(5.0));
        CHECK(jog_y(guard, 10.0, kParkX + 15.0, kParkY).outcome == Outcome::XDisplaced);
    }

    SECTION("X back within the reach frees the rear zone") {
        REQUIRE(jog_x(guard, 15.0).outcome == Outcome::Move);
        REQUIRE(jog_y(guard, 10.0, kParkX + 15.0).outcome == Outcome::XDisplaced);
        REQUIRE(jog_x(guard, -14.0, kParkX + 15.0).outcome == Outcome::Move);
        CHECK(jog_y(guard, 10.0, kParkX + 1.0).delta == Approx(10.0));
    }
}

TEST_CASE("Chute Y clamps to the machine limits on the commanded position", "[chute_jog]") {
    auto guard = settled_guard();
    const double total = jog_y(guard, 10.0).delta + jog_y(guard, 10.0).delta;
    CHECK(total == Approx(kYMax - kParkY));
    const auto blocked = jog_y(guard, 10.0);
    CHECK(blocked.outcome == Outcome::AxisLimit);
    CHECK(blocked.warn);
}

TEST_CASE("Re-arming starts a fresh chute jog session", "[chute_jog]") {
    auto guard = settled_guard();
    CHECK(jog_x(guard, 10.0).outcome == Outcome::Move);
    guard.arm(0);
    constexpr double kNewParkX = 100.0;
    CHECK(guard.jog_x(10.0, false, kSettled, kNewParkX, kParkY, kXMin, kXMax).delta ==
          Approx(10.0));
    CHECK(guard.jog_x(10.0, false, kSettled, kNewParkX, kParkY, kXMin, kXMax).delta == Approx(5.0));
}
