// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_width_sensor_manager.cpp
 * @brief Unit tests for WidthSensorManager
 *
 * Tests cover:
 * - Sensor discovery from Klipper object names (tsl1401cl, hall)
 * - Role assignment (FLOW_COMPENSATION)
 * - State updates from Moonraker status JSON
 * - Subject value correctness for UI binding
 * - Config persistence
 */

#include "../ui_test_utils.h"
#include "width_sensor_manager.h"
#include "width_sensor_types.h"

#include <spdlog/spdlog.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

// ============================================================================
// Test Access
// ============================================================================

namespace helix::sensors {
class WidthSensorManagerTestAccess {
  public:
    static void reset(WidthSensorManager& obj) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex_);
        obj.sensors_.clear();
        obj.states_.clear();
        obj.sync_mode_ = true;
        // Reset subject values but keep subjects initialized
        if (obj.subjects_initialized_) {
            lv_subject_set_int(&obj.sensor_count_, 0);
            lv_subject_set_int(&obj.diameter_, -1);
            lv_subject_copy_string(&obj.diameter_text_, "--");
        }
    }
};
} // namespace helix::sensors

// ============================================================================
// Test Fixture
// ============================================================================

class WidthSensorTestFixture {
  public:
    WidthSensorTestFixture() {
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

        // Initialize subjects first (idempotent - only runs once)
        mgr().init_subjects();

        // Then reset state for test isolation (clears data but keeps subjects)
        WidthSensorManagerTestAccess::reset(mgr());
    }

    ~WidthSensorTestFixture() {
        // Reset after each test
        WidthSensorManagerTestAccess::reset(mgr());
    }

  protected:
    WidthSensorManager& mgr() {
        return WidthSensorManager::instance();
    }

    // Helper to discover standard test sensors
    void discover_test_sensors() {
        std::vector<std::string> sensors = {"tsl1401cl_filament_width_sensor",
                                            "hall_filament_width_sensor"};
        mgr().discover(sensors);
    }

    // Helper to simulate Moonraker status update
    void update_sensor_state(const std::string& klipper_name, float diameter, float raw_value) {
        json status;
        status[klipper_name]["Diameter"] = diameter;
        status[klipper_name]["Raw"] = raw_value;
        mgr().update_from_status(status);
    }

  private:
    static lv_display_t* display_;
    static bool display_created_;
};

// Static members
lv_display_t* WidthSensorTestFixture::display_ = nullptr;
bool WidthSensorTestFixture::display_created_ = false;

// ============================================================================
// Type Helper Tests (width_sensor_types.h)
// ============================================================================

TEST_CASE("WidthSensorTypes - role string conversion", "[width][types]") {
    SECTION("width_role_to_string") {
        REQUIRE(width_role_to_string(WidthSensorRole::NONE) == "none");
        REQUIRE(width_role_to_string(WidthSensorRole::FLOW_COMPENSATION) == "flow_compensation");
    }

    SECTION("width_role_from_string") {
        REQUIRE(width_role_from_string("none") == WidthSensorRole::NONE);
        REQUIRE(width_role_from_string("flow_compensation") == WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(width_role_from_string("invalid") == WidthSensorRole::NONE);
        REQUIRE(width_role_from_string("") == WidthSensorRole::NONE);
    }

    SECTION("width_role_to_display_string") {
        REQUIRE(width_role_to_display_string(WidthSensorRole::NONE) == "Unassigned");
        REQUIRE(width_role_to_display_string(WidthSensorRole::FLOW_COMPENSATION) ==
                "Flow Compensation");
    }
}

TEST_CASE("WidthSensorTypes - type string conversion", "[width][types]") {
    SECTION("width_type_to_string") {
        REQUIRE(width_type_to_string(WidthSensorType::TSL1401CL) == "tsl1401cl");
        REQUIRE(width_type_to_string(WidthSensorType::HALL) == "hall");
    }

    SECTION("width_type_from_string") {
        REQUIRE(width_type_from_string("tsl1401cl") == WidthSensorType::TSL1401CL);
        REQUIRE(width_type_from_string("hall") == WidthSensorType::HALL);
        REQUIRE(width_type_from_string("invalid") == WidthSensorType::TSL1401CL);
        REQUIRE(width_type_from_string("") == WidthSensorType::TSL1401CL);
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - discovery", "[width][discovery]") {
    SECTION("Discovers TSL1401CL sensor") {
        std::vector<std::string> sensors = {"tsl1401cl_filament_width_sensor"};
        mgr().discover(sensors);

        REQUIRE(mgr().has_sensors());
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 1);
        REQUIRE(configs[0].klipper_name == "tsl1401cl_filament_width_sensor");
        REQUIRE(configs[0].sensor_name == "tsl1401cl");
        REQUIRE(configs[0].type == WidthSensorType::TSL1401CL);
        REQUIRE(configs[0].enabled == true);
        // First discovered sensor is auto-assigned to FLOW_COMPENSATION role
        REQUIRE(configs[0].role == WidthSensorRole::FLOW_COMPENSATION);
    }

    SECTION("Discovers Hall sensor") {
        std::vector<std::string> sensors = {"hall_filament_width_sensor"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "hall_filament_width_sensor");
        REQUIRE(configs[0].sensor_name == "hall");
        REQUIRE(configs[0].type == WidthSensorType::HALL);
        // First discovered sensor is auto-assigned to FLOW_COMPENSATION role
        REQUIRE(configs[0].role == WidthSensorRole::FLOW_COMPENSATION);
    }

    SECTION("Discovers Hall sensor alias (hall_fila_switch)") {
        std::vector<std::string> sensors = {"hall_fila_switch"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "hall_fila_switch");
        REQUIRE(configs[0].sensor_name == "hall");
        REQUIRE(configs[0].type == WidthSensorType::HALL);
    }

    SECTION("Discovers multiple sensors") {
        discover_test_sensors();

        REQUIRE(mgr().sensor_count() == 2);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].sensor_name == "tsl1401cl");
        REQUIRE(configs[0].type == WidthSensorType::TSL1401CL);
        // First discovered sensor is auto-assigned to FLOW_COMPENSATION role
        REQUIRE(configs[0].role == WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(configs[1].sensor_name == "hall");
        REQUIRE(configs[1].type == WidthSensorType::HALL);
        // Second sensor remains unassigned (NONE)
        REQUIRE(configs[1].role == WidthSensorRole::NONE);
    }

    SECTION("Ignores unrelated objects") {
        std::vector<std::string> sensors = {"tsl1401cl_filament_width_sensor",
                                            "filament_switch_sensor runout",
                                            "temperature_sensor chamber", "extruder"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "tsl1401cl");
    }

    SECTION("Empty sensor list clears previous sensors") {
        discover_test_sensors();
        REQUIRE(mgr().sensor_count() == 2);

        mgr().discover({});
        REQUIRE(mgr().sensor_count() == 0);
        REQUIRE_FALSE(mgr().has_sensors());
    }

    SECTION("Re-discovery replaces sensor list") {
        std::vector<std::string> sensors1 = {"tsl1401cl_filament_width_sensor"};
        mgr().discover(sensors1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "tsl1401cl");

        std::vector<std::string> sensors2 = {"hall_filament_width_sensor"};
        mgr().discover(sensors2);
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "hall");
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().get_sensor_count_subject();
        REQUIRE(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        REQUIRE(lv_subject_get_int(count_subject) == 2);

        mgr().discover({});
        REQUIRE(lv_subject_get_int(count_subject) == 0);
    }
}

// ============================================================================
// Role Assignment Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - role assignment", "[width][roles]") {
    discover_test_sensors();

    SECTION("Can set FLOW_COMPENSATION role") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "tsl1401cl"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == WidthSensorRole::FLOW_COMPENSATION);
    }

    SECTION("Role assignment is unique - assigning same role clears previous") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);
        mgr().set_sensor_role("hall_filament_width_sensor", WidthSensorRole::FLOW_COMPENSATION);

        auto configs = mgr().get_sensors();

        auto tsl_it = std::find_if(configs.begin(), configs.end(),
                                   [](const auto& c) { return c.sensor_name == "tsl1401cl"; });
        REQUIRE(tsl_it->role == WidthSensorRole::NONE);

        auto hall_it = std::find_if(configs.begin(), configs.end(),
                                    [](const auto& c) { return c.sensor_name == "hall"; });
        REQUIRE(hall_it->role == WidthSensorRole::FLOW_COMPENSATION);
    }

    SECTION("Can assign NONE without affecting other sensors") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);

        mgr().set_sensor_role("tsl1401cl_filament_width_sensor", WidthSensorRole::NONE);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "tsl1401cl"; });
        REQUIRE(it->role == WidthSensorRole::NONE);
    }

    SECTION("Assigning role to unknown sensor does nothing") {
        mgr().set_sensor_role("nonexistent_sensor", WidthSensorRole::FLOW_COMPENSATION);

        for (const auto& config : mgr().get_sensors()) {
            REQUIRE(config.role == WidthSensorRole::NONE);
        }
    }
}

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - state updates", "[width][state]") {
    discover_test_sensors();
    mgr().set_sensor_role("tsl1401cl_filament_width_sensor", WidthSensorRole::FLOW_COMPENSATION);

    SECTION("Parses diameter from status JSON") {
        auto state = mgr().get_sensor_state(WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(state.has_value());
        REQUIRE(state->diameter == 0.0f);

        json status;
        status["tsl1401cl_filament_width_sensor"]["Diameter"] = 1.75f;
        status["tsl1401cl_filament_width_sensor"]["Raw"] = 12345.0f;
        mgr().update_from_status(status);

        state = mgr().get_sensor_state(WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(state->diameter == Catch::Approx(1.75f));
        REQUIRE(state->raw_value == Catch::Approx(12345.0f));
    }

    SECTION("Status update for unknown sensor is ignored") {
        json status;
        status["unknown_sensor"]["Diameter"] = 1.75f;
        mgr().update_from_status(status);

        REQUIRE(mgr().sensor_count() == 2);
    }

    SECTION("Empty status update is handled") {
        json status = json::object();
        mgr().update_from_status(status);

        REQUIRE(mgr().has_sensors());
    }
}

// ============================================================================
// Subject Value Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - subject values",
                 "[width][subjects]") {
    discover_test_sensors();

    SECTION("Diameter subject shows 0 after discovery (first sensor auto-assigned)") {
        // First sensor is auto-assigned to FLOW_COMPENSATION, diameter defaults to 0.0
        REQUIRE(lv_subject_get_int(mgr().get_diameter_subject()) == 0);
    }

    SECTION("Diameter subject updates correctly (diameter x 1000)") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);

        // After assignment, should show 0 since state defaults to 0.0
        REQUIRE(lv_subject_get_int(mgr().get_diameter_subject()) == 0);

        // Update state with diameter 1.75mm
        update_sensor_state("tsl1401cl_filament_width_sensor", 1.75f, 12345.0f);
        REQUIRE(lv_subject_get_int(mgr().get_diameter_subject()) == 1750);

        // Update with different value
        update_sensor_state("tsl1401cl_filament_width_sensor", 1.82f, 54321.0f);
        REQUIRE(lv_subject_get_int(mgr().get_diameter_subject()) == 1820);
    }

    SECTION("Diameter subject shows -1 when sensor disabled") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);
        update_sensor_state("tsl1401cl_filament_width_sensor", 1.75f, 12345.0f);

        mgr().set_sensor_enabled("tsl1401cl_filament_width_sensor", false);
        REQUIRE(lv_subject_get_int(mgr().get_diameter_subject()) == -1);
    }
}

// ============================================================================
// Config Persistence Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - config persistence",
                 "[width][config]") {
    discover_test_sensors();

    SECTION("save_config returns JSON with role assignments") {
        mgr().set_sensor_role("tsl1401cl_filament_width_sensor",
                              WidthSensorRole::FLOW_COMPENSATION);

        json config = mgr().save_config();

        REQUIRE(config.is_object());
        REQUIRE(config.contains("sensors"));
        REQUIRE(config["sensors"].is_array());
        REQUIRE(config["sensors"].size() == 2);

        bool found_tsl = false;
        for (const auto& sensor : config["sensors"]) {
            if (sensor["klipper_name"] == "tsl1401cl_filament_width_sensor") {
                REQUIRE(sensor["role"] == "flow_compensation");
                found_tsl = true;
            }
        }
        REQUIRE(found_tsl);
    }

    SECTION("load_config restores role assignments") {
        // Set up config JSON
        json config;
        json sensors_array = json::array();
        json sensor1;
        sensor1["klipper_name"] = "tsl1401cl_filament_width_sensor";
        sensor1["role"] = "flow_compensation";
        sensor1["enabled"] = true;
        sensors_array.push_back(sensor1);
        config["sensors"] = sensors_array;

        mgr().load_config(config);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "tsl1401cl"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == WidthSensorRole::FLOW_COMPENSATION);
    }

    SECTION("load_config with unknown sensor is handled gracefully") {
        json config;
        json sensors_array = json::array();
        json sensor1;
        sensor1["klipper_name"] = "unknown_sensor";
        sensor1["role"] = "flow_compensation";
        sensors_array.push_back(sensor1);
        config["sensors"] = sensors_array;

        // Should not crash
        mgr().load_config(config);

        // Existing sensors should be unaffected (first sensor auto-assigned to FLOW_COMPENSATION)
        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(configs[1].role == WidthSensorRole::NONE);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - edge cases", "[width][edge]") {
    SECTION("get_sensor_state returns state for auto-assigned FLOW_COMPENSATION role") {
        discover_test_sensors();
        // First sensor is auto-assigned to FLOW_COMPENSATION
        auto state = mgr().get_sensor_state(WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(state.has_value());
        REQUIRE(state->diameter == 0.0f); // Default value
    }

    SECTION("get_sensor_state returns nullopt for NONE role") {
        discover_test_sensors();
        auto state = mgr().get_sensor_state(WidthSensorRole::NONE);
        REQUIRE_FALSE(state.has_value());
    }

    SECTION("is_sensor_available checks role assignment and enabled") {
        discover_test_sensors();
        // First sensor is auto-assigned to FLOW_COMPENSATION, so it should be available
        REQUIRE(mgr().is_sensor_available(WidthSensorRole::FLOW_COMPENSATION));

        mgr().set_sensor_enabled("tsl1401cl_filament_width_sensor", false);
        REQUIRE_FALSE(mgr().is_sensor_available(WidthSensorRole::FLOW_COMPENSATION));
    }

    SECTION("category_name returns 'width'") {
        REQUIRE(mgr().category_name() == "width");
    }
}
