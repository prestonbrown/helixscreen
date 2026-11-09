// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_sigterm_decision.cpp
 * @brief What one SIGTERM delivery does (prestonbrown/helixscreen#2011).
 */

#include "heater_hold.h"
#include "sigterm_decision.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::kSigtermRepeatWindowMs;
using helix::sigterm_action;
using helix::SigtermAction;

TEST_CASE("sigterm: no heater held stays a fast exit", "[sigterm][cold_pull]") {
    CHECK(sigterm_action(false, false, 0) == SigtermAction::FastExit);
}

TEST_CASE("sigterm: a held heater takes the graceful path", "[sigterm][cold_pull]") {
    CHECK(sigterm_action(true, false, 0) == SigtermAction::GracefulQuit);
}

TEST_CASE("sigterm: the duplicate of one stop leaves the graceful quit running",
          "[sigterm][cold_pull]") {
    CHECK(sigterm_action(true, true, 0) == SigtermAction::Ignore);
    CHECK(sigterm_action(true, true, kSigtermRepeatWindowMs - 1) == SigtermAction::Ignore);
    // Teardown may already have dropped the hold while the off is still leaving.
    CHECK(sigterm_action(false, true, 5) == SigtermAction::Ignore);
}

TEST_CASE("sigterm: a later repeat ends a stuck graceful quit", "[sigterm][cold_pull]") {
    CHECK(sigterm_action(true, true, kSigtermRepeatWindowMs) == SigtermAction::FastExit);
    CHECK(sigterm_action(false, true, kSigtermRepeatWindowMs + 1) == SigtermAction::FastExit);
}

TEST_CASE("heater hold: overlapping holds cannot release each other", "[sigterm][heater_hold]") {
    using helix::HeaterHold;
    REQUIRE_FALSE(HeaterHold::any_held_signal_safe());
    HeaterHold first;
    HeaterHold second;
    first.set(true);
    first.set(true); // a repeat is not a second hold
    second.set(true);
    first.set(false);
    first.set(false); // nor is a repeat release
    CHECK(HeaterHold::any_held_signal_safe());
    second.set(false);
    CHECK_FALSE(HeaterHold::any_held_signal_safe());
}

TEST_CASE("heater hold: a destroyed holder releases", "[sigterm][heater_hold]") {
    using helix::HeaterHold;
    auto hold = std::make_unique<HeaterHold>();
    hold->set(true);
    REQUIRE(HeaterHold::any_held_signal_safe());
    hold.reset();
    CHECK_FALSE(HeaterHold::any_held_signal_safe());
}
