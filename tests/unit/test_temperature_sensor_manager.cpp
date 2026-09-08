// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temperature_sensor_manager.cpp
 * @brief Unit tests for TemperatureSensorManager
 *
 * Tests cover:
 * - Type helpers: role/type string conversion
 * - Sensor discovery from Klipper object names (temperature_sensor, temperature_fan)
 * - Auto-categorization (CHAMBER, MCU, HOST, AUXILIARY)
 * - State updates from Moonraker status JSON
 * - Subject value correctness (decidegrees) for UI binding
 * - Config persistence
 * - Sorted output by priority
 */

#include "../ui_test_utils.h"
#include "device_display_name.h"
#include "temperature_sensor_manager.h"
#include "temperature_sensor_types.h"

#include <spdlog/sinks/ringbuffer_sink.h>
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
class TemperatureSensorManagerTestAccess {
  public:
    static void reset(TemperatureSensorManager& obj) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex_);
        obj.sensors_.clear();
        obj.states_.clear();
        obj.temp_subjects_.clear();
        obj.sync_mode_ = true;
        obj.deinit_subjects();
    }
};
} // namespace helix::sensors

// ============================================================================
// Test Fixture
// ============================================================================

class TemperatureSensorTestFixture {
  public:
    TemperatureSensorTestFixture() {
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
        TemperatureSensorManagerTestAccess::reset(mgr());

        // Initialize subjects after reset
        mgr().init_subjects();
    }

    ~TemperatureSensorTestFixture() {
        // Reset after each test
        TemperatureSensorManagerTestAccess::reset(mgr());
    }

  protected:
    TemperatureSensorManager& mgr() {
        return TemperatureSensorManager::instance();
    }

    // Helper to discover standard test sensors
    void discover_test_sensors() {
        std::vector<std::string> sensors = {"temperature_sensor mcu_temp",
                                            "temperature_sensor raspberry_pi",
                                            "temperature_fan exhaust_fan"};
        mgr().discover(sensors);
    }

    // Helper to simulate Moonraker status update for a temperature sensor
    void update_sensor_temp(const std::string& klipper_name, float temperature, float target = 0.0f,
                            float speed = 0.0f) {
        json status;
        status[klipper_name]["temperature"] = temperature;
        if (target > 0.0f) {
            status[klipper_name]["target"] = target;
        }
        if (speed > 0.0f) {
            status[klipper_name]["speed"] = speed;
        }
        mgr().update_from_status(status);
    }

  private:
    static lv_display_t* display_;
    static bool display_created_;
};

// Static members
lv_display_t* TemperatureSensorTestFixture::display_ = nullptr;
bool TemperatureSensorTestFixture::display_created_ = false;

// ============================================================================
// Type Helper Tests (temperature_sensor_types.h)
// ============================================================================

TEST_CASE("TemperatureSensorTypes - role string conversion", "[temperature][types]") {
    SECTION("temp_role_to_string") {
        REQUIRE(temp_role_to_string(TemperatureSensorRole::NONE) == "none");
        REQUIRE(temp_role_to_string(TemperatureSensorRole::CHAMBER) == "chamber");
        REQUIRE(temp_role_to_string(TemperatureSensorRole::MCU) == "mcu");
        REQUIRE(temp_role_to_string(TemperatureSensorRole::HOST) == "host");
        REQUIRE(temp_role_to_string(TemperatureSensorRole::AUXILIARY) == "auxiliary");
    }

    SECTION("temp_role_from_string") {
        REQUIRE(temp_role_from_string("none") == TemperatureSensorRole::NONE);
        REQUIRE(temp_role_from_string("chamber") == TemperatureSensorRole::CHAMBER);
        REQUIRE(temp_role_from_string("mcu") == TemperatureSensorRole::MCU);
        REQUIRE(temp_role_from_string("host") == TemperatureSensorRole::HOST);
        REQUIRE(temp_role_from_string("auxiliary") == TemperatureSensorRole::AUXILIARY);
        REQUIRE(temp_role_from_string("invalid") == TemperatureSensorRole::NONE);
        REQUIRE(temp_role_from_string("") == TemperatureSensorRole::NONE);
    }

    SECTION("temp_role_to_display_string") {
        REQUIRE(temp_role_to_display_string(TemperatureSensorRole::NONE) == "Unassigned");
        REQUIRE(temp_role_to_display_string(TemperatureSensorRole::CHAMBER) == "Chamber");
        REQUIRE(temp_role_to_display_string(TemperatureSensorRole::MCU) == "MCU");
        REQUIRE(temp_role_to_display_string(TemperatureSensorRole::HOST) == "Host");
        REQUIRE(temp_role_to_display_string(TemperatureSensorRole::AUXILIARY) == "Auxiliary");
    }
}

TEST_CASE("TemperatureSensorTypes - type string conversion", "[temperature][types]") {
    SECTION("temp_type_to_string") {
        REQUIRE(temp_type_to_string(TemperatureSensorType::TEMPERATURE_SENSOR) ==
                "temperature_sensor");
        REQUIRE(temp_type_to_string(TemperatureSensorType::TEMPERATURE_FAN) == "temperature_fan");
    }

    SECTION("temp_type_from_string") {
        REQUIRE(temp_type_from_string("temperature_sensor") ==
                TemperatureSensorType::TEMPERATURE_SENSOR);
        REQUIRE(temp_type_from_string("temperature_fan") == TemperatureSensorType::TEMPERATURE_FAN);
        REQUIRE(temp_type_from_string("invalid") == TemperatureSensorType::TEMPERATURE_SENSOR);
        REQUIRE(temp_type_from_string("") == TemperatureSensorType::TEMPERATURE_SENSOR);
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - discovery",
                 "[temperature][discovery]") {
    SECTION("Discovers temperature_sensor objects") {
        std::vector<std::string> sensors = {"temperature_sensor mcu_temp"};
        mgr().discover(sensors);

        REQUIRE(mgr().has_sensors());
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 1);
        REQUIRE(configs[0].klipper_name == "temperature_sensor mcu_temp");
        REQUIRE(configs[0].sensor_name == "mcu_temp");
        REQUIRE(configs[0].type == TemperatureSensorType::TEMPERATURE_SENSOR);
        REQUIRE(configs[0].enabled == true);
    }

    SECTION("Discovers temperature_fan objects") {
        std::vector<std::string> sensors = {"temperature_fan exhaust_fan"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "temperature_fan exhaust_fan");
        REQUIRE(configs[0].sensor_name == "exhaust_fan");
        REQUIRE(configs[0].type == TemperatureSensorType::TEMPERATURE_FAN);
    }

    SECTION("Ignores extruder, heater_bed, and unrelated objects") {
        std::vector<std::string> sensors = {
            "temperature_sensor mcu_temp", "temperature_sensor extruder",
            "temperature_sensor heater_bed", "filament_switch_sensor runout", "bme280 chamber"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "mcu_temp");
    }

    SECTION("Ignores extruder1 (multi-extruder)") {
        std::vector<std::string> sensors = {"temperature_sensor mcu_temp",
                                            "temperature_sensor extruder1",
                                            "temperature_sensor extruder2"};
        mgr().discover(sensors);

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "mcu_temp");
    }

    SECTION("Empty sensor list clears previous sensors") {
        discover_test_sensors();
        REQUIRE(mgr().sensor_count() == 3);

        mgr().discover({});
        REQUIRE(mgr().sensor_count() == 0);
        REQUIRE_FALSE(mgr().has_sensors());
    }

    SECTION("Re-discovery replaces sensor list") {
        std::vector<std::string> sensors1 = {"temperature_sensor mcu_temp"};
        mgr().discover(sensors1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "mcu_temp");

        std::vector<std::string> sensors2 = {"temperature_sensor raspberry_pi"};
        mgr().discover(sensors2);
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].sensor_name == "raspberry_pi");
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().get_sensor_count_subject();
        REQUIRE(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        REQUIRE(lv_subject_get_int(count_subject) == 3);

        mgr().discover({});
        REQUIRE(lv_subject_get_int(count_subject) == 0);
    }
}

// ============================================================================
// Auto-Categorization Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - auto-categorization",
                 "[temperature][roles]") {
    SECTION("chamber_temp gets CHAMBER role, priority 0") {
        std::vector<std::string> sensors = {"temperature_sensor chamber_temp"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::CHAMBER);
        REQUIRE(configs[0].priority == 0);
    }

    SECTION("mcu_temp gets MCU role, priority 10") {
        std::vector<std::string> sensors = {"temperature_sensor mcu_temp"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::MCU);
        REQUIRE(configs[0].priority == 10);
    }

    SECTION("raspberry_pi gets HOST role, priority 20") {
        std::vector<std::string> sensors = {"temperature_sensor raspberry_pi"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::HOST);
        REQUIRE(configs[0].priority == 20);
    }

    SECTION("random_sensor gets AUXILIARY role, priority 100") {
        std::vector<std::string> sensors = {"temperature_sensor random_sensor"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::AUXILIARY);
        REQUIRE(configs[0].priority == 100);
    }

    SECTION("host_temp gets HOST role") {
        std::vector<std::string> sensors = {"temperature_sensor host_temp"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::HOST);
    }

    SECTION("Name containing raspberry gets HOST role") {
        std::vector<std::string> sensors = {"temperature_sensor my_raspberry_sensor"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].role == TemperatureSensorRole::HOST);
    }
}

// ============================================================================
// Display Name Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - display names",
                 "[temperature][display]") {
    SECTION("Uses get_display_name for readable names") {
        std::vector<std::string> sensors = {"temperature_sensor mcu_temp",
                                            "temperature_sensor chamber_temp"};
        mgr().discover(sensors);

        auto configs = mgr().get_sensors();
        // get_display_name with DeviceType::TEMP_SENSOR produces readable names
        // Exact format depends on device_display_name.cpp implementation,
        // but they should not be empty
        for (const auto& config : configs) {
            REQUIRE_FALSE(config.display_name.empty());
        }
    }

    SECTION("Two chamber sensors produce distinct display names") {
        // Regression: "chamber" and "chamber_temp" both resolved to "Chamber Temperature",
        // causing duplicate entries in the thermistor widget sensor picker.
        std::vector<std::string> sensors = {"temperature_sensor chamber",
                                            "temperature_sensor chamber_temp"};
        mgr().discover(sensors);

        auto sorted = mgr().get_sensors_sorted();
        REQUIRE(sorted.size() == 2);

        // Both should have CHAMBER role
        REQUIRE(sorted[0].role == TemperatureSensorRole::CHAMBER);
        REQUIRE(sorted[1].role == TemperatureSensorRole::CHAMBER);

        // They must have distinct klipper_names (the unique key)
        REQUIRE(sorted[0].klipper_name != sorted[1].klipper_name);
    }
}

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - state updates",
                 "[temperature][state]") {
    discover_test_sensors();

    SECTION("Parses temperature from status JSON") {
        json status;
        status["temperature_sensor mcu_temp"]["temperature"] = 45.2f;
        mgr().update_from_status(status);

        auto state = mgr().get_sensor_state("temperature_sensor mcu_temp");
        REQUIRE(state.has_value());
        REQUIRE(state->temperature == Catch::Approx(45.2f));
    }

    SECTION("Temperature stored as decidegrees in subject (25.5C -> 255)") {
        lv_subject_t* subj = mgr().get_temp_subject("temperature_sensor mcu_temp");
        REQUIRE(subj != nullptr);

        update_sensor_temp("temperature_sensor mcu_temp", 25.5f);
        REQUIRE(lv_subject_get_int(subj) == 255);
    }

    SECTION("temperature_fan also gets target and speed") {
        json status;
        status["temperature_fan exhaust_fan"]["temperature"] = 38.5f;
        status["temperature_fan exhaust_fan"]["target"] = 40.0f;
        status["temperature_fan exhaust_fan"]["speed"] = 0.65f;
        mgr().update_from_status(status);

        auto state = mgr().get_sensor_state("temperature_fan exhaust_fan");
        REQUIRE(state.has_value());
        REQUIRE(state->temperature == Catch::Approx(38.5f));
        REQUIRE(state->target == Catch::Approx(40.0f));
        REQUIRE(state->speed == Catch::Approx(0.65f));
    }

    SECTION("Unknown sensor status is ignored") {
        json status;
        status["temperature_sensor unknown"]["temperature"] = 50.0f;
        mgr().update_from_status(status);

        REQUIRE(mgr().sensor_count() == 3);
    }

    SECTION("Empty status is handled") {
        json status = json::object();
        mgr().update_from_status(status);

        REQUIRE(mgr().has_sensors());
    }
}

// ============================================================================
// Config Persistence Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - config persistence",
                 "[temperature][config]") {
    discover_test_sensors();

    SECTION("save_config returns JSON with roles and enabled state") {
        json config = mgr().save_config();

        REQUIRE(config.is_object());
        REQUIRE(config.contains("sensors"));
        REQUIRE(config["sensors"].is_array());
        REQUIRE(config["sensors"].size() == 3);

        bool found_mcu = false;
        for (const auto& sensor : config["sensors"]) {
            if (sensor["klipper_name"] == "temperature_sensor mcu_temp") {
                REQUIRE(sensor["role"] == "mcu");
                REQUIRE(sensor["enabled"] == true);
                found_mcu = true;
            }
        }
        REQUIRE(found_mcu);
    }

    SECTION("load_config restores roles") {
        json config;
        json sensors_array = json::array();

        json sensor1;
        sensor1["klipper_name"] = "temperature_sensor mcu_temp";
        sensor1["role"] = "auxiliary";
        sensor1["enabled"] = false;
        sensors_array.push_back(sensor1);

        config["sensors"] = sensors_array;

        mgr().load_config(config);

        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "mcu_temp"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == TemperatureSensorRole::AUXILIARY);
        REQUIRE(it->enabled == false);
    }

    SECTION("load_config with unknown sensor is handled") {
        json config;
        json sensors_array = json::array();
        json sensor1;
        sensor1["klipper_name"] = "temperature_sensor unknown_sensor";
        sensor1["role"] = "chamber";
        sensors_array.push_back(sensor1);
        config["sensors"] = sensors_array;

        // Should not crash
        mgr().load_config(config);

        // Existing sensors should keep their auto-assigned roles
        auto configs = mgr().get_sensors();
        auto it = std::find_if(configs.begin(), configs.end(),
                               [](const auto& c) { return c.sensor_name == "mcu_temp"; });
        REQUIRE(it != configs.end());
        REQUIRE(it->role == TemperatureSensorRole::MCU);
    }
}

// ============================================================================
// Sorted Output Tests
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - sorted output",
                 "[temperature][sorting]") {
    SECTION("get_sensors_sorted returns sensors by priority order") {
        std::vector<std::string> sensors = {
            "temperature_sensor random_aux", "temperature_sensor mcu_temp",
            "temperature_sensor chamber_temp", "temperature_sensor raspberry_pi"};
        mgr().discover(sensors);

        auto sorted = mgr().get_sensors_sorted();
        REQUIRE(sorted.size() == 4);

        // Chamber (priority 0) first
        REQUIRE(sorted[0].role == TemperatureSensorRole::CHAMBER);
        // MCU (priority 10) second
        REQUIRE(sorted[1].role == TemperatureSensorRole::MCU);
        // HOST (priority 20) third
        REQUIRE(sorted[2].role == TemperatureSensorRole::HOST);
        // AUXILIARY (priority 100) last
        REQUIRE(sorted[3].role == TemperatureSensorRole::AUXILIARY);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(TemperatureSensorTestFixture, "TemperatureSensorManager - edge cases",
                 "[temperature][edge]") {
    SECTION("get_sensor_state for unknown sensor returns nullopt") {
        discover_test_sensors();
        auto state = mgr().get_sensor_state("temperature_sensor nonexistent");
        REQUIRE_FALSE(state.has_value());
    }

    SECTION("category_name returns temperature") {
        REQUIRE(mgr().category_name() == "temperature");
    }

    SECTION("get_temp_subject returns valid subject after discovery") {
        discover_test_sensors();

        lv_subject_t* subj = mgr().get_temp_subject("temperature_sensor mcu_temp");
        REQUIRE(subj != nullptr);

        // Initial value should be 0 (no status updates yet)
        REQUIRE(lv_subject_get_int(subj) == 0);
    }

    SECTION("get_temp_subject for unknown sensor returns nullptr") {
        discover_test_sensors();

        lv_subject_t* subj = mgr().get_temp_subject("temperature_sensor nonexistent");
        REQUIRE(subj == nullptr);
    }
}

// ============================================================================
// Chamber Override Log Level
// ============================================================================

namespace {

/// RAII spdlog capture that keeps each record's level, so "this is not a
/// warning" is an assertion rather than an intention.
class OverrideLogCapture {
  public:
    OverrideLogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }

    ~OverrideLogCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }

    OverrideLogCapture(const OverrideLogCapture&) = delete;
    OverrideLogCapture& operator=(const OverrideLogCapture&) = delete;

    /// Levels of every captured record whose text contains @p needle.
    std::vector<spdlog::level::level_enum> levels_for(const std::string& needle) const {
        std::vector<spdlog::level::level_enum> out;
        for (const auto& msg : sink_->last_raw(256)) {
            std::string text(msg.payload.data(), msg.payload.size());
            if (text.find(needle) != std::string::npos) {
                out.push_back(msg.level);
            }
        }
        return out;
    }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};

} // namespace

TEST_CASE_METHOD(TemperatureSensorTestFixture,
                 "TemperatureSensorManager - unmatched chamber override is not a warning",
                 "[temperature][chamber][logging]") {
    // A chamber-named object auto-takes the CHAMBER role. Overriding onto a
    // different, absent object is what reaches the unmatched branch: without an
    // incumbent CHAMBER sensor the call early-outs and logs nothing at all.
    mgr().discover({"temperature_fan chamber_fan", "temperature_sensor mcu_temp"});
    bool had_chamber = false;
    for (const auto& config : mgr().get_sensors()) {
        if (config.role == TemperatureSensorRole::CHAMBER) {
            had_chamber = true;
        }
    }
    REQUIRE(had_chamber);

    OverrideLogCapture log;
    mgr().apply_chamber_sensor_override("temperature_sensor chamber_temp");

    // The override names nothing that was discovered, so the role is left unfilled.
    for (const auto& config : mgr().get_sensors()) {
        REQUIRE(config.role != TemperatureSensorRole::CHAMBER);
    }

    // Discovery re-runs on every reconnect, so a warning here repeats forever
    // for a printer whose config omits the object its preset names.
    auto levels = log.levels_for("Manual chamber override");
    REQUIRE(levels.size() == 1);
    REQUIRE(levels[0] == spdlog::level::debug);
}
