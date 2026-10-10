// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "locale_formats.h"
#include "panel_widget_registry.h"
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

json watcher_block(const json& settings) {
    json block = enabled("print-watcher", {"http"});
    block["settings"]["print-watcher"] = settings;
    return block;
}

json ntfy_settings(const std::string& dest = "my-topic") {
    return json{{"service", "ntfy"}, {"dest", dest}};
}

void set_str(const char* subject, const char* value) {
    lv_subject_copy_string(lv_xml_get_subject(nullptr, subject), value);
}

void set_int(const char* subject, int value) {
    lv_subject_set_int(lv_xml_get_subject(nullptr, subject), value);
}

std::string watcher_str(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

int watcher_int(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}

/// Printer subjects are global and outlive each case, so every case starts
/// from a known state: standby before load (the plugin snapshots it), with a
/// seeded filename and progress.
void reset_printer(const char* file) {
    set_str("print_state", "standby");
    set_str("print_filename", file);
    set_int("print_progress", 43);
    drain();
}

/// Drains the queue so a state edge reaches its http request (or its refusal).
void edge(const char* state) {
    set_str("print_state", state);
    drain();
}

/// Answers every http request since `next` with `status`, settling each send.
void answer_http(HostRig& rig, size_t& next, int status) {
    while (next < rig.fake.requests.size()) {
        auto& r = rig.fake.requests[next++];
        if (r.kind == "http")
            r.reply(RpcResult{true, json{{"status", status}, {"body", ""}}, {}});
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

const FakeBackend::Request* last_http(HostRig& rig) {
    for (auto it = rig.fake.requests.rbegin(); it != rig.fake.requests.rend(); ++it)
        if (it->kind == "http")
            return &*it;
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "print-watcher loads with a registered adaptive widget",
                 "[plugin][example]") {
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("print-watcher"));
    CHECK(rig.info("print-watcher")->status == PluginStatus::Loaded);

    const helix::PanelWidgetDef* def = helix::find_widget_def("print-watcher__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 2); // one cell, in tracks
    CHECK(def->rowspan == 2);
    CHECK(def->max_colspan == 8);
    CHECK(def->max_rowspan == 4);
    rig.host->disable("print-watcher");
    CHECK(helix::find_widget_def("print-watcher__tile") == nullptr);

    for (const char* file : {"print-watcher__tile", "print-watcher__detail"}) {
        const std::string path = std::string("examples/plugins/print-watcher/ui/") + file + ".xml";
        CAPTURE(path);
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("print-watcher", {"print-watcher__tile", "print-watcher__detail"},
                               xml) == std::string());
    }
}

TEST_CASE_METHOD(XMLTestFixture, "state edges classify and post to ntfy, one at a time",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;

    edge("printing");
    REQUIRE(http_count(rig) == 1);
    {
        const auto& r = *last_http(rig);
        CHECK(r.a == "POST");
        CHECK(r.b == "https://ntfy.sh/my-topic");
        CHECK(r.c == "Started: benchy.gcode (43%)");
        CHECK(r.params.value("X-Title", "") == "Started");
    }
    answer_http(rig, next, 200);
    CHECK(watcher_str("print-watcher__status_text") == "sent");

    edge("paused");
    edge("printing"); // queued behind the paused send
    REQUIRE(http_count(rig) == 2);
    CHECK(last_http(rig)->c == "Paused: benchy.gcode (43%)");
    answer_http(rig, next, 200);
    REQUIRE(http_count(rig) == 3); // the queue only now started the resumed send
    CHECK(last_http(rig)->c == "Resumed: benchy.gcode (43%)");
    answer_http(rig, next, 200); // resumed settles, so the next edge can send

    set_int("print_progress", 100);
    edge("complete");
    REQUIRE(http_count(rig) == 4);
    CHECK(last_http(rig)->c == "Completed: benchy.gcode (100%)");
    answer_http(rig, next, 200);

    // The ring carries the four events, newest first, after each one settles.
    CHECK(watcher_str("print-watcher__log1_ev") == "Completed");
    CHECK(watcher_str("print-watcher__log2_ev") == "Resumed");
    CHECK(watcher_str("print-watcher__log4_ev") == "Started");
    CHECK(watcher_int("print-watcher__has_events") == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "discord posts a content JSON to the webhook",
                 "[plugin][example]") {
    reset_printer("cube.gcode");
    HostRig rig(watcher_block(
        json{{"service", "Discord"}, {"dest", "https://discord.com/api/webhooks/1/abc"}}));
    rig.host->load_from("examples/plugins");
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    const auto& r = *last_http(rig);
    CHECK(r.b == "https://discord.com/api/webhooks/1/abc");
    CHECK(r.params.value("Content-Type", "") == "application/json");
    const json body = json::parse(r.c);
    CHECK(body.value("content", "") == "Started: cube.gcode (43%)");
}

TEST_CASE_METHOD(XMLTestFixture, "telegram posts chat_id and text at the bot URL",
                 "[plugin][example]") {
    reset_printer("cube.gcode");
    HostRig rig(watcher_block(json{{"service", "Telegram"}, {"dest", "123456:ABC-def@98765"}}));
    rig.host->load_from("examples/plugins");
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    const auto& r = *last_http(rig);
    CHECK(r.b == "https://api.telegram.org/bot123456:ABC-def/sendMessage");
    const json body = json::parse(r.c);
    CHECK(body.value("chat_id", "") == "98765");
    CHECK(body.value("text", "") == "Started: cube.gcode (43%)");
}

TEST_CASE_METHOD(XMLTestFixture, "a generic webhook receives the event blob", "[plugin][example]") {
    reset_printer("cube.gcode");
    HostRig rig(watcher_block(json{{"service", "Webhook"}, {"dest", "https://example.com/hook"}}));
    rig.host->load_from("examples/plugins");
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    const auto& r = *last_http(rig);
    const json body = json::parse(r.c);
    CHECK(body.value("event", "") == "started");
    CHECK(body.value("filename", "") == "cube.gcode");
    CHECK(body.value("progress", 0) == 43);
}

TEST_CASE_METHOD(XMLTestFixture, "an empty destination is refused before the network",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings("")));
    rig.host->load_from("examples/plugins");
    edge("printing");
    CHECK(http_count(rig) == 0);
    CHECK(watcher_str("print-watcher__status_text") == "destination not set");
    CHECK(watcher_str("print-watcher__log1_det").find("bad dest") != std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "a look-alike discord url is refused", "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(json{{"service", "Discord"}, {"dest", "https://example.com/x"}}));
    rig.host->load_from("examples/plugins");
    edge("printing");
    CHECK(http_count(rig) == 0);
    CHECK(watcher_str("print-watcher__status_text") == "not a Discord webhook URL");
}

TEST_CASE_METHOD(XMLTestFixture, "muted holds sends and the overlay switch unmutes",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    json settings = ntfy_settings();
    settings["muted"] = true;
    HostRig rig(watcher_block(settings));
    rig.host->load_from("examples/plugins");
    edge("printing");
    CHECK(http_count(rig) == 0);
    CHECK(watcher_str("print-watcher__word") == "muted");

    // The overlay's own handler: a switch tap announces itself, Lua flips the
    // setting through helix.settings.set, on_change snaps the mirror back.
    rig.host->dispatch_event("print-watcher__flip:muted");
    drain();
    CHECK(watcher_str("print-watcher__word") == "armed");
    edge("paused");
    REQUIRE(http_count(rig) == 1);
    CHECK(last_http(rig)->c == "Paused: benchy.gcode (43%)");
}

TEST_CASE_METHOD(XMLTestFixture, "a disabled event drops its edge, others still send",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    json gated = ntfy_settings();
    gated["ev_paused"] = false;
    HostRig rig(watcher_block(gated));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    edge("printing");
    answer_http(rig, next, 200);
    edge("paused");
    CHECK(http_count(rig) == 1); // no paused send
    edge("complete");
    answer_http(rig, next, 200);
    CHECK(http_count(rig) == 2); // completed still sends
}

TEST_CASE_METHOD(XMLTestFixture, "payloads follow the screen language", "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");

    // The app catalog ("Started") loads the same way a real language switch
    // loads it; the plugin's own pack registered at load.
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");
    helix::ui::locale_set_language("de");
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    // "Started" comes from the app catalog, "sent" and "aktiv" from the pack.
    CHECK(last_http(rig)->c == "Gestartet: benchy.gcode (43%)");
    CHECK(last_http(rig)->params.value("X-Title", "") == "Gestartet");
    size_t next = 0;
    answer_http(rig, next, 200);
    CHECK(watcher_str("print-watcher__status_text") == "gesendet");
    CHECK(watcher_str("print-watcher__word") == "aktiv");

    lv_translation_set_language("en");
    helix::ui::locale_set_language("en");
}

#endif // HELIX_HAS_PLUGINS
