// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "config.h"
#include "led/led_controller.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

/// Same fixture-less leak as the rest of the LED files: the discovery tests that
/// drive LedController::init() leave its deferred connection-state notification
/// queued for whichever test drains next (prestonbrown/helixscreen#1167). The
/// drain runs while the controller and its subjects are still alive, before
/// HelixTestFixture's own teardown.
struct LedDiscoveryFixture : public HelixTestFixture {
    ~LedDiscoveryFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

TEST_CASE_METHOD(LedDiscoveryFixture, "PrinterDiscovery detects led_effect objects",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects =
        nlohmann::json::array({"led_effect breathing", "led_effect fire_comet",
                               "led_effect rainbow", "neopixel chamber_light", "extruder"});
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_led_effects());
    REQUIRE(discovery.led_effects().size() == 3);
    REQUIRE(discovery.led_effects()[0] == "led_effect breathing");
    REQUIRE(discovery.led_effects()[1] == "led_effect fire_comet");
    REQUIRE(discovery.led_effects()[2] == "led_effect rainbow");

    // Verify native LEDs still detected
    REQUIRE(discovery.has_led());
    REQUIRE(discovery.leds().size() == 1);
    REQUIRE(discovery.leds()[0] == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedDiscoveryFixture,
                 "PrinterDiscovery: led_effect does not get caught by led prefix",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"led_effect status_effect", "led case_light"});
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_led_effects());
    REQUIRE(discovery.led_effects().size() == 1);
    REQUIRE(discovery.led_effects()[0] == "led_effect status_effect");

    // "led case_light" should be in leds, not effects
    REQUIRE(discovery.has_led());
    REQUIRE(discovery.leds().size() == 1);
    REQUIRE(discovery.leds()[0] == "led case_light");
}

TEST_CASE_METHOD(LedDiscoveryFixture, "PrinterDiscovery detects LED-related macros",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array(
        {"gcode_macro LIGHTS_ON", "gcode_macro LIGHTS_OFF", "gcode_macro LED_PARTY",
         "gcode_macro LAMP_TOGGLE", "gcode_macro BACKLIGHT_SET", "gcode_macro PRINT_START",
         "gcode_macro PRINT_END", "gcode_macro M600", "gcode_macro BED_MESH_CALIBRATE",
         "gcode_macro HOME_ALL"});
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_led_macros());
    auto& led_macros = discovery.led_macros();

    // Should include LED-related macros
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "LIGHTS_ON") != led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "LIGHTS_OFF") != led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "LED_PARTY") != led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "LAMP_TOGGLE") != led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "BACKLIGHT_SET") != led_macros.end());

    // Should NOT include excluded macros
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "PRINT_START") == led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "PRINT_END") == led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "M600") == led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "BED_MESH_CALIBRATE") ==
            led_macros.end());
    REQUIRE(std::find(led_macros.begin(), led_macros.end(), "HOME_ALL") == led_macros.end());
}

TEST_CASE_METHOD(LedDiscoveryFixture, "PrinterDiscovery: non-LED macros not detected",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array(
        {"gcode_macro PARK_TOOLHEAD", "gcode_macro SET_VELOCITY", "gcode_macro START_PRINT"});
    discovery.parse_objects(objects);

    REQUIRE(!discovery.has_led_macros());
    REQUIRE(discovery.led_macros().empty());
}

TEST_CASE_METHOD(LedDiscoveryFixture,
                 "LedController discover_from_hardware with effects and macros",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array(
        {"neopixel chamber_light", "led_effect breathing", "led_effect fire_comet",
         "gcode_macro LIGHTS_ON", "gcode_macro LIGHTS_OFF", "gcode_macro LED_PARTY"});
    discovery.parse_objects(objects);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();

    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(discovery);

    // Native backend
    REQUIRE(ctrl.native().is_available());
    REQUIRE(ctrl.native().strips().size() == 1);

    // Effects backend
    REQUIRE(ctrl.effects().is_available());
    REQUIRE(ctrl.effects().effects().size() == 2);
    REQUIRE(ctrl.effects().effects()[0].display_name == "Breathing");
    REQUIRE(ctrl.effects().effects()[0].icon_hint == "air");
    REQUIRE(ctrl.effects().effects()[1].display_name == "Fire Comet");

    // Discovered macros stored as candidates (for UI dropdown)
    REQUIRE(ctrl.discovered_macros().size() == 3);
    REQUIRE(std::find(ctrl.discovered_macros().begin(), ctrl.discovered_macros().end(),
                      "LIGHTS_ON") != ctrl.discovered_macros().end());

    // No auto-creation of macro devices — macros are user-configured only
    REQUIRE(ctrl.macro().macros().size() == 0);
    REQUIRE(ctrl.macro().is_available() == false);

    // Only native + effects backends available (no macro backend)
    auto backends = ctrl.available_backends();
    REQUIRE(backends.size() == 2);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedDiscoveryFixture, "PrinterDiscovery clear resets LED effects and macros",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"led_effect test", "gcode_macro LIGHTS_ON"});
    discovery.parse_objects(objects);
    REQUIRE(discovery.has_led_effects());
    REQUIRE(discovery.has_led_macros());

    discovery.clear();
    REQUIRE(!discovery.has_led_effects());
    REQUIRE(!discovery.has_led_macros());
    REQUIRE(discovery.led_effects().empty());
    REQUIRE(discovery.led_macros().empty());
}

// ============================================================================
// Exclusion matching
//
// The keyword test is deliberately a substring match so it catches LIGHTS,
// LIGHTING and ILLUMINATE. The exclusion list must NOT be, or it eats real LED
// macros whose names merely contain an exclusion as a fragment.
// ============================================================================

TEST_CASE_METHOD(LedDiscoveryFixture,
                 "PrinterDiscovery: LED exclusions match whole words, not fragments",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({
        "gcode_macro LED_RAPID_FLASH",     // "RAPID" contains "PID"
        "gcode_macro LED_HOMED",           // "HOMED" contains "HOME"
        "gcode_macro BACKLIGHT_CANCELLED", // "CANCELLED" contains "CANCEL"
        "gcode_macro LED_PROBED",          // "PROBED" contains "PROBE"
    });
    discovery.parse_objects(objects);

    auto& m = discovery.led_macros();
    REQUIRE(discovery.has_led_macros());
    CHECK(std::find(m.begin(), m.end(), "LED_RAPID_FLASH") != m.end());
    CHECK(std::find(m.begin(), m.end(), "LED_HOMED") != m.end());
    CHECK(std::find(m.begin(), m.end(), "BACKLIGHT_CANCELLED") != m.end());
    CHECK(std::find(m.begin(), m.end(), "LED_PROBED") != m.end());
    REQUIRE(m.size() == 4);
}

TEST_CASE_METHOD(LedDiscoveryFixture,
                 "PrinterDiscovery: LED exclusions still fire on a real word match",
                 "[led][discovery]") {
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({
        "gcode_macro LED_PAUSE",     // trailing whole word
        "gcode_macro LIGHT_Z_TILT",  // multi-token exclusion, trailing
        "gcode_macro PROBE_LED_DIM", // leading whole word
        "gcode_macro LED_CALIBRATE", // trailing whole word
        "gcode_macro LED_PARTY",     // control: nothing to exclude
    });
    discovery.parse_objects(objects);

    auto& m = discovery.led_macros();
    CHECK(std::find(m.begin(), m.end(), "LED_PAUSE") == m.end());
    CHECK(std::find(m.begin(), m.end(), "LIGHT_Z_TILT") == m.end());
    CHECK(std::find(m.begin(), m.end(), "PROBE_LED_DIM") == m.end());
    CHECK(std::find(m.begin(), m.end(), "LED_CALIBRATE") == m.end());
    REQUIRE(m.size() == 1);
    REQUIRE(m[0] == "LED_PARTY");
}
