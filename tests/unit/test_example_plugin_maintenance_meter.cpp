// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "connection_state.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// Where HostRig's settings path puts the plugin's store.
const char* kStorePath = "/tmp/helix-plugin-host-test/plugin-data/maintenance-meter.json";

/// Printer subjects are global and the store is a file, so each case starts
/// from an idle, connected printer and no store, and leaves no store behind.
class MeterFx : public XMLTestFixture {
  public:
    MeterFx() {
        std::filesystem::remove(kStorePath);
        printer_state("standby");
        connection(helix::ConnectionState::CONNECTED);
    }
    ~MeterFx() override {
        drain();
        std::filesystem::remove(kStorePath);
    }

    static void printer_state(const char* state) {
        lv_subject_set_string(lv_xml_get_subject(nullptr, "print_state"), state);
        drain();
    }
    static void connection(helix::ConnectionState state) {
        lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                           static_cast<int>(state));
        drain();
    }
    static std::string str(const char* name) {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
    }
    static int num(const char* name) {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
    }

    static void seed_store(const json& state) {
        std::filesystem::create_directories(std::filesystem::path(kStorePath).parent_path());
        std::ofstream(kStorePath) << json{{"state", state}}.dump();
    }
    static json stored_state() {
        std::ifstream in(kStorePath);
        if (!in)
            return json();
        return json::parse(in, nullptr, false)["state"];
    }

    /// `n` accrual ticks of virtual time (the plugin ticks once a minute).
    void minutes(int n) {
        for (int i = 0; i < n; ++i) {
            lv_tick_inc(60 * 1000);
            lv_timer_handler_safe();
        }
    }
};

json meter_block(const json& settings = json::object()) {
    json block = enabled("maintenance-meter", {"storage"});
    block["settings"]["maintenance-meter"] = settings;
    return block;
}

json meters(double filter_used, json extra = json::object()) {
    json m = {{"filter", {{"used", filter_used}}}};
    for (auto& [k, v] : extra.items())
        m[k] = v;
    return m;
}

} // namespace

TEST_CASE_METHOD(MeterFx, "maintenance-meter loads with a registered adaptive widget",
                 "[plugin][example]") {
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("maintenance-meter"));
    CHECK(rig.info("maintenance-meter")->status == PluginStatus::Loaded);

    const helix::PanelWidgetDef* def = helix::find_widget_def("maintenance-meter__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->max_colspan == 8);
    CHECK(def->max_rowspan == 4);
    rig.host->disable("maintenance-meter");
    CHECK(helix::find_widget_def("maintenance-meter__tile") == nullptr);

    for (const char* file : {"maintenance-meter__tile", "maintenance-meter__detail"}) {
        const std::string path =
            std::string("examples/plugins/maintenance-meter/ui/") + file + ".xml";
        CAPTURE(path);
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("maintenance-meter",
                               {"maintenance-meter__tile", "maintenance-meter__detail"},
                               xml) == std::string());
    }
}

TEST_CASE_METHOD(MeterFx, "a saved store loads into the meters", "[plugin][example]") {
    seed_store({{"total_h", 120.0},
                {"meters",
                 {{"filter", {{"used", 150.0}, {"reset_at", 40.0}, {"notified", true}}},
                  {"nozzle", {{"used", 100.0}}}}}});
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");

    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 120h");
    CHECK(str("maintenance-meter__m1_line") == "150h / 200h");
    CHECK(str("maintenance-meter__m1_note") == "25% left - reset at 40h");
    CHECK(str("maintenance-meter__m2_line") == "100h / 500h");
    CHECK(str("maintenance-meter__m3_line") == "0h / 300h");
    // The filter is the closest to due (75% spent), so it owns the dial.
    CHECK(str("maintenance-meter__big_name") == "HEPA filter");
    CHECK(str("maintenance-meter__big_value") == "50h");
    // Loading changes nothing, so it writes nothing.
    std::filesystem::remove(kStorePath);
    process_lvgl(1000);
    CHECK_FALSE(std::filesystem::exists(kStorePath));
}

TEST_CASE_METHOD(MeterFx, "a hand-edited store loads with defaults and never faults",
                 "[plugin][example]") {
    seed_store({{"total_h", "lots"},
                {"meters",
                 {{"filter", {{"used", "x"}, {"reset_at", "never"}, {"notified", "yes"}}},
                  {"nozzle", {{"used", -5}, {"reset_at", true}}},
                  {"rails", {{"used", 12.0}, {"reset_at", 1e300}}}}}});
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");

    REQUIRE(rig.info("maintenance-meter"));
    CHECK(rig.info("maintenance-meter")->status == PluginStatus::Loaded);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 0h");
    CHECK(str("maintenance-meter__m1_line") == "0h / 200h");
    CHECK(str("maintenance-meter__m1_note") == "100% left");
    CHECK(str("maintenance-meter__m2_line") == "0h / 500h");
    CHECK(str("maintenance-meter__m2_note") == "100% left");
    CHECK(str("maintenance-meter__m3_line") == "12h / 300h"); // the valid field survives
    CHECK(str("maintenance-meter__m3_note") == "96% left");
}

TEST_CASE_METHOD(MeterFx, "a store that is not a table is ignored", "[plugin][example]") {
    seed_store("garbage");
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    CHECK(rig.info("maintenance-meter")->status == PluginStatus::Loaded);
    CHECK(str("maintenance-meter__m1_line") == "0h / 200h");
}

TEST_CASE_METHOD(MeterFx, "hours accrue each minute of printing and persist", "[plugin][example]") {
    seed_store({{"total_h", 10.4}, {"meters", meters(5.0)}});
    printer_state("printing");
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 10h");

    minutes(12); // 12 minutes = 0.2 h: 10.6 rounds to 11
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 11h");

    process_lvgl(1000); // the store write rides a short debounce
    const json saved = stored_state();
    REQUIRE(saved.is_object());
    CHECK(saved["total_h"].get<double>() == Approx(10.4 + 12.0 / 60).margin(1e-6));
    CHECK(saved["meters"]["filter"]["used"].get<double>() == Approx(5.0 + 12.0 / 60).margin(1e-6));
}

TEST_CASE_METHOD(MeterFx, "nothing accrues while the printer is not printing",
                 "[plugin][example]") {
    seed_store({{"total_h", 10.4}, {"meters", meters(5.0)}});
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    for (const char* state : {"standby", "paused", "complete", "cancelled", "error"}) {
        CAPTURE(state);
        printer_state(state);
        minutes(12);
    }
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 10h");

    printer_state("printing");
    minutes(12);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 11h");
    printer_state("paused");
    minutes(30);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 11h");
}

TEST_CASE_METHOD(MeterFx, "a plugin loaded mid-print accrues from the first tick",
                 "[plugin][example]") {
    seed_store({{"total_h", 10.4}, {"meters", meters(5.0)}});
    printer_state("printing"); // the print began before the plugin loaded
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    minutes(12);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 11h");
}

TEST_CASE_METHOD(MeterFx, "nothing accrues while the printer is disconnected",
                 "[plugin][example]") {
    seed_store({{"total_h", 10.4}, {"meters", meters(5.0)}});
    printer_state("printing");
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");

    connection(helix::ConnectionState::DISCONNECTED); // print_state keeps its last value
    minutes(30);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 10h");

    connection(helix::ConnectionState::CONNECTED);
    minutes(12);
    CHECK(str("maintenance-meter__total_line") == "Total print time on this screen: 11h");
}

TEST_CASE_METHOD(MeterFx, "an interval of 0 hides a meter and the dial re-picks",
                 "[plugin][example]") {
    seed_store({{"meters", meters(150.0)}});
    HostRig rig(meter_block());
    rig.host->load_from("examples/plugins");
    CHECK(num("maintenance-meter__m1_on") == 1);
    CHECK(str("maintenance-meter__big_name") == "HEPA filter");

    REQUIRE(rig.host->set_setting("maintenance-meter", "filter_interval_h", 0));
    CHECK(num("maintenance-meter__m1_on") == 0);
    CHECK(str("maintenance-meter__big_name") != "HEPA filter");
}

TEST_CASE_METHOD(MeterFx, "the due toast fires once, and again after a raised interval is crossed",
                 "[plugin][example]") {
    // 119.9 of a 120h interval: the next minute of printing crosses it.
    seed_store({{"meters", meters(119.99)}});
    printer_state("printing");
    HostRig rig(meter_block(json{{"filter_interval_h", 120}}));
    rig.host->load_from("examples/plugins");

    minutes(1);
    CHECK(str("maintenance-meter__m1_line") == "120h / 120h");
    const auto latch = [&] {
        process_lvgl(1000);
        return stored_state()["meters"]["filter"]["notified"].get<bool>();
    };
    CHECK(latch());

    // Raising the interval puts the due point ahead of the hours: the latch re-arms.
    REQUIRE(rig.host->set_setting("maintenance-meter", "filter_interval_h", 200));
    CHECK_FALSE(latch());
    // Hours below the new interval fire nothing; lowering the interval under
    // the hours makes the next tick announce again.
    minutes(1);
    CHECK_FALSE(latch());
    REQUIRE(rig.host->set_setting("maintenance-meter", "filter_interval_h", 100));
    minutes(1);
    CHECK(latch());
}

#endif // HELIX_HAS_PLUGINS
