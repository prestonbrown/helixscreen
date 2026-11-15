// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_fixtures.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "connection_state.h"
#include "locale_formats.h"
#include "lua_bindings.h"
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
    lv_subject_set_string(lv_xml_get_subject(nullptr, subject), value);
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

void set_connection(helix::ConnectionState state) {
    set_int("printer_connection_state", static_cast<int>(state));
    drain();
}

/// Every string the plugin puts on screen, for proving a secret reaches none.
std::string visible_text() {
    std::string all;
    for (const char* name :
         {"status_text", "last_event", "last_file", "dest_line", "dest_hint", "word"}) {
        all += watcher_str((std::string("print-watcher__") + name).c_str()) + "\n";
    }
    for (int i = 1; i <= 5; ++i) {
        for (const char* part : {"_ev", "_det"}) {
            all += watcher_str(("print-watcher__log" + std::to_string(i) + part).c_str()) + "\n";
        }
    }
    return all;
}

/// Puts the screen language back when a case ends, passed or failed.
struct LanguageGuard {
    ~LanguageGuard() {
        lv_translation_set_language("en");
        helix::ui::locale_set_language("en");
    }
};

/// The tile told its size, as the home panel does on placement.
void size_tile(HostRig& rig, int cols, int rows, int w, int h) {
    LuaRuntime* rt = rig.host->runtime("print-watcher");
    REQUIRE(rt);
    LuaRuntime::PushFn push = [=](lua_State* co) {
        lua_pushinteger(co, cols);
        lua_pushinteger(co, rows);
        lua_pushinteger(co, w);
        lua_pushinteger(co, h);
        return 4;
    };
    REQUIRE(dispatch_widget_hook(*rt, "print-watcher__tile", WidgetHook::Size, push));
    drain();
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

FakeBackend::Request* last_http(HostRig& rig) {
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
    size_tile(rig, 4, 2, 480, 230); // wide enough for all four events
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

TEST_CASE_METHOD(XMLTestFixture, "the test button sends its message without an event prefix",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    rig.host->dispatch_event("print-watcher__test");
    drain();
    REQUIRE(http_count(rig) == 1);
    CHECK(last_http(rig)->c == "Print Watcher test");
    CHECK(last_http(rig)->params.value("X-Title", "") == "Test");
}

TEST_CASE_METHOD(XMLTestFixture, "payloads follow the screen language", "[plugin][example]") {
    reset_printer("benchy.gcode");
    LanguageGuard restore_language;
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

    // Subject-carried strings follow the language too: the 2x1 tile's
    // last-event line reads the German event word.
    CHECK(watcher_str("print-watcher__last_event") == "Gestartet");
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a non-2xx reply is a failure that shows its status, never the token",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    helix::TextLogCapture log;
    HostRig rig(watcher_block(json{{"service", "Telegram"}, {"dest", "123456:SECRETtok@98765"}}));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    edge("printing");
    answer_http(rig, next, 500);

    CHECK(watcher_int("print-watcher__sev") == 2);
    CHECK(watcher_str("print-watcher__status_text") == "HTTP 500");
    CHECK(watcher_str("print-watcher__log1_det").find("failed") != std::string::npos);
    CHECK(log.contains("notify failed: HTTP 500"));
    CHECK(visible_text().find("SECRETtok") == std::string::npos);
    CHECK_FALSE(log.contains("SECRETtok"));
}

TEST_CASE_METHOD(XMLTestFixture, "a transport error shows a fixed phrase, never the request URL",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    helix::TextLogCapture log;
    HostRig rig(watcher_block(json{{"service", "Telegram"}, {"dest", "123456:SECRETtok@98765"}}));
    rig.host->load_from("examples/plugins");
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    // The backend's own error text names the request URL, token included.
    last_http(rig)->reply(RpcResult{
        false, {}, "request to https://api.telegram.org/bot123456:SECRETtok/sendMessage failed"});
    drain();

    CHECK(watcher_int("print-watcher__sev") == 2);
    CHECK(watcher_str("print-watcher__status_text") == "network error");
    CHECK(watcher_str("print-watcher__log1_det").find("failed") != std::string::npos);
    CHECK(log.contains("notify failed: network error"));
    CHECK(visible_text().find("SECRETtok") == std::string::npos);
    CHECK_FALSE(log.contains("SECRETtok"));
}

TEST_CASE_METHOD(XMLTestFixture, "the destination is masked on screen", "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(
        json{{"service", "Discord"}, {"dest", "https://discord.com/api/webhooks/1/SECRETtoken"}}));
    rig.host->load_from("examples/plugins");
    CHECK(watcher_str("print-watcher__dest_line") == "Discord \xC2\xB7 discord.com/...oken");
    CHECK(visible_text().find("SECRET") == std::string::npos);

    REQUIRE(rig.host->set_setting("print-watcher", "service", "Telegram"));
    REQUIRE(rig.host->set_setting("print-watcher", "dest", "123456:SECRETtok@98765"));
    CHECK(watcher_str("print-watcher__dest_line") == "Telegram \xC2\xB7 api.telegram.org/...8765");
    CHECK(visible_text().find("SECRET") == std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "an ntfy topic with URL metacharacters is refused",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings("my topic?x=1")));
    rig.host->load_from("examples/plugins");
    edge("printing");
    CHECK(http_count(rig) == 0);
    CHECK(watcher_str("print-watcher__status_text") == "not a valid ntfy topic");
}

TEST_CASE_METHOD(XMLTestFixture, "cancelled and error end a running print, nothing else does",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;

    edge("printing");
    answer_http(rig, next, 200);
    edge("cancelled");
    REQUIRE(http_count(rig) == 2);
    CHECK(last_http(rig)->c == "Canceled: benchy.gcode (43%)");
    CHECK(last_http(rig)->params.value("X-Title", "") == "Canceled");
    answer_http(rig, next, 200);

    edge("printing");
    answer_http(rig, next, 200);
    edge("error");
    REQUIRE(http_count(rig) == 4);
    CHECK(last_http(rig)->c == "Failed: benchy.gcode (43%)");
    CHECK(last_http(rig)->params.value("X-Title", "") == "Failed");
    answer_http(rig, next, 200);

    // A printer resting in a terminal state that moves to another one
    // (error -> cancelled -> complete) ended no print.
    edge("cancelled");
    edge("complete");
    CHECK(http_count(rig) == 4);
}

TEST_CASE_METHOD(XMLTestFixture, "include_filename off keeps the filename out of every payload",
                 "[plugin][example]") {
    reset_printer("secret-part.gcode");
    json ntfy = ntfy_settings();
    ntfy["include_filename"] = false;
    {
        HostRig rig(watcher_block(ntfy));
        rig.host->load_from("examples/plugins");
        edge("printing");
        REQUIRE(http_count(rig) == 1);
        CHECK(last_http(rig)->c == "Started (43%)");
    }
    reset_printer("secret-part.gcode");
    {
        HostRig rig(watcher_block(json{{"service", "Webhook"},
                                       {"dest", "https://example.com/hook"},
                                       {"include_filename", false}}));
        rig.host->load_from("examples/plugins");
        edge("printing");
        REQUIRE(http_count(rig) == 1);
        const json body = json::parse(last_http(rig)->c);
        CHECK_FALSE(body.contains("filename"));
        CHECK(last_http(rig)->c.find("secret-part") == std::string::npos);
        CHECK(body.value("event", "") == "started");
    }
}

TEST_CASE_METHOD(XMLTestFixture, "the first state after a connect is a baseline, not an event",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;

    // Klipper rests in "complete" after the last job; a screen reboot reads
    // that as the printer's current state.
    set_connection(helix::ConnectionState::CONNECTED);
    edge("complete");
    CHECK(http_count(rig) == 0);

    // The settle window ends on a timer, because an idle printer reports no
    // change at all; the next real start is announced.
    process_lvgl(5100);
    edge("printing");
    REQUIRE(http_count(rig) == 1);
    CHECK(last_http(rig)->c == "Started: benchy.gcode (43%)");
    answer_http(rig, next, 200);
}

TEST_CASE_METHOD(XMLTestFixture, "a connect mid-print is silent until the print ends",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");

    set_connection(helix::ConnectionState::CONNECTED);
    edge("printing");
    CHECK(http_count(rig) == 0);

    process_lvgl(5100);
    set_int("print_progress", 100);
    edge("complete");
    REQUIRE(http_count(rig) == 1);
    CHECK(last_http(rig)->c == "Completed: benchy.gcode (100%)");
}

TEST_CASE_METHOD(XMLTestFixture,
                 "reconnecting raises no plugin error and re-opens the settle window",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    helix::TextLogCapture log;
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;

    // Drop and reconnect while the first settle timer is still pending, then
    // again after it has fired.
    set_connection(helix::ConnectionState::CONNECTED);
    set_connection(helix::ConnectionState::DISCONNECTED);
    set_connection(helix::ConnectionState::CONNECTED);
    process_lvgl(5100);
    set_connection(helix::ConnectionState::DISCONNECTED);
    set_connection(helix::ConnectionState::CONNECTED);

    // The second connect is settling: a state reported now is a baseline.
    edge("printing");
    CHECK(http_count(rig) == 0);
    process_lvgl(5100);
    edge("paused");
    REQUIRE(http_count(rig) == 1);
    answer_http(rig, next, 200);
    edge("printing");
    answer_http(rig, next, 200);
    edge("complete");
    CHECK(last_http(rig)->c == "Completed: benchy.gcode (43%)");

    REQUIRE(rig.info("print-watcher"));
    CHECK(rig.info("print-watcher")->status == PluginStatus::Loaded);
    CHECK_FALSE(log.contains("bad argument"));
    CHECK_FALSE(log.contains("fault"));
}

TEST_CASE_METHOD(XMLTestFixture, "a state that was never a running print posts no ending",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    edge("complete");
    edge("standby");
    edge("cancelled");
    edge("error");
    CHECK(http_count(rig) == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "muting drops events that were queued before it",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    edge("printing"); // in flight
    edge("paused");   // queued behind it
    REQUIRE(http_count(rig) == 1);
    rig.host->dispatch_event("print-watcher__flip:muted");
    drain();
    answer_http(rig, next, 200);
    CHECK(http_count(rig) == 1); // the queued pause never leaves

    // The test button is an explicit request and still goes out.
    rig.host->dispatch_event("print-watcher__test");
    drain();
    CHECK(http_count(rig) == 2);
}

TEST_CASE_METHOD(XMLTestFixture, "the event log shows three rows under four columns, five at four",
                 "[plugin][example]") {
    reset_printer("benchy.gcode");
    HostRig rig(watcher_block(ntfy_settings()));
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    for (int i = 0; i < 3; ++i) {
        edge("printing");
        answer_http(rig, next, 200);
        edge("paused");
        answer_http(rig, next, 200);
    }
    REQUIRE(http_count(rig) == 6); // six events in a ring of five

    size_tile(rig, 2, 2, 233, 230);
    CHECK(watcher_int("print-watcher__log1_on") == 1);
    CHECK(watcher_int("print-watcher__log3_on") == 1);
    CHECK(watcher_int("print-watcher__log4_on") == 0);
    CHECK(watcher_int("print-watcher__log5_on") == 0);

    // A new event on the narrow tile must not widen the list.
    edge("printing");
    answer_http(rig, next, 200);
    CHECK(watcher_int("print-watcher__log4_on") == 0);
    CHECK(watcher_int("print-watcher__log5_on") == 0);

    size_tile(rig, 4, 2, 480, 230);
    CHECK(watcher_int("print-watcher__log4_on") == 1);
    CHECK(watcher_int("print-watcher__log5_on") == 1);
}

#endif // HELIX_HAS_PLUGINS
