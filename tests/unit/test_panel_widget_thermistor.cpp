// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

// ============================================================================
// Test fixture — reuse Config internals access pattern
// ============================================================================

// Fixture using ConfigTestAccess for Config::data
namespace helix {
class ThermistorConfigFixture {
  protected:
    Config config;

    void setup_empty_config() {
        ConfigTestAccess::data(config) = json::object();
    }

    void setup_with_widgets(const json& widgets_json) {
        ConfigTestAccess::data(config) = json::object();
        ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"]["home"] =
            widgets_json;
    }

    json& get_data() {
        return ConfigTestAccess::data(config);
    }
};
} // namespace helix

// ============================================================================
// Registry: thermistor widget definition
// ============================================================================

TEST_CASE("ThermistorWidget: registered in widget registry", "[thermistor][panel_widget]") {
    const auto* def = find_widget_def("thermistor");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Temperature Sensors");
    REQUIRE(std::string(def->icon) == "thermometer");
    REQUIRE(def->hardware_gate_subject != nullptr);
    REQUIRE(std::string(def->hardware_gate_subject) == "temp_sensor_count");
    REQUIRE(def->default_enabled == false); // opt-in widget
}

// ============================================================================
// Config field serialization
// ============================================================================

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config field round-trips through save/load",
                 "[thermistor][panel_widget]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config", {{"sensor", "temperature_sensor mcu_temp"}}},
         {"col", 0},
         {"row", 0}},
        {{"id", "shutdown"}, {"enabled", true}, {"col", 1}, {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Verify config was loaded
    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensor"));
    REQUIRE(cfg["sensor"].get<std::string>() == "temperature_sensor mcu_temp");

    // Save and reload
    wc.save();

    PanelWidgetConfig wc2("home", config);
    wc2.load();

    auto cfg2 = wc2.get_widget_config("thermistor");
    REQUIRE(cfg2.contains("sensor"));
    REQUIRE(cfg2["sensor"].get<std::string>() == "temperature_sensor mcu_temp");
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: get_widget_config is an empty object with no config saved",
                 "[thermistor][panel_widget]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    // A known widget that has none, and an id no widget has.
    for (const char* id : {"shutdown", "nonexistent_widget_xyz"}) {
        auto cfg = wc.get_widget_config(id);
        CHECK(cfg.is_object());
        CHECK(cfg.empty());
    }
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: set_widget_config saves and persists",
                 "[thermistor][panel_widget]") {
    // Multi-instance widgets need a pre-existing entry (not auto-created by defaults)
    json widgets = json::array({
        {{"id", "thermistor:1"}, {"enabled", true}, {"col", 0}, {"row", 0}},
    });
    setup_with_widgets(widgets);
    PanelWidgetConfig wc("home", config);
    wc.load();

    json sensor_config = {{"sensor", "temperature_sensor chamber"}};
    wc.set_widget_config("thermistor:1", sensor_config);

    // Verify immediate read
    auto cfg = wc.get_widget_config("thermistor:1");
    REQUIRE(cfg["sensor"].get<std::string>() == "temperature_sensor chamber");

    // Verify persisted in underlying JSON (now in pages format)
    auto& root = get_data()["printers"]["default"]["panel_widgets"]["home"];
    REQUIRE(root.is_object());
    auto& saved = root["pages"][0]["widgets"];
    bool found = false;
    for (const auto& item : saved) {
        if (item["id"] == "thermistor:1" && item.contains("config")) {
            found = true;
            REQUIRE(item["config"]["sensor"] == "temperature_sensor chamber");
        }
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config field omitted from JSON when empty",
                 "[thermistor][panel_widget]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    // "No config key anywhere" stopped being the invariant once the shipped
    // default layout began attaching one to an anchor — print_status ships as
    // Detailed in both orientations. What must still hold is the thing this
    // test is named for: an EMPTY config is omitted rather than serialised as
    // an empty object.
    auto& root = get_data()["printers"]["default"]["panel_widgets"]["home"];
    auto& saved = root["pages"][0]["widgets"];
    for (const auto& item : saved) {
        CAPTURE(item["id"].get<std::string>());
        if (item.contains("config")) {
            REQUIRE(item["config"].is_object());
            REQUIRE_FALSE(item["config"].empty());
        }
    }
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config preserves unknown fields (forward compatibility)",
                 "[thermistor][panel_widget]") {
    // Include grid coords to prevent pre-grid migration reset
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config",
          {{"sensor", "temperature_sensor mcu_temp"}, {"color", "#FF0000"}, {"threshold", 80}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg["sensor"] == "temperature_sensor mcu_temp");
    REQUIRE(cfg["color"] == "#FF0000");
    REQUIRE(cfg["threshold"] == 80);

    // Round-trip preserves unknown fields
    wc.save();
    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg2 = wc2.get_widget_config("thermistor");
    REQUIRE(cfg2["color"] == "#FF0000");
    REQUIRE(cfg2["threshold"] == 80);
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: set_widget_config on unknown widget is no-op",
                 "[thermistor][panel_widget]") {
    setup_empty_config();
    PanelWidgetConfig wc("home", config);
    wc.load();

    auto entries_before = wc.entries();

    // This should be a no-op (widget not in entries since it's unknown to registry)
    wc.set_widget_config("nonexistent_widget_xyz", {{"key", "value"}});

    // Entries unchanged
    REQUIRE(wc.entries().size() == entries_before.size());
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config field with non-object value in JSON is ignored",
                 "[thermistor][panel_widget]") {
    json widgets = json::array({
        {{"id", "thermistor"}, {"enabled", true}, {"config", "not_an_object"}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    // Non-object config should be ignored (returns empty)
    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg.is_object());
    REQUIRE(cfg.empty());
}

// ============================================================================
// Carousel mode: config backward compatibility
// ============================================================================

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: old sensor config round-trips with new format",
                 "[thermistor][panel_widget][carousel]") {
    // Old format: {"sensor": "temperature_sensor mcu_temp"}
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config", {{"sensor", "temperature_sensor mcu_temp"}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensor"));
    REQUIRE(cfg["sensor"].get<std::string>() == "temperature_sensor mcu_temp");

    // Save and reload — old format persists (ThermistorWidget hasn't migrated it yet)
    wc.save();
    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg2 = wc2.get_widget_config("thermistor");
    REQUIRE(cfg2["sensor"].get<std::string>() == "temperature_sensor mcu_temp");
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: new sensors array config loads correctly",
                 "[thermistor][panel_widget][carousel]") {
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config",
          {{"sensors", json::array({"temperature_sensor mcu_temp", "temperature_sensor chamber"})},
           {"display_mode", "carousel"}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensors"));
    REQUIRE(cfg["sensors"].is_array());
    REQUIRE(cfg["sensors"].size() == 2);
    REQUIRE(cfg["sensors"][0].get<std::string>() == "temperature_sensor mcu_temp");
    REQUIRE(cfg["sensors"][1].get<std::string>() == "temperature_sensor chamber");
    REQUIRE(cfg["display_mode"].get<std::string>() == "carousel");
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: carousel config round-trips through save/load",
                 "[thermistor][panel_widget][carousel]") {
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config",
          {{"sensors", json::array({"temperature_sensor mcu_temp", "temperature_sensor chamber"})},
           {"display_mode", "carousel"}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg = wc2.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensors"));
    REQUIRE(cfg["sensors"].size() == 2);
    REQUIRE(cfg.contains("display_mode"));
    REQUIRE(cfg["display_mode"].get<std::string>() == "carousel");
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config with both old and new format prefers new",
                 "[thermistor][panel_widget][carousel]") {
    // Config has both "sensor" (old) and "sensors" (new) — new takes precedence
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config",
          {{"sensor", "temperature_sensor old"},
           {"sensors", json::array({"temperature_sensor new1", "temperature_sensor new2"})}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    auto cfg = wc.get_widget_config("thermistor");
    // Both fields are present in raw config
    REQUIRE(cfg.contains("sensor"));
    REQUIRE(cfg.contains("sensors"));
    REQUIRE(cfg["sensors"].size() == 2);
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: single sensor config implies no display_mode",
                 "[thermistor][panel_widget][carousel]") {
    // Old-style config with just "sensor" and no "display_mode" — after round-trip,
    // no display_mode key should appear (single sensor = no carousel)
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config", {{"sensor", "temperature_sensor mcu_temp"}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg = wc2.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensor"));
    REQUIRE_FALSE(cfg.contains("display_mode"));
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: empty sensors array round-trips through config",
                 "[thermistor][panel_widget][carousel]") {
    // Empty sensors array should be preserved by the config layer
    // (the widget layer guards against applying empty selection)
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config", {{"sensors", json::array()}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();

    auto cfg = wc.get_widget_config("thermistor");
    REQUIRE(cfg.contains("sensors"));
    REQUIRE(cfg["sensors"].is_array());
    REQUIRE(cfg["sensors"].empty());

    // Round-trip preserves empty array
    wc.save();
    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg2 = wc2.get_widget_config("thermistor");
    REQUIRE(cfg2.contains("sensors"));
    REQUIRE(cfg2["sensors"].is_array());
    REQUIRE(cfg2["sensors"].empty());
}

TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: display_mode persists through save/load",
                 "[thermistor][panel_widget][carousel]") {
    // Config with a single sensor in the array plus display_mode=carousel —
    // display_mode should persist since PanelWidgetConfig preserves all fields
    json widgets = json::array({
        {{"id", "thermistor"},
         {"enabled", true},
         {"config",
          {{"sensors", json::array({"temperature_sensor mcu_temp"})},
           {"display_mode", "carousel"}}},
         {"col", 0},
         {"row", 0}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig wc("home", config);
    wc.load();
    wc.save();

    PanelWidgetConfig wc2("home", config);
    wc2.load();
    auto cfg = wc2.get_widget_config("thermistor");
    REQUIRE(cfg.contains("display_mode"));
    REQUIRE(cfg["display_mode"].get<std::string>() == "carousel");
    REQUIRE(cfg.contains("sensors"));
    REQUIRE(cfg["sensors"].size() == 1);
}
