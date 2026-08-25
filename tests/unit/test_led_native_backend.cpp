// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
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

struct LedPinConfigFixture : public HelixTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedPinConfigFixture() {
        state.init_subjects(false);
        // execute_gcode() halted gate would otherwise reject all G-code: subjects
        // initialize to SHUTDOWN until production code observes a real state update.
        state.set_klippy_state_sync(helix::KlippyState::READY);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    ~LedPinConfigFixture() {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
    }

    void setup_white_only_led(const std::string& strip_id = "led case_light",
                              const std::string& config_name = "led case_light") {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);

        helix::led::LedStripInfo strip;
        strip.name = "Case Light";
        strip.id = strip_id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = false;
        strip.supports_white = false;
        strip.pin_config_known = false;
        ctrl.native().add_strip(strip);
        ctrl.set_selected_strips({strip_id});

        // Simulate configfile pin config update
        nlohmann::json config = {{config_name, {{"white_pin", "PA1"}}}};
        ctrl.native().update_pin_config(config);
    }

    void setup_rgbw_led(const std::string& strip_id = "led chamber_led",
                        const std::string& config_name = "led chamber_led") {
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
    backend.turn_on("neopixel test", nullptr, [&](const std::string& err) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: turn_off with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.turn_off("neopixel test", nullptr,
                     [&](const std::string& err) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: set_brightness with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.set_brightness("neopixel test", 50, 1.0, 1.0, 1.0, 0.0, nullptr,
                           [&](const std::string& err) { error_called = true; });

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
    strip.supports_color = false;
    strip.supports_white = false;
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Configfile uses full section header as key (e.g., "led case_light")
    nlohmann::json config = {{"led case_light", {{"white_pin", "PA1"}}}};
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

    // Configfile uses full section header as key
    nlohmann::json config = {
        {"led chamber_led",
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

    // Configfile uses full section header as key
    nlohmann::json config = {
        {"led status_led", {{"red_pin", "PB0"}, {"green_pin", "PB1"}, {"blue_pin", "PB2"}}}};
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

    // Configfile section for a different LED (won't match "led case_light")
    nlohmann::json config = {{"led other_led", {{"white_pin", "PA1"}}}};
    backend.update_pin_config(config);

    // Should remain unknown
    REQUIRE(backend.strips()[0].pin_config_known == false);
}

TEST_CASE("NativeBackend: set_color converts RGB to white for white-only LED",
          "[led][native][white_only]") {
    LedPinConfigFixture fixture;
    fixture.setup_white_only_led();

    auto& backend = helix::led::LedController::instance().native();

    bool success_called = false;
    backend.set_color(
        "led case_light", 0.8, 0.8, 0.8, 0.0, [&]() { success_called = true; }, nullptr);

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
    LedPinConfigFixture fixture;
    fixture.setup_rgbw_led();

    auto& backend = helix::led::LedController::instance().native();

    bool success_called = false;
    backend.set_color(
        "led chamber_led", 1.0, 0.5, 0.0, 0.8, [&]() { success_called = true; }, nullptr);

    REQUIRE(success_called);

    auto color = backend.get_strip_color("led chamber_led");
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.5).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.001));
}

TEST_CASE("NativeBackend: set_color falls back to prefix detection when pin_config unknown",
          "[led][native][fallback]") {
    LedPinConfigFixture fixture;
    fixture.setup_white_only_led();

    auto& backend = helix::led::LedController::instance().native();

    // Reset pin_config_known to test fallback behavior
    auto& strips = const_cast<std::vector<helix::led::LedStripInfo>&>(backend.strips());
    for (auto& strip : strips) {
        strip.pin_config_known = false;
    }

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

// ============================================================================
// Integration: LedController with mock API — pin config + color commands
// ============================================================================

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: set_color_all sends white-only for white-only LED",
                 "[led][controller][pin_config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_color_all(0.8, 0.8, 0.8, 0.0);

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

    ctrl.set_color_all(1.0, 0.5, 0.0, 0.8);

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

    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_last_brightness(80);

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

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

    REQUIRE(ctrl.get_startup_brightness() == 80);

    ctrl.set_startup_brightness(50);
    REQUIRE(ctrl.get_startup_brightness() == 50);
    REQUIRE(ctrl.last_brightness() == 50);

    ctrl.set_last_color(0xFFFFFF);
    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("led case_light");
    REQUIRE(color.w == Catch::Approx(0.5).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "NativeBackend: turn_on on white-only strip sends white channel via set_color",
                 "[led][native][white_only]") {
    setup_white_only_led();
    auto& backend = helix::led::LedController::instance().native();

    // Pre-cache a color so turn_on restores it
    backend.set_color("led case_light", 0.7, 0.7, 0.7, 0.0, nullptr, nullptr);

    bool success_called = false;
    backend.turn_on("led case_light", [&]() { success_called = true; }, nullptr);
    REQUIRE(success_called);

    // turn_on should route through set_color, applying white-only conversion
    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.7).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "NativeBackend: turn_on with no cached color defaults to full white",
                 "[led][native][white_only]") {
    // Use a unique strip ID so no prior test has cached a color for it
    setup_white_only_led("led fresh_light", "led fresh_light");
    auto& backend = helix::led::LedController::instance().native();

    // No prior set_color — turn_on should use default {1,1,1,0}
    bool success_called = false;
    backend.turn_on("led fresh_light", [&]() { success_called = true; }, nullptr);
    REQUIRE(success_called);

    // Default (1,1,1,0) → luminance=1.0 → W=1.0, RGB=0
    auto color = backend.get_strip_color("led fresh_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(1.0).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture, "NativeBackend: turn_off on white-only strip sends all zeros",
                 "[led][native][white_only]") {
    setup_white_only_led();
    auto& backend = helix::led::LedController::instance().native();

    // Set a color first
    backend.set_color("led case_light", 0.5, 0.5, 0.5, 0.0, nullptr, nullptr);

    bool success_called = false;
    backend.turn_off("led case_light", [&]() { success_called = true; }, nullptr);
    REQUIRE(success_called);

    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "NativeBackend: set_brightness on white-only strip scales white channel",
                 "[led][native][white_only]") {
    setup_white_only_led();
    auto& backend = helix::led::LedController::instance().native();

    // set_brightness applies scale then routes through set_color
    bool success_called = false;
    backend.set_brightness(
        "led case_light", 50, 1.0, 1.0, 1.0, 0.0, [&]() { success_called = true; }, nullptr);
    REQUIRE(success_called);

    // 50% of (1,1,1) = (0.5,0.5,0.5) → luminance=0.5 → W=0.5, RGB=0
    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.5).margin(0.001));
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "NativeBackend: set_color preserves explicit W on white-only strip",
                 "[led][native][white_only]") {
    setup_white_only_led();
    auto& backend = helix::led::LedController::instance().native();

    // Pass W=0.9 with low RGB — W should win over computed luminance
    bool success_called = false;
    backend.set_color(
        "led case_light", 0.2, 0.2, 0.2, 0.9, [&]() { success_called = true; }, nullptr);
    REQUIRE(success_called);

    // Luminance of (0.2,0.2,0.2) = 0.2, but explicit W=0.9 is higher → W=0.9
    auto color = backend.get_strip_color("led case_light");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.001));
    REQUIRE(color.w == Catch::Approx(0.9).margin(0.001));
}

TEST_CASE("NativeBackend: update_pin_config works for neopixel with configfile data",
          "[led][native][pin_config]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Chamber Neopixel";
    strip.id = "neopixel chamber";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true; // prefix-based guess
    strip.pin_config_known = false;
    backend.add_strip(strip);

    // Neopixel with configfile data confirming RGB-only (no white)
    nlohmann::json config = {
        {"neopixel chamber", {{"red_pin", "PA1"}, {"green_pin", "PA2"}, {"blue_pin", "PA3"}}}};
    backend.update_pin_config(config);

    REQUIRE(backend.strips()[0].pin_config_known == true);
    REQUIRE(backend.strips()[0].supports_color == true);
    // Configfile overrides the prefix-based guess that neopixel has W channel
    REQUIRE(backend.strips()[0].supports_white == false);
    REQUIRE(backend.strips()[0].has_white_pin == false);
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "LedController: load_config reads printer-specific startup_brightness",
                 "[led][controller][config]") {
    setup_white_only_led();
    auto& ctrl = helix::led::LedController::instance();

    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    cfg->set(cfg->df() + "leds/startup_brightness", 60);
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array({"led case_light"}));
    cfg->save();

    ctrl.load_config();

    REQUIRE(ctrl.get_startup_brightness() == 60);
}

// ============================================================================
// StripColor::decompose — the W channel carries the level on white-only strips
//
// Klipper's white-only `[led main_led]` (white_pin only, no RGB pins) reports
// its level in the 4th color_data slot: [[0.0, 0.0, 0.0, 0.15]].  Excluding W
// from the brightness max reads that back as 0%, and re-applying 0% then wrote
// the user's light OFF (#1129).
// ============================================================================

TEST_CASE("StripColor::decompose: white-only level comes from the W channel",
          "[led][native][decompose][white_only]") {
    helix::led::NativeBackend::StripColor color;
    color.r = 0.0;
    color.g = 0.0;
    color.b = 0.0;
    color.w = 0.15;

    uint32_t base_color = 0;
    int brightness = 0;
    double base_white = 0.0;
    color.decompose(base_color, brightness, base_white);

    REQUIRE(brightness == 15);
    // Base color is the full-brightness form; W scaled to full is 1.0.
    REQUIRE(base_white == Catch::Approx(1.0).margin(0.01));
    // A white-only strip has no meaningful hue — report neutral white so the
    // swatch shows dimmed white rather than black.
    REQUIRE(base_color == 0xFFFFFFu);
}

TEST_CASE("StripColor::decompose: full white RGBW reads as 100%",
          "[led][native][decompose][white_only]") {
    helix::led::NativeBackend::StripColor color;
    color.w = 1.0;

    uint32_t base_color = 0;
    int brightness = 0;
    double base_white = 0.0;
    color.decompose(base_color, brightness, base_white);

    REQUIRE(brightness == 100);
    REQUIRE(base_white == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("StripColor::decompose: RGB-only decomposition is unchanged",
          "[led][native][decompose]") {
    SECTION("saturated color at full brightness") {
        helix::led::NativeBackend::StripColor color;
        color.r = 1.0;
        color.g = 0.5;
        color.b = 0.0;
        color.w = 0.0;

        uint32_t base_color = 0;
        int brightness = 0;
        double base_white = 0.0;
        color.decompose(base_color, brightness, base_white);

        REQUIRE(brightness == 100);
        REQUIRE(base_color == 0xFF8000u);
        REQUIRE(base_white == Catch::Approx(0.0).margin(0.001));
    }

    SECTION("dimmed color scales back up to a full-brightness base") {
        helix::led::NativeBackend::StripColor color;
        color.r = 0.5;
        color.g = 0.25;
        color.b = 0.0;
        color.w = 0.0;

        uint32_t base_color = 0;
        int brightness = 0;
        double base_white = 0.0;
        color.decompose(base_color, brightness, base_white);

        REQUIRE(brightness == 50);
        REQUIRE(base_color == 0xFF7F00u);
        REQUIRE(base_white == Catch::Approx(0.0).margin(0.001));
    }

    SECTION("black decomposes to zero brightness") {
        helix::led::NativeBackend::StripColor color;

        uint32_t base_color = 0xABCDEF;
        int brightness = 42;
        double base_white = 1.0;
        color.decompose(base_color, brightness, base_white);

        REQUIRE(brightness == 0);
        REQUIRE(base_color == 0x000000u);
        REQUIRE(base_white == Catch::Approx(0.0).margin(0.001));
    }
}

TEST_CASE_METHOD(LedPinConfigFixture,
                 "NativeBackend: white-only status round-trip does not turn the light off",
                 "[led][native][decompose][white_only]") {
    setup_white_only_led();
    auto& backend = helix::led::LedController::instance().native();

    // Exactly what a Klipper white-only [led] reports at 15%
    nlohmann::json status = {{"led case_light", {{"color_data", {{0.0, 0.0, 0.0, 0.15}}}}}};
    backend.update_from_status(status);

    auto reported = backend.get_strip_color("led case_light");
    REQUIRE(reported.w == Catch::Approx(0.15).margin(0.001));

    uint32_t base_color = 0;
    int brightness = 0;
    double base_white = 0.0;
    reported.decompose(base_color, brightness, base_white);
    REQUIRE(brightness == 15);

    // decompose()'s documented contract: re-applying base_white at brightness_pct
    // reproduces the level that was reported. That round-trip is what the overlay's
    // white path rests on. The overlay half of #1129 — that the W channel is scaled
    // rather than sent as a literal 0.0, which switched the user's light off — is
    // asserted against apply_current_color() itself in test_led_control_overlay.cpp
    // ("dimming a white-only strip scales W instead of zeroing it").
    backend.set_color("led case_light", 0.0, 0.0, 0.0,
                      base_white * static_cast<double>(brightness) / 100.0);

    auto sent = backend.get_strip_color("led case_light");
    REQUIRE(sent.w > 0.0);
    REQUIRE(sent.w == Catch::Approx(0.15).margin(0.01));
}

// ============================================================================
// The two color_data parsers (NativeBackend + PrinterLedState) must agree
// ============================================================================

TEST_CASE_METHOD(LedPinConfigFixture,
                 "color_data parsers agree on brightness between NativeBackend and PrinterLedState",
                 "[led][native][decompose][parser_parity]") {
    setup_white_only_led("led parity_light", "led parity_light");
    auto& backend = helix::led::LedController::instance().native();

    state.set_tracked_led("led parity_light");

    struct Payload {
        const char* label;
        double r, g, b, w;
    };
    const Payload payloads[] = {
        {"white-only at 15%", 0.0, 0.0, 0.0, 0.15}, {"white-only at full", 0.0, 0.0, 0.0, 1.0},
        {"mixed RGB", 0.5, 0.25, 0.75, 0.0},        {"saturated red", 1.0, 0.0, 0.0, 0.0},
        {"all channels off", 0.0, 0.0, 0.0, 0.0},
    };

    for (const auto& p : payloads) {
        INFO("payload: " << p.label);
        nlohmann::json status = {{"led parity_light", {{"color_data", {{p.r, p.g, p.b, p.w}}}}}};

        backend.update_from_status(status);
        state.update_from_status(status);

        uint32_t base_color = 0;
        int native_brightness = 0;
        double base_white = 0.0;
        backend.get_strip_color("led parity_light")
            .decompose(base_color, native_brightness, base_white);

        int printer_brightness = lv_subject_get_int(state.get_led_brightness_subject());
        REQUIRE(native_brightness == printer_brightness);
    }
}
