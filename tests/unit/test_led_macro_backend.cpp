// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include <functional>
#include <utility>

#include "../catch_amalgamated.hpp"

TEST_CASE("MacroBackend: an action it cannot send reports an error", "[led][macro]") {
    helix::led::MacroBackend backend;
    helix::led::LedMacroInfo onoff;
    onoff.display_name = "Cabinet Light";
    onoff.on_macro = "LIGHTS_ON";
    onoff.off_macro = "LIGHTS_OFF";
    backend.add_macro(onoff);
    helix::led::LedMacroInfo toggle;
    toggle.display_name = "Light Toggle";
    toggle.toggle_macro = "TOGGLE_LIGHT";
    backend.add_macro(toggle);
    helix::led::LedMacroInfo empty; // neither on_macro nor toggle_macro
    empty.display_name = "Custom";
    backend.add_macro(empty);

    using Send =
        std::function<void(helix::led::MacroBackend&, helix::led::NativeBackend::ErrorCallback)>;
    const std::pair<const char*, Send> cases[] = {
        {"on, no API", [](auto& b, auto err) { b.execute_on("Cabinet Light", nullptr, err); }},
        {"off, no API", [](auto& b, auto err) { b.execute_off("Cabinet Light", nullptr, err); }},
        {"toggle, no API",
         [](auto& b, auto err) { b.execute_toggle("Light Toggle", nullptr, err); }},
        {"custom action, no API",
         [](auto& b, auto err) { b.execute_custom_action("LED_PARTY", nullptr, err); }},
        {"on, no on or toggle macro",
         [](auto& b, auto err) { b.execute_on("Custom", nullptr, err); }},
        {"on, unknown macro", [](auto& b, auto err) { b.execute_on("NonExistent", nullptr, err); }},
    };
    for (const auto& [name, send] : cases) {
        INFO(name);
        bool error_called = false;
        send(backend, [&](const std::string&) { error_called = true; });
        CHECK(error_called);
    }
}

TEST_CASE("MacroBackend: null callbacks don't crash", "[led][macro]") {
    helix::led::MacroBackend backend;

    REQUIRE_NOTHROW(backend.execute_on("NonExistent", nullptr, nullptr));
    REQUIRE_NOTHROW(backend.execute_off("NonExistent", nullptr, nullptr));
    REQUIRE_NOTHROW(backend.execute_toggle("NonExistent", nullptr, nullptr));
    REQUIRE_NOTHROW(backend.execute_custom_action("LED_PARTY", nullptr, nullptr));
}

TEST_CASE("MacroBackend: type is MACRO", "[led][macro]") {
    helix::led::MacroBackend backend;
    REQUIRE(backend.type() == helix::led::LedBackendType::MACRO);
}

TEST_CASE("MacroBackend: macro with presets", "[led][macro]") {
    helix::led::MacroBackend backend;

    helix::led::LedMacroInfo macro;
    macro.display_name = "LED Modes";
    macro.type = helix::led::MacroLedType::PRESET;
    macro.presets = {"LED_PARTY", "LED_NIGHTLIGHT"};
    backend.add_macro(macro);

    REQUIRE(backend.macros().size() == 1);
    REQUIRE(backend.macros()[0].type == helix::led::MacroLedType::PRESET);
    REQUIRE(backend.macros()[0].presets.size() == 2);
    REQUIRE(backend.macros()[0].presets[0] == "LED_PARTY");
    REQUIRE(backend.macros()[0].presets[1] == "LED_NIGHTLIGHT");
}

TEST_CASE("MacroLedType: ON_OFF type has on/off macros", "[led][macro]") {
    helix::led::LedMacroInfo info;
    info.display_name = "Case Light";
    info.type = helix::led::MacroLedType::ON_OFF;
    info.on_macro = "CASELIGHT_ON";
    info.off_macro = "CASELIGHT_OFF";

    REQUIRE(info.type == helix::led::MacroLedType::ON_OFF);
    REQUIRE(!info.on_macro.empty());
    REQUIRE(!info.off_macro.empty());
    REQUIRE(info.toggle_macro.empty());
    REQUIRE(info.presets.empty());
}

TEST_CASE("pretty_print_macro: formats macro names for display", "[led][macro]") {
    using helix::led::pretty_print_macro;

    // Strip LED_ prefix and title-case
    REQUIRE(pretty_print_macro("LED_PARTY_MODE") == "Party Mode");

    // Strip LIGHT_ prefix
    REQUIRE(pretty_print_macro("LIGHT_DIM") == "Dim");

    // Strip STATUS_LED_ prefix
    REQUIRE(pretty_print_macro("STATUS_LED_READY") == "Ready");

    // No prefix to strip
    REQUIRE(pretty_print_macro("CASELIGHT_ON") == "Caselight On");

    // Already short name
    REQUIRE(pretty_print_macro("LED_ON") == "On");

    // Single word after prefix
    REQUIRE(pretty_print_macro("LED_BREATHE") == "Breathe");

    // Empty string
    REQUIRE(pretty_print_macro("") == "");
}

TEST_CASE("MacroLedType: TOGGLE type has toggle macro", "[led][macro]") {
    helix::led::LedMacroInfo info;
    info.display_name = "Chamber LEDs";
    info.type = helix::led::MacroLedType::TOGGLE;
    info.toggle_macro = "CHAMBER_LIGHTS";

    REQUIRE(info.type == helix::led::MacroLedType::TOGGLE);
    REQUIRE(info.toggle_macro == "CHAMBER_LIGHTS");
    REQUIRE(info.on_macro.empty());
    REQUIRE(info.off_macro.empty());
}

// ============================================================================
// Macro field resolution (settings editor)
//
// The editor pairs a dropdown of detected macros with a free-text box. Keyword
// detection will never cover every naming scheme, so the text box is what makes
// a missed macro reachable at all.
// ============================================================================

TEST_CASE("resolve_macro_field: typed text wins over the dropdown", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF"};
    // Dropdown still points at row 0, but the user typed something the detector
    // never offered. Honouring the dropdown here is what made a detection miss
    // unrecoverable.
    REQUIRE(helix::led::resolve_macro_field("MY_STRIP_GLOW", 0, discovered) == "MY_STRIP_GLOW");
}

TEST_CASE("resolve_macro_field: falls back to the dropdown when nothing is typed", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF"};
    REQUIRE(helix::led::resolve_macro_field("", 1, discovered) == "LED_OFF");
}

TEST_CASE("resolve_macro_field: whitespace-only text is not a value", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF"};
    REQUIRE(helix::led::resolve_macro_field("   \t ", 0, discovered) == "LED_ON");
}

TEST_CASE("resolve_macro_field: typed text is trimmed", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON"};
    REQUIRE(helix::led::resolve_macro_field("  GLOW_UP \n", 0, discovered) == "GLOW_UP");
}

TEST_CASE("resolve_macro_field: Custom selected with nothing typed yields empty", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF"};
    const int custom = helix::led::macro_custom_index(discovered);
    REQUIRE(helix::led::resolve_macro_field("", custom, discovered).empty());
}

TEST_CASE("resolve_macro_field: works with no detected macros at all", "[led][macro]") {
    const std::vector<std::string> none;
    REQUIRE(helix::led::resolve_macro_field("HAND_TYPED", 0, none) == "HAND_TYPED");
    REQUIRE(helix::led::resolve_macro_field("", 0, none).empty());
}

TEST_CASE("macro_field_view: a detected macro selects its dropdown row", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF", "LED_PARTY"};
    auto v = helix::led::macro_field_view("LED_PARTY", discovered);
    REQUIRE(v.dropdown_index == 2);
    REQUIRE(v.typed.empty());
}

TEST_CASE("macro_field_view: an undetected macro is preserved in the text box", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF"};
    // Regression: this used to resolve to row 0, so pressing Save rewrote the
    // device's macro to LED_ON -- a different macro, with no warning.
    auto v = helix::led::macro_field_view("CHAMBER_GLOW", discovered);
    REQUIRE(v.dropdown_index == helix::led::macro_custom_index(discovered));
    REQUIRE(v.typed == "CHAMBER_GLOW");

    // And the round-trip must not change it.
    REQUIRE(helix::led::resolve_macro_field(v.typed, v.dropdown_index, discovered) ==
            "CHAMBER_GLOW");
}

TEST_CASE("macro_field_view: an empty field defaults to the first row", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON"};
    auto v = helix::led::macro_field_view("", discovered);
    REQUIRE(v.dropdown_index == 0);
    REQUIRE(v.typed.empty());
}

TEST_CASE("macro_field_view: round-trips every detected macro unchanged", "[led][macro]") {
    const std::vector<std::string> discovered = {"LED_ON", "LED_OFF", "LAMP_TOGGLE"};
    for (const auto& m : discovered) {
        auto v = helix::led::macro_field_view(m, discovered);
        REQUIRE(helix::led::resolve_macro_field(v.typed, v.dropdown_index, discovered) == m);
    }
}
