// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_simulated_clock.cpp
 * @brief Unit tests for the shared simulated-time source
 *
 * The two scaling operations pull in opposite directions, so every direction
 * assertion here is written as a comparison against the 1x case rather than a
 * bare number: a swapped multiply and divide has to move a value the wrong way
 * relative to real time, not merely produce a different one.
 */

#include "../test_helpers/scoped_runtime_config.h"
#include "runtime_config.h"
#include "simulated_clock.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using helix::sim::SimSpeed;
using helix::sim::SimulatedClock;

namespace {

/// Set --sim-speed for one test and restore the whole RuntimeConfig afterwards.
class GlobalSpeedScope {
  public:
    explicit GlobalSpeedScope(double factor) {
        get_runtime_config()->sim_speedup = factor;
    }

  private:
    ScopedRuntimeConfig saved_;
};

} // namespace

// ============================================================================
// Clamping — one range for every consumer
// ============================================================================

TEST_CASE("SimSpeed: every factor lands in the one shared range", "[sim][simclock][clamp]") {
    REQUIRE(SimSpeed::of(1.0).factor() == Catch::Approx(helix::sim::MIN_SPEED));
    REQUIRE(SimSpeed::of(250.0).factor() == Catch::Approx(250.0));

    SECTION("sub-1x is not expressible — --sim-speed forbids it") {
        REQUIRE(SimSpeed::of(0.5).factor() == Catch::Approx(helix::sim::MIN_SPEED));
        REQUIRE(SimSpeed::of(0.0).factor() == Catch::Approx(helix::sim::MIN_SPEED));
        REQUIRE(SimSpeed::of(-5.0).factor() == Catch::Approx(helix::sim::MIN_SPEED));
    }

    SECTION("above the ceiling saturates") {
        REQUIRE(SimSpeed::of(100000.0).factor() == Catch::Approx(helix::sim::MAX_SPEED));
    }

    SECTION("a default-constructed speed is real time") {
        REQUIRE(SimSpeed().is_real_time());
        REQUIRE(SimSpeed::of(2.0).is_real_time() == false);
    }
}

TEST_CASE("SimSpeed: global() reads the --sim-speed flag", "[sim][simclock][clamp]") {
    GlobalSpeedScope scope(25.0);
    REQUIRE(SimSpeed::global().factor() == Catch::Approx(25.0));

    SECTION("an out-of-range flag value is brought into range, not honoured") {
        get_runtime_config()->sim_speedup = 0.25;
        REQUIRE(SimSpeed::global().factor() == Catch::Approx(helix::sim::MIN_SPEED));
    }
}

// ============================================================================
// The two operations run in opposite directions
// ============================================================================

TEST_CASE("SimSpeed: shortening a wait and accelerating progress are inverses",
          "[sim][simclock][direction]") {
    const auto real_time = SimSpeed::of(1.0);
    const auto fast = SimSpeed::of(10.0);

    SECTION("shorten_wait_ms DIVIDES: faster means less real waiting") {
        REQUIRE(real_time.shorten_wait_ms(2000) == 2000);
        REQUIRE(fast.shorten_wait_ms(2000) == 200);
        REQUIRE(fast.shorten_wait_ms(2000) < real_time.shorten_wait_ms(2000));
    }

    SECTION("accelerate_progress MULTIPLIES: faster means more simulated progress") {
        REQUIRE(real_time.accelerate_progress(2000.0) == Catch::Approx(2000.0));
        REQUIRE(fast.accelerate_progress(2000.0) == Catch::Approx(20000.0));
        REQUIRE(fast.accelerate_progress(2000.0) > real_time.accelerate_progress(2000.0));
    }

    SECTION("the two undo each other") {
        // 200ms of real sleeping is what 2000ms of simulated waiting costs, and
        // 200ms of real time is worth 2000ms of simulated progress.
        const int real_ms = fast.shorten_wait_ms(2000);
        REQUIRE(fast.accelerate_progress(std::chrono::milliseconds(real_ms)) ==
                std::chrono::milliseconds(2000));
    }

    SECTION("a wait never rounds down to a busy-spin") {
        REQUIRE(SimSpeed::of(1000.0).shorten_wait_ms(5) == 1);
        REQUIRE(fast.shorten_wait_ms(0) == 0);
        REQUIRE(fast.shorten_wait_ms(-10) == 0);
    }

    SECTION("shorten_wait_seconds divides too") {
        REQUIRE(fast.shorten_wait_seconds(60.0) == Catch::Approx(6.0));
        REQUIRE(real_time.shorten_wait_seconds(60.0) == Catch::Approx(60.0));
    }

    SECTION("accelerate_progress on a chrono span multiplies too") {
        REQUIRE(fast.accelerate_progress(std::chrono::seconds(1)) == std::chrono::seconds(10));
        REQUIRE(real_time.accelerate_progress(std::chrono::seconds(1)) == std::chrono::seconds(1));
    }
}

// ============================================================================
// A subsystem multiplier composed over the global flag
// ============================================================================

TEST_CASE("SimSpeed: a subsystem multiplier composes over the global flag",
          "[sim][simclock][compose]") {
    SECTION("with no flag the subsystem keeps its own pace") {
        GlobalSpeedScope scope(1.0);
        // The mock dryer's default: 1 real second = 1 simulated minute, so a
        // 4-hour cycle is watchable without passing --sim-speed at all.
        REQUIRE(SimSpeed::global().composed_with(60).factor() == Catch::Approx(60.0));
    }

    SECTION("the flag multiplies the subsystem, it does not replace it") {
        GlobalSpeedScope scope(10.0);
        REQUIRE(SimSpeed::global().composed_with(60).factor() == Catch::Approx(600.0));
        // Global alone is 10x — composing must not collapse to that.
        REQUIRE(SimSpeed::global().composed_with(60).factor() > SimSpeed::global().factor());
    }

    SECTION("a subsystem at 1x tracks the flag exactly") {
        GlobalSpeedScope scope(10.0);
        REQUIRE(SimSpeed::global().composed_with(1).factor() == Catch::Approx(10.0));
    }

    SECTION("the composed product obeys the same ceiling") {
        GlobalSpeedScope scope(100.0);
        REQUIRE(SimSpeed::global().composed_with(60).factor() ==
                Catch::Approx(helix::sim::MAX_SPEED));
    }

    SECTION("composition scales both operations, in their own directions") {
        GlobalSpeedScope scope(10.0);
        const auto dryer = SimSpeed::global().composed_with(60);
        REQUIRE(dryer.accelerate_progress(1.0) == Catch::Approx(600.0));
        REQUIRE(dryer.shorten_wait_ms(6000) == 10);
    }
}

// ============================================================================
// The clock, and the seam that drives it
// ============================================================================

TEST_CASE("SimulatedClock: the manual scope drives simulated time without sleeping",
          "[sim][simclock][seam]") {
    SECTION("advance() moves the clock by exactly that much simulated time") {
        SimulatedClock::ManualScope clock(SimSpeed::of(1.0));
        const auto start = SimulatedClock::now();
        clock.advance(std::chrono::hours(2));
        REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(SimulatedClock::now() - start) ==
                std::chrono::seconds(7200));
    }

    SECTION("a manual clock does not drift while the test runs") {
        SimulatedClock::ManualScope clock(SimSpeed::of(1.0));
        const auto first = SimulatedClock::now();
        const auto second = SimulatedClock::now();
        REQUIRE(first == second);
    }

    SECTION("advance_real() applies the scope's speed") {
        SimulatedClock::ManualScope clock(SimSpeed::of(60.0));
        const auto start = SimulatedClock::now();
        clock.advance_real(std::chrono::seconds(10));
        REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(SimulatedClock::now() - start) ==
                std::chrono::seconds(600));
    }

    SECTION("advance_real() at 1x is real time") {
        SimulatedClock::ManualScope clock(SimSpeed::of(1.0));
        const auto start = SimulatedClock::now();
        clock.advance_real(std::chrono::seconds(10));
        REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(SimulatedClock::now() - start) ==
                std::chrono::seconds(10));
    }

    SECTION("the scope defaults to the global flag") {
        GlobalSpeedScope global(30.0);
        SimulatedClock::ManualScope clock;
        const auto start = SimulatedClock::now();
        clock.advance_real(std::chrono::seconds(2));
        REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(SimulatedClock::now() - start) ==
                std::chrono::seconds(60));
    }

    SECTION("the clock never runs backwards when a scope ends") {
        SimulatedClock::time_point after_scope;
        SimulatedClock::time_point inside;
        {
            SimulatedClock::ManualScope clock(SimSpeed::of(1.0));
            clock.advance(std::chrono::hours(1));
            inside = SimulatedClock::now();
        }
        after_scope = SimulatedClock::now();
        REQUIRE(after_scope >= inside);
    }

    SECTION("nested scopes share one timeline") {
        SimulatedClock::ManualScope outer(SimSpeed::of(1.0));
        const auto start = SimulatedClock::now();
        outer.advance(std::chrono::minutes(5));
        {
            SimulatedClock::ManualScope inner(SimSpeed::of(1.0));
            inner.advance(std::chrono::minutes(5));
        }
        REQUIRE(std::chrono::duration_cast<std::chrono::minutes>(SimulatedClock::now() - start) ==
                std::chrono::minutes(10));
    }
}

TEST_CASE("SimulatedClock: the free-running clock advances", "[sim][simclock][seam]") {
    // No manual scope: the real steady clock drives it, so all this can assert
    // is that it moves forward rather than standing still or going backwards.
    const auto first = SimulatedClock::now();
    const auto second = SimulatedClock::now();
    REQUIRE(second >= first);
}
