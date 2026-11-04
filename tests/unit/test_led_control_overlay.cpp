// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_control_overlay.cpp
 * @brief The LEDs overlay model: which device is focused, the subjects its page
 * publishes, and that every control acts on the focused device alone.
 *
 * @see ui_led_control_overlay.h
 */

#include "ui_color_picker.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_auto_state.h"
#include "led/led_backend.h"
#include "led/led_controller.h"
#include "led/led_device_page.h"
#include "led/ui_led_control_overlay.h"
#include "lvgl/src/widgets/label/lv_label_private.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

// Must live in namespace helix::led to match the friend declaration.
namespace helix::led {
class LedControlOverlayTestAccess {
  public:
    LedControlOverlayTestAccess() {
        overlay_.init_subjects();
    }

    /// Opens the overlay on @p requested the way open_led_control_overlay() does,
    /// without building or pushing a widget tree.
    void activate(const std::string& requested) {
        overlay_.request_focus(requested);
        overlay_.on_activate();
    }

    void tap_tab(int i) {
        overlay_.handle_tab_clicked(i);
    }
    [[nodiscard]] std::string focused() const {
        return overlay_.focused_device();
    }
    [[nodiscard]] static int int_subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    [[nodiscard]] static std::string str_subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    void tap_swatch(int i) {
        overlay_.handle_swatch(i);
    }
    void tap_power() {
        overlay_.handle_power();
    }
    void drag_brightness(int pct) {
        overlay_.handle_brightness(pct);
    }
    void tap_white(int tone) {
        overlay_.handle_white(tone);
    }
    void tap_list_chip(int i) {
        overlay_.handle_list_chip(i);
    }
    void tap_level(int pct) {
        overlay_.handle_brightness(pct);
    }
    void tap_macro_on() {
        overlay_.handle_macro_on();
    }
    void tap_macro_off() {
        overlay_.handle_macro_off();
    }
    void tap_macro_toggle() {
        overlay_.handle_macro_toggle();
    }
    void tap_effects_none() {
        overlay_.handle_effects_none();
    }

  private:
    helix::led::LedControlOverlay overlay_;
};
} // namespace helix::led

using helix::led::LedControlOverlayTestAccess;

namespace {

LedStripInfo make_native_strip(const std::string& id, bool supports_color, bool supports_white) {
    LedStripInfo strip;
    strip.name = id;
    strip.id = id;
    strip.backend = LedBackendType::NATIVE;
    strip.supports_color = supports_color;
    strip.supports_white = supports_white;
    return strip;
}

/// Channel-wise equality within the rounding a status round-trip introduces.
bool within_rounding(uint32_t a, uint32_t b) {
    for (int shift : {16, 8, 0}) {
        const int d = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
        if (d < -2 || d > 2) {
            return false;
        }
    }
    return true;
}

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

// A controller wired to a mock API with a live connection, so commands reach
// the mock wire and the native color cache.
struct LedApplyColorFixture : public LVGLTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedApplyColorFixture() {
        state.init_subjects(false);
        // execute_gcode()'s halted gate rejects every command while klippy reads
        // SHUTDOWN, and the global connection state gates dispatch too.
        state.set_klippy_state_sync(helix::KlippyState::READY);
        auto& ps = get_printer_state();
        lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                           static_cast<int>(helix::ConnectionState::CONNECTED));
        ps.set_klippy_state_sync(helix::KlippyState::READY);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);

        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);
        drain();
        mock_client.clear_gcode_script_history();
    }

    ~LedApplyColorFixture() override {
        drain();
        LedAutoState::instance().set_strips({});
        LedController::instance().deinit();
    }

    /// Adds a native strip and selects nothing.
    void add_native(const std::string& id, bool color, bool white) {
        LedController::instance().native().add_strip(make_native_strip(id, color, white));
    }

    void add_effect(const std::string& name, const std::string& target, bool enabled) {
        LedEffectInfo e;
        e.name = name;
        e.display_name = LedEffectBackend::display_name_for_effect(name);
        e.target_leds = {target};
        e.enabled = enabled;
        LedController::instance().effects().add_effect(e);
    }

    void set_macros(const std::vector<LedMacroInfo>& macros) {
        auto& ctrl = LedController::instance();
        ctrl.set_configured_macros(macros);
        ctrl.rebuild_macro_backend();
    }

    static LedMacroInfo lamp_macro() {
        LedMacroInfo m;
        m.display_name = "Lamp";
        m.type = MacroLedType::ON_OFF;
        m.on_macro = "LIGHTS_ON";
        m.off_macro = "LIGHTS_OFF";
        return m;
    }

    static LedMacroInfo party_macro() {
        LedMacroInfo m;
        m.display_name = "Party";
        m.type = MacroLedType::PRESET;
        m.presets = {"LED_PARTY", "LED_RAINBOW"};
        return m;
    }

    [[nodiscard]] helix::led::NativeBackend::StripColor sent(const std::string& id) const {
        return LedController::instance().native().get_strip_color(id);
    }

    [[nodiscard]] bool wire_mentions(const std::string& needle) const {
        const auto h = mock_client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&needle](const std::string& s) {
            return s.find(needle) != std::string::npos;
        });
    }

    static nlohmann::json leds_config() {
        auto* cfg = Config::get_instance();
        const auto* node = cfg->try_get_json(cfg->df() + "leds");
        return node != nullptr ? *node : nlohmann::json();
    }
};

} // namespace

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a tab tap changes focus and nothing else",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    // Auto-state names a different device than the tab tapped below, so a tap
    // that wrote it would show.
    LedAutoState::instance().set_strips({"neopixel chamber_light"});
    const nlohmann::json before = leds_config();

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("");
    REQUIRE(access.focused() == "neopixel chamber_light");
    access.tap_tab(1);

    CHECK(access.focused() == "neopixel sb_leds");
    CHECK(access.int_subject("led_focused_tab") == 1);
    CHECK(LedAutoState::instance().strips() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(leds_config() == before);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: opens on the requested device", "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel sb_leds");

    CHECK(access.focused() == "neopixel sb_leds");
    CHECK(access.int_subject("led_focused_tab") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: opens on the last focused device",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("");
    access.tap_tab(1);
    access.activate("");

    CHECK(access.focused() == "neopixel sb_leds");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: falls back to the chamber light",
                 "[led][overlay]") {
    add_native("neopixel sb_leds", true, false);
    add_native("neopixel chamber_light", true, true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel nonexistent");

    CHECK(access.focused() == "neopixel chamber_light");
    CHECK(access.int_subject("led_focused_tab") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a vanished focused device refocuses the chamber light",
                 "[led][overlay]") {
    add_native("neopixel sb_leds", true, false);
    add_native("neopixel chamber_light", true, true);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("macro:Lamp");
    REQUIRE(access.focused() == "macro:Lamp");

    set_macros({});
    access.activate("");

    CHECK(access.focused() == "neopixel chamber_light");
    CHECK(access.int_subject("led_tab_count") == 2);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: overlay with no devices", "[led][overlay]") {
    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("");

    CHECK(access.int_subject("led_tab_count") == 0);
    CHECK(access.focused().empty());
    access.tap_power();
    access.drag_brightness(50);
    access.tap_swatch(0);
    access.tap_white(1);
    access.tap_list_chip(0);
    access.tap_effects_none();
    CHECK(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: page subjects follow the classifier",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_effect("led_effect rainbow", "neopixel chamber_light", false);
    set_macros({party_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");

    CHECK(access.int_subject("led_page_lamp") == static_cast<int>(LampControl::PowerAndBrightness));
    CHECK(access.int_subject("led_page_white") == static_cast<int>(WhiteMode::WChannel));
    CHECK(access.int_subject("led_page_color_vis") == 1);
    CHECK(access.int_subject("led_page_list") == static_cast<int>(ListKind::Effects));
    CHECK(access.int_subject("led_chip_count") == 1);
    CHECK(access.str_subject("led_chip_label_0") == "Rainbow");
    CHECK(access.int_subject("led_swatch_count") == 7);

    access.activate("macro:Party");

    CHECK(access.int_subject("led_page_lamp") == static_cast<int>(LampControl::None));
    CHECK(access.int_subject("led_page_white") == static_cast<int>(WhiteMode::None));
    CHECK(access.int_subject("led_page_color_vis") == 0);
    CHECK(access.int_subject("led_page_list") == static_cast<int>(ListKind::Presets));
    CHECK(access.int_subject("led_chip_count") == 2);
    CHECK(access.str_subject("led_chip_label_0") == "Party");
    CHECK(access.str_subject("led_chip_label_1") == "Rainbow");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: tabs carry each device's display name",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("");

    CHECK(access.int_subject("led_tab_count") == 2);
    CHECK(access.str_subject("led_tab_name_0") == "Chamber Light");
    CHECK(access.str_subject("led_tab_name_1") == "Lamp");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: tab dots follow device state", "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("");
    REQUIRE(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::Unknown));

    LedController::instance().update_from_status(
        {{"neopixel sb_leds", {{"color_data", {{0.5, 0.5, 0.5}}}}}});
    drain();

    CHECK(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::On));
    CHECK(access.int_subject("led_tab_dot_2") == static_cast<int>(PowerState::Unknown));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a state bump updates dots but not the slider",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.4, 0.4, 0.4, 0.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");
    REQUIRE(access.int_subject("led_page_brightness") == 40);

    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.9, 0.9, 0.9, 0.0}}}}},
                             {"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0}}}}}});
    drain();

    CHECK(access.int_subject("led_page_brightness") == 40);
    CHECK(access.str_subject("led_page_brightness_text") == "40%");
    CHECK(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::On));

    // The next control acts at the brightness the slider shows.
    access.tap_swatch(0);
    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel chamber_light").decompose(base, pct, white);
    CHECK(pct == 40);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: controls act on the focused device only",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    access.tap_swatch(0);

    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel strip_a").decompose(base, pct, white);
    CHECK(within_rounding(base, LedController::instance().color_presets()[0]));
    CHECK(white == Catch::Approx(0.0).margin(0.001));
    CHECK_FALSE(LedController::instance().native().has_strip_color("neopixel strip_b"));
    CHECK(access.int_subject("led_selected_swatch") == 0);

    drain();
    mock_client.clear_gcode_script_history();
    access.tap_power();
    drain();

    CHECK(wire_mentions("strip_a"));
    CHECK_FALSE(wire_mentions("strip_b"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a swatch keeps the brightness and clears W",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedController::instance().update_from_status(
        {{"neopixel chamber_light", {{"color_data", {{0.0, 0.0, 0.0, 0.6}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");
    REQUIRE(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    access.tap_swatch(4);

    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel chamber_light").decompose(base, pct, white);
    CHECK(within_rounding(base, LedController::instance().color_presets()[4]));
    CHECK(pct == 60);
    CHECK(white == Catch::Approx(0.0).margin(0.001));
    CHECK(access.int_subject("led_page_white_sel") == -1);
    CHECK(access.int_subject("led_selected_swatch") == 4);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the white section sets W on RGBW",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");
    access.tap_white(static_cast<int>(WhiteTone::Neutral));

    auto c = sent("neopixel chamber_light");
    CHECK(c.w > 0.0);
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
    CHECK(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    CHECK(access.int_subject("led_selected_swatch") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a single-channel strip dims through W",
                 "[led][overlay]") {
    add_native("led case_light", false, false);
    LedController::instance().update_from_status(
        {{"led case_light", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("led case_light");
    access.drag_brightness(15);

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(0.15).margin(0.001));
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
    CHECK(access.str_subject("led_page_brightness_text") == "15%");
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a level chip sets brightness on a white-only strip", "[led][overlay]") {
    add_native("led case_light", false, false);
    LedController::instance().update_from_status(
        {{"led case_light", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("led case_light");
    REQUIRE(access.int_subject("led_page_list") == static_cast<int>(ListKind::LevelChips));
    REQUIRE(access.int_subject("led_page_level") == 100);

    access.tap_level(25);
    CHECK(access.int_subject("led_page_level") == 25);
    CHECK(sent("led case_light").w == Catch::Approx(0.25).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the None chip stops the focused strip's effect",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", true);
    add_effect("led_effect sparkle", "neopixel strip_b", true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == 1);

    access.tap_effects_none();
    CHECK(access.int_subject("led_active_chip") == -1);
    drain();

    CHECK(wire_mentions("EFFECT=fire STOP=1"));
    CHECK_FALSE(wire_mentions("EFFECT=sparkle"));
    CHECK_FALSE(wire_mentions("EFFECT=breathe"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an effect chip activates that effect",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == -1);

    access.tap_list_chip(1);
    CHECK(access.int_subject("led_active_chip") == 1);
    drain();

    CHECK(wire_mentions("EFFECT=fire"));
    CHECK_FALSE(wire_mentions("EFFECT=breathe"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a macro preset chip runs that macro",
                 "[led][overlay]") {
    set_macros({party_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("macro:Party");
    access.tap_list_chip(1);
    drain();

    CHECK(wire_mentions("LED_RAINBOW"));
    CHECK_FALSE(wire_mentions("LED_PARTY"));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: an off white-only strip after a red look puts it all on W",
                 "[led][overlay]") {
    add_native("led case_light", /*color=*/false, /*white=*/true);
    auto& ctrl = LedController::instance();
    ctrl.set_last_color(0xFF4444);
    ctrl.set_last_white(0.0);
    ctrl.set_last_brightness(60);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("led case_light");
    REQUIRE_FALSE(ctrl.native().has_strip_color("led case_light"));
    access.tap_level(100);

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(1.0).margin(0.001));
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: an off RGB-only strip after a W-only look lights RGB white",
                 "[led][overlay]") {
    add_native("neopixel rgb", true, false);
    auto& ctrl = LedController::instance();
    ctrl.set_last_color(0);
    ctrl.set_last_white(1.0);
    ctrl.set_last_brightness(80);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel rgb");
    REQUIRE(access.int_subject("led_page_brightness") == 80);
    access.drag_brightness(50);

    auto c = sent("neopixel rgb");
    CHECK(c.r == Catch::Approx(0.5).margin(0.005));
    CHECK(c.g == Catch::Approx(0.5).margin(0.005));
    CHECK(c.b == Catch::Approx(0.5).margin(0.005));
    CHECK(c.w == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an output pin's slider sets that pin",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedStripInfo pin;
    pin.name = "Enclosure";
    pin.id = "output_pin enclosure";
    pin.backend = LedBackendType::OUTPUT_PIN;
    pin.supports_color = false;
    pin.supports_white = false;
    pin.is_pwm = true;
    LedController::instance().output_pin().add_pin(pin);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("output_pin enclosure");
    access.drag_brightness(40);
    drain();

    CHECK(wire_mentions("SET_PIN PIN=enclosure VALUE=0.4000"));
    CHECK_FALSE(wire_mentions("SET_LED"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: macro buttons run the focused device's macros",
                 "[led][overlay]") {
    LedMacroInfo toggle;
    toggle.display_name = "Toggle";
    toggle.type = MacroLedType::TOGGLE;
    toggle.toggle_macro = "LIGHT_TOGGLE";
    set_macros({lamp_macro(), toggle});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("macro:Lamp");
    CHECK(access.str_subject("led_page_note") == "ON: LIGHTS_ON | OFF: LIGHTS_OFF");

    access.tap_macro_on();
    drain();
    CHECK(wire_mentions("LIGHTS_ON"));
    CHECK_FALSE(wire_mentions("LIGHTS_OFF"));

    mock_client.clear_gcode_script_history();
    access.tap_macro_off();
    drain();
    CHECK(wire_mentions("LIGHTS_OFF"));
    CHECK_FALSE(wire_mentions("LIGHTS_ON"));

    access.activate("macro:Toggle");
    CHECK(access.str_subject("led_page_note") == "TOGGLE: LIGHT_TOGGLE");
    mock_client.clear_gcode_script_history();
    access.tap_macro_toggle();
    drain();
    CHECK(wire_mentions("LIGHT_TOGGLE"));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a macro button press is what a light button toggles from",
                 "[led][overlay]") {
    LedMacroInfo toggle;
    toggle.display_name = "Toggle";
    toggle.type = MacroLedType::TOGGLE;
    toggle.toggle_macro = "LIGHT_TOGGLE";
    set_macros({lamp_macro(), toggle});
    auto& ctrl = LedController::instance();

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("macro:Lamp");
    access.tap_macro_on();
    drain();
    mock_client.clear_gcode_script_history();
    CHECK_FALSE(ctrl.toggle_power({"macro:Lamp"}));
    drain();
    CHECK(wire_mentions("LIGHTS_OFF"));

    // A TOGGLE lamp last sent off stays off when everything is switched off.
    access.activate("macro:Toggle");
    access.tap_macro_toggle();
    drain();
    CHECK_FALSE(ctrl.toggle_power({"macro:Toggle"}));
    drain();
    mock_client.clear_gcode_script_history();
    ctrl.set_power({"macro:Toggle"}, false);
    drain();
    CHECK_FALSE(wire_mentions("LIGHT_TOGGLE"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the active effect chip follows status",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == -1);

    LedController::instance().update_from_status({{"led_effect fire", {{"enabled", true}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == 1);

    LedController::instance().update_from_status({{"led_effect fire", {{"enabled", false}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an unrelated frame keeps a tapped effect chip",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    LedController::instance().update_from_status(
        {{"neopixel strip_b", {{"color_data", {{1.0, 0.0, 0.0}}}}}});
    drain();

    CHECK(access.int_subject("led_active_chip") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a frame naming another running effect replaces the tapped chip",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    LedController::instance().update_from_status({{"led_effect breathe", {{"enabled", true}}}});
    drain();

    CHECK(access.int_subject("led_active_chip") == 0);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a confirming frame ends the pending chip; later frames follow status",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"led_effect fire", {{"enabled", true}}}});
    drain();
    REQUIRE(access.int_subject("led_active_chip") == 1);

    ctrl.update_from_status({{"led_effect fire", {{"enabled", false}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a tapped effect chip no frame answers times out",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel strip_a");
    access.tap_list_chip(0);
    process_lvgl(1000);
    REQUIRE(access.int_subject("led_active_chip") == 0);

    process_lvgl(4000);
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}

// ============================================================================
// The XML view over the model
// ============================================================================

namespace {

/// The overlay built from its real XML, opened on an RGBW strip with one effect.
struct OverlayXmlFixture : public LVGLUITestFixture {
    lv_obj_t* root = nullptr;

    OverlayXmlFixture() {
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(nullptr, nullptr);
        ctrl.native().add_strip(make_native_strip("neopixel chamber_light", true, true));
        ctrl.native().add_strip(make_native_strip("neopixel sb_leds", true, false));
        LedEffectInfo glow;
        glow.name = "led_effect glow";
        glow.display_name = "Glow";
        glow.target_leds = {"neopixel chamber_light"};
        ctrl.effects().add_effect(glow);

        helix::ui::destroy_static_panels();
        drain();
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(test_screen());
        }
        NavigationManager::instance().set_panels(panels.data());
        root = helix::open_led_control_overlay(test_screen(), "neopixel chamber_light");
        drain();
        REQUIRE(root != nullptr);
    }

    ~OverlayXmlFixture() override {
        drain();
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        drain();
        LedController::instance().deinit();
    }

    lv_obj_t* find(const char* name) const {
        return lv_obj_find_by_name(root, name);
    }

    /// find() for a control the test needs: a missing one fails here, naming it,
    /// instead of reaching LVGL as nullptr.
    lv_obj_t* need(const char* name) const {
        lv_obj_t* obj = find(name);
        INFO("ui_xml has no widget named " << name);
        REQUIRE(obj != nullptr);
        return obj;
    }
};

} // namespace

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: every named control exists",
                 "[led][overlay][xml]") {
    for (const char* name : {"led_tab_row",         "led_tab_0",         "led_tab_1",
                             "led_tab_fade",        "led_power_btn",     "led_brightness_slider",
                             "led_white_cool",      "led_white_neutral", "led_white_warm",
                             "led_swatch_list",     "led_swatch_0",      "led_custom_swatch",
                             "led_chip_row",        "led_chip_none",     "led_chip_0",
                             "led_level_10",        "led_level_50",      "led_level_100",
                             "led_macro_on",        "led_macro_off",     "led_macro_toggle",
                             "led_page_note_label", "led_empty_state"}) {
        INFO(name);
        CHECK(find(name) != nullptr);
    }
    CHECK(find("strip_selector_section") == nullptr);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the slider fill takes the page color",
                 "[led][overlay][xml]") {
    lv_obj_t* slider = need("led_brightness_slider");
    REQUIRE(slider != nullptr);
    lv_subject_set_color(lv_xml_get_subject(nullptr, "led_page_color"), lv_color_hex(0xFF4444));
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR)) ==
          lv_color_to_u32(lv_color_hex(0xFF4444)));
}

TEST_CASE_METHOD(OverlayXmlFixture,
                 "overlay XML: preset and Custom swatches paint through the swatch painter",
                 "[led][overlay][xml]") {
    const auto& presets = LedController::instance().color_presets();
    REQUIRE_FALSE(presets.empty());
    lv_obj_t* swatch = need("led_swatch_0");
    REQUIRE(swatch != nullptr);
    lv_obj_t* disc = lv_obj_find_by_name(swatch, "color_swatch_disc");
    REQUIRE(disc != nullptr);
    CHECK(helix::ui::has_swatch_paint(disc));
    CHECK((lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF) == presets[0]);

    // Custom's disc carries the hue wheel the painter draws.
    lv_obj_t* custom_disc = lv_obj_find_by_name(need("led_custom_swatch"), "color_swatch_disc");
    REQUIRE(custom_disc != nullptr);
    CHECK(helix::ui::has_swatch_paint(custom_disc));
    const lv_grad_dsc_t* grad = lv_obj_get_style_bg_grad(custom_disc, LV_PART_MAIN);
    REQUIRE(grad != nullptr);
    CHECK(grad->stops_count > 2);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: a tab click focuses that tab's device",
                 "[led][overlay][xml]") {
    lv_obj_t* tab = need("led_tab_1");
    REQUIRE(tab != nullptr);
    lv_obj_send_event(tab, LV_EVENT_CLICKED, nullptr);
    CHECK(get_led_control_overlay().focused_device() == "neopixel sb_leds");
    CHECK(LedControlOverlayTestAccess::int_subject("led_focused_tab") == 1);
}

TEST_CASE_METHOD(OverlayXmlFixture,
                 "overlay XML: the page shows only the focused device's sections",
                 "[led][overlay][xml]") {
    // RGBW with an effect: lamp, White, Color and the Effects row; no levels, no macro buttons.
    CHECK_FALSE(lv_obj_has_flag(need("led_power_btn"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(need("led_white_section"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(need("led_color_section"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(need("led_chip_none"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(need("led_level_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(need("led_macro_on"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(need("led_empty_state"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the brightness fill is flat-topped inside a pill",
                 "[led][overlay][xml]") {
    lv_obj_t* slider = need("led_brightness_slider");
    REQUIRE(slider != nullptr);
    // lv_bar clips an indicator whose radius is below the track's to the track's
    // rounded shape, so a square fill reads flat on top and round at the bottom.
    CHECK(lv_obj_get_style_radius(slider, LV_PART_INDICATOR) == 0);
    CHECK(lv_obj_get_style_radius(slider, LV_PART_MAIN) > 0);
    lv_obj_t* pct = need("led_brightness_pct");
    REQUIRE(pct != nullptr);
    CHECK(lv_obj_get_style_align(pct, LV_PART_MAIN) == LV_ALIGN_TOP_MID);
}

TEST_CASE_METHOD(OverlayXmlFixture,
                 "overlay XML: the LEDs title keeps its case; other headers do not",
                 "[led][overlay][xml]") {
    lv_obj_t* title = need("header_title");
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "LEDs");
    CHECK(reinterpret_cast<lv_label_t*>(title)->text_transform_upper == 0);

    const char* attrs[] = {"title", "Other", nullptr};
    auto* other = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "header_bar", attrs));
    REQUIRE(other != nullptr);
    lv_obj_t* other_title = lv_obj_find_by_name(other, "header_title");
    REQUIRE(other_title != nullptr);
    CHECK(reinterpret_cast<lv_label_t*>(other_title)->text_transform_upper == 1);
}

namespace {

/// Sends the tone, then feeds back what the strip reports: each channel as the
/// 8-bit level the LED holds, the way a status frame returns it. Reopens the page.
int white_sel_after_readback(LedControlOverlayTestAccess& access, const std::string& id, int tone) {
    access.tap_white(tone);
    const auto c = LedController::instance().native().get_strip_color(id);
    const bool rgbw = find_strip(LedController::instance().native().strips(), id)->supports_white;
    auto held = [](double v) { return std::round(v * 255.0) / 255.0; };
    nlohmann::json data = rgbw ? nlohmann::json::array({held(c.r), held(c.g), held(c.b), held(c.w)})
                               : nlohmann::json::array({held(c.r), held(c.g), held(c.b)});
    LedController::instance().update_from_status({{id, {{"color_data", {data}}}}});
    drain();
    access.activate(id);
    return LedControlOverlayTestAccess::int_subject("led_page_white_sel");
}

} // namespace

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a white read back from the strip rings its tone",
                 "[led][overlay]") {
    add_native("neopixel rgbw", true, true);
    add_native("neopixel rgb", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    // Dim enough that 8-bit levels scaled back to full brightness drift by a
    // few steps: the ring must still find the tone.
    for (int tone = 0; tone < 3; ++tone) {
        INFO("tone " << tone);
        access.activate("neopixel rgbw");
        access.drag_brightness(10);
        CHECK(white_sel_after_readback(access, "neopixel rgbw", tone) == tone);
        access.activate("neopixel rgb");
        access.drag_brightness(10);
        CHECK(white_sel_after_readback(access, "neopixel rgb", tone) == tone);
    }
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: plain RGB white on an RGBW strip rings Neutral, not Custom",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedController::instance().update_from_status(
        {{"neopixel chamber_light", {{"color_data", {{0.5, 0.5, 0.5, 0.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");

    CHECK(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    CHECK(access.int_subject("led_selected_swatch") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a color that is no preset and no white rings Custom", "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    // A red tint under a lit W: neither a white tone nor a preset.
    LedController::instance().update_from_status(
        {{"neopixel chamber_light", {{"color_data", {{0.6, 0.0, 0.0, 0.3}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");

    CHECK(access.int_subject("led_page_white_sel") == -1);
    CHECK(access.int_subject("led_selected_swatch") == -2);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a WLED page notes where its presets come from",
                 "[led][overlay]") {
    LedStripInfo strip;
    strip.id = "printer_led";
    strip.name = "printer_led";
    strip.backend = LedBackendType::WLED;
    strip.supports_color = true;
    strip.supports_white = true;
    LedController::instance().wled().add_strip(strip);
    set_macros({party_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("printer_led");
    CHECK(access.str_subject("led_page_note") ==
          "Presets come from the WLED device. Edit them in the WLED app.");

    access.activate("macro:Party");
    CHECK(access.str_subject("led_page_note").empty());
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: every preset rings itself at any brightness, tapped and read back",
                 "[led][overlay]") {
    add_native("neopixel rgbw", true, true);
    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    const auto presets = LedController::instance().color_presets();
    for (int pct : {100, 50, 10}) {
        for (size_t i = 0; i < presets.size(); ++i) {
            INFO("brightness " << pct << " preset " << i);
            access.activate("neopixel rgbw");
            access.drag_brightness(pct);
            access.tap_swatch(static_cast<int>(i));
            CHECK(access.int_subject("led_selected_swatch") == static_cast<int>(i));

            const auto c = LedController::instance().native().get_strip_color("neopixel rgbw");
            auto held = [](double v) { return std::round(v * 255.0) / 255.0; };
            LedController::instance().update_from_status(
                {{"neopixel rgbw",
                  {{"color_data", {{held(c.r), held(c.g), held(c.b), held(c.w)}}}}}});
            drain();
            access.activate("neopixel rgbw");
            CHECK(access.int_subject("led_selected_swatch") == static_cast<int>(i));
        }
    }
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: plain white on an RGB-only strip rings Neutral",
                 "[led][overlay]") {
    add_native("neopixel rgb", true, false);
    LedController::instance().update_from_status(
        {{"neopixel rgb", {{"color_data", {{1.0, 1.0, 1.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel rgb");

    CHECK(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    CHECK(access.int_subject("led_selected_swatch") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a WLED page is neutral white, not the last strip's color",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedStripInfo strip;
    strip.id = "printer_led";
    strip.name = "printer_led";
    strip.backend = LedBackendType::WLED;
    strip.supports_color = true;
    strip.supports_white = true;
    LedController::instance().wled().add_strip(strip);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access;
    access.activate("neopixel chamber_light");
    access.tap_swatch(0);
    REQUIRE(lv_color_to_u32(lv_subject_get_color(lv_xml_get_subject(nullptr, "led_page_color"))) !=
            lv_color_to_u32(lv_color_hex(0xFFFFFF)));

    access.tap_tab(1);
    REQUIRE(access.focused() == "printer_led");
    CHECK((lv_color_to_u32(lv_subject_get_color(lv_xml_get_subject(nullptr, "led_page_color"))) &
           0xFFFFFF) == 0xFFFFFFu);
    CHECK(LedController::instance().device_state("printer_led").rgb == 0xFFFFFFu);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the underline follows the focused tab",
                 "[led][overlay][xml]") {
    lv_obj_t* tab0 = need("led_tab_0");
    lv_obj_t* tab1 = need("led_tab_1");
    REQUIRE(tab0 != nullptr);
    REQUIRE(tab1 != nullptr);
    CHECK(lv_obj_get_style_border_width(tab0, LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_border_width(tab1, LV_PART_MAIN) == 0);

    lv_obj_send_event(tab1, LV_EVENT_CLICKED, nullptr);
    CHECK(lv_obj_get_style_border_width(tab0, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_border_width(tab1, LV_PART_MAIN) > 0);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: a tab dot shows exactly its device's state",
                 "[led][overlay][xml]") {
    auto visible_dots = [this](const char* tab) {
        lv_obj_t* dot = lv_obj_find_by_name(need(tab), "light_dot");
        REQUIRE(dot != nullptr);
        std::vector<int> shown;
        for (uint32_t i = 0; i < lv_obj_get_child_count(dot); ++i) {
            if (!lv_obj_has_flag(lv_obj_get_child(dot, static_cast<int32_t>(i)),
                                 LV_OBJ_FLAG_HIDDEN)) {
                shown.push_back(static_cast<int>(i));
            }
        }
        return shown;
    };
    // Children: 0 lit, 1 off ring, 2 unknown ring. No state read yet: unknown.
    CHECK(visible_dots("led_tab_1") == std::vector<int>{2});

    LedController::instance().update_from_status(
        {{"neopixel sb_leds", {{"color_data", {{1.0, 0.0, 0.0}}}}}});
    drain();
    CHECK(visible_dots("led_tab_1") == std::vector<int>{0});
    // The lit dot is a colour swatch: the painter draws the LED's colour and
    // owns its edge, so the dot itself carries no border.
    lv_obj_t* lit = lv_obj_find_by_name(need("led_tab_1"), "light_dot_lit");
    REQUIRE(lit != nullptr);
    lv_obj_t* disc = lv_obj_find_by_name(lit, "color_swatch_disc");
    REQUIRE(disc != nullptr);
    CHECK(helix::ui::has_swatch_paint(disc));
    CHECK((lv_color_to_u32(helix::ui::swatch_color_of(lit)) & 0xFFFFFF) == 0xFF0000u);
    CHECK(lv_obj_get_style_border_width(disc, LV_PART_MAIN) == 0);

    LedController::instance().update_from_status(
        {{"neopixel sb_leds", {{"color_data", {{0.0, 0.0, 0.0}}}}}});
    drain();
    CHECK(visible_dots("led_tab_1") == std::vector<int>{1});
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: a swatch row has no dead gaps and no overlap",
                 "[led][overlay][xml]") {
    lv_obj_t* list = need("led_swatch_list");
    lv_obj_t* swatch = need("led_swatch_0");
    REQUIRE(list != nullptr);
    REQUIRE(swatch != nullptr);
    const int32_t gap = lv_obj_get_style_pad_column(list, LV_PART_MAIN);
    REQUIRE(gap > 0);
    auto reach = [](lv_obj_t* obj) {
        lv_area_t click;
        lv_obj_get_click_area(obj, &click);
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        return coords.x1 - click.x1;
    };
    // Each swatch reaches half the gap to each side: together they cover it exactly.
    CHECK(reach(swatch) * 2 == gap);
    CHECK(reach(need("led_custom_swatch")) * 2 == gap);
    CHECK(reach(need("led_white_cool")) * 2 == gap);

    // Chips do the same across their own gap.
    const int32_t chip_gap = lv_obj_get_style_pad_column(need("led_chip_scroller"), LV_PART_MAIN);
    REQUIRE(chip_gap > 0);
    CHECK(reach(need("led_chip_none")) * 2 == chip_gap);
    CHECK(reach(need("led_chip_0")) * 2 == chip_gap);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the selection ring draws inside its swatch",
                 "[led][overlay][xml]") {
    lv_subject_set_int(lv_xml_get_subject(nullptr, "led_selected_swatch"), -2);
    lv_obj_t* custom = need("led_custom_swatch");
    REQUIRE(custom != nullptr);
    // A border, not an outline: nothing past the swatch's own bounds, so no row clips it.
    CHECK(lv_obj_get_style_border_width(custom, LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_outline_width(custom, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_pad_top(custom, LV_PART_MAIN) >=
          lv_obj_get_style_border_width(custom, LV_PART_MAIN));
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the modes sit a step further from the look",
                 "[led][overlay][xml]") {
    const int32_t look_gap = lv_obj_get_style_margin_top(need("led_color_section"), LV_PART_MAIN);
    const int32_t modes_gap = lv_obj_get_style_margin_top(need("led_list_section"), LV_PART_MAIN);
    CHECK(look_gap > 0);
    CHECK(modes_gap > look_gap);
}

TEST_CASE_METHOD(OverlayXmlFixture,
                 "overlay XML: the LED rows and the preset grid use the same swatch component",
                 "[led][overlay][xml][color_swatch]") {
    const char* attrs[] = {"swatch_callback", "led_swatch_cb", nullptr};
    auto* grid = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "color_swatch_grid", attrs));
    REQUIRE(grid != nullptr);
    // 30 presets after the grid's hidden custom-colour lead tile.
    REQUIRE(lv_obj_get_child_count(grid) == 31);

    for (lv_obj_t* swatch : {need("led_swatch_0"), lv_obj_get_child(grid, 1)}) {
        REQUIRE(swatch != nullptr);
        // Built by color_swatch: a transparent ring holder around the named disc.
        lv_obj_t* disc = lv_obj_get_child(swatch, 0);
        REQUIRE(disc != nullptr);
        CHECK(std::string(lv_obj_get_name(disc)) == "color_swatch_disc");
        CHECK(lv_obj_get_style_bg_opa(swatch, LV_PART_MAIN) == LV_OPA_TRANSP);
        CHECK(lv_obj_get_style_pad_top(swatch, LV_PART_MAIN) > 0);
        CHECK(lv_obj_get_style_radius(disc, LV_PART_MAIN) == LV_RADIUS_CIRCLE); // round
        CHECK(helix::ui::has_swatch_paint(disc));
    }
}
