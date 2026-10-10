// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "locale_formats.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"
#include "plugin_canvas.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"
#include "translation_loader.h"

#include <fstream>
#include <iterator>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

json ambient_block(const json& settings) {
    json block = enabled("ambient-tile", {"http"});
    block["settings"]["ambient-tile"] = settings;
    return block;
}

/// One configured generic endpoint: a 10 s poll (the manifest's minimum), so
/// a reading is one process_lvgl away.
json generic_settings(const std::string& body_suffix = "") {
    return json{{"source", "JSON endpoint"},
                {"url", "https://api.example.com/room" + body_suffix},
                {"temp_pointer", "current.t"},
                {"humidity_pointer", "current.rh"},
                {"poll_s", 10}};
}

json ha_settings() {
    return json{{"source", "Home Assistant"},
                {"url", "http://homeassistant.local:8123"},
                {"token", "secret-token"},
                {"temp_entity", "sensor.room_temp"},
                {"humidity_entity", "sensor.room_rh"},
                {"poll_s", 60}};
}

std::string watcher_str(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

/// Answers every http request since `next` with `status` and `body`,
/// settling each fetch.
void answer_http(HostRig& rig, size_t& next, int status, const std::string& body) {
    while (next < rig.fake.requests.size()) {
        auto& r = rig.fake.requests[next++];
        if (r.kind == "http")
            r.reply(RpcResult{true, json{{"status", status}, {"body", body}}, {}});
    }
    drain();
}

size_t http_count(HostRig& rig) {
    size_t n = 0;
    for (const auto& r : rig.fake.requests)
        if (r.kind == "http")
            ++n;
    return n;
}

const FakeBackend::Request* http_at(HostRig& rig, size_t i) {
    size_t n = 0;
    for (const auto& r : rig.fake.requests)
        if (r.kind == "http" && n++ == i)
            return &r;
    return nullptr;
}

/// The home tile as the home panel builds it: through the registry factory,
/// attached and told its size, so the plugin's on_size ladder runs and the
/// spark zone (a two-row tier) lays its canvas out. The caller owns the
/// widget and must reset it before the case ends.
std::unique_ptr<helix::PanelWidget> make_tile() {
    const helix::PanelWidgetDef* def = helix::find_widget_def("ambient-tile__tile");
    REQUIRE(def);
    auto w = def->factory("ambient-tile__tile");
    REQUIRE(w);
    lv_obj_t* root = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), w->get_component_name().c_str(), nullptr));
    REQUIRE(root);
    w->attach_tile(root, lv_screen_active());
    // The size hook first, so the ladder subjects unhide the spark zone before
    // the tree lays out: a canvas laid out while hidden never receives a size.
    w->notify_size_changed(4, 4, 233, 230);
    lv_obj_set_size(root, 233, 230);
    lv_obj_update_layout(root);
    drain();
    return w;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "ambient-tile loads with a registered adaptive widget",
                 "[plugin][example]") {
    HostRig rig(ambient_block(generic_settings()));
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("ambient-tile"));
    CHECK(rig.info("ambient-tile")->status == PluginStatus::Loaded);

    const helix::PanelWidgetDef* def = helix::find_widget_def("ambient-tile__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 2); // one cell, in tracks
    CHECK(def->rowspan == 2);
    CHECK(def->max_colspan == 8);
    CHECK(def->max_rowspan == 4);
    rig.host->disable("ambient-tile");
    CHECK(helix::find_widget_def("ambient-tile__tile") == nullptr);

    for (const char* file : {"ambient-tile__tile", "ambient-tile__detail"}) {
        const std::string path = std::string("examples/plugins/ambient-tile/ui/") + file + ".xml";
        CAPTURE(path);
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("ambient-tile", {"ambient-tile__tile", "ambient-tile__detail"},
                               xml) == std::string());
    }
}

TEST_CASE_METHOD(XMLTestFixture, "an unconfigured tile asks nothing of the network",
                 "[plugin][example]") {
    HostRig rig(ambient_block(json{{"source", "JSON endpoint"}}));
    rig.host->load_from("examples/plugins");
    drain();
    process_lvgl(2500);
    CHECK(http_count(rig) == 0);
    CHECK(watcher_str("ambient-tile__status_line") == "not configured");
    CHECK(watcher_str("ambient-tile__temp_text") == "--");
    CHECK(watcher_str("ambient-tile__age_text") == "Never");
}

TEST_CASE_METHOD(XMLTestFixture, "a generic endpoint resolves pointers onto the subjects",
                 "[plugin][example]") {
    HostRig rig(ambient_block(generic_settings()));
    rig.host->load_from("examples/plugins");
    auto tile = make_tile();
    process_lvgl(50); // the canvas's first layout fires its on_size
    size_t next = 0;

    answer_http(rig, next, 200, R"({"current":{"t":21.4,"rh":38}})");
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");
    CHECK(watcher_str("ambient-tile__rh_text") == "38%");
    CHECK(watcher_str("ambient-tile__age_text") == "Updated just now");
    CHECK(watcher_str("ambient-tile__status_line") == "");

    // The next poll on the virtual clock; a changed reading moves the subject.
    process_lvgl(10500);
    CHECK(http_count(rig) >= 2);
    answer_http(rig, next, 200, R"({"current":{"t":22.6,"rh":41}})");
    CHECK(watcher_str("ambient-tile__temp_text") == "22.6°");
    CHECK(watcher_str("ambient-tile__rh_text") == "41%");
}

TEST_CASE_METHOD(XMLTestFixture, "array pointer segments index from zero", "[plugin][example]") {
    json settings = generic_settings();
    settings["temp_pointer"] = "sensors.0.t";
    settings["humidity_pointer"] = "sensors.1.t";
    HostRig rig(ambient_block(settings));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_http(rig, next, 200, R"({"sensors":[{"t":19.5},{"t":52}]})");
    CHECK(watcher_str("ambient-tile__temp_text") == "19.5°");
    CHECK(watcher_str("ambient-tile__rh_text") == "52%");
}

TEST_CASE_METHOD(XMLTestFixture, "a numeric string state reads as a number", "[plugin][example]") {
    json settings = generic_settings();
    settings["temp_pointer"] = "temp";
    settings["humidity_pointer"] = "";
    HostRig rig(ambient_block(settings));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_http(rig, next, 200, R"({"temp":"21.4"})");
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");
    CHECK(watcher_str("ambient-tile__rh_text") == "--"); // humidity stays unset
}

TEST_CASE_METHOD(XMLTestFixture, "home assistant fetches both entities with the bearer token",
                 "[plugin][example]") {
    HostRig rig(ambient_block(ha_settings()));
    rig.host->load_from("examples/plugins");

    REQUIRE(http_count(rig) == 1); // the load-time poll, at the temp entity first
    const auto& first = *http_at(rig, 0);
    CHECK(first.b == "http://homeassistant.local:8123/api/states/sensor.room_temp");
    CHECK(first.params.value("Authorization", "") == "Bearer secret-token");
    first.reply(RpcResult{true, json{{"status", 200}, {"body", R"({"state":"21.4"})"}}, {}});
    drain();

    REQUIRE(http_count(rig) == 2); // then the humidity entity
    const auto& second = *http_at(rig, 1);
    CHECK(second.b == "http://homeassistant.local:8123/api/states/sensor.room_rh");
    second.reply(RpcResult{true, json{{"status", 200}, {"body", R"({"state":"38"})"}}, {}});
    drain();

    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");
    CHECK(watcher_str("ambient-tile__rh_text") == "38%");
    // The source line names the host; the token appears nowhere user-facing.
    CHECK(watcher_str("ambient-tile__source_line") == "Home Assistant · homeassistant.local:8123");
    CHECK(watcher_str("ambient-tile__source_line").find("secret-token") == std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "a failed poll keeps the last reading and ages it",
                 "[plugin][example]") {
    HostRig rig(ambient_block(generic_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_http(rig, next, 200, R"({"current":{"t":21.4,"rh":38}})");
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");

    // The endpoint goes away: the values stay, the status carries the failure
    // and the age line counts up on the virtual clock. Every pending poll is
    // answered as it fires, so the two-in-flight cap never trips.
    process_lvgl(10500);
    answer_http(rig, next, 500, "");
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");
    CHECK(watcher_str("ambient-tile__status_line") == "HTTP 500");

    for (int i = 0; i < 40; ++i) { // ~44s more of failing polls, aged past a minute
        process_lvgl(1100);
        answer_http(rig, next, 500, "");
    }
    CHECK(watcher_str("ambient-tile__age_text") == "Updated 1m ago");
}

TEST_CASE_METHOD(XMLTestFixture, "a body without the pointer is bad data", "[plugin][example]") {
    json settings = generic_settings();
    settings["humidity_pointer"] = "no.such.path";
    HostRig rig(ambient_block(settings));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_http(rig, next, 200, R"({"current":{"t":21.4}})");
    // The temperature still lands; the missing humidity alone is not an error.
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°");
    CHECK(watcher_str("ambient-tile__status_line") == "");

    settings["temp_pointer"] = "also.missing";
    REQUIRE(rig.host->set_setting("ambient-tile", "temp_pointer", "also.missing"));
    answer_http(rig, next, 200, R"({"current":{"t":21.4}})");
    CHECK(watcher_str("ambient-tile__status_line") == "bad data");
    CHECK(watcher_str("ambient-tile__temp_text") == "21.4°"); // last good kept
}

TEST_CASE_METHOD(XMLTestFixture, "two readings commit a sparkline onto the canvas",
                 "[plugin][example]") {
    HostRig rig(ambient_block(generic_settings()));
    rig.host->load_from("examples/plugins");
    auto tile = make_tile();
    process_lvgl(50); // the canvas's first layout fires its on_size
    size_t next = 0;
    answer_http(rig, next, 200, R"({"current":{"t":20.0,"rh":40}})");
    process_lvgl(10500);
    answer_http(rig, next, 200, R"({"current":{"t":22.0,"rh":40}})");
    CHECK(watcher_str("ambient-tile__temp_text") == "22.0\u00B0");
    const DisplayList* list = canvas_committed("ambient-tile__spark");
    REQUIRE(list);
    bool polyline = false;
    for (const CanvasPrim& p : list->prims)
        if (p.op == CanvasOp::Polyline)
            polyline = true;
    CHECK(polyline);
    tile.reset();
}

TEST_CASE_METHOD(XMLTestFixture, "payloads and status follow the screen language",
                 "[plugin][example]") {
    HostRig rig(ambient_block(json{{"source", "JSON endpoint"}}));
    rig.host->load_from("examples/plugins");
    CHECK(watcher_str("ambient-tile__age_text") == "Never");

    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");
    helix::ui::locale_set_language("de");
    CHECK(watcher_str("ambient-tile__age_text") == "Nie");
    CHECK(watcher_str("ambient-tile__status_line") == "nicht konfiguriert");
    CHECK(watcher_str("ambient-tile__source_line") == "JSON endpoint · (nicht gesetzt)");

    lv_translation_set_language("en");
    helix::ui::locale_set_language("en");
    CHECK(watcher_str("ambient-tile__age_text") == "Never");
}

#endif // HELIX_HAS_PLUGINS
