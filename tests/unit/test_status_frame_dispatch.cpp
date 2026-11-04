// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "led/led_controller.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "status_dispatch.h"
#include "temperature_sensor_manager.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

struct DispatchFixture : LVGLTestFixture {
    ~DispatchFixture() override {
        helix::led::LedController::instance().deinit();
        helix::ui::UpdateQueue::instance().drain_until_empty();
        auto* cfg = Config::get_instance();
        cfg->set(cfg->df() + "leds/auto_state/strips", nlohmann::json());
        cfg->set(cfg->df() + "leds/light_button_pending", nlohmann::json());
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
        helix::sensors::TemperatureSensorManager::instance().discover({});
    }
};

} // namespace

// Every consumer of a status frame sees it: PrinterState's domains, the LED
// controller's strip-colour cache and the sensor managers.
TEST_CASE_METHOD(DispatchFixture, "dispatch_status_frame reaches every status consumer",
                 "[printer_state][dispatch]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);

    PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"neopixel chamber"}));
    auto& led = helix::led::LedController::instance();
    led.deinit();
    led.init(nullptr, nullptr);
    led.discover_from_hardware(discovery);

    auto& sensors = helix::sensors::TemperatureSensorManager::instance();
    sensors.init_subjects();
    sensors.discover(helix::test::discovery_of({"temperature_sensor cabinet"}));

    const nlohmann::json status = {
        {"heater_bed", {{"temperature", 61.5}}},
        {"neopixel chamber", {{"color_data", nlohmann::json::array({{0.2, 0.4, 0.6, 0.0}})}}},
        {"temperature_sensor cabinet", {{"temperature", 31.5}}}};
    StatusFrame frame;
    frame.status = &status;
    frame.eventtime = 10.0;

    dispatch_status_frame(frame, ps.network_state().klippy_epoch());
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(lv_subject_get_int(ps.temperature_state().get_bed_temp_subject()) == 615);

    auto color = led.native().get_strip_color("neopixel chamber");
    CHECK(color.r == Catch::Approx(0.2));
    CHECK(color.g == Catch::Approx(0.4));
    CHECK(color.b == Catch::Approx(0.6));

    auto cabinet = sensors.get_sensor_state("temperature_sensor cabinet");
    REQUIRE(cabinet.has_value());
    CHECK(cabinet->temperature == Catch::Approx(31.5f));
}

// A query response carries no notification, so it must reach the same consumers
// a notification does: the LED colour cache reads back STOP_LED_EFFECTS and a
// same-colour SET_LED only through this path.
TEST_CASE_METHOD(DispatchFixture, "an LED query response reaches the LED colour cache",
                 "[printer_state][dispatch][led]") {
    get_printer_state().init_subjects(false);

    PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"neopixel chamber"}));
    auto& led = helix::led::LedController::instance();
    led.deinit();
    led.init(nullptr, nullptr);
    led.discover_from_hardware(discovery);

    helix::led::LedController::apply_query_response(
        nlohmann::json{{"result",
                        {{"status",
                          {{"neopixel chamber",
                            {{"color_data", nlohmann::json::array({{0.1, 0.5, 0.9, 0.0}})}}}}}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();

    auto color = led.native().get_strip_color("neopixel chamber");
    CHECK(color.r == Catch::Approx(0.1));
    CHECK(color.g == Catch::Approx(0.5));
    CHECK(color.b == Catch::Approx(0.9));
}
