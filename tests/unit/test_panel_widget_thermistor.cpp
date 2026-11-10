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

// PanelWidgetConfig keeps a sensor tile's config object verbatim across a load
// and a save/load: the single "sensor", the carousel "sensors" list (empty or
// not) with its display_mode, both at once, and fields it does not know.
TEST_CASE_METHOD(helix::ThermistorConfigFixture,
                 "ThermistorWidget: config objects round-trip through save/load verbatim",
                 "[thermistor][panel_widget][carousel]") {
    const json two = json::array({"temperature_sensor mcu_temp", "temperature_sensor chamber"});
    const json configs[] = {
        {{"sensor", "temperature_sensor mcu_temp"}},
        {{"sensor", "temperature_sensor mcu_temp"}, {"color", "#FF0000"}, {"threshold", 80}},
        {{"sensors", two}, {"display_mode", "carousel"}},
        {{"sensors", json::array({"temperature_sensor mcu_temp"})}, {"display_mode", "carousel"}},
        {{"sensors", json::array()}},
        {{"sensor", "temperature_sensor old"}, {"sensors", two}},
    };
    for (const auto& c : configs) {
        CAPTURE(c.dump());
        // Grid coords keep the pre-grid migration from resetting the layout.
        setup_with_widgets(json::array(
            {{{"id", "thermistor"}, {"enabled", true}, {"config", c}, {"col", 0}, {"row", 0}}}));
        PanelWidgetConfig wc("home", config);
        wc.load();
        CHECK(wc.get_widget_config("thermistor") == c);
        wc.save();
        PanelWidgetConfig wc2("home", config);
        wc2.load();
        CHECK(wc2.get_widget_config("thermistor") == c);
    }
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
