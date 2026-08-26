// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_sensor_manager.cpp
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

#include "../ui_test_utils.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"

#include <spdlog/spdlog.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

// Test access helper — avoids polluting production API with test methods
namespace helix {
class FilamentSensorManagerTestAccess {
  public:
    static void reset(FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

        // Clear all sensors and states
        mgr.sensors_.clear();
        mgr.states_.clear();

        // Reset master enabled
        mgr.master_enabled_ = true;

        // Clear callback
        mgr.state_change_callback_ = nullptr;

        // Enable sync mode for testing (avoids lv_async_call)
        mgr.sync_mode_ = true;

        // Reset initial status tracking (ensures first update_from_status triggers subjects)
        mgr.initial_status_received_ = false;

        // Reset startup time to 10 seconds in the past so the 2-second grace period
        // is already expired in tests (avoids flaky timing-dependent failures)
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        // Reset subjects if initialized
        if (mgr.subjects_initialized_) {
            lv_subject_set_int(&mgr.runout_detected_, -1);
            lv_subject_set_int(&mgr.toolhead_detected_, -1);
            lv_subject_set_int(&mgr.entry_detected_, -1);
            lv_subject_set_int(&mgr.probe_triggered_, -1);
            lv_subject_set_int(&mgr.any_runout_, 0);
            lv_subject_set_int(&mgr.motion_active_, 0);
            lv_subject_set_int(&mgr.master_enabled_subject_, 1);
            lv_subject_set_int(&mgr.sensor_count_, 0);
        }
    }

    static void clear_startup_grace_period(FilamentSensorManager& mgr) {
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};
} // namespace helix

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class FilamentSensorTestFixture {
  public:
    FilamentSensorTestFixture() {
        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_,
                                    [](lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
                                        lv_display_flush_ready(disp);
                                    });
            display_created_ = true;
        }

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
        mgr().discover_sensors(sensors);
        FilamentSensorManagerTestAccess::clear_startup_grace_period(mgr());
    }

    // Helper to simulate Moonraker status update
    void update_sensor_state(const std::string& klipper_name, bool detected) {
        json status;
        status[klipper_name]["filament_detected"] = detected;
        mgr().update_from_status(status);
    }

  private:
    static lv_display_t* display_;
    static bool display_created_;
};

// Static members
lv_display_t* FilamentSensorTestFixture::display_ = nullptr;
bool FilamentSensorTestFixture::display_created_ = false;

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
        // Z_PROBE is the only non-contiguous enumerator (= 10). Without this, deleting its
        // case arm silently falls through to `default: "Unassigned"` and nothing goes red.
        REQUIRE(std::string(role_to_display_string(FilamentSensorRole::Z_PROBE)) == "Z Probe");
    }

    SECTION("role_to_config_string") {
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::NONE)) == "none");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::RUNOUT)) == "runout");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::TOOLHEAD)) == "toolhead");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::ENTRY)) == "entry");
        REQUIRE(std::string(role_to_config_string(FilamentSensorRole::Z_PROBE)) == "z_probe");
    }

    SECTION("role_from_config_string") {
        REQUIRE(role_from_config_string("none") == FilamentSensorRole::NONE);
        REQUIRE(role_from_config_string("runout") == FilamentSensorRole::RUNOUT);
        REQUIRE(role_from_config_string("toolhead") == FilamentSensorRole::TOOLHEAD);
        REQUIRE(role_from_config_string("entry") == FilamentSensorRole::ENTRY);
        REQUIRE(role_from_config_string("z_probe") == FilamentSensorRole::Z_PROBE);
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
        mgr().discover_sensors(sensors);

        REQUIRE(mgr().has_sensors());
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
        mgr().discover_sensors(sensors);

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == FilamentSensorType::MOTION);
        REQUIRE(configs[0].sensor_name == "encoder");
    }

    SECTION("Discovers multiple sensors") {
        std::vector<std::string> sensors = {"filament_switch_sensor runout",
                                            "filament_switch_sensor toolhead",
                                            "filament_motion_sensor encoder"};
        mgr().discover_sensors(sensors);

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
        mgr().discover_sensors(sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "valid");
    }

    SECTION("Empty sensor list clears previous sensors") {
        // First discover some sensors
        std::vector<std::string> sensors = {"filament_switch_sensor test"};
        mgr().discover_sensors(sensors);
        REQUIRE(mgr().sensor_count() == 1);

        // Then discover empty list
        mgr().discover_sensors({});
        REQUIRE(mgr().sensor_count() == 0);
        REQUIRE_FALSE(mgr().has_sensors());
    }

    SECTION("Re-discovery replaces sensor list") {
        std::vector<std::string> sensors1 = {"filament_switch_sensor old"};
        mgr().discover_sensors(sensors1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "old");

        std::vector<std::string> sensors2 = {"filament_switch_sensor new"};
        mgr().discover_sensors(sensors2);
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "new");
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().get_sensor_count_subject();
        REQUIRE(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        REQUIRE(lv_subject_get_int(count_subject) == 3);

        mgr().discover_sensors({});
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
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "runout"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == FilamentSensorRole::RUNOUT);
    }

    SECTION("Role assignment is unique - assigning same role clears previous") {
        // Assign RUNOUT to first sensor
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

        // Assign RUNOUT to second sensor - should clear from first
        mgr().set_sensor_role("filament_switch_sensor toolhead", FilamentSensorRole::RUNOUT);

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
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);
        mgr().set_sensor_role("filament_switch_sensor toolhead", FilamentSensorRole::TOOLHEAD);

        // Clear runout assignment
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::NONE);

        auto configs = mgr().get_sensors();
        auto runout_it = std::find_if(configs.begin(), configs.end(),
                                      [](const auto& c) { return c.sensor_name == "runout"; });
        auto toolhead_it = std::find_if(configs.begin(), configs.end(),
                                        [](const auto& c) { return c.sensor_name == "toolhead"; });

        REQUIRE(runout_it->role == FilamentSensorRole::NONE);
        REQUIRE(toolhead_it->role == FilamentSensorRole::TOOLHEAD);
    }

    SECTION("Assigning role to unknown sensor does nothing") {
        mgr().set_sensor_role("filament_switch_sensor nonexistent", FilamentSensorRole::RUNOUT);

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
        mgr().set_sensor_enabled("filament_switch_sensor runout", false);

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
        lv_subject_t* subject = mgr().get_master_enabled_subject();
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
    mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

    SECTION("Updates filament_detected from status JSON") {
        // Initial state defaults to filament_detected=true (commit c176470bb)
        // so pre-Moonraker reads don't render as false runouts.
        auto state = mgr().get_sensor_state(FilamentSensorRole::RUNOUT);
        REQUIRE(state.has_value());
        REQUIRE(state->filament_detected == true);

        // Update via status — flip to empty
        json status;
        status["filament_switch_sensor runout"]["filament_detected"] = false;
        mgr().update_from_status(status);

        state = mgr().get_sensor_state(FilamentSensorRole::RUNOUT);
        REQUIRE(state->filament_detected == false);
    }

    SECTION("Motion sensor updates include detection_count") {
        mgr().set_sensor_role("filament_motion_sensor encoder", FilamentSensorRole::ENTRY);

        json status;
        status["filament_motion_sensor encoder"]["filament_detected"] = true;
        status["filament_motion_sensor encoder"]["enabled"] = true;
        status["filament_motion_sensor encoder"]["detection_count"] = 42;
        mgr().update_from_status(status);

        auto state = mgr().get_sensor_state(FilamentSensorRole::ENTRY);
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
    mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);
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
        mgr().set_sensor_enabled("filament_switch_sensor runout", false);
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

        mgr().set_sensor_enabled("filament_switch_sensor runout", false);
        REQUIRE(mgr().is_sensor_available(FilamentSensorRole::RUNOUT) == false);
    }

    SECTION("get_sensor_state returns nullopt for unassigned role") {
        auto state = mgr().get_sensor_state(FilamentSensorRole::TOOLHEAD);
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
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::NONE);
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
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == -1);
        REQUIRE(lv_subject_get_int(mgr().get_toolhead_detected_subject()) == -1);
        REQUIRE(lv_subject_get_int(mgr().get_entry_detected_subject()) == -1);
    }

    SECTION("Role subjects update when sensor assigned and state changes") {
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

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
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);
        update_sensor_state("filament_switch_sensor runout", true);

        mgr().set_master_enabled(false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 2);
    }

    SECTION("Role subjects show 2 (disabled) when sensor disabled") {
        // Per-sensor enabled=false has the same UX implication as a master
        // off: runout protection is inactive on this role and the user
        // needs a visible indicator that says so.
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);
        update_sensor_state("filament_switch_sensor runout", true);

        mgr().set_sensor_enabled("filament_switch_sensor runout", false);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 2);
    }

    SECTION("any_runout subject reflects runout state") {
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);
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
    mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

    // set_sensor_role() calls update_subjects() internally, which moves subjects
    // from -1 to 1 (the post-c176470bb optimistic default is filament_detected=true).
    // The bug this case guards against: update_from_status() with a value matching
    // the in-memory default would NOT call update_subjects() because no state change
    // was detected. The fix: always trigger update_subjects() on the first status
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
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

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
    mgr().set_sensor_role("filament_motion_sensor encoder", FilamentSensorRole::ENTRY);

    SECTION("Motion sensor type is correctly identified") {
        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "encoder"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->type == FilamentSensorType::MOTION);
    }

    SECTION("is_motion_active requires enabled motion sensor") {
        json status;
        status["filament_motion_sensor encoder"]["filament_detected"] = true;
        status["filament_motion_sensor encoder"]["enabled"] = true;
        mgr().update_from_status(status);

        REQUIRE(mgr().is_motion_active() == true);

        // Disable sensor
        mgr().set_sensor_enabled("filament_motion_sensor encoder", false);
        REQUIRE(mgr().is_motion_active() == false);
    }

    SECTION("motion_active subject updates correctly") {
        json status;
        status["filament_motion_sensor encoder"]["filament_detected"] = true;
        status["filament_motion_sensor encoder"]["enabled"] = true;
        mgr().update_from_status(status);

        REQUIRE(lv_subject_get_int(mgr().get_motion_active_subject()) == 1);

        // Master disable should hide motion
        mgr().set_master_enabled(false);
        REQUIRE(lv_subject_get_int(mgr().get_motion_active_subject()) == 0);
    }
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - edge cases",
                 "[filament][edge]") {
    SECTION("Handles sensors with spaces in names") {
        std::vector<std::string> sensors = {"filament_switch_sensor my runout sensor"};
        mgr().discover_sensors(sensors);

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
        REQUIRE(mgr().has_sensors());
    }

    SECTION("Multiple rapid state changes fire callbacks correctly") {
        discover_test_sensors();
        mgr().set_sensor_role("filament_switch_sensor runout", FilamentSensorRole::RUNOUT);

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
        mgr().set_sensor_role("filament_switch_sensor toolhead", FilamentSensorRole::TOOLHEAD);

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
// SwitchSensorTypes Tests (switch_sensor_types.h)
// ============================================================================

// Include the new types header for testing the helpers
#include "switch_sensor_types.h"

using namespace helix::sensors;

TEST_CASE("SwitchSensorTypes - role string conversion", "[sensors][switch][types]") {
    SECTION("switch_role_to_string") {
        REQUIRE(switch_role_to_string(SwitchSensorRole::NONE) == "none");
        REQUIRE(switch_role_to_string(SwitchSensorRole::FILAMENT_RUNOUT) == "filament_runout");
        REQUIRE(switch_role_to_string(SwitchSensorRole::FILAMENT_TOOLHEAD) == "filament_toolhead");
        REQUIRE(switch_role_to_string(SwitchSensorRole::FILAMENT_ENTRY) == "filament_entry");
        REQUIRE(switch_role_to_string(SwitchSensorRole::Z_PROBE) == "z_probe");
        REQUIRE(switch_role_to_string(SwitchSensorRole::DOCK_DETECT) == "dock_detect");
    }

    SECTION("switch_role_from_string") {
        REQUIRE(switch_role_from_string("none") == SwitchSensorRole::NONE);
        REQUIRE(switch_role_from_string("filament_runout") == SwitchSensorRole::FILAMENT_RUNOUT);
        REQUIRE(switch_role_from_string("filament_toolhead") ==
                SwitchSensorRole::FILAMENT_TOOLHEAD);
        REQUIRE(switch_role_from_string("filament_entry") == SwitchSensorRole::FILAMENT_ENTRY);
        REQUIRE(switch_role_from_string("z_probe") == SwitchSensorRole::Z_PROBE);
        REQUIRE(switch_role_from_string("dock_detect") == SwitchSensorRole::DOCK_DETECT);
        REQUIRE(switch_role_from_string("invalid") == SwitchSensorRole::NONE);
        REQUIRE(switch_role_from_string("") == SwitchSensorRole::NONE);
    }

    SECTION("switch_role_from_string - backwards compatibility") {
        // Old config strings should still work
        REQUIRE(switch_role_from_string("runout") == SwitchSensorRole::FILAMENT_RUNOUT);
        REQUIRE(switch_role_from_string("toolhead") == SwitchSensorRole::FILAMENT_TOOLHEAD);
        REQUIRE(switch_role_from_string("entry") == SwitchSensorRole::FILAMENT_ENTRY);
    }

    SECTION("switch_role_to_display_string") {
        REQUIRE(switch_role_to_display_string(SwitchSensorRole::NONE) == "Unassigned");
        REQUIRE(switch_role_to_display_string(SwitchSensorRole::FILAMENT_RUNOUT) == "Runout");
        REQUIRE(switch_role_to_display_string(SwitchSensorRole::Z_PROBE) == "Z Probe");
        REQUIRE(switch_role_to_display_string(SwitchSensorRole::DOCK_DETECT) == "Dock Detect");
    }
}

TEST_CASE("SwitchSensorTypes - role category helpers", "[sensors][switch][types]") {
    SECTION("is_filament_role") {
        REQUIRE(is_filament_role(SwitchSensorRole::FILAMENT_RUNOUT) == true);
        REQUIRE(is_filament_role(SwitchSensorRole::FILAMENT_TOOLHEAD) == true);
        REQUIRE(is_filament_role(SwitchSensorRole::FILAMENT_ENTRY) == true);
        REQUIRE(is_filament_role(SwitchSensorRole::Z_PROBE) == false);
        REQUIRE(is_filament_role(SwitchSensorRole::DOCK_DETECT) == false);
        REQUIRE(is_filament_role(SwitchSensorRole::NONE) == false);
    }

    SECTION("is_probe_role") {
        REQUIRE(is_probe_role(SwitchSensorRole::Z_PROBE) == true);
        REQUIRE(is_probe_role(SwitchSensorRole::FILAMENT_RUNOUT) == false);
        REQUIRE(is_probe_role(SwitchSensorRole::NONE) == false);
    }
}

TEST_CASE("SwitchSensorTypes - type string conversion", "[sensors][switch][types]") {
    SECTION("switch_type_to_string") {
        REQUIRE(switch_type_to_string(SwitchSensorType::SWITCH) == "switch");
        REQUIRE(switch_type_to_string(SwitchSensorType::MOTION) == "motion");
    }

    SECTION("switch_type_from_string") {
        REQUIRE(switch_type_from_string("switch") == SwitchSensorType::SWITCH);
        REQUIRE(switch_type_from_string("motion") == SwitchSensorType::MOTION);
        REQUIRE(switch_type_from_string("invalid") == SwitchSensorType::SWITCH);
        REQUIRE(switch_type_from_string("") == SwitchSensorType::SWITCH);
    }
}

// ============================================================================
// Z_PROBE Role Tests
// ============================================================================

// ============================================================================
// Z_PROBE Role Tests
// ============================================================================

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - Z_PROBE role assignment",
                 "[filament][probe]") {
    // Discover sensors that can be assigned as probe
    std::vector<std::string> sensors = {"filament_switch_sensor e1_sensor",
                                        "filament_switch_sensor e2_sensor"};
    mgr().discover_sensors(sensors);

    SECTION("Can assign Z_PROBE role") {
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::Z_PROBE);
        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "e1_sensor"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == FilamentSensorRole::Z_PROBE);
    }

    SECTION("Z_PROBE role is stored correctly after assignment") {
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::Z_PROBE);

        auto configs = mgr().get_sensors();
        int probe_count = 0;
        for (const auto& config : configs) {
            if (config.role == FilamentSensorRole::Z_PROBE) {
                probe_count++;
                REQUIRE(config.klipper_name == "filament_switch_sensor e1_sensor");
            }
        }
        REQUIRE(probe_count == 1);
    }

    SECTION("Z_PROBE role assignment is unique - clears from previous sensor") {
        // Assign Z_PROBE to first sensor
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::Z_PROBE);

        // Assign Z_PROBE to second sensor - should clear from first
        mgr().set_sensor_role("filament_switch_sensor e2_sensor", FilamentSensorRole::Z_PROBE);

        auto configs = mgr().get_sensors();

        // First sensor should now have NONE
        auto e1_it = std::find_if(configs.begin(), configs.end(),
                                  [](const auto& c) { return c.sensor_name == "e1_sensor"; });
        REQUIRE(e1_it->role == FilamentSensorRole::NONE);

        // Second sensor should have Z_PROBE
        auto e2_it = std::find_if(configs.begin(), configs.end(),
                                  [](const auto& c) { return c.sensor_name == "e2_sensor"; });
        REQUIRE(e2_it->role == FilamentSensorRole::Z_PROBE);
    }

    SECTION("Can clear Z_PROBE role by assigning NONE") {
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::Z_PROBE);
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::NONE);

        auto configs = mgr().get_sensors();
        for (const auto& config : configs) {
            REQUIRE(config.role != FilamentSensorRole::Z_PROBE);
        }
    }

    SECTION("Z_PROBE and filament roles are independent") {
        // Assign both Z_PROBE and RUNOUT to different sensors
        mgr().set_sensor_role("filament_switch_sensor e1_sensor", FilamentSensorRole::Z_PROBE);
        mgr().set_sensor_role("filament_switch_sensor e2_sensor", FilamentSensorRole::RUNOUT);

        auto configs = mgr().get_sensors();

        auto e1_it = std::find_if(configs.begin(), configs.end(),
                                  [](const auto& c) { return c.sensor_name == "e1_sensor"; });
        auto e2_it = std::find_if(configs.begin(), configs.end(),
                                  [](const auto& c) { return c.sensor_name == "e2_sensor"; });

        REQUIRE(e1_it->role == FilamentSensorRole::Z_PROBE);
        REQUIRE(e2_it->role == FilamentSensorRole::RUNOUT);
    }
}

TEST_CASE_METHOD(FilamentSensorTestFixture, "FilamentSensorManager - is_probe_triggered behavior",
                 "[filament][probe]") {
    SECTION("Returns false when no probe sensor is assigned") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        // No role assignment
        REQUIRE_FALSE(mgr().is_probe_triggered());
    }

    SECTION("Returns false when master is disabled") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        mgr().set_sensor_role("filament_switch_sensor e1", FilamentSensorRole::Z_PROBE);

        // Trigger the probe
        json status;
        status["filament_switch_sensor e1"]["filament_detected"] = true;
        mgr().update_from_status(status);

        // Disable master
        mgr().set_master_enabled(false);
        REQUIRE_FALSE(mgr().is_probe_triggered());
    }

    SECTION("Returns false when probe sensor is individually disabled") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        mgr().set_sensor_role("filament_switch_sensor e1", FilamentSensorRole::Z_PROBE);

        // Trigger the probe
        json status;
        status["filament_switch_sensor e1"]["filament_detected"] = true;
        mgr().update_from_status(status);

        // Disable just the probe sensor
        mgr().set_sensor_enabled("filament_switch_sensor e1", false);
        REQUIRE_FALSE(mgr().is_probe_triggered());
    }

    SECTION("Returns true when probe is enabled and triggered") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        mgr().set_sensor_role("filament_switch_sensor e1", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor e1"]["filament_detected"] = true;
        mgr().update_from_status(status);

        REQUIRE(mgr().is_probe_triggered());
    }

    SECTION("Returns false when probe is enabled but not triggered") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        mgr().set_sensor_role("filament_switch_sensor e1", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor e1"]["filament_detected"] = false;
        mgr().update_from_status(status);

        REQUIRE_FALSE(mgr().is_probe_triggered());
    }

    SECTION("Only the configured probe sensor affects is_probe_triggered") {
        // Multiple sensors, only one is probe
        mgr().discover_sensors(
            {"filament_switch_sensor probe_sensor", "filament_switch_sensor other_sensor"});
        mgr().set_sensor_role("filament_switch_sensor probe_sensor", FilamentSensorRole::Z_PROBE);
        mgr().set_sensor_role("filament_switch_sensor other_sensor", FilamentSensorRole::RUNOUT);

        // Trigger the non-probe sensor only
        json status;
        status["filament_switch_sensor other_sensor"]["filament_detected"] = true;
        status["filament_switch_sensor probe_sensor"]["filament_detected"] = false;
        mgr().update_from_status(status);

        // Probe should not be triggered
        REQUIRE_FALSE(mgr().is_probe_triggered());

        // Now trigger the probe sensor
        json status2;
        status2["filament_switch_sensor probe_sensor"]["filament_detected"] = true;
        mgr().update_from_status(status2);

        REQUIRE(mgr().is_probe_triggered());
    }

    SECTION("Probe state transitions correctly") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        mgr().set_sensor_role("filament_switch_sensor e1", FilamentSensorRole::Z_PROBE);

        // Initial state — default detected=true means probe reports as triggered
        // until the first Klipper status flips it.
        REQUIRE(mgr().is_probe_triggered());

        // Untrigger
        json status1;
        status1["filament_switch_sensor e1"]["filament_detected"] = false;
        mgr().update_from_status(status1);
        REQUIRE_FALSE(mgr().is_probe_triggered());

        // Trigger
        json status2;
        status2["filament_switch_sensor e1"]["filament_detected"] = true;
        mgr().update_from_status(status2);
        REQUIRE(mgr().is_probe_triggered());

        // Untrigger again
        json status3;
        status3["filament_switch_sensor e1"]["filament_detected"] = false;
        mgr().update_from_status(status3);
        REQUIRE_FALSE(mgr().is_probe_triggered());
    }
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - probe_triggered subject updates", "[filament][probe]") {
    SECTION("Subject returns -1 when no probe assigned") {
        mgr().discover_sensors({"filament_switch_sensor e1"});
        // No Z_PROBE role assigned
        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(subject != nullptr);
        REQUIRE(lv_subject_get_int(subject) == -1);
    }

    SECTION("Subject returns 1 after probe assignment with no state update") {
        // Default detected=true means the subject reports triggered until
        // the first explicit status update flips it (c176470bb optimism).
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("Subject returns 1 when probe is triggered") {
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = true;
        mgr().update_from_status(status);

        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("Subject returns 0 when probe is not triggered") {
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = false;
        mgr().update_from_status(status);

        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("Subject returns 2 (disabled) when master disabled") {
        // A configured Z_PROBE sensor with the master toggle off surfaces as
        // "disabled" (value 2), not "no sensor" (-1) — matches the runout
        // role behavior so the indicator shows a muted icon instead of hiding.
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = true;
        mgr().update_from_status(status);

        mgr().set_master_enabled(false);

        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(lv_subject_get_int(subject) == 2);
    }

    SECTION("Subject returns 2 (disabled) when probe sensor disabled") {
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = true;
        mgr().update_from_status(status);

        mgr().set_sensor_enabled("filament_switch_sensor probe", false);

        auto* subject = mgr().get_probe_triggered_subject();
        REQUIRE(lv_subject_get_int(subject) == 2);
    }

    SECTION("Subject updates correctly via update_from_status with JSON") {
        mgr().discover_sensors({"filament_switch_sensor probe"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

        auto* subject = mgr().get_probe_triggered_subject();

        // Sequence of state changes
        json status1;
        status1["filament_switch_sensor probe"]["filament_detected"] = true;
        mgr().update_from_status(status1);
        REQUIRE(lv_subject_get_int(subject) == 1);

        json status2;
        status2["filament_switch_sensor probe"]["filament_detected"] = false;
        mgr().update_from_status(status2);
        REQUIRE(lv_subject_get_int(subject) == 0);

        json status3;
        status3["filament_switch_sensor probe"]["filament_detected"] = true;
        mgr().update_from_status(status3);
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("Probe subject is independent of filament role subjects") {
        mgr().discover_sensors(
            {"filament_switch_sensor probe", "filament_switch_sensor runout_sensor"});
        mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);
        mgr().set_sensor_role("filament_switch_sensor runout_sensor", FilamentSensorRole::RUNOUT);

        // Update both sensors
        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = true;
        status["filament_switch_sensor runout_sensor"]["filament_detected"] = false;
        mgr().update_from_status(status);

        // Check subjects are independent
        REQUIRE(lv_subject_get_int(mgr().get_probe_triggered_subject()) == 1);
        REQUIRE(lv_subject_get_int(mgr().get_runout_detected_subject()) == 0);
    }
}

TEST_CASE_METHOD(FilamentSensorTestFixture,
                 "FilamentSensorManager - Z_PROBE with state change callback",
                 "[filament][probe]") {
    mgr().discover_sensors({"filament_switch_sensor probe"});
    mgr().set_sensor_role("filament_switch_sensor probe", FilamentSensorRole::Z_PROBE);

    SECTION("State change callback fires for probe sensor changes") {
        bool callback_fired = false;
        std::string changed_sensor;
        bool old_detected = false;
        bool new_detected = true;

        mgr().set_state_change_callback([&](const std::string& name,
                                            const FilamentSensorState& old_state,
                                            const FilamentSensorState& new_state) {
            callback_fired = true;
            changed_sensor = name;
            old_detected = old_state.filament_detected;
            new_detected = new_state.filament_detected;
        });

        // Default detected=true → flip to false to observe a transition
        json status;
        status["filament_switch_sensor probe"]["filament_detected"] = false;
        mgr().update_from_status(status);

        REQUIRE(callback_fired);
        REQUIRE(changed_sensor == "filament_switch_sensor probe");
        REQUIRE(old_detected == true);
        REQUIRE(new_detected == false);
    }
}
