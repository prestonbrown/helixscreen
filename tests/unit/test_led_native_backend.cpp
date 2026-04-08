// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui_test_utils.h"
#include "config.h"
#include "led/led_controller.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

// ============================================================================
// Fixture for tests that need mock API + configurable LED strips
// ============================================================================

struct LedPinConfigFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedPinConfigFixture() {
        state.init_subjects(false);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    ~LedPinConfigFixture() {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
    }

    void setup_white_only_led(const std::string& strip_id = "led case_light",
                              const std::string& config_name = "case_light") {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);

        helix::led::LedStripInfo strip;
        strip.name = "Case Light";
        strip.id = strip_id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = false;
        strip.supports_white = false; // prefix-based guess
        strip.pin_config_known = false;
        ctrl.native().add_strip(strip);
        ctrl.set_selected_strips({strip_id});

        // Simulate configfile pin config update
        nlohmann::json config = {{config_name, {{"white_pin", "PA1"}}}};
        ctrl.native().update_pin_config(config);
    }

    void setup_rgbw_led(const std::string& strip_id = "led chamber_led",
                        const std::string& config_name = "chamber_led") {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);

        helix::led::LedStripInfo strip;
        strip.name = "Chamber LED";
        strip.id = strip_id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = false;
        strip.supports_white = false;
        strip.pin_config_known = false;
        ctrl.native().add_strip(strip);
        ctrl.set_selected_strips({strip_id});

        // Simulate configfile pin config update
        nlohmann::json config = {{config_name,
                                  {{"red_pin", "PA1"},
                                   {"green_pin", "PA2"},
                                   {"blue_pin", "PA3"},
                                   {"white_pin", "PA4"}}}};
        ctrl.native().update_pin_config(config);
    }
};

TEST_CASE("NativeBackend: set_color with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;
    // No API set

    bool error_called = false;
    std::string error_msg;
    backend.set_color("neopixel test", 1.0, 0.0, 0.0, 0.0, nullptr, [&](const std::string& err) {
        error_called = true;
        error_msg = err;
    });

    REQUIRE(error_called);
    REQUIRE(!error_msg.empty());
}

TEST_CASE("NativeBackend: turn_on with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.turn_on("neopixel test", nullptr, [&](const std::string&) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: turn_off with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.turn_off("neopixel test", nullptr, [&](const std::string&) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: set_brightness with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.set_brightness("neopixel test", 50, 1.0, 1.0, 1.0, 0.0, nullptr,
                           [&](const std::string&) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: null error callback with null API doesn't crash", "[led][native]") {
    helix::led::NativeBackend backend;

    // Should not crash even without callbacks
    backend.set_color("neopixel test", 1.0, 0.0, 0.0, 0.0, nullptr, nullptr);
    backend.turn_on("neopixel test", nullptr, nullptr);
    backend.turn_off("neopixel test", nullptr, nullptr);
    backend.set_brightness("neopixel test", 50, 1.0, 1.0, 1.0, 0.0, nullptr, nullptr);
}

TEST_CASE("NativeBackend: strip type detection", "[led][native]") {
    helix::led::NativeBackend backend;

    REQUIRE(backend.type() == helix::led::LedBackendType::NATIVE);
}

TEST_CASE("NativeBackend: update_from_status detects RGBW from 4-element color_data",
          "[led][native][rgbw]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Chamber";
    strip.id = "neopixel chamber";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true; // prefix-based default
    backend.add_strip(strip);

    // 4-element color_data confirms RGBW
    nlohmann::json status = {{"neopixel chamber", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}};
    backend.update_from_status(status);
    REQUIRE(backend.strips()[0].supports_white == true);

    // 3-element color_data overrides to RGB-only
    nlohmann::json status_rgb = {{"neopixel chamber", {{"color_data", {{1.0, 0.0, 0.0}}}}}};
    backend.update_from_status(status_rgb);
    REQUIRE(backend.strips()[0].supports_white == false);
}

TEST_CASE("NativeBackend: update_from_status corrects wrong RGBW guess", "[led][native][rgbw]") {
    helix::led::NativeBackend backend;

    // Neopixel guessed as RGBW but is actually RGB
    helix::led::LedStripInfo strip;
    strip.name = "Status";
    strip.id = "neopixel status_led";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true; // wrong guess
    backend.add_strip(strip);

    nlohmann::json status = {{"neopixel status_led", {{"color_data", {{0.5, 0.5, 0.5}}}}}};
    backend.update_from_status(status);
    REQUIRE(backend.strips()[0].supports_white == false);
}

// ============================================================================
// LED pin config detection from configfile (white-only vs RGBW)
// ============================================================================

TEST_CASE("NativeBackend: update_pin_config detects white-only LED", "[led][native][pin_config]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Case Light";
    strip.id = "led case_light";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = false; // initial guess
    strip.supports_white = false; // initial guess
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Configfile section with only white_pin
    nlohmann::json config = {{"case_light", {{"white_pin", "PA1"}}}};
    backend.update_pin_config(config);

    REQUIRE(backend.strips()[0].pin_config_known == true);
    REQUIRE(backend.strips()[0].has_red_pin == false);
    REQUIRE(backend.strips()[0].has_green_pin == false);
    REQUIRE(backend.strips()[0].has_blue_pin == false);
    REQUIRE(backend.strips()[0].has_white_pin == true);
    REQUIRE(backend.strips()[0].supports_color == false);
    REQUIRE(backend.strips()[0].supports_white == true);
}

TEST_CASE("NativeBackend: update_pin_config detects RGBW LED", "[led][native][pin_config]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Chamber LED";
    strip.id = "led chamber_led";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = false;
    strip.supports_white = false;
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Configfile section with all RGBW pins
    nlohmann::json config = {
        {"chamber_led",
         {{"red_pin", "PA1"}, {"green_pin", "PA2"}, {"blue_pin", "PA3"}, {"white_pin", "PA4"}}}};
    backend.update_pin_config(config);

    REQUIRE(backend.strips()[0].pin_config_known == true);
    REQUIRE(backend.strips()[0].has_red_pin == true);
    REQUIRE(backend.strips()[0].has_green_pin == true);
    REQUIRE(backend.strips()[0].has_blue_pin == true);
    REQUIRE(backend.strips()[0].has_white_pin == true);
    REQUIRE(backend.strips()[0].supports_color == true);
    REQUIRE(backend.strips()[0].supports_white == true);
}

TEST_CASE("NativeBackend: update_pin_config detects RGB-only LED", "[led][native][pin_config]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Status LED";
    strip.id = "led status_led";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = false;
    strip.supports_white = false;
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Configfile section with RGB pins only (no white)
    nlohmann::json config = {
        {"status_led", {{"red_pin", "PB0"}, {"green_pin", "PB1"}, {"blue_pin", "PB2"}}}};
    backend.update_pin_config(config);

    REQUIRE(backend.strips()[0].pin_config_known == true);
    REQUIRE(backend.strips()[0].has_white_pin == false);
    REQUIRE(backend.strips()[0].supports_color == true);
    REQUIRE(backend.strips()[0].supports_white == false);
}

TEST_CASE("NativeBackend: update_pin_config ignores unknown strips", "[led][native][pin_config]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Case Light";
    strip.id = "led case_light";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = false;
    strip.supports_white = false;
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Configfile section for a different LED
    nlohmann::json config = {{"other_led", {{"white_pin", "PA1"}}}};
    backend.update_pin_config(config);

    // Should remain unknown
    REQUIRE(backend.strips()[0].pin_config_known == false);
}

TEST_CASE("NativeBackend: set_color converts RGB to white for white-only LED",
          "[led][native][white_only]") {
    // Use fixture to provide API so set_color doesn't return early
    LedPinConfigFixture fixture;
    fixture.setup_white_only_led();

    auto& backend = helix::led::LedController::instance().native();

    // set_color should convert RGB luminance to white channel
    bool success_called = false;
    backend.set_color(
        "led case_light", 0.8, 0.8, 0.8, 0.0, [&]() { success_called = true; }, nullptr);

    // Should succeed because fixture provides API
    REQUIRE(success_called);

    // Cached color should have RGB=0 and W=luminance
    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    // Luminance of (0.8, 0.8, 0.8) = 0.299*0.8 + 0.587*0.8 + 0.114*0.8 = 0.8
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE("NativeBackend: set_color keeps RGB for RGBW LED with known pins",
          "[led][native][rgbw]") {
    // Use fixture to provide API
    LedPinConfigFixture fixture;
    fixture.setup_rgbw_led();

    auto& backend = helix::led::LedController::instance().native();

    // set_color should preserve RGBW values
    bool success_called = false;
    backend.set_color(
        "led chamber_led", 1.0, 0.5, 0.0, 0.8, [&]() { success_called = true; }, nullptr);

    // Should succeed because fixture provides API
    REQUIRE(success_called);

    // Cached color should have original RGBW values
    auto color = backend.get_strip_color("led chamber_led");
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.5).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE("NativeBackend: set_color falls back to prefix detection when pin_config unknown",
          "[led][native][fallback]") {
    // Use fixture to provide API so set_color doesn't return early
    LedPinConfigFixture fixture;
    fixture.setup_white_only_led();

    auto& backend = helix::led::LedController::instance().native();

    // Manually reset pin_config_known to test fallback behavior
    auto& strips = const_cast<std::vector<helix::led::LedStripInfo>&>(backend.strips());
    for (auto& strip : strips) {
        strip.pin_config_known = false;
    }

    // set_color should use prefix-based detection (led = white-only)
    bool success_called = false;
    backend.set_color(
        "led case_light", 0.6, 0.6, 0.6, 0.0, [&]() { success_called = true; }, nullptr);

    REQUIRE(success_called);

    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.6).margin(0.001));
}

// Note: Prefix-based fallback for neopixel is tested via integration tests
// in the [led][controller][pin_config] section below.

// ============================================================================
// Integration: LedController with mock API — pin config + color commands
// ============================================================================

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: set_color_all sends white-only for white-only LED",
                 "[led][controller][pin_config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    // Set color via set_color_all (what LED auto-state uses)
    ctrl.set_color_all(0.8, 0.8, 0.8, 0.0);

    // Cached color should have RGB=0, W=0.8
    auto color = ctrl.native().get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture, "LedController: set_color_all preserves RGBW for RGBW LED",
                 "[led][controller][pin_config]") {
    setup_rgbw_led();
    auto& ctrl = helix::led::LedController::instance();

    // Set color with all channels
    ctrl.set_color_all(1.0, 0.5, 0.0, 0.8);

    // All channels should be preserved
    auto color = ctrl.native().get_strip_color("led chamber_led");
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.5).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: toggle_all uses white channel for white-only LED",
                 "[led][controller][pin_config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    // Set initial color + brightness
    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_last_brightness(80);

    // Toggle on
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    // Cached color should have W=0.8, RGB=0
    auto color = ctrl.native().get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: set_startup_brightness updates last_brightness immediately",
                 "[led][controller][config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    // Initial defaults
    REQUIRE(ctrl.get_startup_brightness() == 80);
    REQUIRE(ctrl.last_brightness() == 100); // default from deinit

    // Change startup brightness
    ctrl.set_startup_brightness(50);
    REQUIRE(ctrl.get_startup_brightness() == 50);
    REQUIRE(ctrl.last_brightness() == 50); // should also update last_brightness

    // Now toggle should use new brightness
    ctrl.set_last_color(0xFFFFFF);
    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("led case_light");
    REQUIRE(color.w == Catch::Approx(0.5).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: load_config reads printer-specific startup_brightness",
                 "[led][controller][config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    // Clear any stale config and write to printer-specific config path
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    cfg->set(cfg->df() + "leds/startup_brightness", 60);
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array({"led case_light"}));
    cfg->save();

    // Reload config
    ctrl.load_config();

    // Should have loaded the value
    REQUIRE(ctrl.get_startup_brightness() == 60);
}
