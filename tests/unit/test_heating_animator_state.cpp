// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_heating_animator.h"
#include "ui_temperature_utils.h"

#include "../test_helpers/scoped_animations_enabled.h"
#include "lvgl_test_fixture.h"
#include "theme_manager.h"

#include <type_traits>

#include "../catch_amalgamated.hpp"

using helix::ChamberMode;
using helix::ui::temperature::HeatState;

// The animator's state must be the same function of (current, target) that the
// temperature label uses — otherwise the icon and the number contradict each
// other. Values here are DECIDEGREES; the animator's tolerance is 20 (= 2 deg).

TEST_CASE("HeatingIconAnimator::State is the shared HeatState", "[animator][heat_state]") {
    STATIC_REQUIRE(std::is_same_v<HeatingIconAnimator::State, HeatState>);
}

TEST_CASE("HeatingIconAnimator: unattached update does not change state",
          "[animator][heat_state]") {
    HeatingIconAnimator animator;
    REQUIRE_FALSE(animator.is_attached());
    animator.update(1500, 2000);
    // update() early-returns when icon_ is null; state stays at its initial value.
    REQUIRE(animator.get_state() == HeatState::Off);
}

TEST_CASE("HeatingIconAnimator: classifies cooling above target", "[animator][heat_state]") {
    // The pre-change state machine had no cooling branch: current far above target
    // matched `current >= target - TEMP_TOLERANCE` and reported AT_TARGET.
    REQUIRE(helix::ui::temperature::classify_heat_state(2500, 2000, 20) == HeatState::Cooling);
}

TEST_CASE("HeatingIconAnimator: decidegree tolerance matches the label's degrees",
          "[animator][heat_state]") {
    using helix::ui::temperature::classify_heat_state;
    // 199.9 deg vs 200.0 deg target -> at-temp in both units
    REQUIRE(classify_heat_state(1999, 2000, 20) == HeatState::AtTemp);
    REQUIRE(classify_heat_state(199, 200, 2) == HeatState::AtTemp);
    // 197.0 vs 200.0 -> heating in both
    REQUIRE(classify_heat_state(1970, 2000, 20) == HeatState::Heating);
    REQUIRE(classify_heat_state(197, 200, 2) == HeatState::Heating);
}

// ============================================================================
// update(current, target, mode) — chamber Maintaining-mode awareness.
//
// These need a real attached icon: update() early-returns when icon_ is null
// (see the "unattached update does not change state" case above), so state_
// and the pulse animation only move once attach() has run.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "HeatingIconAnimator: default mode is a passthrough (nozzle/bed unaffected)",
                 "[animator][heat_state][chamber_mode]") {
    lv_obj_t* icon = lv_obj_create(test_screen());
    HeatingIconAnimator animator;
    animator.attach(icon);

    // No mode argument — must classify identically to the plain classifier,
    // which is what every nozzle/bed call site (no ChamberMode concept) relies on.
    animator.update(1500, 2000);
    REQUIRE(animator.get_state() == HeatState::Heating);

    animator.detach();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "HeatingIconAnimator: Maintaining mode classifies Neutral even when cold, "
                 "never Heating",
                 "[animator][heat_state][chamber_mode]") {
    lv_obj_t* icon = lv_obj_create(test_screen());
    HeatingIconAnimator animator;
    animator.attach(icon);

    // Regression case: stone cold (current=0) and far below the Maintaining
    // ceiling (target=2000 = 200C). The plain classifier would call this
    // Heating; classify_heat_state_with_mode() must not.
    animator.update(0, 2000, ChamberMode::Maintaining);
    REQUIRE(animator.get_state() == HeatState::Neutral);
    REQUIRE_FALSE(animator.get_state() == HeatState::Heating);

    animator.detach();
}

TEST_CASE_METHOD(LVGLTestFixture, "HeatingIconAnimator: Maintaining above ceiling is Cooling",
                 "[animator][heat_state][chamber_mode]") {
    lv_obj_t* icon = lv_obj_create(test_screen());
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(2500, 2000, ChamberMode::Maintaining);
    REQUIRE(animator.get_state() == HeatState::Cooling);

    animator.detach();
}

// Pulse means "working toward a setpoint from below" — Maintaining never has a
// setpoint to work toward (the target is a cooling ceiling), so it must never
// pulse, even for the coldest possible input. lv_anim_get(&animator, nullptr)
// finds any animation registered against this animator instance as its `var`
// regardless of which exec callback it uses (NULL exec_cb matches any).
TEST_CASE_METHOD(LVGLTestFixture, "HeatingIconAnimator: Maintaining mode never pulses",
                 "[animator][heat_state][chamber_mode]") {
    // The pulse is gated on the animations preference, which the fixture forces
    // off — without this the state machine is never even consulted.
    helix::ui::ScopedAnimationsEnabled animations_on;
    lv_obj_t* icon = lv_obj_create(test_screen());
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(0, 2000, ChamberMode::Maintaining); // coldest possible input
    REQUIRE(animator.get_state() == HeatState::Neutral);
    REQUIRE(lv_anim_get(&animator, nullptr) == nullptr);

    animator.detach();
}

// Regression guard on the sibling behavior: real Heating must still pulse.
// Without this, a change that accidentally stopped ALL pulsing (not just
// Maintaining's) would pass the test above and go unnoticed.
TEST_CASE_METHOD(LVGLTestFixture, "HeatingIconAnimator: Heating mode still pulses",
                 "[animator][heat_state][chamber_mode]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    lv_obj_t* icon = lv_obj_create(test_screen());
    HeatingIconAnimator animator;
    animator.attach(icon);

    animator.update(1500, 2000, ChamberMode::Heating);
    REQUIRE(animator.get_state() == HeatState::Heating);
    REQUIRE(lv_anim_get(&animator, nullptr) != nullptr);

    animator.detach();
}
