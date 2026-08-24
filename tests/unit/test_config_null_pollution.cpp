// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression coverage for #1129.
//
// nlohmann's non-const operator[](json_pointer) calls get_and_create(), which
// inserts a null at every missing component of the path. Config::get_json()
// wraps exactly that overload, so any caller using it merely to *probe* for a
// key permanently wrote null nodes into the user's settings.json — most
// visibly a top-level {"led": {"selected_strips": null}} that looked
// authoritative while the real value lived under printers/<id>/leds/.
//
// Two halves are covered here:
//   1. Startup probes must not create nodes (Work items 1/2/4).
//   2. Migration v19 -> v20 must clean up configs that already carry the
//      garbage, folding any real legacy /led values into the active printer
//      first (Work items 3/4).

#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "hardware_validator.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "printer_discovery.h"
#include "settings_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

/// Collect JSON-pointer paths of every null leaf in the document.
/// Nulls nested inside arrays are reported too — the assertion is "no nulls
/// anywhere", which is stricter than what the migration itself removes.
void collect_null_leaves(const json& node, const std::string& path, std::vector<std::string>& out) {
    if (node.is_null()) {
        out.push_back(path.empty() ? "/" : path);
        return;
    }
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            collect_null_leaves(it.value(), path + "/" + it.key(), out);
        }
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i) {
            collect_null_leaves(node[i], path + "/" + std::to_string(i), out);
        }
    }
}

std::vector<std::string> null_leaves(const json& doc) {
    std::vector<std::string> out;
    collect_null_leaves(doc, "", out);
    return out;
}

/// The exact polluted shape reported in #1129 (values trimmed to the garbage
/// plus enough real settings to prove they survive).
json polluted_v19_config() {
    return json{{"config_version", 19},
                {"active_printer_id", "voronv2"},
                {"brightness", 80},
                {"led", {{"auto_state", {{"enabled", nullptr}}}, {"selected_strips", nullptr}}},
                {"printer", {{"leds", {{"startup_brightness", nullptr}}}}},
                {"printers",
                 {{"voronv2",
                   {{"moonraker_host", "192.168.1.112"},
                    {"leds",
                     {{"selected_strips", json::array({"neopixel case_lights"})},
                      {"auto_state", {{"mappings", nullptr}}}}},
                    {"probe_sensors", {{"sensors", nullptr}}},
                    {"width_sensors", nullptr}}}}}};
}

class ConfigPollutionFixture : public HelixTestFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;
    Config* saved_instance_ = nullptr;

    void SetUp() {
        temp_dir = (fs::temp_directory_path() / "helix_config_pollution_test").string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";

        // Point the Config singleton at our sandboxed instance so singleton
        // consumers (LedController, LedAutoState) read/write the temp config.
        saved_instance_ = ConfigTestAccess::instance_ref();
        ConfigTestAccess::instance_ref() = &config;
    }

    void TearDown() {
        helix::led::LedController::instance().deinit();
        ConfigTestAccess::instance_ref() = saved_instance_;
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    /// Read back what save() actually persisted.
    json read_saved() {
        std::ifstream f(config_path);
        json out;
        f >> out;
        return out;
    }

  public:
    ConfigPollutionFixture() {
        SetUp();
    }
    ~ConfigPollutionFixture() override {
        TearDown();
    }
};

/// A clean, already-current config with no legacy garbage in it.
json clean_config() {
    return json{{"config_version", CURRENT_CONFIG_VERSION},
                {"active_printer_id", "voronv2"},
                {"printers",
                 {{"voronv2",
                   {{"moonraker_host", "192.168.1.112"},
                    {"leds", {{"selected_strips", json::array({"neopixel case_lights"})}}}}}}}};
}

} // namespace

// ============================================================================
// Work items 1/2/4 — startup probes must not vivify
// ============================================================================

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config #1129: LED startup path creates no null nodes in settings.json",
                 "[config][led][pollution]") {
    write_and_init(clean_config());

    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr); // calls load_config()
    helix::led::LedAutoState::instance().load_config();

    REQUIRE(config.save());
    json saved = read_saved();

    INFO("saved config: " << saved.dump(2));
    // The orphan top-level nodes the legacy /led probes used to create.
    REQUIRE_FALSE(saved.contains("led"));
    REQUIRE_FALSE(saved.contains("printer"));

    auto nulls = null_leaves(saved);
    INFO("null leaves: " << json(nulls).dump());
    REQUIRE(nulls.empty());

    // The real value must survive the round trip.
    REQUIRE(ctrl.selected_strips().size() == 1);
    REQUIRE(ctrl.selected_strips()[0] == "neopixel case_lights");
    REQUIRE(saved["printers"]["voronv2"]["leds"]["selected_strips"] ==
            json::array({"neopixel case_lights"}));
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config #1129: HardwareValidator survives a malformed filament_sensors entry",
                 "[config][hardware][pollution]") {
    // json::value(key, default) throws type_error.302 when the key EXISTS with a
    // non-string value — it is not the safe probe its signature suggests. The old
    // code hid that behind a blanket try/catch; the probe rewrite dropped the catch
    // without adding a type check.
    //
    // validate() runs from the discovery-complete callback in application.cpp with
    // no enclosing try/catch, so a corrupt entry (hand-edited config, a truncated
    // write, a null left by the very pollution this branch cleans up) would
    // propagate an exception straight out of an LVGL/queue frame.
    json cfg = clean_config();
    cfg["printers"]["voronv2"]["filament_sensors"]["sensors"] =
        json::array({{{"name", 42}, {"klipper_name", nullptr}},
                     {{"name", nullptr}},
                     {{"name", json::array({"nested"})}, {"klipper_name", 7}},
                     "not even an object",
                     {{"name", "filament_switch_sensor real"},
                      {"klipper_name", "filament_switch_sensor real"}}});
    write_and_init(cfg);

    helix::PrinterDiscovery hardware;
    HardwareValidator validator;

    // The whole point: no throw.
    REQUIRE_NOTHROW(validator.validate(&config, hardware));

    // ...and the one well-formed entry is still processed — the guard must skip
    // the junk, not bail out of the loop.
    auto result = validator.validate(&config, hardware);
    const bool saw_real_sensor =
        std::any_of(result.expected_missing.begin(), result.expected_missing.end(),
                    [](const HardwareIssue& issue) {
                        return issue.hardware_name == "filament_switch_sensor real";
                    });
    INFO("expected_missing count: " << result.expected_missing.size());
    REQUIRE(saw_real_sensor);
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config #1129: HardwareValidator read-only probes create no null nodes",
                 "[config][hardware][pollution]") {
    write_and_init(clean_config());

    // Config::init() pre-creates hardware/{optional,expected,last_snapshot},
    // which would mask a vivifying probe. Drop the whole section first so the
    // probes below are genuinely reading absent paths.
    ConfigTestAccess::data(config)["printers"]["voronv2"].erase("hardware");
    REQUIRE_FALSE(config.exists("/printers/voronv2/hardware"));

    // Pure queries — none of these should write anything.
    REQUIRE_FALSE(HardwareValidator::is_hardware_optional(&config, "neopixel case_lights"));
    REQUIRE_FALSE(HardwareValidator::load_session_snapshot(&config).has_value());

    REQUIRE_FALSE(config.exists("/printers/voronv2/hardware"));

    REQUIRE(config.save());
    json saved = read_saved();

    INFO("saved config: " << saved.dump(2));
    auto nulls = null_leaves(saved);
    INFO("null leaves: " << json(nulls).dump());
    REQUIRE(nulls.empty());
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config #1129: clear_external_spool_info creates no filament null node",
                 "[config][settings][pollution]") {
    write_and_init(clean_config());

    // Probes /filament then save()s unconditionally — get_json() here left a
    // permanent "filament": null behind on every call.
    SettingsManager::instance().clear_external_spool_info();

    json saved = read_saved();
    INFO("saved config: " << saved.dump(2));
    REQUIRE_FALSE(saved["printers"]["voronv2"].contains("filament"));
    REQUIRE(null_leaves(saved).empty());
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config #1129: try_get_json and get<T> never create missing nodes",
                 "[config][pollution]") {
    write_and_init(clean_config());

    REQUIRE(config.try_get_json("/does/not/exist") == nullptr);
    REQUIRE(config.get<int>("/also/missing", 42) == 42);
    REQUIRE(config.get_string_array("/nope/nothing").empty());
    REQUIRE_THROWS(config.get<int>("/still/missing"));

    REQUIRE_FALSE(config.exists("/does"));
    REQUIRE_FALSE(config.exists("/also"));
    REQUIRE_FALSE(config.exists("/nope"));
    REQUIRE_FALSE(config.exists("/still"));

    REQUIRE(config.save());
    REQUIRE(null_leaves(read_saved()).empty());
}

TEST_CASE_METHOD(ConfigPollutionFixture, "Config #1129: get_string_array reads string arrays",
                 "[config][pollution]") {
    write_and_init(clean_config());

    auto strips = config.get_string_array("/printers/voronv2/leds/selected_strips");
    REQUIRE(strips.size() == 1);
    REQUIRE(strips[0] == "neopixel case_lights");

    // Non-array and mixed-type nodes degrade gracefully.
    config.set<std::string>("/scalar", "hello");
    REQUIRE(config.get_string_array("/scalar").empty());
    config.set<json>("/mixed", json::array({"a", 7, nullptr, "b"}));
    auto mixed = config.get_string_array("/mixed");
    REQUIRE(mixed == std::vector<std::string>{"a", "b"});
}

// ============================================================================
// Work item 3/4 — migration v19 -> v20
// ============================================================================

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config migration v20: erases the orphan top-level led and printer nodes",
                 "[config][migration][pollution]") {
    write_and_init(polluted_v19_config());

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE_FALSE(config.exists("/led"));
    REQUIRE_FALSE(config.exists("/printer"));

    REQUIRE(config.save());
    json saved = read_saved();
    INFO("saved config: " << saved.dump(2));
    REQUIRE_FALSE(saved.contains("led"));
    REQUIRE_FALSE(saved.contains("printer"));
}

TEST_CASE_METHOD(ConfigPollutionFixture, "Config migration v20: strips null leaves",
                 "[config][migration][pollution]") {
    write_and_init(polluted_v19_config());

    REQUIRE(config.save());
    json saved = read_saved();
    INFO("saved config: " << saved.dump(2));

    auto nulls = null_leaves(saved);
    INFO("null leaves: " << json(nulls).dump());
    REQUIRE(nulls.empty());
}

TEST_CASE_METHOD(ConfigPollutionFixture, "Config migration v20: real settings survive untouched",
                 "[config][migration][pollution]") {
    write_and_init(polluted_v19_config());

    REQUIRE(config.get<int>("/brightness", 0) == 80);
    REQUIRE(config.get<std::string>("/active_printer_id", "") == "voronv2");
    REQUIRE(config.get<std::string>("/printers/voronv2/moonraker_host", "") == "192.168.1.112");
    REQUIRE(config.get_string_array("/printers/voronv2/leds/selected_strips") ==
            std::vector<std::string>{"neopixel case_lights"});
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config migration v20: folds real legacy /led values into the active printer",
                 "[config][migration][pollution]") {
    json macro = {
        {"name", "Migration Macro"}, {"type", "toggle"}, {"toggle_macro", "MIGRATE_TOGGLE"}};
    json v19 = {{"config_version", 19},
                {"active_printer_id", "voronv2"},
                {"led",
                 {{"selected_strips", json::array({"neopixel legacy_light"})},
                  {"last_color", "#AA5500"},
                  {"last_brightness", 55},
                  {"color_presets", json::array({"#FF0000", "#00FF00"})},
                  {"macro_devices", json::array({macro})},
                  {"auto_state",
                   {{"enabled", true}, {"mappings", {{"printing", {{"action", "color"}}}}}}}}},
                {"printers", {{"voronv2", {{"moonraker_host", "192.168.1.112"}}}}}};
    write_and_init(v19);

    REQUIRE_FALSE(config.exists("/led"));
    REQUIRE(config.get_string_array("/printers/voronv2/leds/selected_strips") ==
            std::vector<std::string>{"neopixel legacy_light"});
    REQUIRE(config.get<int>("/printers/voronv2/leds/last_brightness", 0) == 55);
    REQUIRE(config.get_string_array("/printers/voronv2/leds/color_presets").size() == 2);
    REQUIRE(config.get<bool>("/printers/voronv2/leds/auto_state/enabled", false) == true);
    REQUIRE(config.exists("/printers/voronv2/leds/auto_state/mappings"));

    // And the controllers pick the folded values up on the normal startup path.
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.selected_strips() == std::vector<std::string>{"neopixel legacy_light"});
    REQUIRE(ctrl.last_color() == 0xAA5500);
    REQUIRE(ctrl.last_brightness() == 55);
    REQUIRE(ctrl.color_presets().size() == 2);
    REQUIRE(ctrl.color_presets()[0] == 0xFF0000);
    REQUIRE(ctrl.color_presets()[1] == 0x00FF00);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Migration Macro");
    REQUIRE(ctrl.configured_macros()[0].toggle_macro == "MIGRATE_TOGGLE");

    helix::led::LedAutoState::instance().load_config();
    REQUIRE(helix::led::LedAutoState::instance().is_enabled());

    // Idempotent: a second boot (config is now v20) changes nothing.
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.selected_strips() == std::vector<std::string>{"neopixel legacy_light"});
    REQUIRE(ctrl.last_color() == 0xAA5500);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE_FALSE(config.exists("/led"));
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config migration v20: does not overwrite existing per-printer LED values",
                 "[config][migration][pollution]") {
    json v19 = {
        {"config_version", 19},
        {"active_printer_id", "voronv2"},
        {"led", {{"selected_strips", json::array({"neopixel legacy_light"})}}},
        {"printers",
         {{"voronv2", {{"leds", {{"selected_strips", json::array({"neopixel case_lights"})}}}}}}}};
    write_and_init(v19);

    REQUIRE_FALSE(config.exists("/led"));
    REQUIRE(config.get_string_array("/printers/voronv2/leds/selected_strips") ==
            std::vector<std::string>{"neopixel case_lights"});
}

TEST_CASE_METHOD(
    ConfigPollutionFixture,
    "Config migration v20: a null-valued fold target does not swallow the legacy value",
    "[config][migration][pollution]") {
    // The exact shape main leaves behind. LedController::load_config() probed
    // leds/last_color, leds/last_white, leds/color_presets, leds/macro_devices,
    // leds/led_on_at_start and leds/startup_brightness through Config::get_json(),
    // and LedAutoState::load_config() probed leds/auto_state/mappings — every one
    // of those paths is a v20 fold TARGET, and every one was vivified to null.
    //
    // nlohmann's contains() answers true for a key whose value is null, so a
    // contains()-only "target already exists" test reads that pollution as a real
    // value, erases the source, and strip_null_leaves() then deletes the null
    // target. The user's legacy value is gone and nothing replaced it —
    // irreversible, on the first boot after upgrade.
    //
    // main only ran its own fold when /led/selected_strips was a non-empty array,
    // so a user with real /led/color_presets or /led/macro_devices but no strip
    // selection reaches v20 with the legacy values still in place. They are
    // exactly the users this test protects.
    json macro = {{"name", "Legacy Macro"}, {"type", "toggle"}, {"toggle_macro", "LEGACY_TOGGLE"}};
    json v19 = {{"config_version", 19},
                {"active_printer_id", "voronv2"},
                {"led",
                 {{"last_color", "#AA5500"},
                  {"last_brightness", 55},
                  {"color_presets", json::array({"#FF0000", "#00FF00"})},
                  {"macro_devices", json::array({macro})},
                  {"auto_state", {{"mappings", {{"printing", {{"action", "color"}}}}}}}}},
                {"printers",
                 {{"voronv2",
                   {{"moonraker_host", "192.168.1.112"},
                    {"leds",
                     {// Every target below is a probe-vivified null, NOT a user value.
                      {"last_color", nullptr},
                      {"last_white", nullptr},
                      {"last_brightness", nullptr},
                      {"color_presets", nullptr},
                      {"macro_devices", nullptr},
                      {"led_on_at_start", nullptr},
                      {"startup_brightness", nullptr},
                      {"auto_state", {{"mappings", nullptr}}}}}}}}}};
    write_and_init(v19);

    INFO("post-migration config: " << ConfigTestAccess::data(config).dump(2));

    // The legacy values MUST have moved, not evaporated.
    REQUIRE(config.get<std::string>("/printers/voronv2/leds/last_color", "") == "#AA5500");
    REQUIRE(config.get<int>("/printers/voronv2/leds/last_brightness", 0) == 55);
    REQUIRE(config.get_string_array("/printers/voronv2/leds/color_presets") ==
            std::vector<std::string>{"#FF0000", "#00FF00"});
    REQUIRE(config.exists("/printers/voronv2/leds/macro_devices"));
    REQUIRE(config.exists("/printers/voronv2/leds/auto_state/mappings"));

    // ...and the controller reads them back on the normal startup path.
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    REQUIRE(ctrl.last_color() == 0xAA5500);
    REQUIRE(ctrl.last_brightness() == 55);
    REQUIRE(ctrl.color_presets().size() == 2);
    REQUIRE(ctrl.configured_macros().size() == 1);
    REQUIRE(ctrl.configured_macros()[0].display_name == "Legacy Macro");

    REQUIRE_FALSE(config.exists("/led"));
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config migration v20: an unresolvable fold target never destroys /led",
                 "[config][migration][pollution]") {
    // The fold is gated on active_printer_id naming a live printer object, but the
    // erase used to run unconditionally — so a config whose active_printer_id
    // dangles lost the whole /led block. Config::init()'s own "pick the first
    // printer" fallback cannot save it: that runs AFTER run_versioned_migrations(),
    // and config_version is already 20 by then, so the migration never gets a
    // second chance.
    SECTION("dangling active_printer_id still folds into the first real printer") {
        json v19 = {{"config_version", 19},
                    {"active_printer_id", "ghost"}, // names a printer that isn't there
                    {"led",
                     {{"selected_strips", json::array({"neopixel legacy_light"})},
                      {"last_brightness", 42}}},
                    {"printers", {{"voronv2", {{"moonraker_host", "192.168.1.112"}}}}}};
        write_and_init(v19);

        INFO("post-migration config: " << ConfigTestAccess::data(config).dump(2));
        REQUIRE(config.get_string_array("/printers/voronv2/leds/selected_strips") ==
                std::vector<std::string>{"neopixel legacy_light"});
        REQUIRE(config.get<int>("/printers/voronv2/leds/last_brightness", 0) == 42);
        REQUIRE_FALSE(config.exists("/led"));
    }

    SECTION("no printers map at all leaves /led intact for a later boot") {
        json v19 = {{"config_version", 19},
                    {"led",
                     {{"selected_strips", json::array({"neopixel legacy_light"})},
                      {"last_brightness", 42}}}};
        write_and_init(v19);

        INFO("post-migration config: " << ConfigTestAccess::data(config).dump(2));
        // Nothing to fold into — but the user's values must survive rather than
        // be erased on the way past.
        REQUIRE(config.get_string_array("/led/selected_strips") ==
                std::vector<std::string>{"neopixel legacy_light"});
        REQUIRE(config.get<int>("/led/last_brightness", 0) == 42);
    }

    SECTION("a value-free /led is still erased") {
        // The pollution-only case the migration exists for: no target, but nothing
        // worth keeping either. Erasing is correct here.
        json v19 = {
            {"config_version", 19},
            {"active_printer_id", "ghost"},
            {"led", {{"selected_strips", nullptr}, {"auto_state", {{"enabled", nullptr}}}}}};
        write_and_init(v19);

        INFO("post-migration config: " << ConfigTestAccess::data(config).dump(2));
        REQUIRE_FALSE(config.exists("/led"));
    }
}

TEST_CASE_METHOD(ConfigPollutionFixture,
                 "Config migration v20: already-current config is left alone",
                 "[config][migration][pollution]") {
    write_and_init(clean_config());

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    REQUIRE(config.get_string_array("/printers/voronv2/leds/selected_strips") ==
            std::vector<std::string>{"neopixel case_lights"});
    REQUIRE(null_leaves(ConfigTestAccess::data(config)).empty());
}
