// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tap_lockout.cpp
 * @brief TapLockout: a target that appeared under the finger refuses the taps
 * of the gesture that raised it
 */

#include "tap_lockout.h"

#include "../catch_amalgamated.hpp"

using helix::ui::TapLockout;

TEST_CASE("TapLockout: the press, timed from the first frame, decides", "[edit-callout]") {
    constexpr uint32_t kCreated = 1000;
    TapLockout lockout;
    lockout.reset(kCreated);

    SECTION("a double tap: the press lands inside the lockout and lifts after it") {
        lockout.on_drawn(kCreated + 10);
        lockout.on_press(kCreated + 240);
        CHECK_FALSE(lockout.release_acts(true, kCreated + 310));
    }
    SECTION("a press the lockout after the first frame acts") {
        lockout.on_drawn(kCreated + 10);
        lockout.on_press(kCreated + 10 + TapLockout::kDefaultMs);
        CHECK(lockout.release_acts(true, kCreated + 400));
    }
    SECTION("a slow board: the lockout runs from the frame, not the creation") {
        lockout.on_drawn(kCreated + 500);
        lockout.on_press(kCreated + 600);
        CHECK_FALSE(lockout.release_acts(true, kCreated + 650));
        // Later frames do not move it.
        lockout.on_drawn(kCreated + 900);
        lockout.on_press(kCreated + 500 + TapLockout::kDefaultMs);
        CHECK(lockout.release_acts(true, kCreated + 900));
    }
    SECTION("a press read before the first frame is inside the lockout") {
        lockout.on_press(kCreated + 400);
        lockout.on_drawn(kCreated + 450);
        CHECK_FALSE(lockout.release_acts(true, kCreated + 1000));
    }
    SECTION("a release that lifted off the target does not act") {
        lockout.on_drawn(kCreated);
        lockout.on_press(kCreated + 1000);
        CHECK_FALSE(lockout.release_acts(false, kCreated + 1050));
    }
    SECTION("a lost press clears the latch, so a click is timed at the click") {
        lockout.on_drawn(kCreated);
        lockout.on_press(kCreated + 100);
        lockout.on_press_lost();
        CHECK(lockout.release_acts(true, kCreated + TapLockout::kDefaultMs));
    }
    SECTION("a click with no press behind it is timed at the click") {
        lockout.on_drawn(kCreated);
        CHECK_FALSE(lockout.release_acts(true, kCreated + TapLockout::kDefaultMs - 1));
        CHECK(lockout.release_acts(true, kCreated + TapLockout::kDefaultMs));
    }
    SECTION("a release consumes its press") {
        lockout.on_drawn(kCreated);
        lockout.on_press(kCreated + 100);
        CHECK_FALSE(lockout.release_acts(true, kCreated + 400));
        CHECK(lockout.release_acts(true, kCreated + 400));
    }
    SECTION("reset starts a new target's lockout") {
        lockout.on_drawn(kCreated);
        lockout.on_press(kCreated + 100);
        lockout.reset(kCreated + 2000);
        CHECK_FALSE(lockout.release_acts(true, kCreated + 2100));
    }
}
