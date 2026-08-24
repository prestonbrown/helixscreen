// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_android_platform.cpp
 * @brief Unit tests for Android platform detection and wizard step logic
 *
 * Tests the runtime-overridable platform detection and the extracted
 * wizard step counting logic used for Android conditionalization.
 */

#include "platform_info.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::wizard::StepId;

// ----------------------------------------------------------------------------
// Id-based navigation test helpers. The wizard uses a StepId registry; these
// tests build a full 13-entry skip vector and flip named steps off, mirroring
// what the registry produces. `id()` is shorthand for StepId(int) so the
// step-index comments below still read clearly.
// ----------------------------------------------------------------------------
static StepId id(int i) {
    return static_cast<StepId>(i);
}

static std::vector<StepSkip> all_steps() {
    std::vector<StepSkip> v;
    for (int i = 0; i < wizard::STEP_COUNT; ++i)
        v.push_back({id(i), false});
    return v;
}

static void skip(std::vector<StepSkip>& v, StepId s) {
    for (auto& e : v)
        if (e.id == s)
            e.skipped = true;
}

// ============================================================================
// Platform Detection Tests
// ============================================================================

TEST_CASE("Platform detection defaults to non-Android on macOS/Linux", "[android][platform]") {
    // Reset any previous override
    set_platform_override(-1);

    // On test builds (macOS/Linux), __ANDROID__ is not defined
    REQUIRE(is_android_platform() == false);
}

TEST_CASE("Platform override to true makes is_android_platform return true",
          "[android][platform]") {
    set_platform_override(1);
    REQUIRE(is_android_platform() == true);

    // Clean up
    set_platform_override(-1);
}

TEST_CASE("Platform override to false makes is_android_platform return false",
          "[android][platform]") {
    set_platform_override(0);
    REQUIRE(is_android_platform() == false);

    // Clean up
    set_platform_override(-1);
}

TEST_CASE("Platform override reset to -1 restores compile-time default", "[android][platform]") {
    // Force Android
    set_platform_override(1);
    REQUIRE(is_android_platform() == true);

    // Reset to compile-time default
    set_platform_override(-1);
    REQUIRE(is_android_platform() == false);
}

// ============================================================================
// Wizard Step Logic -- Total Steps (id-based registry navigation)
// ============================================================================

TEST_CASE("Wizard total steps with no skips is 13", "[android][wizard]") {
    REQUIRE(wizard_visible_count(all_steps()) == 13);
}

TEST_CASE("Wizard total steps with wifi skipped is 12", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_visible_count(v) == 12);
}

TEST_CASE("Wizard total steps with wifi + touch_cal + language skipped is 10",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::TouchCalibration);
    skip(v, StepId::Language);
    REQUIRE(wizard_visible_count(v) == 10);
}

// ============================================================================
// Wizard Step Logic -- Display Step Numbers
//
// wizard_display_number() returns a 1-based display number: 1 + the number of
// visible entries strictly before the given step.
// ============================================================================

TEST_CASE("Display step calculation with wifi skipped", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);

    // touch_cal: first step -> display 1
    REQUIRE(wizard_display_number(StepId::TouchCalibration, v) == 1);
    // language: touch_cal not skipped -> display 2
    REQUIRE(wizard_display_number(StepId::Language, v) == 2);
    // connection: touch_cal,language not skipped, wifi skipped -> display 3
    REQUIRE(wizard_display_number(StepId::Connection, v) == 3);
    // printer_identify: touch_cal,language,connection not skipped -> display 4
    REQUIRE(wizard_display_number(StepId::PrinterIdentify, v) == 4);
}

TEST_CASE("Display step at telemetry (last) with wifi skipped is 12", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    // 12 steps before telemetry, one skipped (wifi) -> 11 non-skipped + 1 = 12
    REQUIRE(wizard_display_number(StepId::Telemetry, v) == 12);
}

// ============================================================================
// Wizard Step Logic -- Navigation Forward
// ============================================================================

TEST_CASE("wizard_next(Language, wifi skipped) returns Connection", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_next(StepId::Language, v) == StepId::Connection);
}

TEST_CASE("wizard_next(Wifi, wifi skipped) returns Connection", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    // Even if somehow on the skipped wifi step, next non-skipped is connection
    REQUIRE(wizard_next(StepId::Wifi, v) == StepId::Connection);
}

TEST_CASE("Navigation forward skips all disabled steps correctly", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::AmsIdentify);
    skip(v, StepId::LedSelect);

    // Forward from FanSelect: next non-skipped skips Ams + Led -> FilamentSensor
    REQUIRE(wizard_next(StepId::FanSelect, v) == StepId::FilamentSensor);
}

TEST_CASE("wizard_next reports done at end", "[android][wizard]") {
    auto v = all_steps();
    REQUIRE(wizard_is_last(StepId::Telemetry, v));
    REQUIRE_FALSE(wizard_next(StepId::Telemetry, v).has_value());
}

// ============================================================================
// Wizard Step Logic -- Navigation Backward
// ============================================================================

TEST_CASE("wizard_prev(Connection, wifi skipped) returns Language", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_prev(StepId::Connection, v) == StepId::Language);
}

TEST_CASE("wizard_prev(Connection, wifi+language skipped) returns TouchCalibration",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::Language);
    REQUIRE(wizard_prev(StepId::Connection, v) == StepId::TouchCalibration);
}

TEST_CASE("wizard_prev(Connection, wifi+language+touch_cal skipped) returns none",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::Language);
    skip(v, StepId::TouchCalibration);
    REQUIRE_FALSE(wizard_prev(StepId::Connection, v).has_value());
}

TEST_CASE("Navigation backward skips all disabled steps correctly", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::AmsIdentify);
    skip(v, StepId::LedSelect);

    // Backward from FilamentSensor: should skip Led and Ams -> FanSelect
    REQUIRE(wizard_prev(StepId::FilamentSensor, v) == StepId::FanSelect);
}

TEST_CASE("wizard_prev reports none at beginning", "[android][wizard]") {
    auto v = all_steps();
    REQUIRE_FALSE(wizard_prev(StepId::TouchCalibration, v).has_value());
}

// ============================================================================
// Multiple Skips -- Display Step Verification
// ============================================================================

TEST_CASE("Multiple skips: wifi + ams + led, display number at FilamentSensor",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::AmsIdentify);
    skip(v, StepId::LedSelect);

    // Steps before FilamentSensor (idx 9): 0..8
    // Skipped: wifi(2), ams(7), led(8) = 3 skipped
    // Non-skipped before FilamentSensor: 6 -> display = 1 + 6 = 7
    REQUIRE(wizard_display_number(StepId::FilamentSensor, v) == 7);

    // Total: 13 - 3 = 10
    REQUIRE(wizard_visible_count(v) == 10);
}

// ============================================================================
// Combined Android Scenario
// ============================================================================

TEST_CASE("Android scenario: wifi skipped when platform is Android",
          "[android][platform][wizard]") {
    // Simulate Android platform
    set_platform_override(1);
    REQUIRE(is_android_platform() == true);

    // On Android, wifi step should be skipped
    auto v = all_steps();
    bool android_skips_wifi = is_android_platform();
    if (android_skips_wifi)
        skip(v, StepId::Wifi);

    REQUIRE(android_skips_wifi == true);
    REQUIRE(wizard_visible_count(v) == 12);

    // Step after language should be connection, not wifi
    REQUIRE(wizard_next(StepId::Language, v) == StepId::Connection);

    // Clean up
    set_platform_override(-1);
}
