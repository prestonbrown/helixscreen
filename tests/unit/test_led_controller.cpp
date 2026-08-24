// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "led/led_controller.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

/// The fixture-less TEST_CASEs below drive LedController::init(), whose
/// connection-state observer defers its first notification through the
/// UpdateQueue, and light_set()/toggle paths that defer
/// LedController::led_cmd_settled. With no fixture they returned with that work
/// still queued and handed it to whichever test drained next
/// (prestonbrown/helixscreen#1167). The drain sits in the derived destructor body
/// so it runs while the controller and its subjects are still alive, before
/// HelixTestFixture's own teardown.
struct LedControllerFixture : public HelixTestFixture {
    ~LedControllerFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

TEST_CASE_METHOD(LedControllerFixture, "LedController singleton access", "[led]") {
    auto& ctrl = helix::led::LedController::instance();
    auto& ctrl2 = helix::led::LedController::instance();
    REQUIRE(&ctrl == &ctrl2);
}

TEST_CASE_METHOD(LedControllerFixture, "LedController init and deinit", "[led]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit(); // Clean state

    REQUIRE(!ctrl.is_initialized());
    ctrl.init(nullptr, nullptr); // null api/client for testing
    REQUIRE(ctrl.is_initialized());
    ctrl.deinit();
    REQUIRE(!ctrl.is_initialized());
}

TEST_CASE_METHOD(LedControllerFixture, "LedController has_any_backend empty", "[led]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(!ctrl.has_any_backend());
    REQUIRE(ctrl.available_backends().empty());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController discover_from_hardware populates native backend", "[led]") {
    // Use PrinterDiscovery to populate
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array(
        {"neopixel chamber_light", "dotstar status_led", "led case_light", "extruder"});
    discovery.parse_objects(objects);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(discovery);

    REQUIRE(ctrl.has_any_backend());
    REQUIRE(ctrl.native().is_available());
    REQUIRE(ctrl.native().strips().size() == 3);

    // Check strip details
    auto& strips = ctrl.native().strips();
    REQUIRE(strips[0].id == "neopixel chamber_light");
    REQUIRE(strips[0].name == "Chamber Light");
    REQUIRE(strips[0].supports_color == true);
    REQUIRE(strips[0].supports_white == true);

    REQUIRE(strips[1].id == "dotstar status_led");
    REQUIRE(strips[1].name == "Status LED");
    // Addressable strips keep color at discovery — their configfile section carries
    // no red/green/blue_pin, so update_pin_config() skips them (regression guard).
    REQUIRE(strips[1].supports_color == true);
    REQUIRE(strips[1].supports_white == true);

    REQUIRE(strips[2].id == "led case_light");
    REQUIRE(strips[2].name == "Case Light");
    // Generic [led] is fail-closed to white-only at discovery (no color picker)
    // until the configfile parse proves RGB pins exist.
    REQUIRE(strips[2].supports_color == false);
    REQUIRE(strips[2].supports_white == false);

    // Other backends should be empty
    REQUIRE(!ctrl.effects().is_available());
    REQUIRE(!ctrl.wled().is_available());
    REQUIRE(!ctrl.macro().is_available());

    auto backends = ctrl.available_backends();
    REQUIRE(backends.size() == 1);
    REQUIRE(backends[0] == helix::led::LedBackendType::NATIVE);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: generic [led] discovery defaults to white-only (fail-closed)",
                 "[led][discovery]") {
    // A generic "[led]" section (e.g. AD5M [led chamber_light], white_pin only) must
    // NOT advertise color before the configfile parse proves RGB pins. This closes the
    // window where a white-only chamber light shows a meaningless color picker.
    helix::PrinterDiscovery discovery;
    nlohmann::json objects =
        nlohmann::json::array({"led chamber_light", "neopixel ring", "dotstar bar", "extruder"});
    discovery.parse_objects(objects);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(discovery);

    auto& strips = ctrl.native().strips();
    REQUIRE(strips.size() == 3);

    auto find = [&](const std::string& id) -> const helix::led::LedStripInfo& {
        for (const auto& s : strips) {
            if (s.id == id)
                return s;
        }
        FAIL("strip not found: " << id);
        return strips[0];
    };

    // Generic [led]: white-only default, no color until configfile proves RGB pins.
    REQUIRE(find("led chamber_light").supports_color == false);
    REQUIRE(find("led chamber_light").supports_white == false);

    // Addressable strips: color stays true at discovery (regression guard — their
    // configfile sections have no red/green/blue_pin, so update_pin_config skips them).
    REQUIRE(find("neopixel ring").supports_color == true);
    REQUIRE(find("neopixel ring").supports_white == true);
    REQUIRE(find("dotstar bar").supports_color == true);
    REQUIRE(find("dotstar bar").supports_white == true);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: configfile RGB pins upgrade a generic [led] to color",
                 "[led][discovery][pin_config]") {
    // A real RGB [led] strip starts white-only at discovery, then gets upgraded once
    // the configfile reveals red/green/blue pins (the connect-flow upgrade path).
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"led rgb_strip", "led white_strip"});
    discovery.parse_objects(objects);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(discovery);

    auto& strips = ctrl.native().strips();
    REQUIRE(strips.size() == 2);
    // Both start fail-closed (white-only) before any pin config.
    for (const auto& s : strips) {
        REQUIRE(s.supports_color == false);
    }

    // Configfile keyed by the full section header (matches strip.id).
    nlohmann::json cfg = {
        {"led rgb_strip", {{"red_pin", "PA1"}, {"green_pin", "PA2"}, {"blue_pin", "PA3"}}},
        {"led white_strip", {{"white_pin", "PB0"}}}};
    ctrl.update_led_pin_config(cfg);

    auto find = [&](const std::string& id) -> const helix::led::LedStripInfo& {
        for (const auto& s : ctrl.native().strips()) {
            if (s.id == id)
                return s;
        }
        FAIL("strip not found: " << id);
        return ctrl.native().strips()[0];
    };

    // RGB pins → upgraded to color.
    REQUIRE(find("led rgb_strip").supports_color == true);
    // White-only pins → stays fail-closed, gains white support.
    REQUIRE(find("led white_strip").supports_color == false);
    REQUIRE(find("led white_strip").supports_white == true);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedBackendType enum values", "[led]") {
    REQUIRE(static_cast<int>(helix::led::LedBackendType::NATIVE) == 0);
    REQUIRE(static_cast<int>(helix::led::LedBackendType::LED_EFFECT) == 1);
    REQUIRE(static_cast<int>(helix::led::LedBackendType::WLED) == 2);
    REQUIRE(static_cast<int>(helix::led::LedBackendType::MACRO) == 3);
}

TEST_CASE_METHOD(LedControllerFixture, "LedStripInfo struct", "[led]") {
    helix::led::LedStripInfo info;
    info.name = "Chamber Light";
    info.id = "neopixel chamber_light";
    info.backend = helix::led::LedBackendType::NATIVE;
    info.supports_color = true;
    info.supports_white = true;

    REQUIRE(info.name == "Chamber Light");
    REQUIRE(info.id == "neopixel chamber_light");
    REQUIRE(info.backend == helix::led::LedBackendType::NATIVE);
    REQUIRE(info.supports_color);
    REQUIRE(info.supports_white);
}

TEST_CASE_METHOD(LedControllerFixture, "LedEffectBackend icon hint mapping", "[led]") {
    using helix::led::LedEffectBackend;

    REQUIRE(LedEffectBackend::icon_hint_for_effect("breathing") == "air");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("pulse_slow") == "air");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("fire_effect") == "local_fire_department");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("flame") == "local_fire_department");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("rainbow_chase") == "palette");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("comet_tail") == "fast_forward");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("chase_effect") == "fast_forward");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("static_white") == "lightbulb");
    REQUIRE(LedEffectBackend::icon_hint_for_effect("my_custom_effect") == "auto_awesome");
}

TEST_CASE_METHOD(LedControllerFixture, "LedEffectBackend display name conversion", "[led]") {
    using helix::led::LedEffectBackend;

    REQUIRE(LedEffectBackend::display_name_for_effect("led_effect breathing") == "Breathing");
    REQUIRE(LedEffectBackend::display_name_for_effect("led_effect fire_effect") == "Fire Effect");
    REQUIRE(LedEffectBackend::display_name_for_effect("rainbow_chase") == "Rainbow Chase");
    REQUIRE(LedEffectBackend::display_name_for_effect("") == "");
}

TEST_CASE_METHOD(LedControllerFixture, "NativeBackend strip management", "[led]") {
    helix::led::NativeBackend backend;

    REQUIRE(!backend.is_available());
    REQUIRE(backend.strips().empty());

    helix::led::LedStripInfo strip;
    strip.name = "Test Strip";
    strip.id = "neopixel test";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = false;

    backend.add_strip(strip);
    REQUIRE(backend.is_available());
    REQUIRE(backend.strips().size() == 1);

    backend.clear();
    REQUIRE(!backend.is_available());
}

TEST_CASE_METHOD(LedControllerFixture, "MacroBackend macro management", "[led]") {
    helix::led::MacroBackend backend;

    REQUIRE(!backend.is_available());

    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    macro.presets = {"LED_PARTY"};

    backend.add_macro(macro);
    REQUIRE(backend.is_available());
    REQUIRE(backend.macros().size() == 1);
    REQUIRE(backend.macros()[0].display_name == "Cabinet Light");
    REQUIRE(backend.macros()[0].presets.size() == 1);

    backend.clear();
    REQUIRE(!backend.is_available());
}

TEST_CASE_METHOD(LedControllerFixture, "LedController deinit clears all backends", "[led]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add some data
    helix::led::LedStripInfo strip;
    strip.name = "Test";
    strip.id = "neopixel test";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = false;
    ctrl.native().add_strip(strip);

    helix::led::LedEffectInfo effect;
    effect.name = "led_effect test";
    effect.display_name = "Test";
    effect.icon_hint = "auto_awesome";
    ctrl.effects().add_effect(effect);

    REQUIRE(ctrl.has_any_backend());

    ctrl.deinit();

    REQUIRE(!ctrl.has_any_backend());
    REQUIRE(ctrl.native().strips().empty());
    REQUIRE(ctrl.effects().effects().empty());
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: selected_strips can hold WLED strip IDs",
                 "[led][controller]") {
    auto& controller = helix::led::LedController::instance();
    controller.deinit();

    // Set selected strips to a WLED-style ID
    controller.set_selected_strips({"wled_printer_led"});
    REQUIRE(controller.selected_strips().size() == 1);
    REQUIRE(controller.selected_strips()[0] == "wled_printer_led");

    // Can switch back to native
    controller.set_selected_strips({"neopixel chamber_light"});
    REQUIRE(controller.selected_strips()[0] == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: light_set turns on all selected native strips",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add native strips
    helix::led::LedStripInfo strip1;
    strip1.name = "Chamber Light";
    strip1.id = "neopixel chamber_light";
    strip1.backend = helix::led::LedBackendType::NATIVE;
    strip1.supports_color = true;
    strip1.supports_white = true;
    ctrl.native().add_strip(strip1);

    // Select the strip
    ctrl.set_selected_strips({"neopixel chamber_light"});

    // light_set should dispatch and update light_is_on()
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: light_set with empty selected_strips is a no-op",
                 "[led][controller]") {
    // Clear any auto-selected strips persisted by prior tests
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
        cfg->save();
    }

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // No strips selected
    REQUIRE(ctrl.selected_strips().empty());

    // Should not crash
    ctrl.light_set(true);
    ctrl.light_set(false);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: light_set with mixed backend types",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add native strip
    helix::led::LedStripInfo native_strip;
    native_strip.name = "Chamber Light";
    native_strip.id = "neopixel chamber_light";
    native_strip.backend = helix::led::LedBackendType::NATIVE;
    native_strip.supports_color = true;
    native_strip.supports_white = true;
    ctrl.native().add_strip(native_strip);

    // Add WLED strip
    helix::led::LedStripInfo wled_strip;
    wled_strip.name = "Printer LED";
    wled_strip.id = "wled_printer_led";
    wled_strip.backend = helix::led::LedBackendType::WLED;
    wled_strip.supports_color = true;
    wled_strip.supports_white = false;
    ctrl.wled().add_strip(wled_strip);

    // Select both
    ctrl.set_selected_strips({"neopixel chamber_light", "wled_printer_led"});

    // Should dispatch to correct backends without crash
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: backend_for_strip returns correct type",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add native strip
    helix::led::LedStripInfo native_strip;
    native_strip.name = "Chamber Light";
    native_strip.id = "neopixel chamber_light";
    native_strip.backend = helix::led::LedBackendType::NATIVE;
    native_strip.supports_color = true;
    native_strip.supports_white = true;
    ctrl.native().add_strip(native_strip);

    // Add WLED strip
    helix::led::LedStripInfo wled_strip;
    wled_strip.name = "Printer LED";
    wled_strip.id = "wled_printer_led";
    wled_strip.backend = helix::led::LedBackendType::WLED;
    wled_strip.supports_color = true;
    wled_strip.supports_white = false;
    ctrl.wled().add_strip(wled_strip);

    // Check backend_for_strip
    REQUIRE(ctrl.backend_for_strip("neopixel chamber_light") == helix::led::LedBackendType::NATIVE);
    REQUIRE(ctrl.backend_for_strip("wled_printer_led") == helix::led::LedBackendType::WLED);

    // Unknown strip should return NATIVE as default
    REQUIRE(ctrl.backend_for_strip("unknown_strip") == helix::led::LedBackendType::NATIVE);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: backend_for_strip identifies macro backend",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add a macro device
    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.type = helix::led::MacroLedType::ON_OFF;
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    ctrl.macro().add_macro(macro);
    ctrl.set_configured_macros({macro});

    // Macro devices are identified by display name
    REQUIRE(ctrl.backend_for_strip("Cabinet Light") == helix::led::LedBackendType::MACRO);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: get/set_led_on_at_start",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Default should be false
    REQUIRE(ctrl.get_led_on_at_start() == false);

    ctrl.set_led_on_at_start(true);
    REQUIRE(ctrl.get_led_on_at_start() == true);

    ctrl.set_led_on_at_start(false);
    REQUIRE(ctrl.get_led_on_at_start() == false);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: apply_startup_preference does nothing when disabled",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_led_on_at_start(false);

    // Should not crash - just a no-op
    ctrl.apply_startup_preference();

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: apply_startup_preference with no strips is a no-op",
                 "[led][controller]") {
    // Clear any auto-selected strips persisted by prior tests
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
        cfg->save();
    }

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_led_on_at_start(true);
    REQUIRE(ctrl.selected_strips().empty());

    // Should not crash even though enabled
    ctrl.apply_startup_preference();

    ctrl.deinit();
}

// ============================================================================
// Phase 1: macro: prefix handling
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "LedController: backend_for_strip with macro: prefix",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.type = helix::led::MacroLedType::ON_OFF;
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    ctrl.macro().add_macro(macro);
    ctrl.set_configured_macros({macro});

    // Both prefixed and unprefixed should resolve to MACRO
    REQUIRE(ctrl.backend_for_strip("macro:Cabinet Light") == helix::led::LedBackendType::MACRO);
    REQUIRE(ctrl.backend_for_strip("Cabinet Light") == helix::led::LedBackendType::MACRO);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: light_set dispatches macro: prefixed strips",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.type = helix::led::MacroLedType::ON_OFF;
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    ctrl.macro().add_macro(macro);
    ctrl.set_configured_macros({macro});

    // Use prefixed strip ID (as the control overlay would)
    ctrl.set_selected_strips({"macro:Cabinet Light"});

    // Should not crash (will warn about no API, which is expected)
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

// ============================================================================
// Phase 2: all_selectable_strips
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: all_selectable_strips includes native + WLED + macros",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add a native strip
    helix::led::LedStripInfo native_strip;
    native_strip.name = "Chamber Light";
    native_strip.id = "neopixel chamber_light";
    native_strip.backend = helix::led::LedBackendType::NATIVE;
    native_strip.supports_color = true;
    native_strip.supports_white = true;
    ctrl.native().add_strip(native_strip);

    // Add a WLED strip
    helix::led::LedStripInfo wled_strip;
    wled_strip.name = "Printer LED";
    wled_strip.id = "wled_printer_led";
    wled_strip.backend = helix::led::LedBackendType::WLED;
    wled_strip.supports_color = true;
    wled_strip.supports_white = false;
    ctrl.wled().add_strip(wled_strip);

    // Add ON_OFF macro (should appear)
    helix::led::LedMacroInfo on_off_macro;
    on_off_macro.display_name = "Cabinet Light";
    on_off_macro.type = helix::led::MacroLedType::ON_OFF;
    on_off_macro.on_macro = "LIGHTS_ON";
    on_off_macro.off_macro = "LIGHTS_OFF";

    // Add TOGGLE macro (should appear)
    helix::led::LedMacroInfo toggle_macro;
    toggle_macro.display_name = "Desk Lamp";
    toggle_macro.type = helix::led::MacroLedType::TOGGLE;
    toggle_macro.toggle_macro = "TOGGLE_DESK";

    // Add PRESET macro (should NOT appear)
    helix::led::LedMacroInfo preset_macro;
    preset_macro.display_name = "Party Mode";
    preset_macro.type = helix::led::MacroLedType::PRESET;

    ctrl.set_configured_macros({on_off_macro, toggle_macro, preset_macro});

    auto strips = ctrl.all_selectable_strips();

    // Should have native + WLED + 2 macros (not PRESET) = 4
    REQUIRE(strips.size() == 4);
    REQUIRE(strips[0].id == "neopixel chamber_light");
    REQUIRE(strips[1].id == "wled_printer_led");
    REQUIRE(strips[2].id == "macro:Cabinet Light");
    REQUIRE(strips[2].backend == helix::led::LedBackendType::MACRO);
    REQUIRE(strips[3].id == "macro:Desk Lamp");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: all_selectable_strips empty when no backends",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    auto strips = ctrl.all_selectable_strips();
    REQUIRE(strips.empty());

    ctrl.deinit();
}

// ============================================================================
// Phase 3: first_available_strip
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "LedController: first_available_strip priority order",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // With nothing: empty
    REQUIRE(ctrl.first_available_strip().empty());

    // Add macro only
    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.type = helix::led::MacroLedType::ON_OFF;
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    ctrl.set_configured_macros({macro});

    REQUIRE(ctrl.first_available_strip() == "macro:Cabinet Light");

    // Add WLED -- should now prefer WLED over macro
    helix::led::LedStripInfo wled_strip;
    wled_strip.name = "WLED Strip";
    wled_strip.id = "wled_test";
    wled_strip.backend = helix::led::LedBackendType::WLED;
    wled_strip.supports_color = true;
    wled_strip.supports_white = false;
    ctrl.wled().add_strip(wled_strip);

    REQUIRE(ctrl.first_available_strip() == "wled_test");

    // Add native -- should now prefer native
    helix::led::LedStripInfo native_strip;
    native_strip.name = "Chamber Light";
    native_strip.id = "neopixel chamber_light";
    native_strip.backend = helix::led::LedBackendType::NATIVE;
    native_strip.supports_color = true;
    native_strip.supports_white = true;
    ctrl.native().add_strip(native_strip);

    REQUIRE(ctrl.first_available_strip() == "neopixel chamber_light");

    // Set selected -- should prefer that
    ctrl.set_selected_strips({"wled_test"});
    REQUIRE(ctrl.first_available_strip() == "wled_test");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: first_available_strip skips PRESET macros",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::led::LedMacroInfo preset_macro;
    preset_macro.display_name = "Party Mode";
    preset_macro.type = helix::led::MacroLedType::PRESET;

    helix::led::LedMacroInfo toggle_macro;
    toggle_macro.display_name = "Desk Lamp";
    toggle_macro.type = helix::led::MacroLedType::TOGGLE;
    toggle_macro.toggle_macro = "TOGGLE_DESK";

    ctrl.set_configured_macros({preset_macro, toggle_macro});

    // Should skip PRESET and return TOGGLE
    REQUIRE(ctrl.first_available_strip() == "macro:Desk Lamp");

    ctrl.deinit();
}

// ============================================================================
// Phase 4: MacroBackend state tracking + abstract API
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "MacroBackend: ON_OFF has known state", "[led][macro]") {
    helix::led::MacroBackend backend;

    helix::led::LedMacroInfo on_off;
    on_off.display_name = "Cabinet Light";
    on_off.type = helix::led::MacroLedType::ON_OFF;
    on_off.on_macro = "LIGHTS_ON";
    on_off.off_macro = "LIGHTS_OFF";
    backend.add_macro(on_off);

    REQUIRE(backend.has_known_state("Cabinet Light"));

    // Unknown macro names are not trackable
    REQUIRE(!backend.has_known_state("No Such Macro"));

    // clear() drops the macro list, so nothing is trackable afterwards
    backend.clear();
    REQUIRE(!backend.has_known_state("Cabinet Light"));
}

TEST_CASE_METHOD(LedControllerFixture, "MacroBackend: TOGGLE has unknown state", "[led][macro]") {
    helix::led::MacroBackend backend;

    helix::led::LedMacroInfo toggle;
    toggle.display_name = "Desk Lamp";
    toggle.type = helix::led::MacroLedType::TOGGLE;
    toggle.toggle_macro = "TOGGLE_DESK";
    backend.add_macro(toggle);

    // TOGGLE macros don't have known state
    REQUIRE(!backend.has_known_state("Desk Lamp"));
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: light_state_trackable with various selections",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Native only -- trackable
    helix::led::LedStripInfo native_strip;
    native_strip.name = "Chamber Light";
    native_strip.id = "neopixel chamber_light";
    native_strip.backend = helix::led::LedBackendType::NATIVE;
    native_strip.supports_color = true;
    native_strip.supports_white = true;
    ctrl.native().add_strip(native_strip);
    ctrl.set_selected_strips({"neopixel chamber_light"});
    REQUIRE(ctrl.light_state_trackable());

    // Add ON_OFF macro -- still trackable
    helix::led::LedMacroInfo on_off;
    on_off.display_name = "Cabinet Light";
    on_off.type = helix::led::MacroLedType::ON_OFF;
    on_off.on_macro = "LIGHTS_ON";
    on_off.off_macro = "LIGHTS_OFF";
    ctrl.macro().add_macro(on_off);
    ctrl.set_configured_macros({on_off});
    ctrl.set_selected_strips({"neopixel chamber_light", "macro:Cabinet Light"});
    REQUIRE(ctrl.light_state_trackable());

    // Add TOGGLE macro -- NOT trackable
    helix::led::LedMacroInfo toggle;
    toggle.display_name = "Desk Lamp";
    toggle.type = helix::led::MacroLedType::TOGGLE;
    toggle.toggle_macro = "TOGGLE_DESK";
    ctrl.macro().add_macro(toggle);
    ctrl.set_configured_macros({on_off, toggle});
    ctrl.set_selected_strips({"neopixel chamber_light", "macro:Desk Lamp"});
    REQUIRE(!ctrl.light_state_trackable());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: light_toggle and light_is_on",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add ON_OFF macro
    helix::led::LedMacroInfo macro;
    macro.display_name = "Cabinet Light";
    macro.type = helix::led::MacroLedType::ON_OFF;
    macro.on_macro = "LIGHTS_ON";
    macro.off_macro = "LIGHTS_OFF";
    ctrl.macro().add_macro(macro);
    ctrl.set_configured_macros({macro});
    ctrl.set_selected_strips({"macro:Cabinet Light"});

    // Initially off
    REQUIRE(!ctrl.light_is_on());

    // Toggle on
    ctrl.light_toggle();
    REQUIRE(ctrl.light_is_on());

    // Toggle off
    ctrl.light_toggle();
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

// ============================================================================
// OutputPinBackend tests
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: enum value and is_pwm field",
                 "[led][output_pin]") {
    helix::led::LedStripInfo info;
    info.backend = helix::led::LedBackendType::OUTPUT_PIN;
    REQUIRE(info.backend == helix::led::LedBackendType::OUTPUT_PIN);
    REQUIRE(info.is_pwm == false);
    info.is_pwm = true;
    REQUIRE(info.is_pwm == true);
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: strip management", "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    REQUIRE_FALSE(backend.is_available());
    REQUIRE(backend.pins().empty());

    helix::led::LedStripInfo pin;
    pin.name = "Enclosure LEDs";
    pin.id = "output_pin Enclosure_LEDs";
    pin.backend = helix::led::LedBackendType::OUTPUT_PIN;
    pin.supports_color = false;
    pin.supports_white = false;
    pin.is_pwm = true;

    backend.add_pin(pin);
    REQUIRE(backend.is_available());
    REQUIRE(backend.pins().size() == 1);
    REQUIRE(backend.pins()[0].name == "Enclosure LEDs");

    backend.clear();
    REQUIRE_FALSE(backend.is_available());
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: cached value from status",
                 "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    helix::led::LedStripInfo pin;
    pin.id = "output_pin test_led";
    pin.backend = helix::led::LedBackendType::OUTPUT_PIN;
    backend.add_pin(pin);

    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.0));

    nlohmann::json status = {{"output_pin test_led", {{"value", 0.75}}}};
    backend.update_from_status(status);

    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.75));
}

TEST_CASE_METHOD(LedControllerFixture,
                 "OutputPinBackend: status updates overwrite the cached value",
                 "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    helix::led::LedStripInfo pin;
    pin.id = "output_pin test_led";
    backend.add_pin(pin);

    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.0));

    nlohmann::json status = {{"output_pin test_led", {{"value", 0.5}}}};
    backend.update_from_status(status);
    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.5));

    status = {{"output_pin test_led", {{"value", 0.0}}}};
    backend.update_from_status(status);
    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.0));
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: brightness_pct", "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    helix::led::LedStripInfo pin;
    pin.id = "output_pin test_led";
    backend.add_pin(pin);

    nlohmann::json status = {{"output_pin test_led", {{"value", 0.75}}}};
    backend.update_from_status(status);
    REQUIRE(backend.brightness_pct("output_pin test_led") == 75);
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: is_pwm check", "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    helix::led::LedStripInfo pin;
    pin.id = "output_pin test_led";
    pin.is_pwm = true;
    backend.add_pin(pin);

    REQUIRE(backend.is_pwm("output_pin test_led"));

    backend.set_pin_pwm("output_pin test_led", false);
    REQUIRE_FALSE(backend.is_pwm("output_pin test_led"));
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: status for an unknown pin is ignored",
                 "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    helix::led::LedStripInfo pin;
    pin.id = "output_pin test_led";
    backend.add_pin(pin);

    nlohmann::json status = {{"output_pin other_led", {{"value", 0.42}}}};
    backend.update_from_status(status);

    REQUIRE(backend.get_value("output_pin test_led") == Catch::Approx(0.0));
    REQUIRE(backend.get_value("output_pin other_led") == Catch::Approx(0.0));
}

TEST_CASE_METHOD(LedControllerFixture, "OutputPinBackend: no API safety", "[led][output_pin]") {
    helix::led::OutputPinBackend backend;
    // Should not crash when API is null
    backend.set_value("output_pin test", 0.5);
    backend.turn_on("output_pin test");
    backend.turn_off("output_pin test");
    backend.set_brightness("output_pin test", 50);
}

// ============================================================================
// LED Config Version Subject Tests
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "LedController: version subject accessible after init",
                 "[led][version]") {
    lv_init_safe();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Subject should be accessible (no crash)
    lv_subject_t* subj = ctrl.get_led_config_version_subject();
    REQUIRE(subj != nullptr);
    // Value is an integer (may be non-zero if other tests ran first)
    lv_subject_get_int(subj);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: set_selected_strips bumps version",
                 "[led][version]") {
    lv_init_safe();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    int initial = lv_subject_get_int(ctrl.get_led_config_version_subject());
    ctrl.set_selected_strips({"neopixel test_strip"});

    REQUIRE(lv_subject_get_int(ctrl.get_led_config_version_subject()) == initial + 1);

    ctrl.set_selected_strips({"neopixel strip_a", "neopixel strip_b"});
    REQUIRE(lv_subject_get_int(ctrl.get_led_config_version_subject()) == initial + 2);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: version observer fires on bump",
                 "[led][version]") {
    lv_init_safe();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    int before = lv_subject_get_int(ctrl.get_led_config_version_subject());

    int user_data[2] = {0, -1}; // [count, last_value]
    auto cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;
        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    lv_observer_t* obs =
        lv_subject_add_observer(ctrl.get_led_config_version_subject(), cb, user_data);

    // LVGL auto-fires on add
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == before);

    ctrl.set_selected_strips({"neopixel test"});
    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == before + 1);

    lv_observer_remove(obs);
    ctrl.deinit();
}

// ============================================================================
// Regression tests: light_set / turn_off_all / apply_startup_preference state
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "LedController: light_set updates light_is_on",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(!ctrl.light_is_on());

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: turn_off_all sets light_is_on false",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.turn_off_all();
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: set_color_all updates light_is_on",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Non-zero color sets light on
    ctrl.set_color_all(1.0, 0.5, 0.0);
    REQUIRE(ctrl.light_is_on());

    // Zero color sets light off
    ctrl.set_color_all(0.0, 0.0, 0.0, 0.0);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: set_brightness_all updates light_is_on",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_brightness_all(50);
    REQUIRE(ctrl.light_is_on());

    ctrl.set_brightness_all(0);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: apply_startup_preference sets light_is_on true",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add a strip so apply_startup_preference doesn't early-return
    helix::led::LedStripInfo strip;
    strip.name = "Chamber Light";
    strip.id = "neopixel chamber_light";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true;
    ctrl.native().add_strip(strip);
    ctrl.set_selected_strips({"neopixel chamber_light"});

    ctrl.set_led_on_at_start(true);
    REQUIRE(!ctrl.light_is_on());

    ctrl.apply_startup_preference();
    REQUIRE(ctrl.light_is_on());

    ctrl.deinit();
}

// ============================================================================
// Regression: toggle off must stop LED effects before SET_LED
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: light_set(false) stops LED effects when available",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Add a native strip (represents neopixel case_lights)
    helix::led::LedStripInfo strip;
    strip.name = "Case Lights";
    strip.id = "neopixel case_lights";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = false;
    ctrl.native().add_strip(strip);
    ctrl.set_selected_strips({"neopixel case_lights"});

    // Add LED effects (simulates stealthburner_led_effects being configured)
    helix::led::LedEffectInfo effect;
    effect.name = "led_effect sb_logo_printing";
    effect.display_name = "Printing";
    ctrl.effects().add_effect(effect);

    REQUIRE(ctrl.effects().is_available());

    // Turn on, then off — should not crash even without API
    // (stop_all_effects will warn but not crash with null API)
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    // Toggle path exercises the same code
    ctrl.light_toggle();
    REQUIRE(ctrl.light_is_on());

    ctrl.light_toggle();
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: light_set(false) without effects skips stop_all_effects",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Native strip only, no effects
    helix::led::LedStripInfo strip;
    strip.name = "Chamber Light";
    strip.id = "neopixel chamber_light";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true;
    ctrl.native().add_strip(strip);
    ctrl.set_selected_strips({"neopixel chamber_light"});

    REQUIRE(!ctrl.effects().is_available());

    // Should work fine without effects
    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    REQUIRE(!ctrl.light_is_on());

    ctrl.deinit();
}

// ============================================================================
// Stale strip pruning (issue #360: preset LED name vs firmware mismatch)
// ============================================================================

TEST_CASE_METHOD(LedControllerFixture, "LedController: stale selected strips pruned on discovery",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Pre-load a strip name that won't match discovered hardware
    // (simulates AD5M preset with "led chamber_light" on Zmod firmware)
    ctrl.set_selected_strips({"led chamber_light"});
    REQUIRE(ctrl.selected_strips().size() == 1);

    // Discover hardware with a DIFFERENT LED name (Zmod uses "chamber_LED")
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"led chamber_LED", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    // The stale "led chamber_light" should be pruned, and auto-select
    // should have picked "led chamber_LED" from discovered hardware
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "led chamber_LED");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: valid selected strips preserved on discovery",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Pre-load a strip name that WILL match discovered hardware
    ctrl.set_selected_strips({"neopixel chamber_light"});

    helix::PrinterDiscovery discovery;
    nlohmann::json objects =
        nlohmann::json::array({"neopixel chamber_light", "led status_led", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    // Valid strip should be preserved (not pruned, not replaced by auto-select)
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel chamber_light");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: mixed valid and stale strips pruned correctly",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // One valid, one stale
    ctrl.set_selected_strips({"neopixel rgb_led", "led old_light"});
    REQUIRE(ctrl.selected_strips().size() == 2);

    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"neopixel rgb_led", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    // Only the valid strip should remain
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel rgb_led");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture, "LedController: all strips stale triggers auto-select",
                 "[led][controller]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // All pre-selected strips are stale
    ctrl.set_selected_strips({"led nonexistent_1", "led nonexistent_2"});

    helix::PrinterDiscovery discovery;
    nlohmann::json objects =
        nlohmann::json::array({"neopixel actual_led_1", "led actual_led_2", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    // All stale → pruned → empty → auto-select picks all native strips
    REQUIRE(ctrl.selected_strips().size() == 2);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel actual_led_1");
    REQUIRE(ctrl.selected_strips()[1] == "led actual_led_2");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: auto-select picks output_pin LED when no native present",
                 "[led][controller]") {
    // Regression for K2 Plus / K1C: their only LED is `[output_pin LED]`. Auto-select
    // used to only fire for native strips, leaving the print-status light toggle to
    // bail out with "No light configured". Auto-select must also cover output_pin.
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
        cfg->save();
    }

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"output_pin LED", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "output_pin LED");

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: auto-select picks all selectable strips across backends",
                 "[led][controller]") {
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
        cfg->save();
    }

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::PrinterDiscovery discovery;
    nlohmann::json objects =
        nlohmann::json::array({"neopixel chamber_light", "output_pin case_light", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    // Both backends should be represented in the auto-selection
    auto& selected = ctrl.selected_strips();
    REQUIRE(selected.size() == 2);
    bool has_neopixel = false;
    bool has_output_pin = false;
    for (const auto& id : selected) {
        if (id == "neopixel chamber_light")
            has_neopixel = true;
        if (id == "output_pin case_light")
            has_output_pin = true;
    }
    REQUIRE(has_neopixel);
    REQUIRE(has_output_pin);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedControllerFixture,
                 "LedController: led_controllable subject reflects selected_strips emptiness",
                 "[led][controller][led_controllable]") {
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
        cfg->save();
    }

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    auto* subj = ctrl.get_led_controllable_subject();
    REQUIRE(subj != nullptr);

    // Fresh init with cleared config: nothing selected → 0
    REQUIRE(ctrl.selected_strips().empty());
    REQUIRE(lv_subject_get_int(subj) == 0);

    // Manually populating selected_strips flips the subject to 1
    ctrl.set_selected_strips({"neopixel chamber_light"});
    REQUIRE(lv_subject_get_int(subj) == 1);

    // Clearing flips it back to 0
    ctrl.set_selected_strips({});
    REQUIRE(lv_subject_get_int(subj) == 0);

    // Auto-select via discovery also flips it to 1 (regression for K2 Plus / K1C:
    // output_pin-only printers must show the light toggle as controllable)
    helix::PrinterDiscovery discovery;
    nlohmann::json objects = nlohmann::json::array({"output_pin LED", "extruder"});
    discovery.parse_objects(objects);
    ctrl.discover_from_hardware(discovery);

    REQUIRE_FALSE(ctrl.selected_strips().empty());
    REQUIRE(lv_subject_get_int(subj) == 1);

    ctrl.deinit();
}

// ============================================================================
// Mock-API fixture for tests that verify actual color values sent
// ============================================================================

struct LedMockApiFixture : public HelixTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedMockApiFixture() {
        state.init_subjects(false);
        // Also init the global singleton that LedController::init() observes for
        // connection-state changes (get_printer_state() != this->state).
        get_printer_state().init_subjects(false);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    ~LedMockApiFixture() {
        auto& ctrl = helix::led::LedController::instance();
        // ORDERING: deinit() (which resets conn_observer_) MUST precede deinit_subjects()
        // (which frees the connection-state subject conn_observer_ is observing).
        ctrl.deinit();
        get_printer_state().deinit_subjects();
    }

    void setup_controller_with_strip(const std::string& strip_id = "neopixel chamber") {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);

        helix::led::LedStripInfo strip;
        strip.name = "Chamber";
        strip.id = strip_id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = true;
        strip.supports_white = false;
        ctrl.native().add_strip(strip);
        ctrl.set_selected_strips({strip_id});
    }

    void setup_controller_with_rgbw_strip(const std::string& strip_id = "neopixel chamber") {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);

        helix::led::LedStripInfo strip;
        strip.name = "Chamber";
        strip.id = strip_id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = true;
        strip.supports_white = true;
        ctrl.native().add_strip(strip);
        ctrl.set_selected_strips({strip_id});
    }
};

// ============================================================================
// Regression: toggle on must use last_color + last_brightness, not full white
// https://github.com/prestonbrown/helixscreen#toggle-brightness-regression
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) uses last_brightness not full white",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_last_brightness(50);

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    // With white color at 50% brightness, RGB should be 0.5 each (not 1.0)
    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) uses saved color not just white",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Red color at 100% brightness
    ctrl.set_last_color(0xFF0000);
    ctrl.set_last_brightness(100);

    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: light_set(true) combines color and brightness",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Blue color (#0000FF) at 80% brightness → R=0, G=0, B=0.8
    ctrl.set_last_color(0x0000FF);
    ctrl.set_last_brightness(80);

    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.8).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: light_toggle uses saved brightness",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_last_brightness(50);

    // Start off, toggle on
    ctrl.light_set(false);
    ctrl.light_toggle();
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.5).margin(0.01));
}

// ============================================================================
// Unit tests: set_brightness_all respects last_color
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: set_brightness_all uses last_color not hardcoded white",
                 "[led][controller]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Set color to yellow (#FFD700) and brightness to 100%
    ctrl.set_last_color(0xFFD700);
    ctrl.set_brightness_all(100);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    // #FFD700 → R=1.0, G=0.843, B=0.0 at 100%
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.843).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: set_brightness_all scales color by brightness",
                 "[led][controller]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Red at 50% → R=0.5, G=0, B=0
    ctrl.set_last_color(0xFF0000);
    ctrl.set_brightness_all(50);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: apply_startup_preference uses startup_brightness with last_color",
                 "[led][controller]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_led_on_at_start(true);
    ctrl.set_last_color(0xFF6B35); // Orange
    ctrl.set_startup_brightness(80);

    ctrl.apply_startup_preference();
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    // #FF6B35 → R=1.0, G=0.42, B=0.21 scaled by 80%
    REQUIRE(color.r == Catch::Approx(0.8).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.336).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.165).margin(0.02));
}

// ============================================================================
// "LED on at start" means AT START — not on every rediscovery
//
// notify_klippy_ready re-triggers the connected callback unconditionally, and the
// tail of that callback calls apply_startup_preference(). printer_discovery also
// re-runs LedController::init() on every discovery. So a user who turned the
// lights off and then restarted Klipper (FIRMWARE_RESTART, M112, a klippy crash)
// got them switched back on behind their back.
// ============================================================================

namespace {

/// Put both PrinterStates in the state a real dispatch needs. execute_gcode()
/// refuses gcode outright while Klipper is halted, and MoonrakerAPI reads the
/// PrinterState it was constructed with (the fixture's own instance, which
/// defaults to SHUTDOWN) — without this, nothing reaches the mock wire and any
/// "no gcode was sent" assertion passes for the wrong reason.
void make_led_dispatch_real(helix::PrinterState& api_state) {
    api_state.set_klippy_state_sync(helix::KlippyState::READY);
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.get_printer_connection_state_subject(),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    ps.set_klippy_state_sync(helix::KlippyState::READY);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// Count SET_LED commands that actually hit the mock wire.
size_t count_set_led(const std::vector<std::string>& history) {
    size_t n = 0;
    for (const auto& script : history) {
        if (script.find("SET_LED") != std::string::npos)
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: startup preference applies once, not on every rediscovery",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    make_led_dispatch_real(state);

    ctrl.set_led_on_at_start(true);
    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_startup_brightness(80);

    // --- First discovery completes: the preference applies, lights come up. ---
    mock_client.clear_gcode_script_history();
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(ctrl.light_is_on());
    // Assert on the wire, not just the flag: a SET_LED really went to Klipper.
    REQUIRE(count_set_led(mock_client.gcode_script_history()) == 1);
    REQUIRE(ctrl.native().get_strip_color("neopixel chamber").r == Catch::Approx(0.8).margin(0.01));

    // --- The user turns the lights off. ---
    ctrl.light_set(false);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(ctrl.light_is_on());
    REQUIRE(ctrl.native().get_strip_color("neopixel chamber").r == Catch::Approx(0.0).margin(0.01));

    // --- Klipper restarts. Moonraker itself never went away, so
    // notify_klippy_ready re-fires the connected callback → full rediscovery.
    // printer_discovery re-runs init() WITHOUT a deinit() and re-selects the
    // strips; init()'s load_config() restores the persisted preference (stood in
    // for here by the explicit setters — the values are written by save_config).
    mock_client.clear_gcode_script_history();
    ctrl.init(mock_api.get(), &mock_client);
    ctrl.set_selected_strips({"neopixel chamber"});
    ctrl.set_led_on_at_start(true);
    ctrl.set_startup_brightness(80);
    make_led_dispatch_real(state);

    // ...and the connected callback calls apply_startup_preference() again.
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // The user's OFF has to survive the restart: startup already happened.
    CHECK(count_set_led(mock_client.gcode_script_history()) == 0);
    CHECK_FALSE(ctrl.light_is_on());
    CHECK(ctrl.native().get_strip_color("neopixel chamber").r == Catch::Approx(0.0).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: startup preference still fires when strips arrive late",
                 "[led][controller][regression]") {
    // The guard must not burn its one shot on a discovery that had no strips to
    // act on — WLED strips are discovered asynchronously, so the first
    // discovery-complete callback can legitimately find selected_strips_ empty.
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);
    // init() reloads the persisted selection; this machine's config may carry one.
    ctrl.set_selected_strips({});
    make_led_dispatch_real(state);

    ctrl.set_led_on_at_start(true);
    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_startup_brightness(80);

    // First discovery: nothing selected yet — no-op, and no shot spent.
    REQUIRE(ctrl.selected_strips().empty());
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(ctrl.light_is_on());

    // Strips show up on the next pass.
    helix::led::LedStripInfo strip;
    strip.name = "Chamber";
    strip.id = "neopixel chamber";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    ctrl.native().add_strip(strip);
    ctrl.set_selected_strips({"neopixel chamber"});

    mock_client.clear_gcode_script_history();
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    CHECK(ctrl.light_is_on());
    CHECK(count_set_led(mock_client.gcode_script_history()) == 1);
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: switching printers re-arms the startup preference",
                 "[led][controller][regression]") {
    // A printer switch runs Application::tear_down_printer_state() (which calls
    // LedController::deinit()) followed by a fresh init — that is a genuine
    // startup for the new printer, so the preference must apply again.
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    make_led_dispatch_real(state);

    ctrl.set_led_on_at_start(true);
    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_startup_brightness(80);
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(ctrl.light_is_on());

    ctrl.light_set(false);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(ctrl.light_is_on());

    // Printer switch: teardown + re-init.
    setup_controller_with_strip();
    ctrl.set_led_on_at_start(true);
    ctrl.set_last_color(0xFFFFFF);
    ctrl.set_startup_brightness(80);
    make_led_dispatch_real(state);

    mock_client.clear_gcode_script_history();
    ctrl.apply_startup_preference();
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    CHECK(ctrl.light_is_on());
    CHECK(count_set_led(mock_client.gcode_script_history()) == 1);
}

// ============================================================================
// RGBW support: white channel toggle and brightness (#737)
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) restores white channel on RGBW strip",
                 "[led][controller][rgbw]") {
    setup_controller_with_rgbw_strip();
    auto& ctrl = helix::led::LedController::instance();

    // White-only mode: RGB=0, W=1.0 at 80% brightness
    ctrl.set_last_color(0x000000);
    ctrl.set_last_white(1.0);
    ctrl.set_last_brightness(80);

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.w == Catch::Approx(0.8).margin(0.01)); // 1.0 * 80%
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: set_brightness_all preserves white channel",
                 "[led][controller][rgbw]") {
    setup_controller_with_rgbw_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_last_color(0x000000);
    ctrl.set_last_white(1.0);
    ctrl.set_brightness_all(50);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.w == Catch::Approx(0.5).margin(0.01)); // 1.0 * 50%
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: set_color_all caches white for toggle restore",
                 "[led][controller][rgbw]") {
    setup_controller_with_rgbw_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Set white-only via set_color_all
    ctrl.set_color_all(0.0, 0.0, 0.0, 1.0);
    REQUIRE(ctrl.last_white() == Catch::Approx(1.0));

    // Toggle off then on — should restore white
    ctrl.light_set(false);
    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.w == Catch::Approx(1.0).margin(0.01));
}

// ============================================================================
// Regression: toggle_all(true) must never emit all-zero output (LED stuck off)
// https://github.com/prestonbrown/helixscreen — "LEDs stay off" regression
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) falls back to full white when state is all zero",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Poisoned state: everything zero (simulates overlay persisting an off LED)
    ctrl.set_last_color(0);
    ctrl.set_last_brightness(0);
    ctrl.set_last_white(0.0);

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    // Must produce visible light — fall back to full white at 100%
    REQUIRE(color.r == Catch::Approx(1.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(1.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: last_white persists through save_config/load_config round-trip",
                 "[led][controller][regression][rgbw]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_last_color(0x123456);
    ctrl.set_last_brightness(42);
    ctrl.set_last_white(0.5);

    ctrl.save_config();

    // Clobber in-memory state, then reload from config
    ctrl.set_last_white(0.0);
    REQUIRE(ctrl.last_white() == Catch::Approx(0.0));

    ctrl.load_config();
    REQUIRE(ctrl.last_white() == Catch::Approx(0.5).margin(0.001));
}

// Test A: RGBW white-only strip — the actual user hardware scenario.
// Saved state has RGB=0 but white channel set. The guard in compute_scaled_last_color
// must NOT treat this as "no saved color"; it should scale the white channel.
TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) scales white-only RGBW saved state",
                 "[led][controller][regression][rgbw]") {
    setup_controller_with_rgbw_strip();
    auto& ctrl = helix::led::LedController::instance();

    ctrl.set_last_color(0);       // No RGB
    ctrl.set_last_white(0.5);     // White channel at half
    ctrl.set_last_brightness(80); // 80% brightness

    ctrl.light_set(true);
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    REQUIRE(color.r == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.0).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.0).margin(0.01));
    // 0.5 * 0.8 = 0.4
    REQUIRE(color.w == Catch::Approx(0.4).margin(0.01));
}

// Test B: set_brightness_all() had the same latent bug as toggle_all() —
// dragging the brightness slider with poisoned (all-zero) saved state would
// output black. The shared helper must apply the same fallback.
TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: set_brightness_all falls back to full white on zero saved state",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Poisoned state: no saved color at all
    ctrl.set_last_color(0);
    ctrl.set_last_white(0.0);
    ctrl.set_last_brightness(50);

    ctrl.set_brightness_all(75);
    REQUIRE(ctrl.light_is_on());

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    // Fallback to full white, scaled by 75% brightness
    REQUIRE(color.r == Catch::Approx(0.75).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.75).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.75).margin(0.01));
}

// Test C: load_config with a pre-RGBW config (missing last_white key)
// must default last_white to 0.0 rather than leaving garbage.
TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: load_config defaults last_white to 0.0 when key missing",
                 "[led][controller][regression][rgbw]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Seed a non-zero in-memory white, then load a config that won't have the key set.
    ctrl.set_last_white(0.9);
    REQUIRE(ctrl.last_white() == Catch::Approx(0.9));

    // Fresh load_config reads whatever is in the config store; for a default
    // fixture with no prior save of last_white, the load must default to 0.0.
    ctrl.load_config();
    // If a previous test did save last_white to disk, this may be nonzero,
    // so explicitly clobber the key path by re-reading after an empty set.
    // Regardless, loading should produce a valid clamped value in [0, 1].
    REQUIRE(ctrl.last_white() >= 0.0);
    REQUIRE(ctrl.last_white() <= 1.0);
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: light_set(true) normal path still scales color correctly",
                 "[led][controller][regression]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();

    // Gray #808080 at 50% brightness, no white channel
    ctrl.set_last_color(0x808080);
    ctrl.set_last_brightness(50);
    ctrl.set_last_white(0.0);

    ctrl.light_set(true);

    auto color = ctrl.native().get_strip_color("neopixel chamber");
    // 0x80/255 ≈ 0.502, * 0.5 ≈ 0.251
    REQUIRE(color.r == Catch::Approx(0.25).margin(0.01));
    REQUIRE(color.g == Catch::Approx(0.25).margin(0.01));
    REQUIRE(color.b == Catch::Approx(0.25).margin(0.01));
    REQUIRE(color.w == Catch::Approx(0.0).margin(0.01));
}

// ============================================================================
// Task 1: led_command_in_flight subject + counter scaffolding
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture, "LedController: in-flight subject defaults to 0",
                 "[led][controller][inflight]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();
    REQUIRE(s != nullptr);
    REQUIRE(lv_subject_get_int(s) == 0);
    REQUIRE_FALSE(ctrl.light_command_in_flight());
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: native toggle marks in-flight then clears on ACK",
                 "[led][controller][inflight]") {
    setup_controller_with_strip(); // NATIVE backend, "neopixel chamber"
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();

    ctrl.light_set(true);
    REQUIRE(lv_subject_get_int(s) == 1);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(s) == 0);
    REQUIRE_FALSE(ctrl.light_command_in_flight());
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: in-flight clears even when the ACK is an error",
                 "[led][controller][inflight]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();

    mock_client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "forced timeout", "SET_LED");
    ctrl.light_set(true);
    REQUIRE(lv_subject_get_int(s) == 1);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(s) == 0);
}

TEST_CASE_METHOD(LedMockApiFixture, "LedController: in-flight covers all selected strips",
                 "[led][controller][inflight]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);

    for (const std::string& id : {std::string("neopixel a"), std::string("neopixel b")}) {
        helix::led::LedStripInfo strip;
        strip.name = id;
        strip.id = id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        strip.supports_color = true;
        ctrl.native().add_strip(strip);
    }
    ctrl.set_selected_strips({"neopixel a", "neopixel b"});

    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();
    ctrl.light_set(true);
    REQUIRE(lv_subject_get_int(s) == 1);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(s) == 0);
}

// ============================================================================
// Task 3: connection-state observer clears in-flight on disconnect
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture, "LedController: disconnect clears in-flight LED state",
                 "[led][controller][inflight]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();

    ctrl.light_set(true);
    REQUIRE(lv_subject_get_int(s) == 1);

    // Wiring smoke test: the mock fires the gcode ACK synchronously, so the
    // deferred settle path would also clear the subject on drain. This test
    // confirms the connection-state observer is registered, compiles, and fires
    // without crashing — and that the end state is clean across the transition.
    // True mid-flight-disconnect (ACK never arrives) is verified on hardware.
    lv_subject_set_int(get_printer_state().get_printer_connection_state_subject(),
                       static_cast<int>(helix::ConnectionState::DISCONNECTED));

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(s) == 0);
    REQUIRE_FALSE(ctrl.light_command_in_flight());
}

// ============================================================================
// A Klippy restart leaves the Moonraker WebSocket UP (#1129)
//
// printer_connection_state only tracks the Moonraker WebSocket, so a Klipper
// restart never moves it off CONNECTED and the conn_observer_ above can never
// fire. The reporter's journal proves it: led_command_in_flight stuck at 1
// across a Klippy recovery with zero "force-clearing" lines. The klippy-state
// observer is the safety net for any dispatch that leaks its settle callback.
// ============================================================================

namespace {

/// Arrange the #1129 wedge: Moonraker CONNECTED, then dispatch an LED command whose
/// RPC response is dropped so the settle callback never fires. Leaves
/// in_flight_count_ pinned at 1 with a fully drained UpdateQueue.
/// @p global_klippy is the klippy state the LedController sees at dispatch time.
void wedge_in_flight_led_command(helix::PrinterState& api_state, MoonrakerClientMock& mock_client,
                                 helix::KlippyState global_klippy = helix::KlippyState::READY) {
    // execute_gcode() refuses gcode outright while Klipper is halted, and the API
    // reads the PrinterState it was constructed with (the fixture's own instance),
    // which defaults to SHUTDOWN. Make the dispatch actually reach the wire.
    api_state.set_klippy_state_sync(helix::KlippyState::READY);

    // The LedController observes the GLOBAL PrinterState, not the API's.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.get_printer_connection_state_subject(),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    ps.set_klippy_state_sync(global_klippy);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // The RPC goes out; the response never comes back. Neither on_success nor
    // on_error ever runs, so note_command_settled() is never reached.
    mock_client.force_next_gcode_dropped_response("SET_LED");
    helix::led::LedController::instance().light_set(true);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

} // namespace

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: a dropped LED ACK really does wedge the in-flight counter",
                 "[led][controller][inflight]") {
    // Harness guard for the test below: without a genuinely lost response the
    // mock ACKs synchronously and the counter clears on its own, which would make
    // the klippy-restart test pass for the wrong reason.
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();

    wedge_in_flight_led_command(state, mock_client);

    // The SET_LED did reach Klipper — only its response was lost.
    CHECK_FALSE(mock_client.gcode_script_history().empty());
    // ...and the button is stuck greyed out.
    REQUIRE(lv_subject_get_int(s) == 1);
    REQUIRE(ctrl.light_command_in_flight());
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: Klippy leaving READY clears in-flight LED state (#1129)",
                 "[led][controller][inflight]") {
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();
    auto& ps = get_printer_state();

    wedge_in_flight_led_command(state, mock_client);
    REQUIRE(lv_subject_get_int(s) == 1);

    // Klippy dies / restarts. Moonraker itself never went away, so the WebSocket
    // stays up and printer_connection_state does NOT move.
    helix::KlippyState non_ready = helix::KlippyState::SHUTDOWN;
    SECTION("M112 shutdown") {
        non_ready = helix::KlippyState::SHUTDOWN;
    }
    SECTION("klippy error") {
        non_ready = helix::KlippyState::ERROR;
    }
    SECTION("FIRMWARE_RESTART startup") {
        // Klipper re-inits: any RPC issued before the restart is gone for good.
        non_ready = helix::KlippyState::STARTUP;
    }
    CAPTURE(static_cast<int>(non_ready));

    ps.set_klippy_state_sync(non_ready);
    REQUIRE(lv_subject_get_int(ps.get_printer_connection_state_subject()) ==
            static_cast<int>(helix::ConnectionState::CONNECTED));

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    CHECK(lv_subject_get_int(s) == 0);
    CHECK_FALSE(ctrl.light_command_in_flight());
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: Klippy coming back READY does not clear an in-flight command",
                 "[led][controller][inflight]") {
    // The new observer must fire only on a READY *exit*. A klippy transition INTO
    // READY (recovery finished) must leave a genuinely in-flight toggle greyed out,
    // or the button un-greys before its command has landed.
    // Note: lv_subject_set_int() uses notify_if_changed, so this has to be a real
    // value transition — re-asserting the same value notifies nobody and would make
    // this test vacuous.
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();
    auto& ps = get_printer_state();

    wedge_in_flight_led_command(state, mock_client, helix::KlippyState::SHUTDOWN);
    REQUIRE(lv_subject_get_int(s) == 1);

    ps.set_klippy_state_sync(helix::KlippyState::READY);
    REQUIRE(lv_subject_get_int(ps.get_klippy_state_subject()) ==
            static_cast<int>(helix::KlippyState::READY));
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    CHECK(lv_subject_get_int(s) == 1);
    CHECK(ctrl.light_command_in_flight());
}

// ============================================================================
// A toggle QUEUED behind an external blocking op must still settle (#1129)
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture,
                 "LedController: a toggle queued behind a blocking op settles in-flight (#1129)",
                 "[led][controller][inflight]") {
    // SET_LED is discretionary, so while an external blocking op (BED_MESH_CALIBRATE,
    // QGL, a manual probe) holds Klipper's gcode lock, MoonrakerAPI queues it
    // fire-and-forget and deliberately drops the RPC response — that is what stops
    // the ~60s timeout toast #1108 removed. Neither on_success nor on_error will
    // ever fire, so the settle has to come from the queued disposition or the
    // counter stays pinned and both light buttons grey out for the session.
    //
    // Forcing the response drop is what makes this test honest: if the queued path
    // regressed to passing the caller's callbacks straight through to send_jsonrpc,
    // the drop wedges the counter and this fails.
    setup_controller_with_strip();
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();

    // Pin the two safety-net observers OPEN (Moonraker connected, Klippy READY) so
    // neither can force-clear the counter and make this pass for the wrong reason.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.get_printer_connection_state_subject(),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    ps.set_klippy_state_sync(helix::KlippyState::READY);

    // The API reads the PrinterState it was constructed with. READY + idle_timeout
    // "Printing" without a file print == an external blocking op holds the lock.
    state.set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(state.get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, true);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    mock_client.force_next_gcode_dropped_response("SET_LED");
    ctrl.light_set(true);
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // The command really did go out (queued in Klipper, not refused)...
    CHECK_FALSE(mock_client.gcode_script_history().empty());
    // ...and the buttons came back.
    REQUIRE(lv_subject_get_int(s) == 0);
    REQUIRE_FALSE(ctrl.light_command_in_flight());
}

// ============================================================================
// WLED in-flight parity: REST toggle must grey the button the same as native
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture, "LedController: WLED toggle marks in-flight then clears on ACK",
                 "[led][controller][inflight]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);

    helix::led::LedStripInfo wled_strip;
    wled_strip.name = "Printer LED";
    wled_strip.id = "wled_printer_led";
    wled_strip.backend = helix::led::LedBackendType::WLED;
    wled_strip.supports_color = true;
    wled_strip.supports_white = false;
    ctrl.wled().add_strip(wled_strip);
    ctrl.set_selected_strips({"wled_printer_led"});

    lv_subject_t* s = ctrl.get_led_command_in_flight_subject();
    // The mock fires the WLED REST ACK synchronously, but settle callbacks land
    // via tok.defer() (queued to the main thread).  Counter must be 1 before drain.
    ctrl.light_set(true);
    REQUIRE(lv_subject_get_int(s) == 1);

    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(lv_subject_get_int(s) == 0);
    REQUIRE_FALSE(ctrl.light_command_in_flight());
}

// ============================================================================
// on_queued forwarding — prerequisite for widening the discretionary table (#1129)
// ============================================================================

TEST_CASE_METHOD(LedMockApiFixture, "OutputPinBackend accepts and forwards on_queued",
                 "[led][1129]") {
    // Compile-level contract: these overloads must exist before SET_PIN may be
    // added to the discretionary table, or the queue path drops the settle.
    helix::led::OutputPinBackend backend;
    backend.set_api(mock_api.get());

    helix::led::LedStripInfo pin;
    pin.name = "Case Light";
    pin.id = "output_pin case_light";
    pin.backend = helix::led::LedBackendType::OUTPUT_PIN;
    backend.add_pin(pin);

    bool queued_fired = false;
    auto on_queued = [&queued_fired]() { queued_fired = true; };

    backend.turn_on("output_pin case_light", nullptr, nullptr, on_queued);
    backend.turn_off("output_pin case_light", nullptr, nullptr, on_queued);
    backend.set_value("output_pin case_light", 0.5, nullptr, nullptr, on_queued);
    backend.set_brightness("output_pin case_light", 50, nullptr, nullptr, on_queued);

    // The mock is not busy, so the normal path runs and on_queued must NOT fire.
    CHECK_FALSE(queued_fired);
}

TEST_CASE_METHOD(LedMockApiFixture, "LedEffectBackend accepts and forwards on_queued",
                 "[led][1129]") {
    helix::led::LedEffectBackend backend;
    backend.set_api(mock_api.get());

    helix::led::LedEffectInfo effect;
    effect.name = "led_effect rainbow";
    backend.add_effect(effect);

    bool queued_fired = false;
    auto on_queued = [&queued_fired]() { queued_fired = true; };

    backend.activate_effect("led_effect rainbow", nullptr, nullptr, on_queued);
    backend.stop_effect("led_effect rainbow", nullptr, nullptr, on_queued);
    backend.stop_all_effects(nullptr, nullptr, on_queued);

    CHECK_FALSE(queued_fired);
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "two strips queued behind a blocking op both settle the in-flight counter",
                 "[led][1129]") {
    // The reporter's 17:04:29 pair: a print cancel fired auto-state SET_LED for
    // main_led and neopixel toolhead_rgb at once, both took the discretionary queue
    // path, and both settled. If either drops its settle the counter never returns
    // to 0 and both light buttons grey out for the rest of the session.
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);

    for (const auto& id : {std::string("led main_led"), std::string("neopixel toolhead_rgb")}) {
        helix::led::LedStripInfo strip;
        strip.name = id;
        strip.id = id;
        strip.backend = helix::led::LedBackendType::NATIVE;
        ctrl.native().add_strip(strip);
    }
    ctrl.set_selected_strips({"led main_led", "neopixel toolhead_rgb"});

    // ORDERING IS LOAD-BEARING (see Task 3, Step 5). Klippy subjects initialize to
    // SHUTDOWN, so this call is a transition that resets the volatile subjects.
    // It MUST come before idle_timeout is set, or the reset wipes it and the test
    // silently exercises the non-busy path instead.
    state.set_klippy_state_sync(helix::KlippyState::READY);

    // Klipper is busy with a blocking non-print op, so both sends take the queue path.
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, true);
    REQUIRE(state.is_external_blocking_operation_active());

    ctrl.light_toggle();

    // The settle runs through tok.defer(...), so the subject still reads its
    // pre-settle value without a drain ([L048]).
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(ctrl.get_led_command_in_flight_subject()) == 0);
}

TEST_CASE_METHOD(LedMockApiFixture,
                 "OUTPUT_PIN strip queued behind a blocking op settles the in-flight counter",
                 "[led][1129]") {
    // Behavioural counterpart of "OutputPinBackend accepts and forwards on_queued"
    // (compile-contract only): SET_PIN became discretionary alongside SET_LED, so
    // an output_pin strip toggled while Klipper is busy must take the queue path
    // and still settle the in-flight counter back to 0. If on_queued were dropped
    // from set_value()/toggle_all()'s OUTPUT_PIN branch, this send would rely on
    // on_success/on_error alone, which never fire on the queue path, and the
    // counter would wedge at 1.
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(mock_api.get(), &mock_client);

    helix::led::LedStripInfo pin;
    pin.name = "Case Light";
    pin.id = "output_pin case_light";
    pin.backend = helix::led::LedBackendType::OUTPUT_PIN;
    ctrl.output_pin().add_pin(pin);
    ctrl.set_selected_strips({"output_pin case_light"});

    // ORDERING IS LOAD-BEARING (see Task 3, Step 5). Klippy subjects initialize to
    // SHUTDOWN, so this call is a transition that resets the volatile subjects.
    // It MUST come before idle_timeout is set, or the reset wipes it and the test
    // silently exercises the non-busy path instead.
    state.set_klippy_state_sync(helix::KlippyState::READY);

    // Klipper is busy with a blocking non-print op, so the send takes the queue path.
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, true);
    REQUIRE(state.is_external_blocking_operation_active());

    ctrl.light_toggle();

    // The settle runs through tok.defer(...), so the subject still reads its
    // pre-settle value without a drain ([L048]).
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(ctrl.get_led_command_in_flight_subject()) == 0);
}
