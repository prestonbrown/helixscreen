// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

#include "../catch_amalgamated.hpp"

using helix::favorite_macro_config_from_json;
using helix::favorite_macro_config_to_json;
using helix::FavoriteMacroConfig;

TEST_CASE("favorite macro config round-trips all fields", "[macro][favorite_config]") {
    FavoriteMacroConfig c;
    c.macro = "BED_MESH_CALIBRATE";
    c.icon = "wrench";
    c.color = 0x7B1FA2;
    c.require_confirmation = false;

    auto j = favorite_macro_config_to_json(c);
    auto back = favorite_macro_config_from_json(j);

    REQUIRE(back.macro == "BED_MESH_CALIBRATE");
    REQUIRE(back.icon == "wrench");
    REQUIRE(back.color == 0x7B1FA2u);
    REQUIRE(back.require_confirmation == false);
}

TEST_CASE("favorite macro config omits defaults from json", "[macro][favorite_config]") {
    FavoriteMacroConfig c;
    c.macro = "G28";
    // icon empty, color 0, require_confirmation true (the default) -> omitted

    auto j = favorite_macro_config_to_json(c);
    REQUIRE(j.contains("macro"));
    REQUIRE_FALSE(j.contains("icon"));
    REQUIRE_FALSE(j.contains("color"));
    REQUIRE_FALSE(j.contains("require_confirmation"));
}

TEST_CASE("favorite macro config tolerates wrong json types", "[macro][favorite_config]") {
    nlohmann::json j;
    j["macro"] = 42;                   // wrong type
    j["require_confirmation"] = "yes"; // wrong type
    auto c = favorite_macro_config_from_json(j);
    REQUIRE(c.macro.empty());
    REQUIRE(c.require_confirmation == true); // falls back to the default
}

TEST_CASE("favorite macro config always writes macro key even when empty",
          "[macro][favorite_config]") {
    FavoriteMacroConfig c; // all defaults: empty macro
    auto j = favorite_macro_config_to_json(c);
    REQUIRE(j.contains("macro"));
    REQUIRE(j["macro"] == "");
}

TEST_CASE("favorite macro config rejects negative color", "[macro][favorite_config]") {
    nlohmann::json j;
    j["macro"] = "G28";
    j["color"] = -5;
    auto c = favorite_macro_config_from_json(j);
    REQUIRE(c.color == 0u);
}

// Confirmation defaults ON: a widget config written before the key existed, and
// one that omits it entirely, must both come back requiring confirmation. A
// wrong default here silently disarms every existing macro button.
TEST_CASE("favorite macro config requires confirmation by default", "[macro][favorite_config]") {
    SECTION("empty object") {
        auto c = favorite_macro_config_from_json(nlohmann::json::object());
        REQUIRE(c.require_confirmation == true);
    }
    SECTION("config with other keys but no confirmation key") {
        nlohmann::json j;
        j["macro"] = "G28";
        j["icon"] = "home";
        auto c = favorite_macro_config_from_json(j);
        REQUIRE(c.require_confirmation == true);
    }
}

// Pre-v23 configs stored the inverse switch as "skip_param_prompt". The reader
// keeps honouring it for configs the versioned migration cannot reach (preset
// assets, hand-edited files, an imported widget config).
TEST_CASE("favorite macro config reads the legacy skip_param_prompt key",
          "[macro][favorite_config]") {
    SECTION("legacy true means confirmation off") {
        nlohmann::json j;
        j["macro"] = "LOAD_FILAMENT";
        j["skip_param_prompt"] = true;
        auto c = favorite_macro_config_from_json(j);
        REQUIRE(c.require_confirmation == false);
    }
    SECTION("legacy false means confirmation on") {
        nlohmann::json j;
        j["macro"] = "LOAD_FILAMENT";
        j["skip_param_prompt"] = false;
        auto c = favorite_macro_config_from_json(j);
        REQUIRE(c.require_confirmation == true);
    }
    SECTION("wrong-typed legacy value falls back to the default") {
        nlohmann::json j;
        j["skip_param_prompt"] = "yes";
        auto c = favorite_macro_config_from_json(j);
        REQUIRE(c.require_confirmation == true);
    }
    SECTION("the new key wins when both are present") {
        nlohmann::json j;
        j["skip_param_prompt"] = true; // would mean confirmation off
        j["require_confirmation"] = true;
        auto c = favorite_macro_config_from_json(j);
        REQUIRE(c.require_confirmation == true);
    }
}

// Round-tripping a legacy config must shed the legacy key, so the file converges
// on one spelling instead of carrying both forever.
TEST_CASE("favorite macro config drops the legacy key on write", "[macro][favorite_config]") {
    nlohmann::json legacy;
    legacy["macro"] = "PURGE";
    legacy["skip_param_prompt"] = true;

    auto c = favorite_macro_config_from_json(legacy);
    auto j = favorite_macro_config_to_json(c);

    REQUIRE_FALSE(j.contains("skip_param_prompt"));
    REQUIRE(j.contains("require_confirmation"));
    REQUIRE(j["require_confirmation"] == false);
    REQUIRE(favorite_macro_config_from_json(j).require_confirmation == false);
}
