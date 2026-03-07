// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_utils.h"
#include "config.h"
#include "led/led_controller.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
TEST_CASE("LedController config: default values after init", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.last_color() == 0xFFFFFF);
    REQUIRE(ctrl.last_brightness() == 100);
    REQUIRE(ctrl.selected_strips().empty());
    // Default presets loaded during init->load_config
    REQUIRE(ctrl.color_presets().size() == 8);
    REQUIRE(ctrl.color_presets()[0] == 0xFFFFFF);
    REQUIRE(ctrl.color_presets()[1] == 0xFFD700);
    REQUIRE(ctrl.configured_macros().empty());

    ctrl.deinit();
}

TEST_CASE("LedController config: set and get last_color", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_last_color(0xFF0000);
    REQUIRE(ctrl.last_color() == 0xFF0000);

    ctrl.set_last_color(0x00FF00);
    REQUIRE(ctrl.last_color() == 0x00FF00);

    ctrl.deinit();
}

TEST_CASE("LedController config: set and get last_brightness", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_last_brightness(75);
    REQUIRE(ctrl.last_brightness() == 75);

    ctrl.set_last_brightness(0);
    REQUIRE(ctrl.last_brightness() == 0);

    ctrl.deinit();
}

TEST_CASE("LedController config: set and get selected_strips", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    std::vector<std::string> strips = {"neopixel chamber", "dotstar status"};
    ctrl.set_selected_strips(strips);
    REQUIRE(ctrl.selected_strips().size() == 2);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel chamber");
    REQUIRE(ctrl.selected_strips()[1] == "dotstar status");

    ctrl.deinit();
}

TEST_CASE("LedController config: set and get color_presets", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    std::vector<uint32_t> presets = {0xFF0000, 0x00FF00, 0x0000FF};
    ctrl.set_color_presets(presets);
    REQUIRE(ctrl.color_presets().size() == 3);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[2] == 0x0000FF);

    ctrl.deinit();
}

TEST_CASE("LedController config: configured macros round-trip", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    std::vector<helix::led::LedMacroInfo> macros;
    helix::led::LedMacroInfo m;
    m.display_name = "Cabinet Light";
    m.on_macro = "LIGHTS_ON";
    m.off_macro = "LIGHTS_OFF";
    m.toggle_macro = "";
    m.type = helix::led::MacroLedType::PRESET;
    m.presets = {"LED_PARTY", "LED_DIM"};
    macros.push_back(m);

    helix::led::LedMacroInfo m2;
    m2.display_name = "Status LED";
    m2.type = helix::led::MacroLedType::TOGGLE;
    m2.toggle_macro = "STATUS_TOGGLE";
    macros.push_back(m2);

    ctrl.set_configured_macros(macros);
    REQUIRE(ctrl.configured_macros().size() == 2);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Cabinet Light");
    REQUIRE(ctrl.configured_macros()[0].on_macro == "LIGHTS_ON");
    REQUIRE(ctrl.configured_macros()[0].off_macro == "LIGHTS_OFF");
    REQUIRE(ctrl.configured_macros()[0].type == helix::led::MacroLedType::PRESET);
    REQUIRE(ctrl.configured_macros()[0].presets.size() == 2);
    REQUIRE(ctrl.configured_macros()[0].presets[0] == "LED_PARTY");
    REQUIRE(ctrl.configured_macros()[0].presets[1] == "LED_DIM");
    REQUIRE(ctrl.configured_macros()[1].display_name == "Status LED");
    REQUIRE(ctrl.configured_macros()[1].type == helix::led::MacroLedType::TOGGLE);
    REQUIRE(ctrl.configured_macros()[1].toggle_macro == "STATUS_TOGGLE");

    ctrl.deinit();
}

TEST_CASE("LedController config: deinit resets config state to defaults", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Modify state
    ctrl.set_last_color(0xFF0000);
    ctrl.set_last_brightness(50);
    ctrl.set_selected_strips({"neopixel test"});
    ctrl.set_color_presets({0xABCDEF});

    helix::led::LedMacroInfo m;
    m.display_name = "Test";
    m.toggle_macro = "TEST_MACRO";
    ctrl.set_configured_macros({m});

    REQUIRE(ctrl.last_color() == 0xFF0000);
    REQUIRE(ctrl.last_brightness() == 50);
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.color_presets().size() == 1);
    REQUIRE(ctrl.configured_macros().size() == 1);

    ctrl.deinit();

    // After deinit, re-init should restore defaults
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.last_color() == 0xFFFFFF);
    REQUIRE(ctrl.last_brightness() == 100);
    REQUIRE(ctrl.selected_strips().empty());
    REQUIRE(ctrl.color_presets().size() == 8); // Default presets restored
    REQUIRE(ctrl.configured_macros().empty());

    ctrl.deinit();
}

TEST_CASE("LedController config: default presets have correct values", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    auto& presets = ctrl.color_presets();
    REQUIRE(presets.size() == 8);
    REQUIRE(presets[0] == 0xFFFFFF); // White
    REQUIRE(presets[1] == 0xFFD700); // Gold
    REQUIRE(presets[2] == 0xFF6B35); // Orange
    REQUIRE(presets[3] == 0x4FC3F7); // Light Blue
    REQUIRE(presets[4] == 0xFF4444); // Red
    REQUIRE(presets[5] == 0x66BB6A); // Green
    REQUIRE(presets[6] == 0x9C27B0); // Purple
    REQUIRE(presets[7] == 0x00BCD4); // Cyan

    ctrl.deinit();
}

TEST_CASE("LedController config: paths use df() + leds/ prefix", "[led][config]") {
    // This test verifies that after save + reload, data persists under the new paths
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_selected_strips({"neopixel test_strip"});
    ctrl.set_last_color(0xAABBCC);
    ctrl.set_last_brightness(42);
    ctrl.save_config();

    // Verify config was written to new paths
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    auto& strips_json = cfg->get_json(cfg->df() + "leds/selected_strips");
    REQUIRE(strips_json.is_array());
    REQUIRE(strips_json.size() == 1);
    REQUIRE(strips_json[0].get<std::string>() == "neopixel test_strip");

    REQUIRE(cfg->get<std::string>(cfg->df() + "leds/last_color", "") == "#AABBCC");
    REQUIRE(cfg->get<int>(cfg->df() + "leds/last_brightness", 0) == 42);

    // Reload and verify
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel test_strip");
    REQUIRE(ctrl.last_color() == 0xAABBCC);
    REQUIRE(ctrl.last_brightness() == 42);

    // Cleanup
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/last_color", 0xFFFFFF);
    cfg->set(cfg->df() + "leds/last_brightness", 100);
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: migration from old /led/ paths", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Write data to OLD paths (simulating pre-migration config)
    nlohmann::json old_strips = nlohmann::json::array({"neopixel old_strip"});
    cfg->set("/led/selected_strips", old_strips);
    cfg->set("/led/last_color", static_cast<int>(0x112233));
    cfg->set("/led/last_brightness", 77);

    nlohmann::json old_presets =
        nlohmann::json::array({static_cast<int>(0xFF0000), static_cast<int>(0x00FF00)});
    cfg->set("/led/color_presets", old_presets);
    cfg->save();

    // Clear new paths to simulate first run after update
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_brightness", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    // Init should migrate old -> new
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel old_strip");
    REQUIRE(ctrl.last_color() == 0x112233);
    REQUIRE(ctrl.last_brightness() == 77);
    REQUIRE(ctrl.color_presets().size() == 2);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);

    // Cleanup old and new paths
    cfg->set("/led/selected_strips", nlohmann::json());
    cfg->set("/led/last_color", nlohmann::json());
    cfg->set("/led/last_brightness", nlohmann::json());
    cfg->set("/led/color_presets", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_brightness", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: legacy /printer/leds/selected migration", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate old SettingsManager data at df()+"leds/selected" (JSON array)
    nlohmann::json legacy_selected = nlohmann::json::array({"neopixel legacy_led"});
    cfg->set(cfg->df() + "leds/selected", legacy_selected);

    // Make sure new-style selected_strips is empty
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
    cfg->set("/led/selected_strips", nlohmann::json());
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Should have migrated legacy selected -> selected_strips
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel legacy_led");

    // Cleanup
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: legacy /printer/leds/strip string migration", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate oldest format: single string at df()+"leds/strip"
    cfg->set<std::string>(cfg->df() + "leds/strip", "neopixel oldest_led");

    // Make sure newer formats are empty
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
    cfg->set("/led/selected_strips", nlohmann::json());
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Should have migrated string -> array in selected_strips
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel oldest_led");

    // Cleanup
    cfg->set<std::string>(cfg->df() + "leds/strip", "");
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: led_on_at_start save/load round-trip", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Default is false
    REQUIRE(ctrl.get_led_on_at_start() == false);

    // Set and save
    ctrl.set_led_on_at_start(true);
    ctrl.save_config();

    // Reload
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.get_led_on_at_start() == true);

    // Reset for other tests
    ctrl.set_led_on_at_start(false);
    ctrl.save_config();

    ctrl.deinit();
}

TEST_CASE("LedController config: macro_devices save/load at new path", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    helix::led::LedMacroInfo m;
    m.display_name = "Test Macro";
    m.type = helix::led::MacroLedType::ON_OFF;
    m.on_macro = "TEST_ON";
    m.off_macro = "TEST_OFF";
    ctrl.set_configured_macros({m});
    ctrl.save_config();

    // Verify saved to new path
    auto* cfg = Config::get_instance();
    auto& macros_json = cfg->get_json(cfg->df() + "leds/macro_devices");
    REQUIRE(macros_json.is_array());
    REQUIRE(macros_json.size() == 1);
    REQUIRE(macros_json[0]["name"] == "Test Macro");

    // Reload
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Test Macro");

    // Cleanup
    cfg->set(cfg->df() + "leds/macro_devices", nlohmann::json::array());
    cfg->save();

    ctrl.deinit();
}

// ============================================================================
// Integration test: end-to-end config migration chain
// ============================================================================

TEST_CASE("LedController config: full migration chain end-to-end", "[led][config][integration]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();

    // --- Setup: write data to ALL old config paths ---

    // Old /led/ prefix paths (Phase 1 migration source)
    nlohmann::json old_strips = nlohmann::json::array({"neopixel migration_test"});
    cfg->set("/led/selected_strips", old_strips);
    cfg->set("/led/last_color", static_cast<int>(0xAA5500));
    cfg->set("/led/last_brightness", 65);

    nlohmann::json old_presets = nlohmann::json::array(
        {static_cast<int>(0xFF0000), static_cast<int>(0x00FF00), static_cast<int>(0x0000FF)});
    cfg->set("/led/color_presets", old_presets);

    nlohmann::json old_macros = nlohmann::json::array();
    {
        nlohmann::json m;
        m["name"] = "Migration Macro";
        m["type"] = "toggle";
        m["toggle_macro"] = "MIGRATE_TOGGLE";
        old_macros.push_back(m);
    }
    cfg->set("/led/macro_devices", old_macros);

    // NOTE: auto_state migration (/led/auto_state/ → /printer/leds/auto_state/)
    // is handled by LedAutoState::load_config(), not LedController::load_config().
    // Tested separately in LedAutoState tests.

    // --- Clear ALL new paths to simulate fresh upgrade ---
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_brightness", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->set(cfg->df() + "leds/macro_devices", nlohmann::json());
    cfg->save();

    // --- Init LedController (triggers migration) ---
    ctrl.init(nullptr, nullptr);

    // --- Verify all data migrated to new paths ---
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel migration_test");
    REQUIRE(ctrl.last_color() == 0xAA5500);
    REQUIRE(ctrl.last_brightness() == 65);
    REQUIRE(ctrl.color_presets().size() == 3);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[1] == 0x00FF00);
    REQUIRE(ctrl.color_presets()[2] == 0x0000FF);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Migration Macro");
    REQUIRE(ctrl.configured_macros()[0].toggle_macro == "MIGRATE_TOGGLE");

    // --- Re-init to verify idempotent (no double migration) ---
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel migration_test");
    REQUIRE(ctrl.last_color() == 0xAA5500);
    REQUIRE(ctrl.last_brightness() == 65);
    REQUIRE(ctrl.color_presets().size() == 3);
    REQUIRE(ctrl.configured_macros().size() == 1);

    // --- Cleanup all paths ---
    cfg->set("/led/selected_strips", nlohmann::json());
    cfg->set("/led/last_color", nlohmann::json());
    cfg->set("/led/last_brightness", nlohmann::json());
    cfg->set("/led/color_presets", nlohmann::json());
    cfg->set("/led/macro_devices", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_brightness", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->set(cfg->df() + "leds/macro_devices", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/led_on_at_start", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: hex string colors saved to config", "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_last_color(0xFF0000);
    ctrl.set_color_presets({0x00FF00, 0x0000FF});
    ctrl.save_config();

    auto* cfg = Config::get_instance();

    // Verify saved as hex strings, not integers
    auto& color_json = cfg->get_json(cfg->df() + "leds/last_color");
    REQUIRE(color_json.is_string());
    REQUIRE(color_json.get<std::string>() == "#FF0000");

    auto& presets_json = cfg->get_json(cfg->df() + "leds/color_presets");
    REQUIRE(presets_json.is_array());
    REQUIRE(presets_json.size() == 2);
    REQUIRE(presets_json[0].is_string());
    REQUIRE(presets_json[0].get<std::string>() == "#00FF00");
    REQUIRE(presets_json[1].get<std::string>() == "#0000FF");

    // Cleanup
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: loads hex string colors from config", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Write hex string colors to config
    cfg->set<std::string>(cfg->df() + "leds/last_color", "#AABB00");
    nlohmann::json presets = nlohmann::json::array({"#FF0000", "#00FF00", "#0000FF"});
    cfg->set(cfg->df() + "leds/color_presets", presets);
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.last_color() == 0xAABB00);
    REQUIRE(ctrl.color_presets().size() == 3);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[1] == 0x00FF00);
    REQUIRE(ctrl.color_presets()[2] == 0x0000FF);

    // Cleanup
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: loads legacy integer colors from config", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Write old-style integer colors
    cfg->set(cfg->df() + "leds/last_color", static_cast<int>(0xFF8800));
    nlohmann::json presets =
        nlohmann::json::array({static_cast<int>(0xFF0000), static_cast<int>(0x00FF00)});
    cfg->set(cfg->df() + "leds/color_presets", presets);
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.last_color() == 0xFF8800);
    REQUIRE(ctrl.color_presets().size() == 2);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[1] == 0x00FF00);

    // Cleanup
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("LedController config: mixed integer and hex string presets", "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Mix of old integers and new hex strings
    nlohmann::json presets = nlohmann::json::array();
    presets.push_back("#FF0000");
    presets.push_back(static_cast<int>(0x00FF00));
    presets.push_back("#0000FF");
    cfg->set(cfg->df() + "leds/color_presets", presets);
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.color_presets().size() == 3);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[1] == 0x00FF00);
    REQUIRE(ctrl.color_presets()[2] == 0x0000FF);

    // Cleanup
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE("color_to_hex_string produces correct output", "[color][utils]") {
    REQUIRE(helix::color_to_hex_string(0xFFFFFF) == "#FFFFFF");
    REQUIRE(helix::color_to_hex_string(0xFF0000) == "#FF0000");
    REQUIRE(helix::color_to_hex_string(0x00FF00) == "#00FF00");
    REQUIRE(helix::color_to_hex_string(0x0000FF) == "#0000FF");
    REQUIRE(helix::color_to_hex_string(0x000000) == "#000000");
    REQUIRE(helix::color_to_hex_string(0xAABBCC) == "#AABBCC");
}

TEST_CASE("LedController config: migration skips when new paths already populated",
          "[led][config][integration]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();

    // Write data to BOTH old and new paths (new should take priority)
    nlohmann::json old_strips = nlohmann::json::array({"neopixel OLD"});
    cfg->set("/led/selected_strips", old_strips);
    cfg->set("/led/last_color", static_cast<int>(0x111111));

    nlohmann::json new_strips = nlohmann::json::array({"neopixel NEW"});
    cfg->set(cfg->df() + "leds/selected_strips", new_strips);
    cfg->set(cfg->df() + "leds/last_color", static_cast<int>(0x222222));
    cfg->save();

    ctrl.init(nullptr, nullptr);

    // New paths should NOT be overwritten by old data
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel NEW");
    REQUIRE(ctrl.last_color() == 0x222222);

    // Cleanup
    cfg->set("/led/selected_strips", nlohmann::json());
    cfg->set("/led/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}
