// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_android_platform.cpp
 * @brief Unit tests for Android platform detection and wizard step logic
 *
 * Tests the runtime-overridable platform detection and the extracted
 * wizard step counting logic used for Android conditionalization.
 */

#include "board_identity.h"
#include "platform_info.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::wizard::StepId;

// ----------------------------------------------------------------------------
// Id-based navigation test helpers. The wizard uses a StepId registry; these
// tests build a full STEP_COUNT-entry skip vector and flip named steps off, mirroring
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

TEST_CASE("Wizard total steps with no skips is STEP_COUNT", "[android][wizard]") {
    REQUIRE(wizard_visible_count(all_steps()) == helix::wizard::STEP_COUNT);
}

TEST_CASE("Wizard total steps with wifi skipped is one fewer", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_visible_count(v) == helix::wizard::STEP_COUNT - 1);
}

TEST_CASE("Wizard total steps with wifi + touch_cal + language skipped is three fewer",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::TouchCalibration);
    skip(v, StepId::Language);
    REQUIRE(wizard_visible_count(v) == helix::wizard::STEP_COUNT - 3);
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
    // appearance: touch_cal,language not skipped -> display 3
    REQUIRE(wizard_display_number(StepId::Appearance, v) == 3);
    // connection: touch_cal,language,appearance not skipped, wifi skipped -> display 4
    REQUIRE(wizard_display_number(StepId::Connection, v) == 4);
    // printer_identify: touch_cal,language,appearance,connection not skipped -> display 5
    REQUIRE(wizard_display_number(StepId::PrinterIdentify, v) == 5);
}

TEST_CASE("Display step at telemetry (last) with wifi skipped is the last number",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    // Telemetry is last: every step but the skipped wifi is numbered.
    REQUIRE(wizard_display_number(StepId::Telemetry, v) == helix::wizard::STEP_COUNT - 1);
}

// ============================================================================
// Wizard Step Logic -- Navigation Forward
// ============================================================================

TEST_CASE("wizard_next(Appearance, wifi skipped) returns Connection", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_next(StepId::Appearance, v) == StepId::Connection);
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

TEST_CASE("wizard_prev(Connection, wifi skipped) returns Appearance", "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    REQUIRE(wizard_prev(StepId::Connection, v) == StepId::Appearance);
}

TEST_CASE("wizard_prev(Connection, wifi+appearance+language skipped) returns TouchCalibration",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::Appearance);
    skip(v, StepId::Language);
    REQUIRE(wizard_prev(StepId::Connection, v) == StepId::TouchCalibration);
}

TEST_CASE("wizard_prev(Connection, wifi+appearance+language+touch_cal skipped) returns none",
          "[android][wizard]") {
    auto v = all_steps();
    skip(v, StepId::Wifi);
    skip(v, StepId::Appearance);
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

    // Steps before FilamentSensor (idx 10): 0..9
    // Skipped: wifi(3), ams(8), led(9) = 3 skipped
    // Non-skipped before FilamentSensor: 7 -> display = 1 + 7 = 8
    REQUIRE(wizard_display_number(StepId::FilamentSensor, v) == 8);

    REQUIRE(wizard_visible_count(v) == helix::wizard::STEP_COUNT - 3);
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
    REQUIRE(wizard_visible_count(v) == helix::wizard::STEP_COUNT - 1);

    // Step after appearance should be connection, not wifi
    REQUIRE(wizard_next(StepId::Appearance, v) == StepId::Connection);

    // Clean up
    set_platform_override(-1);
}

// ============================================================================
// is_printer_embedded()
// ============================================================================
//
// The first-run wizard asks for a Moonraker address. On a package that runs ON
// the printer, Moonraker is on the same box and the question has exactly one
// answer, so the wizard's connected-fast-path treats it as nothing to ask.
// A desktop or Pi build has no such guarantee and must still ask.

TEST_CASE("is_printer_embedded is overridable for tests", "[platform]") {
    set_printer_embedded_override(1);
    CHECK(is_printer_embedded());

    set_printer_embedded_override(0);
    CHECK_FALSE(is_printer_embedded());

    set_printer_embedded_override(-1);
}

TEST_CASE("is_printer_embedded follows the board", "[platform]") {
    set_printer_embedded_override(-1);

    board::set_current_override(board::Board::Generic);
    CHECK_FALSE(is_printer_embedded());

    for (board::Board b :
         {board::Board::Ad5m, board::Board::Ad5x, board::Board::Creator5, board::Board::Creator5Pro,
          board::Board::K1, board::Board::K2, board::Board::Cc1, board::Board::SnapmakerU1}) {
        board::set_current_override(b);
        INFO("board: " << board::info(b).key);
        CHECK(is_printer_embedded());
    }
    board::set_current_override(std::nullopt);
}

// An override must not leak into the platform answer for the OTHER predicate —
// they are independent questions (an Android tablet is not printer-embedded,
// but neither is a desktop).
TEST_CASE("printer-embedded override is independent of the Android override", "[platform]") {
    set_printer_embedded_override(1);
    set_platform_override(0);
    CHECK(is_printer_embedded());
    CHECK_FALSE(is_android_platform());

    set_printer_embedded_override(-1);
    set_platform_override(-1);
}
