// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("LedEffectBackend: with no API, activate and stop report an error", "[led][effects]") {
    helix::led::LedEffectBackend backend;
    int errors = 0;
    auto count = [&](const std::string&) { ++errors; };
    backend.activate_effect("led_effect breathing", nullptr, count);
    backend.stop_all_effects(nullptr, count);
    CHECK(errors == 2);

    // With no callbacks either, nothing is left to report to.
    REQUIRE_NOTHROW(backend.activate_effect("led_effect breathing", nullptr, nullptr));
    REQUIRE_NOTHROW(backend.stop_all_effects(nullptr, nullptr));
}

TEST_CASE("LedEffectBackend: type is LED_EFFECT", "[led][effects]") {
    helix::led::LedEffectBackend backend;
    REQUIRE(backend.type() == helix::led::LedBackendType::LED_EFFECT);
}

TEST_CASE("LedEffectBackend: effect management", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    REQUIRE(!backend.is_available());

    helix::led::LedEffectInfo effect;
    effect.name = "led_effect breathing";
    effect.display_name = "Breathing";
    effect.icon_hint = "air";
    backend.add_effect(effect);

    REQUIRE(backend.is_available());
    REQUIRE(backend.effects().size() == 1);
    REQUIRE(backend.effects()[0].name == "led_effect breathing");

    backend.clear();
    REQUIRE(!backend.is_available());
}

TEST_CASE("LedEffectBackend: icon hint mapping comprehensive", "[led][effects]") {
    using EB = helix::led::LedEffectBackend;

    // Breathing/pulse
    REQUIRE(EB::icon_hint_for_effect("breathing") == "air");
    REQUIRE(EB::icon_hint_for_effect("slow_pulse") == "air");
    REQUIRE(EB::icon_hint_for_effect("BREATHING_FAST") == "air");

    // Fire/flame
    REQUIRE(EB::icon_hint_for_effect("fire") == "local_fire_department");
    REQUIRE(EB::icon_hint_for_effect("flame_effect") == "local_fire_department");

    // Rainbow
    REQUIRE(EB::icon_hint_for_effect("rainbow") == "palette");
    REQUIRE(EB::icon_hint_for_effect("rainbow_chase") == "palette");

    // Chase/comet
    REQUIRE(EB::icon_hint_for_effect("chase") == "fast_forward");
    REQUIRE(EB::icon_hint_for_effect("comet_tail") == "fast_forward");

    // Static
    REQUIRE(EB::icon_hint_for_effect("static_white") == "lightbulb");

    // Default
    REQUIRE(EB::icon_hint_for_effect("unknown_effect") == "auto_awesome");
    REQUIRE(EB::icon_hint_for_effect("my_custom") == "auto_awesome");
}

TEST_CASE("LedEffectBackend: display name conversion", "[led][effects]") {
    using EB = helix::led::LedEffectBackend;

    SECTION("strips led_effect prefix") {
        REQUIRE(EB::display_name_for_effect("led_effect breathing") == "Breathing");
    }

    SECTION("replaces underscores and title cases") {
        REQUIRE(EB::display_name_for_effect("led_effect fire_comet") == "Fire Comet");
    }

    SECTION("handles name without prefix") {
        REQUIRE(EB::display_name_for_effect("rainbow_chase") == "Rainbow Chase");
    }

    SECTION("handles empty string") {
        REQUIRE(EB::display_name_for_effect("") == "");
    }

    SECTION("single word") {
        REQUIRE(EB::display_name_for_effect("led_effect fire") == "Fire");
    }
}

TEST_CASE("LedEffectBackend: set_effect_targets assigns targets", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    backend.add_effect(e1);

    helix::led::LedEffectInfo e2;
    e2.name = "led_effect rainbow";
    e2.display_name = "Rainbow";
    backend.add_effect(e2);

    // Set targets
    backend.set_effect_targets("led_effect breathing", {"neopixel chamber_light"});
    backend.set_effect_targets("led_effect rainbow", {"neopixel status_led"});

    REQUIRE(backend.effects()[0].target_leds.size() == 1);
    REQUIRE(backend.effects()[0].target_leds[0] == "neopixel chamber_light");
    REQUIRE(backend.effects()[1].target_leds.size() == 1);
    REQUIRE(backend.effects()[1].target_leds[0] == "neopixel status_led");
}

TEST_CASE("LedEffectBackend: set_effect_targets on unknown effect is safe", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.target_leds = {"neopixel chamber_light"};
    backend.add_effect(e1);

    backend.set_effect_targets("led_effect nonexistent", {"neopixel foo"});

    // No effect is created for the unknown name and the known one keeps its targets.
    REQUIRE(backend.effects().size() == 1);
    REQUIRE(backend.effects()[0].target_leds == std::vector<std::string>{"neopixel chamber_light"});
}

TEST_CASE("LedEffectBackend: effects_for_strip filters by target", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    e1.target_leds = {"neopixel chamber_light"};
    backend.add_effect(e1);

    helix::led::LedEffectInfo e2;
    e2.name = "led_effect rainbow";
    e2.display_name = "Rainbow";
    e2.target_leds = {"neopixel status_led"};
    backend.add_effect(e2);

    helix::led::LedEffectInfo e3;
    e3.name = "led_effect static_white";
    e3.display_name = "Static White";
    e3.target_leds = {"neopixel chamber_light"};
    backend.add_effect(e3);

    SECTION("filters to chamber_light effects") {
        auto filtered = backend.effects_for_strip("neopixel chamber_light");
        REQUIRE(filtered.size() == 2);
        REQUIRE(filtered[0].name == "led_effect breathing");
        REQUIRE(filtered[1].name == "led_effect static_white");
    }

    SECTION("filters to status_led effects") {
        auto filtered = backend.effects_for_strip("neopixel status_led");
        REQUIRE(filtered.size() == 1);
        REQUIRE(filtered[0].name == "led_effect rainbow");
    }

    SECTION("unknown strip returns empty") {
        auto filtered = backend.effects_for_strip("neopixel unknown");
        REQUIRE(filtered.empty());
    }
}

TEST_CASE("LedEffectBackend: effects_for_strip includes effects with no targets",
          "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    e1.target_leds = {"neopixel chamber_light"};
    backend.add_effect(e1);

    helix::led::LedEffectInfo e2;
    e2.name = "led_effect global_glow";
    e2.display_name = "Global Glow";
    // No target_leds set - should appear for any strip
    backend.add_effect(e2);

    auto filtered = backend.effects_for_strip("neopixel chamber_light");
    REQUIRE(filtered.size() == 2);

    auto filtered2 = backend.effects_for_strip("neopixel other_strip");
    REQUIRE(filtered2.size() == 1);
    REQUIRE(filtered2[0].name == "led_effect global_glow");
}

TEST_CASE("LedEffectBackend: parse_klipper_led_target conversions", "[led][effects]") {
    using EB = helix::led::LedEffectBackend;

    SECTION("basic colon to space") {
        REQUIRE(EB::parse_klipper_led_target("neopixel:chamber_light") == "neopixel chamber_light");
    }

    SECTION("strips LED range suffix with space") {
        REQUIRE(EB::parse_klipper_led_target("neopixel:chamber_light (1-10)") ==
                "neopixel chamber_light");
    }

    SECTION("strips LED range suffix without space") {
        REQUIRE(EB::parse_klipper_led_target("neopixel:chamber_light(1-10)") ==
                "neopixel chamber_light");
    }

    SECTION("dotstar type") {
        REQUIRE(EB::parse_klipper_led_target("dotstar:my_strip") == "dotstar my_strip");
    }

    SECTION("led type") {
        REQUIRE(EB::parse_klipper_led_target("led:my_led") == "led my_led");
    }

    SECTION("no colon passes through") {
        REQUIRE(EB::parse_klipper_led_target("already_formatted") == "already_formatted");
    }

    SECTION("empty string") {
        REQUIRE(EB::parse_klipper_led_target("") == "");
    }
}

TEST_CASE("LedEffectBackend: update_from_status tracks enabled state", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    backend.add_effect(e1);

    helix::led::LedEffectInfo e2;
    e2.name = "led_effect fire_comet";
    e2.display_name = "Fire Comet";
    backend.add_effect(e2);

    SECTION("initially all effects disabled") {
        REQUIRE(!backend.is_effect_enabled("led_effect breathing"));
        REQUIRE(!backend.is_effect_enabled("led_effect fire_comet"));
    }

    SECTION("status update enables specific effect") {
        nlohmann::json status = {
            {"led_effect breathing", {{"enabled", true}, {"run_complete", false}}}};
        backend.update_from_status(status);

        REQUIRE(backend.is_effect_enabled("led_effect breathing"));
        REQUIRE(!backend.is_effect_enabled("led_effect fire_comet"));
    }

    SECTION("status update disables effect") {
        // First enable
        nlohmann::json enable_status = {{"led_effect breathing", {{"enabled", true}}}};
        backend.update_from_status(enable_status);
        REQUIRE(backend.is_effect_enabled("led_effect breathing"));

        // Then disable
        nlohmann::json disable_status = {{"led_effect breathing", {{"enabled", false}}}};
        backend.update_from_status(disable_status);
        REQUIRE(!backend.is_effect_enabled("led_effect breathing"));
    }

    SECTION("status update with multiple effects") {
        nlohmann::json status = {{"led_effect breathing", {{"enabled", true}}},
                                 {"led_effect fire_comet", {{"enabled", true}}}};
        backend.update_from_status(status);

        REQUIRE(backend.is_effect_enabled("led_effect breathing"));
        REQUIRE(backend.is_effect_enabled("led_effect fire_comet"));
    }

    SECTION("status with unknown effect is ignored safely") {
        nlohmann::json status = {{"led_effect unknown_effect", {{"enabled", true}}}};
        backend.update_from_status(status);
        // Unknown effects are ignored
        REQUIRE(!backend.is_effect_enabled("led_effect unknown_effect"));
    }

    SECTION("status without enabled field is ignored") {
        nlohmann::json status = {{"led_effect breathing", {{"frame_rate", 24.0}}}};
        backend.update_from_status(status);
        // enabled not present, should stay at default (false)
        REQUIRE(!backend.is_effect_enabled("led_effect breathing"));
    }

    SECTION("is_effect_enabled returns false for unknown effect") {
        REQUIRE(!backend.is_effect_enabled("led_effect nonexistent"));
    }
}

TEST_CASE("LedEffectBackend: enabled state survives clear and re-add", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    backend.add_effect(e1);

    nlohmann::json status = {{"led_effect breathing", {{"enabled", true}}}};
    backend.update_from_status(status);
    REQUIRE(backend.is_effect_enabled("led_effect breathing"));

    // Clear resets everything
    backend.clear();
    REQUIRE(!backend.is_effect_enabled("led_effect breathing"));

    // Re-add starts fresh (disabled)
    backend.add_effect(e1);
    REQUIRE(!backend.is_effect_enabled("led_effect breathing"));
}

TEST_CASE("LedEffectBackend: effects() reflects enabled state", "[led][effects]") {
    helix::led::LedEffectBackend backend;

    helix::led::LedEffectInfo e1;
    e1.name = "led_effect breathing";
    e1.display_name = "Breathing";
    backend.add_effect(e1);

    REQUIRE(!backend.effects()[0].enabled);

    nlohmann::json status = {{"led_effect breathing", {{"enabled", true}}}};
    backend.update_from_status(status);

    REQUIRE(backend.effects()[0].enabled);
}
