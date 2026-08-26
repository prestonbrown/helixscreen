// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "color_utils.h"
#include "config.h"
#include "led/led_controller.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

/// Every TEST_CASE here drives LedController::init(), whose connection-state
/// observer defers its first notification through the UpdateQueue. With no
/// fixture at all these files returned with that work still queued and handed it
/// to whichever test drained next (prestonbrown/helixscreen#1167). The drain sits
/// in the derived destructor body so it runs while the controller and its
/// subjects are still alive, before HelixTestFixture's own teardown.
struct LedConfigFixture : public HelixTestFixture {
    ~LedConfigFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// Clear all LED-related Config paths to prevent cross-test contamination.
/// Tests run in random order and the Config singleton persists between tests.
static void clear_led_config_paths() {
    auto* cfg = Config::get_instance();
    if (!cfg)
        return;
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->set(cfg->df() + "leds/strip", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->set(cfg->df() + "leds/last_brightness", nlohmann::json());
    cfg->set(cfg->df() + "leds/color_presets", nlohmann::json());
    cfg->set(cfg->df() + "leds/macro_devices", nlohmann::json());
    cfg->set(cfg->df() + "leds/auto_paired_bases", nlohmann::json());
    cfg->set(cfg->df() + "leds/led_on_at_start", nlohmann::json());
    cfg->save();
}
TEST_CASE_METHOD(LedConfigFixture, "LedController config: default values after init",
                 "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: set and get last_color",
                 "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_last_color(0xFF0000);
    REQUIRE(ctrl.last_color() == 0xFF0000);

    ctrl.set_last_color(0x00FF00);
    REQUIRE(ctrl.last_color() == 0x00FF00);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: set and get last_brightness",
                 "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.set_last_brightness(75);
    REQUIRE(ctrl.last_brightness() == 75);

    ctrl.set_last_brightness(0);
    REQUIRE(ctrl.last_brightness() == 0);

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: set and get selected_strips",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: set and get color_presets",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: configured macros round-trip",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: deinit resets config state to defaults",
                 "[led][config]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
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
    clear_led_config_paths();

    // After deinit, re-init should restore defaults
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.last_color() == 0xFFFFFF);
    REQUIRE(ctrl.last_brightness() == 100);
    REQUIRE(ctrl.selected_strips().empty());
    REQUIRE(ctrl.color_presets().size() == 8); // Default presets restored
    REQUIRE(ctrl.configured_macros().empty());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: default presets have correct values",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: paths use df() + leds/ prefix",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: legacy /printer/leds/selected migration",
                 "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate old SettingsManager data at df()+"leds/selected" (JSON array)
    nlohmann::json legacy_selected = nlohmann::json::array({"neopixel legacy_led"});
    cfg->set(cfg->df() + "leds/selected", legacy_selected);

    // Make sure new-style selected_strips is empty
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
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

TEST_CASE_METHOD(LedConfigFixture,
                 "LedController config: legacy /printer/leds/strip string migration",
                 "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate oldest format: single string at df()+"leds/strip"
    cfg->set<std::string>(cfg->df() + "leds/strip", "neopixel oldest_led");

    // Make sure newer formats are empty
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json());
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

TEST_CASE_METHOD(LedConfigFixture,
                 "LedController config: wizard saves both strip and selected_strips",
                 "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate wizard behavior: saves to both leds/strip (string for dropdown
    // restore) and leds/selected_strips (array for LedController)
    cfg->set<std::string>(cfg->df() + "leds/strip", "output_pin LED");
    nlohmann::json strips = nlohmann::json::array();
    strips.push_back("output_pin LED");
    cfg->set(cfg->df() + "leds/selected_strips", strips);
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // selected_strips should read from the canonical array path
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "output_pin LED");

    // Cleanup
    cfg->set<std::string>(cfg->df() + "leds/strip", "");
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture,
                 "LedController config: selected_strips takes priority over legacy strip",
                 "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Both paths set with different values — selected_strips should win
    cfg->set<std::string>(cfg->df() + "leds/strip", "neopixel old_led");
    nlohmann::json strips = nlohmann::json::array();
    strips.push_back("output_pin NEW_LED");
    cfg->set(cfg->df() + "leds/selected_strips", strips);
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // Should use selected_strips, NOT the legacy strip value
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "output_pin NEW_LED");

    // Cleanup
    cfg->set<std::string>(cfg->df() + "leds/strip", "");
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: wizard None selection saves empty array",
                 "[led][config]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    // Simulate wizard saving "None" — empty string and empty array
    cfg->set<std::string>(cfg->df() + "leds/strip", "");
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->save();

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.selected_strips().empty());

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: led_on_at_start save/load round-trip",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: macro_devices save/load at new path",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: hex string colors saved to config",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: loads hex string colors from config",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: loads legacy integer colors from config",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "LedController config: mixed integer and hex string presets",
                 "[led][config]") {
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

TEST_CASE_METHOD(LedConfigFixture, "color_to_hex_string produces correct output",
                 "[color][utils]") {
    REQUIRE(helix::color_to_hex_string(0xFFFFFF) == "#FFFFFF");
    REQUIRE(helix::color_to_hex_string(0xFF0000) == "#FF0000");
    REQUIRE(helix::color_to_hex_string(0x00FF00) == "#00FF00");
    REQUIRE(helix::color_to_hex_string(0x0000FF) == "#0000FF");
    REQUIRE(helix::color_to_hex_string(0x000000) == "#000000");
    REQUIRE(helix::color_to_hex_string(0xAABBCC) == "#AABBCC");
}

// The legacy top-level /led -> printers/<id>/leds fold moved out of
// LedController::load_config() into migrate_v19_to_v20() (config.cpp), where it
// runs once instead of on every boot — see tests/unit/test_config_null_pollution.cpp
// for its coverage. What stays LedController's job is preferring the per-printer
// path over the older per-printer legacy formats.
TEST_CASE_METHOD(LedConfigFixture,
                 "LedController config: per-printer selected_strips wins over legacy formats",
                 "[led][config][integration]") {
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();

    cfg->set(cfg->df() + "leds/selected", nlohmann::json::array({"neopixel OLD"}));
    cfg->set<std::string>(cfg->df() + "leds/strip", "neopixel OLDEST");
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array({"neopixel NEW"}));
    cfg->set(cfg->df() + "leds/last_color", static_cast<int>(0x222222));
    cfg->save();

    ctrl.init(nullptr, nullptr);

    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel NEW");
    REQUIRE(ctrl.last_color() == 0x222222);

    // Cleanup
    cfg->set(cfg->df() + "leds/selected", nlohmann::json());
    cfg->set(cfg->df() + "leds/strip", nlohmann::json());
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array());
    cfg->set(cfg->df() + "leds/last_color", nlohmann::json());
    cfg->save();

    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController config: draft macro is never persisted",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);

    helix::led::LedMacroInfo named;
    named.display_name = "Cabinet Light";
    named.type = helix::led::MacroLedType::ON_OFF;
    named.on_macro = "LED_ON";
    named.off_macro = "LED_OFF";

    // The blank row the + Add button appends. It has to live in memory so the
    // editor can render it, but it must not reach settings.json -- an unnamed
    // entry reloads as a permanently broken device the user cannot address.
    helix::led::LedMacroInfo draft;
    draft.type = helix::led::MacroLedType::ON_OFF;

    ctrl.set_configured_macros({named, draft});
    REQUIRE(ctrl.configured_macros().size() == 2);

    ctrl.save_config();

    const auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    const nlohmann::json* saved =
        Config::get_instance()->try_get_json(Config::get_instance()->df() + "leds/macro_devices");
    REQUIRE(saved != nullptr);
    REQUIRE(saved->is_array());
    REQUIRE(saved->size() == 1);
    REQUIRE((*saved)[0].value("name", "") == "Cabinet Light");

    // And the reload agrees: the draft is gone, the real device came back.
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Cabinet Light");

    ctrl.deinit();
    clear_led_config_paths();
}

// ============================================================================
// Auto-pairing <BASE>_ON / <BASE>_OFF
//
// Detection used to stop at "candidate", so a printer whose lights are pure
// macros showed nothing in the LED panel until the user hand-built a device.
// A clean ON/OFF pair is unambiguous, so seed it. The seeded bases are recorded
// so deleting the device is not undone by the next discovery.
// ============================================================================

static helix::PrinterDiscovery discovery_with(const std::vector<std::string>& macros) {
    helix::PrinterDiscovery d;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& m : macros) {
        objects.push_back("gcode_macro " + m);
    }
    d.parse_objects(objects);
    return d;
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: auto-pairs LED_ON/LED_OFF into a device",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.configured_macros().empty());

    auto d = discovery_with({"LED_ON", "LED_OFF"});
    ctrl.discover_from_hardware(d);

    REQUIRE(ctrl.configured_macros().size() == 1);
    const auto& m = ctrl.configured_macros()[0];
    CHECK(m.type == helix::led::MacroLedType::ON_OFF);
    CHECK(m.on_macro == "LED_ON");
    CHECK(m.off_macro == "LED_OFF");
    CHECK(!m.display_name.empty());

    // It has to be usable, not just present.
    REQUIRE(ctrl.macro().is_available());
    const auto strips = ctrl.all_selectable_strips();
    REQUIRE(strips.size() == 1);
    CHECK(strips[0].backend == helix::led::LedBackendType::MACRO);

    ctrl.deinit();
    clear_led_config_paths();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: an unpaired ON macro is not seeded",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);

    // No matching _OFF, so the pairing is a guess -- leave it as a candidate.
    auto d = discovery_with({"LED_ON", "LIGHT_TOGGLE", "LAMP_PARTY"});
    ctrl.discover_from_hardware(d);

    CHECK(ctrl.configured_macros().empty());
    CHECK(ctrl.discovered_macros().size() == 3);

    ctrl.deinit();
    clear_led_config_paths();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: deleting an auto-paired device sticks",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);

    auto d = discovery_with({"LED_ON", "LED_OFF"});
    ctrl.discover_from_hardware(d);
    REQUIRE(ctrl.configured_macros().size() == 1);

    // User deletes it.
    ctrl.set_configured_macros({});
    ctrl.save_config();
    REQUIRE(ctrl.configured_macros().empty());

    // Rediscovery must not resurrect it -- that is the whole point of recording
    // which bases have already been offered.
    ctrl.discover_from_hardware(d);
    CHECK(ctrl.configured_macros().empty());

    // And it survives a restart.
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(d);
    CHECK(ctrl.configured_macros().empty());

    ctrl.deinit();
    clear_led_config_paths();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: auto-pairing never claims a user's macro",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);

    // The user already wired LED_ON/LED_OFF into a device of their own.
    helix::led::LedMacroInfo mine;
    mine.display_name = "My Lights";
    mine.type = helix::led::MacroLedType::ON_OFF;
    mine.on_macro = "LED_ON";
    mine.off_macro = "LED_OFF";
    ctrl.set_configured_macros({mine});

    auto d = discovery_with({"LED_ON", "LED_OFF"});
    ctrl.discover_from_hardware(d);

    REQUIRE(ctrl.configured_macros().size() == 1);
    CHECK(ctrl.configured_macros()[0].display_name == "My Lights");

    ctrl.deinit();
    clear_led_config_paths();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: auto-pairing is idempotent across rediscovery",
                 "[led][config][macro]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);

    auto d = discovery_with({"CHAMBER_LIGHT_ON", "CHAMBER_LIGHT_OFF"});
    ctrl.discover_from_hardware(d);
    ctrl.discover_from_hardware(d);
    ctrl.discover_from_hardware(d);

    REQUIRE(ctrl.configured_macros().size() == 1);
    CHECK(ctrl.configured_macros()[0].on_macro == "CHAMBER_LIGHT_ON");
    CHECK(ctrl.configured_macros()[0].off_macro == "CHAMBER_LIGHT_OFF");

    ctrl.deinit();
    clear_led_config_paths();
}
