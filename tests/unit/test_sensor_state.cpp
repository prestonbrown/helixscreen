// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "sensor_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

TEST_CASE_METHOD(LVGLTestFixture, "SensorState tracks sensors", "[sensor_state]") {
    auto& state = SensorState::instance();

    std::vector<SensorInfo> sensors = {
        {"tasmota_power", "Tasmota Power Meter", "MQTT", {"power", "voltage", "current", "energy"}},
        {"temp_probe", "Chamber Probe", "DS18B20", {"temperature"}},
    };

    // Set initial values via JSON update
    state.set_sensors(sensors);

    // Simulate initial values by setting subjects through set_sensors + a manual update
    // We need to push values through the update path. Since set_sensors creates subjects
    // at 0, we'll check the API shape first, then verify value subjects exist.

    SECTION("discovers sensor IDs") {
        REQUIRE(state.sensor_ids().size() == 2);
    }

    SECTION("identifies energy sensors") {
        auto energy_ids = state.energy_sensor_ids();
        REQUIRE(energy_ids.size() == 1);
        REQUIRE(energy_ids[0] == "tasmota_power");
    }

    SECTION("is_energy_sensor detects energy keys") {
        REQUIRE(SensorState::is_energy_sensor(sensors[0]) == true);
        REQUIRE(SensorState::is_energy_sensor(sensors[1]) == false);
    }

    SECTION("creates value subjects with initial centi-unit values") {
        SubjectLifetime lt;

        // Subjects are created with initial value 0 by set_sensors
        auto* power_subj = state.get_value_subject("tasmota_power", "power", lt);
        REQUIRE(power_subj != nullptr);
        REQUIRE(lv_subject_get_int(power_subj) == 0);

        auto* voltage_subj = state.get_value_subject("tasmota_power", "voltage", lt);
        REQUIRE(voltage_subj != nullptr);

        auto* current_subj = state.get_value_subject("tasmota_power", "current", lt);
        REQUIRE(current_subj != nullptr);

        auto* energy_subj = state.get_value_subject("tasmota_power", "energy", lt);
        REQUIRE(energy_subj != nullptr);

        auto* temp_subj = state.get_value_subject("temp_probe", "temperature", lt);
        REQUIRE(temp_subj != nullptr);
    }

    SECTION("returns nullptr for unknown sensor or key") {
        SubjectLifetime lt;
        REQUIRE(state.get_value_subject("nonexistent", "power", lt) == nullptr);
        REQUIRE(state.get_value_subject("tasmota_power", "nonexistent", lt) == nullptr);
    }

    SECTION("get_sensor_info returns correct info") {
        const auto info = state.get_sensor_info("tasmota_power");
        REQUIRE(info.has_value());
        REQUIRE(info->friendly_name == "Tasmota Power Meter");
        REQUIRE(info->type == "MQTT");
        REQUIRE(info->value_keys.size() == 4);

        const auto temp_info = state.get_sensor_info("temp_probe");
        REQUIRE(temp_info.has_value());
        REQUIRE(temp_info->friendly_name == "Chamber Probe");
        REQUIRE(temp_info->type == "DS18B20");
        REQUIRE(temp_info->value_keys.size() == 1);

        REQUIRE_FALSE(state.get_sensor_info("nonexistent").has_value());
    }

    SECTION("re-discovery replaces sensors") {
        std::vector<SensorInfo> new_sensors = {
            {"new_sensor", "New Sensor", "BME280", {"humidity"}},
        };
        state.set_sensors(new_sensors);

        REQUIRE(state.sensor_ids().size() == 1);

        SubjectLifetime lt;
        REQUIRE(state.get_value_subject("tasmota_power", "power", lt) == nullptr);
        REQUIRE(state.get_value_subject("new_sensor", "humidity", lt) != nullptr);
    }

    SECTION("deinit clears all sensors") {
        state.deinit_subjects();
        REQUIRE(state.sensor_ids().empty());

        SubjectLifetime lt;
        REQUIRE(state.get_value_subject("tasmota_power", "power", lt) == nullptr);
        // Re-init not needed since the outer cleanup will also call deinit
        return;
    }

    state.deinit_subjects();
}

TEST_CASE("SensorState centi-unit conversion", "[sensor_state]") {
    // power: value * 100
    REQUIRE(SensorState::to_centi_units("power", 15.2) == 1520);
    REQUIRE(SensorState::to_centi_units("power", 0.0) == 0);
    REQUIRE(SensorState::to_centi_units("power", 1500.7) == 150070);

    // voltage: value * 100
    REQUIRE(SensorState::to_centi_units("voltage", 230.1) == 23010);

    // current: value * 100000 (centi-milliamps)
    REQUIRE(SensorState::to_centi_units("current", 0.065) == 6500);
    REQUIRE(SensorState::to_centi_units("current", 1.2) == 120000);
    REQUIRE(SensorState::to_centi_units("current", 0.001) == 100);

    // energy: value * 100
    REQUIRE(SensorState::to_centi_units("energy", 42.5) == 4250);
}

TEST_CASE("SensorState format_value", "[sensor_state]") {
    // power
    REQUIRE(SensorState::format_value("power", 1520) == "15.2 W");
    REQUIRE(SensorState::format_value("power", 0) == "0.0 W");

    // voltage
    REQUIRE(SensorState::format_value("voltage", 23010) == "230 V");

    // current — displayed in mA or A
    REQUIRE(SensorState::format_value("current", 6500) == "65.0 mA");
    REQUIRE(SensorState::format_value("current", 120000) == "1.20 A");
    REQUIRE(SensorState::format_value("current", 100) == "1.0 mA");

    // energy
    REQUIRE(SensorState::format_value("energy", 4250) == "42.5 kWh");
}

namespace {

/// Mock API wired to a mock client, subscribed to notify_sensor_update.
struct SensorStateFeed {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState printer_state;
    std::unique_ptr<MoonrakerAPIMock> api;

    SensorStateFeed() {
        printer_state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, printer_state);
        SensorState::instance().subscribe(*api);
    }
    ~SensorStateFeed() {
        SensorState::instance().unsubscribe(*api);
    }
    void notify(const json& values) {
        client.dispatch_method_callback(
            "notify_sensor_update",
            {{"method", "notify_sensor_update"}, {"params", json::array({values})}});
    }
    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

const std::vector<SensorInfo> kTwoMeters = {
    {"meter_a", "Meter A", "INA219", {"power", "voltage"}},
    {"meter_b", "Meter B", "MQTT", {"power"}},
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SensorState applies notify_sensor_update on the main thread",
                 "[sensor_state]") {
    SensorStateFeed feed;
    auto& state = SensorState::instance();
    state.set_sensors(kTwoMeters);

    SensorStateFeed::drain(); // set_sensors' own refresh must not mask the notify path
    SubjectLifetime lt;
    auto* power = state.get_value_subject("meter_a", "power", lt);
    REQUIRE(power != nullptr);

    feed.notify({{"meter_a", {{"power", 15.2}, {"voltage", 230.0}}}, {"ghost", {{"power", 1.0}}}});
    CHECK(lv_subject_get_int(power) == 0);
    SensorStateFeed::drain();
    CHECK(lv_subject_get_int(power) == 1520);
    CHECK(lv_subject_get_int(state.get_value_subject("meter_a", "voltage", lt)) == 23000);
    CHECK(state.get_value_subject("ghost", "power", lt) == nullptr);

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SensorState notify after deinit is dropped", "[sensor_state]") {
    SensorStateFeed feed;
    auto& state = SensorState::instance();
    state.set_sensors(kTwoMeters);

    feed.notify({{"meter_a", {{"power", 99.0}}}});
    state.deinit_subjects();
    SensorStateFeed::drain();

    CHECK(state.sensor_ids().empty());
    SubjectLifetime lt;
    CHECK(state.get_value_subject("meter_a", "power", lt) == nullptr);

    state.set_sensors(kTwoMeters);
    CHECK(lv_subject_get_int(state.get_value_subject("meter_a", "power", lt)) == 0);
    SensorStateFeed::drain();
    CHECK(lv_subject_get_int(state.get_value_subject("meter_a", "power", lt)) == 0);

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SensorState reconnect with fewer sensors removes their subjects",
                 "[sensor_state]") {
    auto& state = SensorState::instance();
    state.set_sensors(kTwoMeters);

    SubjectLifetime kept_lt;
    SubjectLifetime gone_lt;
    auto* kept = state.get_value_subject("meter_a", "power", kept_lt);
    REQUIRE(kept != nullptr);
    REQUIRE(state.get_value_subject("meter_b", "power", gone_lt) != nullptr);
    REQUIRE(state.get_value_subject("meter_a", "voltage", kept_lt) != nullptr);
    SubjectLifetime voltage_lt;
    state.get_value_subject("meter_a", "voltage", voltage_lt);

    state.set_sensors({{"meter_a", "Meter A", "INA219", {"power"}}});

    CHECK(state.sensor_ids() == std::vector<std::string>{"meter_a"});
    CHECK(state.sensor_count() == 1);
    CHECK_FALSE(*gone_lt);
    CHECK_FALSE(*voltage_lt);
    SubjectLifetime lt;
    CHECK(state.get_value_subject("meter_b", "power", lt) == nullptr);
    CHECK(state.get_value_subject("meter_a", "voltage", lt) == nullptr);
    CHECK(state.get_value_subject("meter_a", "power", lt) == kept);
    CHECK_FALSE(state.get_sensor_info("meter_b").has_value());

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SensorState set_sensors applies initial values at once",
                 "[sensor_state]") {
    auto& state = SensorState::instance();
    state.set_sensors(kTwoMeters, {{"meter_a", {{"power", 15.2}, {"voltage", 230.1}}},
                                   {"meter_b", {{"power", "n/a"}}}});

    SubjectLifetime lt;
    CHECK(lv_subject_get_int(state.get_value_subject("meter_a", "power", lt)) == 1520);
    CHECK(lv_subject_get_int(state.get_value_subject("meter_a", "voltage", lt)) == 23010);
    CHECK(lv_subject_get_int(state.get_value_subject("meter_b", "power", lt)) == 0);

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SensorState is the last registry entry and ignores discover()",
                 "[sensor_state][sensor_manager_base]") {
    const auto& managers = helix::sensors::sensor_managers();
    REQUIRE_FALSE(managers.empty());
    CHECK(managers.back() == &SensorState::instance());

    auto& state = SensorState::instance();
    state.set_sensors(kTwoMeters);
    helix::PrinterDiscovery hw;
    state.discover(hw);
    CHECK(state.sensor_count() == 2);
    CHECK_FALSE(state.rediscovers_on_config());

    state.deinit_subjects();
}
