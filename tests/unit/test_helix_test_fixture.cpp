// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_emergency_stop.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "system_settings_manager.h"
#include "temperature_sensor_manager.h"
#include "thermal_rate_model.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture::reset_all resets SystemSettingsManager language",
                 "[test-fixture][isolation]") {
    // Ctor reset already ran — default should be visible.
    REQUIRE(SystemSettingsManager::instance().get_language() == "en");

    // Mutation observable.
    SystemSettingsManager::instance().set_language("fr");
    REQUIRE(SystemSettingsManager::instance().get_language() == "fr");

    // Manual reset restores default — proves reset_all() itself resets, not just ctor.
    HelixTestFixture::reset_all();
    REQUIRE(SystemSettingsManager::instance().get_language() == "en");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture::reset_all clears EmergencyStopOverlay recovery state",
                 "[test-fixture][isolation]") {
    // EmergencyStopOverlay is a process-wide singleton and its suppression window
    // is a wall-clock deadline: production arms 10-30s, while a test advances
    // lv_tick by tens of milliseconds. One armed window therefore outlives every
    // test that follows it in the same binary, and each of these gates whether a
    // recovery dialog may appear at all.
    auto& estop = EmergencyStopOverlay::instance();

    REQUIRE_FALSE(estop.is_recovery_suppressed());
    REQUIRE_FALSE(estop.is_expected_restart());

    estop.suppress_recovery_dialog(recovery_suppression::EXTRA);
    EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, true);
    REQUIRE(estop.is_recovery_suppressed());
    REQUIRE(estop.is_expected_restart());

    HelixTestFixture::reset_all();

    REQUIRE_FALSE(estop.is_recovery_suppressed());
    REQUIRE_FALSE(estop.is_expected_restart());
    REQUIRE(EmergencyStopOverlayTestAccess::pending_recovery_reason(estop) == RecoveryReason::NONE);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture::reset_all clears discovered temperature sensors",
                 "[test-fixture][isolation]") {
    // TemperatureSensorManager is a process-wide singleton with no lifetime hook
    // of its own. A widget built against an empty config auto-selects
    // get_sensors_sorted().front(), so a sensor left behind by an earlier test
    // makes the thermistor tile render a live reading and display name where a
    // clean process renders its placeholder strings - wider content, measured
    // against the same tile.
    auto& sensors = helix::sensors::TemperatureSensorManager::instance();

    REQUIRE(sensors.get_sensors_sorted().empty());

    sensors.discover(helix::test::discovery_of({"temperature_sensor helix_reset_all_probe"}));
    REQUIRE_FALSE(sensors.get_sensors_sorted().empty());

    HelixTestFixture::reset_all();
    REQUIRE(sensors.get_sensors_sorted().empty());
}

TEST_CASE_METHOD(HelixTestFixture, "HelixTestFixture::reset_all forgets the heating rates",
                 "[test-fixture][isolation]") {
    // The rates price every pre-print phase and its fallback timeout, so one
    // left set moves the next test's deadlines.
    auto& rates = ThermalRateManager::instance();
    const float fresh = rates.estimate_heating_seconds("heater_bed", 0.0f, 100.0f);

    rates.get_model("heater_bed").set_default_rate(7.0f);
    REQUIRE(rates.estimate_heating_seconds("heater_bed", 0.0f, 100.0f) != fresh);

    HelixTestFixture::reset_all();
    REQUIRE(rates.estimate_heating_seconds("heater_bed", 0.0f, 100.0f) == fresh);
}
