// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_filament_sensor_select.cpp
 * @brief Unit tests for WizardFilamentSensorSelectStep skip logic and auto-configuration
 *
 * Tests cover:
 * - get_standalone_sensor_count() queries FilamentSensorManager directly
 * - should_skip() returns correct values based on sensor count
 * - auto_configure_single_sensor() sets RUNOUT role and saves config
 * - Integration: wizard skip flow with single sensor auto-configuration
 */

#include "ui_update_queue.h"
#include "ui_wizard_filament_sensor_select.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "printer_discovery.h"
#include "printer_hardware.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class WizardFilamentSensorSelectTestFixture : public LVGLTestFixture {
  public:
    WizardFilamentSensorSelectTestFixture() {
        // Initialize FilamentSensorManager subjects (idempotent)
        sensor_mgr().init_subjects();

        // Reset state for test isolation
        FilamentSensorManagerTestAccess::reset(sensor_mgr());

        // Anchor the wizard's AMS filter to a no-MMU baseline. The wizard
        // consults get_printer_state().get_discovery() through
        // is_standalone_filament_sensor(); PrinterState is a
        // singleton, so a prior test that installed an MMU discovery
        // could otherwise flip the filter and make our "standalone"
        // expectations (e.g. "toolhead") fail. Capture the prior state
        // in saved_discovery_ so we can restore it in the dtor — leaving
        // a permanently-empty discovery behind would leak into any later
        // test that exercises PrinterState directly.
        saved_discovery_ = get_printer_state().get_discovery();
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        // set_hardware fans out through capabilities_state_.set_hardware,
        // which sets lv_subjects; per L048, drain before assertions so
        // queued observer notifications can't race the test body.
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    ~WizardFilamentSensorSelectTestFixture() {
        // Reset after each test, then restore the prior PrinterState
        // discovery so subsequent fixtures in this shard see whatever
        // they expected (not a forcibly-empty discovery).
        FilamentSensorManagerTestAccess::reset(sensor_mgr());
        get_printer_state().set_hardware(std::move(saved_discovery_));
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

  protected:
    FilamentSensorManager& sensor_mgr() {
        return FilamentSensorManager::instance();
    }

    // Helper to discover test sensors (standalone, non-AMS)
    void discover_standalone_sensors(const std::vector<std::string>& sensors) {
        discover_filament_sensors(sensor_mgr(), sensors);
    }

    // Helper to discover AMS sensors (should be filtered out)
    // These use patterns is_standalone_filament_sensor() rejects
    void discover_ams_sensors() {
        std::vector<std::string> sensors = {
            "filament_switch_sensor lane_1",   // AFC lane sensor - matches "lane"
            "filament_switch_sensor slot_2",   // AFC slot sensor - matches "slot"
            "filament_switch_sensor turtle_1", // BoxTurtle sensor - matches "turtle"
            "filament_switch_sensor afc_hub"   // AFC hub - matches "afc"
        };
        discover_filament_sensors(sensor_mgr(), sensors);
    }

    // Helper to discover mixed sensors (some AMS, some standalone)
    void discover_mixed_sensors() {
        std::vector<std::string> sensors = {
            "filament_switch_sensor runout",   // Standalone
            "filament_switch_sensor lane_1",   // AMS (AFC)
            "filament_switch_sensor toolhead", // Standalone
            "filament_switch_sensor slot_2"    // AMS (AFC)
        };
        discover_filament_sensors(sensor_mgr(), sensors);
    }

  private:
    helix::PrinterDiscovery saved_discovery_;
};

// Static members

// ============================================================================
// get_standalone_sensor_count() Tests
// ============================================================================

TEST_CASE_METHOD(WizardFilamentSensorSelectTestFixture,
                 "WizardFilamentSensorSelectStep - get_standalone_sensor_count",
                 "[wizard][filament][count]") {
    WizardFilamentSensorSelectStep step;
    step.init_subjects();

    SECTION("Returns 0 when FilamentSensorManager is empty") {
        // No sensors discovered
        REQUIRE(step.get_standalone_sensor_count() == 0);
    }

    SECTION("Returns 1 when FilamentSensorManager has 1 standalone sensor") {
        discover_standalone_sensors({"filament_switch_sensor runout"});
        REQUIRE(step.get_standalone_sensor_count() == 1);
    }

    SECTION("Returns 2 when FilamentSensorManager has 2 standalone sensors") {
        discover_standalone_sensors(
            {"filament_switch_sensor runout", "filament_switch_sensor toolhead"});
        REQUIRE(step.get_standalone_sensor_count() == 2);
    }

    SECTION("Returns 3 when FilamentSensorManager has 3 standalone sensors") {
        discover_standalone_sensors({"filament_switch_sensor runout",
                                     "filament_switch_sensor toolhead",
                                     "filament_motion_sensor encoder"});
        REQUIRE(step.get_standalone_sensor_count() == 3);
    }

    SECTION("Filters out AMS sensors - returns 0 when only AMS sensors exist") {
        discover_ams_sensors();
        REQUIRE(step.get_standalone_sensor_count() == 0);
    }

    SECTION("Filters out AMS sensors - returns correct count for mixed sensors") {
        discover_mixed_sensors();
        // Should only count "runout" and "toolhead", not "lane_1" and "slot_2"
        REQUIRE(step.get_standalone_sensor_count() == 2);
    }

    SECTION("Works without calling create() - queries manager directly") {
        // This is critical: the step should query FilamentSensorManager directly
        // rather than relying on internal state that requires create() to be called
        discover_standalone_sensors({"filament_switch_sensor runout"});

        // Do NOT call create() - step should still work
        REQUIRE(step.get_standalone_sensor_count() == 1);
    }
}

// ============================================================================
// should_skip() Tests
// ============================================================================

TEST_CASE_METHOD(WizardFilamentSensorSelectTestFixture,
                 "WizardFilamentSensorSelectStep - should_skip", "[wizard][filament][skip]") {
    WizardFilamentSensorSelectStep step;
    step.init_subjects();

    SECTION("Returns true when 0 sensors (skip - nothing to configure)") {
        // No sensors discovered
        REQUIRE(step.should_skip() == true);
    }

    SECTION("Returns true when 1 sensor (skip - auto-configure)") {
        discover_standalone_sensors({"filament_switch_sensor runout"});
        REQUIRE(step.should_skip() == true);
    }

    SECTION("Returns false when 2 sensors (show wizard for manual selection)") {
        discover_standalone_sensors(
            {"filament_switch_sensor runout", "filament_switch_sensor toolhead"});
        REQUIRE(step.should_skip() == false);
    }

    SECTION("Returns false when 3+ sensors (show wizard for manual selection)") {
        discover_standalone_sensors({"filament_switch_sensor runout",
                                     "filament_switch_sensor toolhead",
                                     "filament_motion_sensor encoder"});
        REQUIRE(step.should_skip() == false);
    }

    SECTION("Returns true when only AMS sensors (skip - no standalone sensors)") {
        discover_ams_sensors();
        REQUIRE(step.should_skip() == true);
    }

    SECTION("Considers only standalone sensors for skip logic") {
        // Mixed: 2 standalone + 2 AMS = should NOT skip (2 standalone requires wizard)
        discover_mixed_sensors();
        REQUIRE(step.should_skip() == false);
    }
}

// ============================================================================
// auto_configure_single_sensor() Tests
// ============================================================================

TEST_CASE_METHOD(WizardFilamentSensorSelectTestFixture,
                 "WizardFilamentSensorSelectStep - auto_configure_single_sensor",
                 "[wizard][filament][autoconfig]") {
    WizardFilamentSensorSelectStep step;
    step.init_subjects();

    SECTION("Sets RUNOUT role on the single standalone sensor") {
        // Use a sensor name without "runout" to avoid auto-assignment during discovery
        discover_standalone_sensors({"filament_switch_sensor fsensor"});

        // Verify sensor starts with NONE role (no auto-assignment for "fsensor")
        auto sensors_before = sensor_mgr().get_sensors();
        REQUIRE(sensors_before.size() == 1);
        REQUIRE(sensors_before[0].role == FilamentSensorRole::NONE);

        // Auto-configure
        step.auto_configure_single_sensor();

        // Verify RUNOUT role is assigned
        auto sensors_after = sensor_mgr().get_sensors();
        REQUIRE(sensors_after.size() == 1);
        REQUIRE(sensors_after[0].role == FilamentSensorRole::RUNOUT);
    }

    SECTION("Works with sensor that already has RUNOUT (from auto-discovery)") {
        // Note: sensors with "runout" in the name get auto-assigned RUNOUT during discovery
        discover_standalone_sensors({"filament_switch_sensor runout"});

        // Verify auto-assignment happened during discovery
        auto sensors = sensor_mgr().get_sensors();
        REQUIRE(sensors.size() == 1);
        REQUIRE(sensors[0].role == FilamentSensorRole::RUNOUT);

        // auto_configure_single_sensor should still work (idempotent)
        step.auto_configure_single_sensor();

        // Should still have RUNOUT role
        auto sensors_after = sensor_mgr().get_sensors();
        REQUIRE(sensors_after[0].role == FilamentSensorRole::RUNOUT);
    }

    SECTION("Selects first standalone sensor when multiple exist") {
        // Use names without "runout" to test the auto_configure logic itself
        discover_standalone_sensors(
            {"filament_switch_sensor sensor_a", "filament_switch_sensor sensor_b"});

        step.auto_configure_single_sensor();

        // Should assign RUNOUT to first sensor found
        auto sensors = sensor_mgr().get_sensors();
        auto runout_it = std::find_if(sensors.begin(), sensors.end(), [](const auto& s) {
            return s.role == FilamentSensorRole::RUNOUT;
        });
        REQUIRE(runout_it != sensors.end());
        REQUIRE(runout_it->sensor_name == "sensor_a");
    }

    SECTION("Ignores AMS sensors when auto-configuring") {
        // Mixed sensors: should only configure standalone sensor
        // Use "fsensor" instead of "runout" to avoid auto-assignment confusion
        std::vector<std::string> sensors = {
            "filament_switch_sensor lane_1", // AMS - should be ignored
            "filament_switch_sensor fsensor" // Standalone - should get RUNOUT
        };
        discover_filament_sensors(sensor_mgr(), sensors);

        step.auto_configure_single_sensor();

        // Verify only the standalone sensor got RUNOUT role
        auto all_sensors = sensor_mgr().get_sensors();
        for (const auto& s : all_sensors) {
            if (s.sensor_name == "fsensor") {
                REQUIRE(s.role == FilamentSensorRole::RUNOUT);
            } else {
                // AMS sensors should remain NONE
                REQUIRE(s.role == FilamentSensorRole::NONE);
            }
        }
    }

    SECTION("Does nothing when no standalone sensors exist") {
        discover_ams_sensors();

        // Should not crash and no sensor should have RUNOUT
        step.auto_configure_single_sensor();

        for (const auto& s : sensor_mgr().get_sensors()) {
            REQUIRE(s.role == FilamentSensorRole::NONE);
        }
    }

    SECTION("Does nothing when no sensors exist") {
        // No sensors discovered
        step.auto_configure_single_sensor(); // Should not crash
        REQUIRE(sensor_mgr().sensor_count() == 0);
    }
}

// ============================================================================
// Integration Tests: Wizard Skip Flow
// ============================================================================

TEST_CASE_METHOD(WizardFilamentSensorSelectTestFixture,
                 "WizardFilamentSensorSelectStep - wizard skip flow integration",
                 "[wizard][filament][integration]") {
    WizardFilamentSensorSelectStep step;
    step.init_subjects();

    SECTION("Complete flow: 1 sensor -> should_skip -> get_count -> auto_configure") {
        // Step 1: Populate FilamentSensorManager with 1 sensor
        // Use "fsensor" to avoid auto-assignment during discovery
        discover_standalone_sensors({"filament_switch_sensor fsensor"});

        // Step 2: Check should_skip() - should return true
        REQUIRE(step.should_skip() == true);

        // Step 3: Verify get_standalone_sensor_count() returns 1
        REQUIRE(step.get_standalone_sensor_count() == 1);

        // Step 4: Call auto_configure_single_sensor()
        step.auto_configure_single_sensor();

        // Step 5: Verify sensor has RUNOUT role
        auto sensors = sensor_mgr().get_sensors();
        REQUIRE(sensors.size() == 1);
        REQUIRE(sensors[0].role == FilamentSensorRole::RUNOUT);
        REQUIRE(sensors[0].sensor_name == "fsensor");
    }

    SECTION("Flow with 0 sensors: should_skip without auto-configure") {
        // No sensors
        REQUIRE(step.should_skip() == true);
        REQUIRE(step.get_standalone_sensor_count() == 0);

        // auto_configure would do nothing, but shouldn't crash
        step.auto_configure_single_sensor();
        REQUIRE(sensor_mgr().sensor_count() == 0);
    }

    SECTION("Flow with 2+ sensors: should NOT skip, no auto-configure") {
        discover_standalone_sensors(
            {"filament_switch_sensor runout", "filament_switch_sensor toolhead"});

        // Should NOT skip - wizard needs to be shown
        REQUIRE(step.should_skip() == false);
        REQUIRE(step.get_standalone_sensor_count() == 2);

        // In real code, auto_configure_single_sensor() would NOT be called
        // because should_skip() returned false
    }

    SECTION("Flow with AMS-only sensors: should_skip like 0 sensors") {
        discover_ams_sensors();

        REQUIRE(step.should_skip() == true);
        REQUIRE(step.get_standalone_sensor_count() == 0);

        // auto_configure does nothing since no standalone sensors
        step.auto_configure_single_sensor();

        // All sensors should still have NONE role
        for (const auto& s : sensor_mgr().get_sensors()) {
            REQUIRE(s.role == FilamentSensorRole::NONE);
        }
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(WizardFilamentSensorSelectTestFixture,
                 "WizardFilamentSensorSelectStep - edge cases", "[wizard][filament][edge]") {
    WizardFilamentSensorSelectStep step;
    step.init_subjects();

    SECTION("Motion sensors are treated as standalone") {
        discover_standalone_sensors({"filament_motion_sensor encoder"});

        REQUIRE(step.get_standalone_sensor_count() == 1);
        REQUIRE(step.should_skip() == true);

        step.auto_configure_single_sensor();

        auto sensors = sensor_mgr().get_sensors();
        REQUIRE(sensors[0].role == FilamentSensorRole::RUNOUT);
        REQUIRE(sensors[0].type == FilamentSensorType::MOTION);
    }

    SECTION("Sensors with spaces in names are handled correctly") {
        discover_standalone_sensors({"filament_switch_sensor my runout sensor"});

        REQUIRE(step.get_standalone_sensor_count() == 1);

        step.auto_configure_single_sensor();

        auto sensors = sensor_mgr().get_sensors();
        REQUIRE(sensors[0].role == FilamentSensorRole::RUNOUT);
        REQUIRE(sensors[0].sensor_name == "my runout sensor");
    }

    SECTION("Multiple calls to init_subjects are safe") {
        step.init_subjects();
        step.init_subjects();
        step.init_subjects();

        // Should not crash or corrupt state
        REQUIRE(step.get_standalone_sensor_count() == 0);
    }

    SECTION("Sensor rediscovery updates count correctly") {
        discover_standalone_sensors({"filament_switch_sensor runout"});
        REQUIRE(step.get_standalone_sensor_count() == 1);

        // Re-discover with different sensors
        discover_standalone_sensors({"filament_switch_sensor sensor_a",
                                     "filament_switch_sensor sensor_b",
                                     "filament_switch_sensor sensor_c"});
        REQUIRE(step.get_standalone_sensor_count() == 3);

        // Re-discover with no sensors
        discover_filament_sensors(sensor_mgr(), {});
        REQUIRE(step.get_standalone_sensor_count() == 0);
    }
}

// ============================================================================
// One standalone filter for the wizard and Settings
// ============================================================================

TEST_CASE_METHOD(
    WizardFilamentSensorSelectTestFixture,
    "WizardFilamentSensorSelectStep - the wizard and overlay agree on standalone sensors",
    "[wizard][filament][sensors]") {
    discover_mixed_sensors();
    WizardFilamentSensorSelectStep step;

    std::vector<std::string> listed;
    for (const auto& entry : sensor_mgr().entries()) {
        if (sensor_mgr().listed_in_settings(entry)) {
            listed.push_back(entry.display_name);
        }
        CHECK(sensor_mgr().listed_in_settings(entry) ==
              is_standalone_filament_sensor(get_printer_state().get_discovery(), entry.klipper_name,
                                            AmsState::instance().filament_hardware_absent()));
    }
    CHECK(listed == std::vector<std::string>{"runout", "toolhead"});
    CHECK(step.get_standalone_sensor_count() == listed.size());
}
