// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_switch_sensor_manager.cpp
 * @brief Unit tests for FilamentSensorManager
 *
 * Tests cover:
 * - Sensor discovery from Klipper object names
 * - Role assignment and uniqueness enforcement
 * - Enable/disable functionality (per-sensor and master)
 * - State updates from Moonraker status JSON
 * - Subject value correctness for UI binding
 * - State change callbacks
 * - Missing sensor handling
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/golden_json.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "observer_factory.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class FilamentSensorTestFixture : public LVGLTestFixture {
  public:
    FilamentSensorTestFixture() {
        // Reset state for test isolation first (clears subjects)
        FilamentSensorManagerTestAccess::reset(mgr());

        // Initialize subjects after reset
        mgr().init_subjects();
    }

    ~FilamentSensorTestFixture() {
        // Reset after each test
        FilamentSensorManagerTestAccess::reset(mgr());
    }

  protected:
    FilamentSensorManager& mgr() {
        return FilamentSensorManager::instance();
    }

    // Helper to discover standard test sensors
    void discover_test_sensors() {
        std::vector<std::string> sensors = {"filament_switch_sensor runout",
                                            "filament_switch_sensor toolhead",
                                            "filament_motion_sensor encoder"};
        discover_filament_sensors(mgr(), sensors);
        FilamentSensorManagerTestAccess::clear_startup_grace_period(mgr());
    }

    // Helper to simulate Moonraker status update
    void update_sensor_state(const std::string& klipper_name, bool detected) {
        json status;
        status[klipper_name]["filament_detected"] = detected;
        mgr().update_from_status(status);
    }

  private:
};

// Static members

// ============================================================================
// Type Helper Tests (filament_sensor_types.h)
// ============================================================================

TEST_CASE("FilamentSensorTypes - role string conversion", "[filament][types]") {
    SECTION("role_to_display_string") {
        REQUIRE(std::string(role_to_display_string(FilamentSensorRole::NONE)) == "Unassigned");
        REQUIRE(std::string(role_to_display_string(FilamentSensorRole::RUNOUT)) == "Runout Sensor");
        REQUIRE(std::string(role_to_display_string(FilamentSensorRole::TOOLHEAD)) ==
                "Toolhead Sensor");
        REQUIRE(std::string(role_to_display_string(FilamentSensorRole::ENTRY)) == "Entry Sensor");
    }

    SECTION("role_to_config_string") {
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::NONE)) == "none");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::RUNOUT)) == "runout");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::TOOLHEAD)) == "toolhead");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::ENTRY)) == "entry");
    }

    SECTION("role_from_config_string") {
        REQUIRE(role_from_config_string("none") == FilamentSensorRole::NONE);
        REQUIRE(role_from_config_string("runout") == FilamentSensorRole::RUNOUT);
        REQUIRE(role_from_config_string("toolhead") == FilamentSensorRole::TOOLHEAD);
        REQUIRE(role_from_config_string("entry") == FilamentSensorRole::ENTRY);
        // A filament sensor is never a Z probe; a saved "z_probe" reads as unassigned.
        REQUIRE(role_from_config_string("z_probe") == FilamentSensorRole::NONE);
        REQUIRE(role_from_config_string("invalid") == FilamentSensorRole::NONE);
        REQUIRE(role_from_config_string("") == FilamentSensorRole::NONE);
    }
}

TEST_CASE("FilamentSensorTypes - type string conversion", "[filament][types]") {
    SECTION("type_to_config_string") {
        REQUIRE(std::string(type_to_config_string(FilamentSensorType::SWITCH)) == "switch");
        REQUIRE(std::string(type_to_config_string(FilamentSensorType::MOTION)) == "motion");
    }

    SECTION("type_from_config_string") {
        REQUIRE(type_from_config_string("switch") == FilamentSensorType::SWITCH);
        REQUIRE(type_from_config_string("motion") == FilamentSensorType::MOTION);
        REQUIRE(type_from_config_string("invalid") == FilamentSensorType::SWITCH);
        REQUIRE(type_from_config_string("") == FilamentSensorType::SWITCH);
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - discovery",
                 "[filament][discovery]") {
    SECTION("Discovers switch sensors") {
        std::vector<std::string> sensors = {"filament_switch_sensor fsensor"};
        discover_filament_sensors(mgr(), sensors);

        REQUIRE(mgr().sensor_count() > 0);
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 1);
        REQUIRE(configs[0].klipper_name == "filament_switch_sensor fsensor");
        REQUIRE(configs[0].sensor_name == "fsensor");
        REQUIRE(configs[0].type == FilamentSensorType::SWITCH);
        REQUIRE(configs[0].enabled == true);
        REQUIRE(configs[0].role == FilamentSensorRole::NONE);
    }

    SECTION("Discovers motion sensors") {
        std::vector<std::string> sensors = {"filament_motion_sensor encoder"};
        discover_filament_sensors(mgr(), sensors);

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == FilamentSensorType::MOTION);
        REQUIRE(configs[0].sensor_name == "encoder");
    }

    SECTION("Discovers multiple sensors") {
        std::vector<std::string> sensors = {"filament_switch_sensor runout",
                                            "filament_switch_sensor toolhead",
                                            "filament_motion_sensor encoder"};
        discover_filament_sensors(mgr(), sensors);

        REQUIRE(mgr().sensor_count() == 3);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].sensor_name == "runout");
        REQUIRE(configs[1].sensor_name == "toolhead");
        REQUIRE(configs[2].sensor_name == "encoder");
        REQUIRE(configs[2].type == FilamentSensorType::MOTION);
    }

    SECTION("Ignores invalid sensor names") {
        std::vector<std::string> sensors = {"filament_switch_sensor valid",
                                            "invalid_sensor_name",    // Missing proper prefix
                                            "filament_switch_sensor", // Missing sensor name
                                            "temperature_sensor chamber"};
        discover_filament_sensors(mgr(), sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "valid");
    }

    SECTION("Empty sensor list clears previous sensors") {
        // First discover some sensors
        std::vector<std::string> sensors = {"filament_switch_sensor test"};
        discover_filament_sensors(mgr(), sensors);
        REQUIRE(mgr().sensor_count() == 1);

        // Then discover empty list
        discover_filament_sensors(mgr(), {});
        REQUIRE(mgr().sensor_count() == 0);
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("Re-discovery replaces sensor list") {
        std::vector<std::string> sensors1 = {"filament_switch_sensor old"};
        discover_filament_sensors(mgr(), sensors1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "old");

        std::vector<std::string> sensors2 = {"filament_switch_sensor new"};
        discover_filament_sensors(mgr(), sensors2);
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "new");
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().count_subject();
        REQUIRE(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        REQUIRE(lv_subject_get_int(count_subject) == 3);

        discover_filament_sensors(mgr(), {});
        REQUIRE(lv_subject_get_int(count_subject) == 0);
    }
}

// ============================================================================
// Role Assignment Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - role assignment",
                 "[filament][roles]") {
    discover_test_sensors();

    SECTION("Assign role to sensor") {
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "runout"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == FilamentSensorRole::RUNOUT);
    }

    SECTION("Role assignment is unique - assigning same role clears previous") {
        // Assign RUNOUT to first sensor
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        // Assign RUNOUT to second sensor - should clear from first
        mgr().set_role("filament_switch_sensor toolhead",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        auto configs = mgr().get_sensors();

        // First sensor should now have NONE
        auto runout_it = std::find_if(configs.begin(), configs.end(),
                                      [](const auto& c) { return c.sensor_name == "runout"; });
        REQUIRE(runout_it->role == FilamentSensorRole::NONE);

        // Second sensor should have RUNOUT
        auto toolhead_it = std::find_if(configs.begin(), configs.end(),
                                        [](const auto& c) { return c.sensor_name == "toolhead"; });
        REQUIRE(toolhead_it->role == FilamentSensorRole::RUNOUT);
    }

    SECTION("Can assign NONE without affecting other sensors") {
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        mgr().set_role("filament_switch_sensor toolhead",
                       static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));

        // Clear runout assignment
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::NONE));

        auto configs = mgr().get_sensors();
        auto runout_it = std::find_if(configs.begin(), configs.end(),
                                      [](const auto& c) { return c.sensor_name == "runout"; });
        auto toolhead_it = std::find_if(configs.begin(), configs.end(),
                                        [](const auto& c) { return c.sensor_name == "toolhead"; });

        REQUIRE(runout_it->role == FilamentSensorRole::NONE);
        REQUIRE(toolhead_it->role == FilamentSensorRole::TOOLHEAD);
    }

    SECTION("Assigning role to unknown sensor does nothing") {
        mgr().set_role("filament_switch_sensor nonexistent",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        // No sensor should have RUNOUT assigned
        for (const auto& config : mgr().get_sensors()) {
            REQUIRE(config.role == FilamentSensorRole::NONE);
        }
    }
}

// ============================================================================
// Enable/Disable Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - enable/disable",
                 "[filament][enable]") {
    discover_test_sensors();

    SECTION("Sensors start enabled by default") {
        for (const auto& config : mgr().get_sensors()) {
            REQUIRE(config.enabled == true);
        }
    }

    SECTION("Can disable individual sensor") {
        mgr().set_enabled("filament_switch_sensor runout", false);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "runout"; });
        REQUIRE(it->enabled == false);

        // Other sensors should still be enabled
        auto other = std::find_if(configs.begin(), configs.end(),
                                  [](const auto& c) { return c.sensor_name == "toolhead"; });
        REQUIRE(other->enabled == true);
    }

    SECTION("Master enable defaults to true") {
        REQUIRE(mgr().is_master_enabled() == true);
    }

    SECTION("Master enable can be toggled") {
        mgr().set_master_enabled(false);
        REQUIRE(mgr().is_master_enabled() == false);

        mgr().set_master_enabled(true);
        REQUIRE(mgr().is_master_enabled() == true);
    }

    SECTION("Master enabled subject is updated") {
        lv_subject_t* subject = FilamentSensorManagerTestAccess::master_enabled_subject(mgr());
        REQUIRE(lv_subject_get_int(subject) == 1);

        mgr().set_master_enabled(false);
        REQUIRE(lv_subject_get_int(subject) == 0);

        mgr().set_master_enabled(true);
        REQUIRE(lv_subject_get_int(subject) == 1);
    }
}

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - state updates",
                 "[filament][state]") {
    discover_test_sensors();
    mgr().set_role("filament_switch_sensor runout",
                   static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

    SECTION("Updates filament_detected from status JSON") {
        // Initial state defaults to filament_detected=true (commit c176470bb)
        // so pre-Moonraker reads don't render as false runouts.
        auto state = FilamentSensorManagerTestAccess::role_state(mgr(), FilamentSensorRole::RUNOUT);
        REQUIRE(state.has_value());
        REQUIRE(state->filament_detected == true);

        // Update via status — flip to empty
        json status;
        status["filament_switch_sensor runout"]["filament_detected"] = false;
        mgr().update_from_status(status);

        state = FilamentSensorManagerTestAccess::role_state(mgr(), FilamentSensorRole::RUNOUT);
        REQUIRE(state->filament_detected == false);
    }

    SECTION("Motion sensor updates include detection_count") {
        mgr().set_role("filament_motion_sensor encoder",
                       static_cast<uint8_t>(FilamentSensorRole::ENTRY));

        json status;
        status["filament_motion_sensor encoder"]["filament_detected"] = true;
        status["filament_motion_sensor encoder"]["enabled"] = true;
        status["filament_motion_sensor encoder"]["detection_count"] = 42;
        mgr().update_from_status(status);

        auto state = FilamentSensorManagerTestAccess::role_state(mgr(), FilamentSensorRole::ENTRY);
        REQUIRE(state.has_value());
        REQUIRE(state->filament_detected == true);
        REQUIRE(state->detection_count == 42);
    }

    SECTION("State change callback is fired") {
        bool callback_fired = false;
        std::string changed_sensor;
        bool old_detected = false;
        bool new_detected = false;

        mgr().set_state_change_callback([&](const std::string& name,
                                            const FilamentSensorState& old_state,
                                            const FilamentSensorState& new_state) {
            callback_fired = true;
            changed_sensor = name;
            old_detected = old_state.filament_detected;
            new_detected = new_state.filament_detected;
        });

        // Trigger state change (default is true → flip to false)
        update_sensor_state("filament_switch_sensor runout", false);

        REQUIRE(callback_fired);
        REQUIRE(changed_sensor == "filament_switch_sensor runout");
        REQUIRE(old_detected == true);
        REQUIRE(new_detected == false);
    }

    SECTION("No callback when state doesn't change") {
        // Set initial state to non-default value first so a same-value update is observable
        update_sensor_state("filament_switch_sensor runout", false);

        int callback_count = 0;
        mgr().set_state_change_callback([&](const std::string&, const FilamentSensorState&,
                                            const FilamentSensorState&) { callback_count++; });

        // Update with same value
        update_sensor_state("filament_switch_sensor runout", false);

        REQUIRE(callback_count == 0);
    }
}

// ============================================================================
// State Query Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - state queries",
                 "[filament][queries]") {
    discover_test_sensors();
    mgr().set_role("filament_switch_sensor runout",
                   static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
    update_sensor_state("filament_switch_sensor runout", true);

    SECTION("is_filament_detected returns correct state") {
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::RUNOUT) == true);

        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::RUNOUT) == false);
    }

    SECTION("is_filament_detected returns false when master disabled") {
        mgr().set_master_enabled(false);
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::RUNOUT) == false);
    }

    SECTION("is_filament_detected returns false when sensor disabled") {
        mgr().set_enabled("filament_switch_sensor runout", false);
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::RUNOUT) == false);
    }

    SECTION("is_filament_detected returns false for unassigned role") {
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::TOOLHEAD) == false);
    }

    SECTION("is_filament_detected returns false for NONE role") {
        REQUIRE(mgr().is_filament_detected(FilamentSensorRole::NONE) == false);
    }

    SECTION("is_sensor_available checks role assignment and enabled") {
        REQUIRE(mgr().is_sensor_available(FilamentSensorRole::RUNOUT) == true);
        REQUIRE(mgr().is_sensor_available(FilamentSensorRole::TOOLHEAD) == false);

        mgr().set_enabled("filament_switch_sensor runout", false);
        REQUIRE(mgr().is_sensor_available(FilamentSensorRole::RUNOUT) == false);
    }

    SECTION("get_sensor_state returns nullopt for unassigned role") {
        auto state =
            FilamentSensorManagerTestAccess::role_state(mgr(), FilamentSensorRole::TOOLHEAD);
        REQUIRE_FALSE(state.has_value());
    }

    SECTION("has_any_runout detects runout condition") {
        // Filament present = no runout
        REQUIRE(mgr().has_any_runout() == false);

        // Remove filament = runout
        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(mgr().has_any_runout() == true);
    }

    SECTION("has_any_runout ignores unassigned sensors") {
        // Clear role from sensor
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::NONE));
        update_sensor_state("filament_switch_sensor runout", false);

        // Should not report runout since sensor has no role
        REQUIRE(mgr().has_any_runout() == false);
    }

    SECTION("has_any_runout returns false when master disabled") {
        update_sensor_state("filament_switch_sensor runout", false);
        mgr().set_master_enabled(false);

        REQUIRE(mgr().has_any_runout() == false);
    }
}

// ============================================================================
// Subject Value Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - subject values",
                 "[filament][subjects]") {
    discover_test_sensors();

    SECTION("Role subjects show -1 when no sensor assigned") {
        // "runout" takes the RUNOUT role by name at discovery.
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::NONE));
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == -1);
        REQUIRE(lv_subject_get_int(mgr().get_toolhead_detected_subject()) == -1);
        REQUIRE(lv_subject_get_int(mgr().get_entry_detected_subject()) == -1);
    }

    SECTION("Role subjects update when sensor assigned and state changes") {
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        // After assignment, should show 1 (loaded) — FilamentSensorState now
        // defaults filament_detected=true (commit c176470bb) so pre-Moonraker
        // reads don't render as false runouts.
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);

        // Update state to empty
        // Note: reset_for_testing() enables sync_mode, so update_from_status()
        // updates subjects synchronously instead of using lv_async_call().
        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 0);

        // Update state back to detected
        update_sensor_state("filament_switch_sensor runout", true);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);
    }

    SECTION("Role subjects show 2 (disabled) when master disabled") {
        // A configured sensor with the master toggle off must surface as
        // "disabled" (value 2), NOT "no sensor" (-1). The icon needs to
        // render a muted state so the user can see runout protection is
        // off — a hidden indicator would be mistaken for "all fine".
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        update_sensor_state("filament_switch_sensor runout", true);

        mgr().set_master_enabled(false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 2);
    }

    SECTION("Role subjects show 2 (disabled) when sensor disabled") {
        // Per-sensor enabled=false has the same UX implication as a master
        // off: runout protection is inactive on this role and the user
        // needs a visible indicator that says so.
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        update_sensor_state("filament_switch_sensor runout", true);

        mgr().set_enabled("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 2);
    }

    SECTION("any_runout subject reflects runout state") {
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        update_sensor_state("filament_switch_sensor runout", true);

        // Filament detected = no runout
        REQUIRE(lv_subject_get_int(mgr().get_any_runout_subject()) == 0);

        // Filament removed = runout detected
        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_any_runout_subject()) == 1);
    }
}

// ============================================================================
// Initial Status Update Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - first status update always fires subjects",
                 "[filament][subjects][initial_status]") {
    discover_test_sensors();
    mgr().set_role("filament_switch_sensor runout",
                   static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

    // set_role() publishes, which moves subjects
    // from -1 to 1 (the post-c176470bb optimistic default is filament_detected=true).
    // The bug this case guards against: update_from_status() with a value matching
    // the in-memory default would NOT refresh the subjects because no state change
    // was detected. The fix: always refresh the subjects on the first status
    // update — even when the observed value happens to equal the default.

    SECTION("First update with true (matching default) still updates subjects") {
        // Subjects should be 1 after role assignment (default detected=true)
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);

        // Simulate what happens on first Moonraker status — sensor reports true
        // (matching default). Before the fix, this was a no-op because any_changed=false.
        update_sensor_state("filament_switch_sensor runout", true);

        // Subject should still be 1 (not -1)
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);
    }

    SECTION("First update with false correctly sets subject to 0") {
        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 0);
    }

    SECTION("Re-discovery resets initial status tracking") {
        // First update
        update_sensor_state("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 0);

        // Re-discover (simulates Moonraker reconnect)
        discover_test_sensors();
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        // After re-discovery, first status with true should still trigger subjects
        update_sensor_state("filament_switch_sensor runout", true);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);
    }
}

// ============================================================================
// Motion Sensor Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - motion sensors",
                 "[filament][motion]") {
    discover_test_sensors();
    mgr().set_role("filament_motion_sensor encoder",
                   static_cast<uint8_t>(FilamentSensorRole::ENTRY));

    SECTION("Motion sensor type is correctly identified") {
        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "encoder"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->type == FilamentSensorType::MOTION);
    }
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - edge cases",
                 "[filament][edge]") {
    SECTION("Handles sensors with spaces in names") {
        std::vector<std::string> sensors = {"filament_switch_sensor my runout sensor"};
        discover_filament_sensors(mgr(), sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "my runout sensor");
    }

    SECTION("Status update for unknown sensor is ignored") {
        discover_test_sensors();

        json status;
        status["filament_switch_sensor unknown"]["filament_detected"] = true;
        mgr().update_from_status(status);

        // Should not crash or affect known sensors
        REQUIRE(mgr().sensor_count() == 3);
    }

    SECTION("Empty status update is handled") {
        discover_test_sensors();

        json status = json::object();
        mgr().update_from_status(status);

        // Should not crash
        REQUIRE(mgr().sensor_count() > 0);
    }

    SECTION("Multiple rapid state changes fire callbacks correctly") {
        discover_test_sensors();
        mgr().set_role("filament_switch_sensor runout",
                       static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

        int callback_count = 0;
        mgr().set_state_change_callback([&](const std::string&, const FilamentSensorState&,
                                            const FilamentSensorState&) { callback_count++; });

        // Rapid changes — start with false since default is now true
        update_sensor_state("filament_switch_sensor runout", false);
        update_sensor_state("filament_switch_sensor runout", true);
        update_sensor_state("filament_switch_sensor runout", false);
        update_sensor_state("filament_switch_sensor runout", true);

        REQUIRE(callback_count == 4);
    }
}

// ============================================================================
// Thread Safety Tests (basic validation)
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - thread safety basics",
                 "[filament][threading]") {
    discover_test_sensors();

    SECTION("Concurrent get_sensors returns consistent copy") {
        // This tests that get_sensors() returns a copy, not a reference
        // Note: Use "toolhead" sensor because "runout" gets auto-assigned RUNOUT role
        auto copy1 = mgr().get_sensors();

        // Modify manager state
        mgr().set_role("filament_switch_sensor toolhead",
                       static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));

        auto copy2 = mgr().get_sensors();

        // copy1 should still have the old state (NONE)
        auto it1 = std::find_if(copy1.begin(), copy1.end(),
                                [](const auto& c) { return c.sensor_name == "toolhead"; });
        REQUIRE(it1->role == FilamentSensorRole::NONE);

        // copy2 should have new state
        auto it2 = std::find_if(copy2.begin(), copy2.end(),
                                [](const auto& c) { return c.sensor_name == "toolhead"; });
        REQUIRE(it2->role == FilamentSensorRole::TOOLHEAD);
    }
}

// ============================================================================
// Saved Settings
// ============================================================================

namespace {

json& filament_settings_slot() {
    return Config::get_instance()->get_json(Config::get_instance()->df() + "filament_sensors");
}

const FilamentSensorConfig* find_config(const std::vector<FilamentSensorConfig>& configs,
                                        const std::string& klipper_name) {
    for (const auto& c : configs) {
        if (c.klipper_name == klipper_name) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - saved config reads back",
                 "[filament][sensors]") {
    std::ifstream in(helix::test::golden_path("sensor_settings", "filament"));
    REQUIRE(in.good());
    const json fixture = json::parse(in, nullptr, false);
    REQUIRE_FALSE(fixture.is_discarded());

    std::vector<std::string> objects;
    for (const auto& sensor : fixture["sensors"]) {
        objects.push_back(sensor["klipper_name"].get<std::string>());
    }
    discover_filament_sensors(mgr(), objects);
    const json previous = filament_settings_slot();
    filament_settings_slot() = fixture;
    mgr().set_master_enabled(false);
    mgr().load_settings();

    const auto configs = mgr().get_sensors();
    REQUIRE(configs.size() == objects.size());
    CHECK(find_config(configs, "filament_switch_sensor tool_start")->role ==
          FilamentSensorRole::RUNOUT);
    CHECK(find_config(configs, "filament_switch_sensor tool_end")->role ==
          FilamentSensorRole::NONE);
    CHECK(find_config(configs, "filament_switch_sensor lane2_load")->lane == 1);
    CHECK(find_config(configs, "filament_switch_sensor lane1_load")->lane == -1);
    CHECK(mgr().is_master_enabled());

    filament_settings_slot() = json::object();
    mgr().save_settings();
    CHECK(filament_settings_slot().dump() == fixture.dump());
    filament_settings_slot() = previous;
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - a wrong-typed saved field is skipped, not fatal",
                 "[filament][sensors]") {
    discover_test_sensors(); // "runout" takes RUNOUT by name
    const json previous = filament_settings_slot();
    filament_settings_slot() =
        json{{"master_enabled", "no"},
             {"sensors", json::array({
                             {{"klipper_name", "filament_switch_sensor runout"}, {"role", 5}},
                             {{"klipper_name", "filament_switch_sensor toolhead"},
                              {"role", "toolhead"},
                              {"enabled", "yes"}},
                             {{"klipper_name", "filament_motion_sensor encoder"},
                              {"role", "entry"},
                              {"lane", "two"}},
                         })}};
    mgr().set_master_enabled(false);
    mgr().load_settings();
    filament_settings_slot() = previous;

    // A role that is not a string reads as none, like an unknown role name.
    const auto configs = mgr().get_sensors();
    CHECK(find_config(configs, "filament_switch_sensor runout")->role == FilamentSensorRole::NONE);
    CHECK(find_config(configs, "filament_switch_sensor toolhead")->role ==
          FilamentSensorRole::TOOLHEAD);
    CHECK(find_config(configs, "filament_switch_sensor toolhead")->enabled);
    CHECK(find_config(configs, "filament_motion_sensor encoder")->role ==
          FilamentSensorRole::ENTRY);
    CHECK(find_config(configs, "filament_motion_sensor encoder")->lane == -1);
    CHECK(mgr().is_master_enabled());
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - rediscovery keeps the user's role choice",
                 "[filament][discovery]") {
    discover_test_sensors();
    mgr().set_role("filament_switch_sensor runout", static_cast<uint8_t>(FilamentSensorRole::NONE));
    mgr().set_role("filament_switch_sensor toolhead",
                   static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));

    // The configfile pass runs discovery again without reloading settings.
    discover_test_sensors();
    auto configs = mgr().get_sensors();
    CHECK(find_config(configs, "filament_switch_sensor runout")->role == FilamentSensorRole::NONE);
    CHECK(find_config(configs, "filament_switch_sensor toolhead")->role ==
          FilamentSensorRole::TOOLHEAD);

    // A runout-named sensor new to a later discovery gets the name default.
    discover_filament_sensors(
        mgr(), {"filament_switch_sensor toolhead", "filament_switch_sensor runout2"});
    configs = mgr().get_sensors();
    CHECK(find_config(configs, "filament_switch_sensor runout2")->role ==
          FilamentSensorRole::RUNOUT);
    CHECK(find_config(configs, "filament_switch_sensor toolhead")->role ==
          FilamentSensorRole::TOOLHEAD);
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - a sensor missing from a discovery keeps its reading",
                 "[filament][discovery]") {
    discover_test_sensors();
    update_sensor_state("filament_switch_sensor runout", false);

    discover_filament_sensors(
        mgr(), {"filament_switch_sensor toolhead", "filament_motion_sensor encoder"});
    REQUIRE(mgr().sensor_count() == 2);
    CHECK_FALSE(mgr().has_any_runout());

    // Back on the next reconnect, with the reading it had.
    discover_test_sensors();
    CHECK(mgr().has_any_runout());
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - config revision ticks on configuration, not readings",
                 "[filament][subjects]") {
    discover_test_sensors();
    lv_subject_t* revision = mgr().get_config_revision_subject();
    const int start = lv_subject_get_int(revision);

    update_sensor_state("filament_switch_sensor runout", false);
    CHECK(lv_subject_get_int(revision) == start);

    mgr().set_enabled("filament_switch_sensor toolhead", false);
    CHECK(lv_subject_get_int(revision) == start + 1);

    mgr().set_role("filament_switch_sensor toolhead",
                   static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));
    CHECK(lv_subject_get_int(revision) == start + 2);

    mgr().set_master_enabled(false);
    CHECK(lv_subject_get_int(revision) == start + 3);
}

// Filament reads no config sections, so the configfile pass leaves its
// per-connect state alone.
TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - the configfile pass does not reset the connection",
                 "[filament][discovery]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"filament_switch_sensor runout"}));
    mgr().discover(hw);
    FilamentSensorManagerTestAccess::clear_startup_grace_period(mgr());
    update_sensor_state("filament_switch_sensor runout", true);
    REQUIRE_FALSE(mgr().is_in_startup_grace_period());
    REQUIRE(FilamentSensorManagerTestAccess::initial_status_received(mgr()));

    PrinterDiscovery configured = hw;
    configured.parse_config_keys(
        json{{"filament_switch_sensor runout", {{"pause_on_runout", "True"}}}});
    helix::sensors::rediscover_from_config(configured);

    CHECK_FALSE(mgr().is_in_startup_grace_period());
    CHECK(FilamentSensorManagerTestAccess::initial_status_received(mgr()));
}

// Setting a subject runs its observers synchronously. One that calls back into
// the manager from another thread (AmsState under its own lock does) would wait
// forever if the refresh held the manager's mutex.
TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "a runout observer calling set_role during refresh does not deadlock",
                 "[filament][threading]") {
    discover_test_sensors(); // "runout" takes RUNOUT by name
    update_sensor_state("filament_switch_sensor runout", true);

    struct Probe {
        bool fired = false;
        bool finished_in_time = false;
        std::thread worker;
    } probe;
    ObserverGuard observer = helix::ui::observe<int>(
        mgr().get_any_runout_subject(), &probe,
        [](Probe* p, int any_runout) {
            if (any_runout != 1 || p->fired) {
                return;
            }
            p->fired = true;
            std::promise<void> promise;
            std::future<void> done = promise.get_future();
            p->worker = std::thread([promise = std::move(promise)]() mutable {
                FilamentSensorManager::instance().set_role(
                    "filament_switch_sensor toolhead",
                    static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));
                promise.set_value();
            });
            p->finished_in_time =
                done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        },
        mgr().subjects_lifetime(), helix::ui::Dispatch::Immediate);

    update_sensor_state("filament_switch_sensor runout", false);
    if (probe.worker.joinable()) {
        probe.worker.join();
    }
    observer.reset();

    REQUIRE(probe.fired);
    CHECK(probe.finished_in_time);
    CHECK(find_config(mgr().get_sensors(), "filament_switch_sensor toolhead")->role ==
          FilamentSensorRole::TOOLHEAD);
}

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - a saved z_probe reads as none",
                 "[filament][probe]") {
    discover_filament_sensors(mgr(), {"filament_switch_sensor probe"});
    json& node = filament_settings_slot();
    const json previous = node;
    node = json{{"master_enabled", true},
                {"sensors", json::array({{{"klipper_name", "filament_switch_sensor probe"},
                                          {"role", "z_probe"},
                                          {"enabled", true},
                                          {"type", "switch"}}})}};
    mgr().set_role("filament_switch_sensor probe",
                   static_cast<uint8_t>(FilamentSensorRole::TOOLHEAD));

    mgr().load_settings();
    node = previous;

    REQUIRE(mgr().get_sensors()[0].role == FilamentSensorRole::NONE);
    REQUIRE(lv_subject_get_int(mgr().get_toolhead_detected_subject()) == -1);
}

// A deferral queued before deinit_subjects() must not land on the subjects a
// later init_subjects() creates.
TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - deferred update dropped after deinit",
                 "[filament][lifetime]") {
    discover_test_sensors();
    mgr().set_role("filament_switch_sensor runout",
                   static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
    mgr().set_sync_mode(false);

    update_sensor_state("filament_switch_sensor runout", false);
    mgr().deinit_subjects();
    // The refresh queued above runs with no subjects to land on.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    mgr().init_subjects();
    REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == -1);
    // init_subjects() publishes the current reading.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 0);

    update_sensor_state("filament_switch_sensor runout", true);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 1);
    mgr().set_sync_mode(true);
}

// The filament manager's subjects are reachable from XML under their own names.
TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - subjects are reachable from XML", "[filament]") {
    mgr().deinit_subjects();
    mgr().init_subjects();

    REQUIRE(lv_xml_get_subject(nullptr, "filament_runout_detected") ==
            mgr().get_runout_detected_subject());
    REQUIRE(lv_xml_get_subject(nullptr, "filament_sensor_count") == mgr().count_subject());
}
