// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_api_error_i18n.cpp
 * @brief Error text the network layer hands the UI reaches the user in their language.
 *
 * MoonrakerError::localized_message() translates on the main thread while
 * user_message() stays English for any thread; notify_error_tr() carries a
 * WebSocket-thread failure to the main thread before translating; and
 * MoonrakerEvent keeps its untranslated template for the presenter.
 */

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/abort_manager_test_access.h"
#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/moonraker_request_tracker_test_access.h"
#include "../test_helpers/scoped_language.h"
#include "../ui_test_utils.h"
#include "abort_manager.h"
#include "ams_backend_afc.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "moonraker_events.h"
#include "moonraker_manager.h"
#include "moonraker_request_tracker.h"
#include "printer_state.h"
#include "rpc_error_policy.h"
#include "translation_loader.h"

#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLTestFixture,
                 "MoonrakerError text: localized on the main thread, English elsewhere",
                 "[api-error-i18n][i18n]") {
    ScopedLanguage de("de");

    const MoonrakerError timeout = MoonrakerError::timeout("printer.gcode.script", 30000);
    CHECK(timeout.localized_message() ==
          "Zeitüberschreitung der Anfrage. Der Drucker ist möglicherweise beschäftigt.");
    // user_message() is the any-thread accessor, so it never consults lv_tr.
    CHECK(timeout.user_message() == "Request timed out. The printer may be busy.");

    CHECK(MoonrakerError::connection_lost("printer.gcode.script").localized_message() ==
          "Verbindung zum Drucker verloren.");

    // A refusal HelixScreen issues itself carries its key.
    const MoonrakerError busy = MoonrakerError::refusal(
        "printer.gcode.script", TR_NOOP("Printer is busy — try again in a moment"));
    CHECK(busy.user_message() == "Printer is busy — try again in a moment");
    CHECK(busy.localized_message() == "Drucker ist beschäftigt — versuche es gleich noch einmal");

    // Klipper's own words are not ours to translate.
    MoonrakerError klipper;
    klipper.type = MoonrakerErrorType::JSON_RPC_ERROR;
    klipper.message = "Must home axis first";
    CHECK(klipper.localized_message() == "Must home axis first");
}

TEST_CASE_METHOD(LVGLTestFixture, "notify_error_tr translates on the main thread",
                 "[api-error-i18n][i18n]") {
    ScopedLanguage de("de");
    std::string shown;
    int calls = 0;
    helix::ui::set_test_notification_error_hook([&](const std::string& m) {
        shown = m;
        ++calls;
    });

    const MoonrakerError err = MoonrakerError::timeout("printer.gcode.script", 30000);
    std::thread ws(
        [err]() { helix::ui::notify_error_tr(TR_NOOP("Failed to set print speed: {}"), err); });
    ws.join();
    // Nothing is formatted or shown on the calling thread.
    CHECK(calls == 0);

    helix::ui::UpdateQueue::instance().drain();
    helix::ui::set_test_notification_error_hook(nullptr);
    REQUIRE(calls == 1);
    CHECK(shown.find("Zeitüberschreitung der Anfrage") != std::string::npos);
    CHECK(shown.find("Request timed out") == std::string::npos);
}

namespace {

/// Holds the error callback so the test can answer from a thread of its choosing.
class HeldErrorApi : public MoonrakerAPIMock {
  public:
    using MoonrakerAPIMock::MoonrakerAPIMock;

    void execute_gcode(const std::string& /*gcode*/, SuccessCallback /*on_success*/,
                       ErrorCallback on_error, uint32_t /*timeout_ms*/ = 0, bool /*silent*/ = false,
                       SuccessCallback /*on_queued*/ = nullptr,
                       bool /*caller_surfaces_errors*/ = true,
                       bool /*bypass_busy_gate*/ = false) override {
        held_error = std::move(on_error);
    }

    ErrorCallback held_error;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AFC G-code timeout toast translates on the main thread",
                 "[api-error-i18n][i18n][afc]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    HeldErrorApi api{client, state};
    AmsBackendAfc backend{&api, &client};

    std::string shown;
    int calls = 0;
    helix::ui::set_test_notification_warning_hook([&](const std::string& m) {
        shown = m;
        ++calls;
    });

    AfcTestAccess::execute_gcode_notify(backend, std::string("AFC_RESET"), std::string(),
                                        std::string("AFC reset"));
    REQUIRE(api.held_error);

    std::thread ws(
        [&api]() { api.held_error(MoonrakerError::timeout("printer.gcode.script", 30000)); });
    ws.join();
    // The WebSocket thread neither translates nor shows anything.
    CHECK(calls == 0);

    helix::ui::UpdateQueue::instance().drain();
    helix::ui::set_test_notification_warning_hook(nullptr);
    REQUIRE(calls == 1);
    CHECK(shown.find("AFC reset") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Tracker RPC_ERROR event renders in the active language",
                 "[api-error-i18n][i18n]") {
    helix::AbortManagerTestAccess::reset(helix::AbortManager::instance());
    MoonrakerRequestTracker tracker;

    // No error callback and not silent: the generic RPC_ERROR toast is the only surface.
    PendingRequest request;
    request.id = 4243;
    request.method = "printer.objects.query";
    request.timeout_ms = 60000;
    request.timestamp = std::chrono::steady_clock::now();
    request.intent = helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                           /*surfaces_errors=*/false};
    const json error_response = {
        {"jsonrpc", "2.0"},
        {"id", 4243},
        {"error", {{"code", -32601}, {"message", "Klippy not ready"}}},
    };

    std::vector<MoonrakerEvent> events;
    MoonrakerRequestTrackerTestAccess::inject_request(tracker, 4243, request);
    REQUIRE(tracker.route_response(error_response,
                                   [&events](const MoonrakerEvent& e) { events.push_back(e); }));
    REQUIRE(events.size() == 1);
    const MoonrakerEvent& evt = events[0];

    // The log copy stays English whatever the UI language.
    CHECK(evt.message == "Printer command 'printer.objects.query' failed: Klippy not ready");
    REQUIRE(evt.message_tag != nullptr);

    ScopedLanguage de("de");
    CHECK(evt.render(lv_tr(evt.message_tag)) ==
          "Druckerbefehl 'printer.objects.query' fehlgeschlagen: Klippy not ready");
}

// The ESP32 client emits these same factories, so a tag here is a tag on both.
TEST_CASE_METHOD(LVGLTestFixture, "Shared Moonraker client events carry translated templates",
                 "[api-error-i18n][i18n]") {
    struct Expect {
        MoonrakerEvent evt;
        MoonrakerEventType type;
        bool is_error;
    };
    const std::vector<Expect> cases = {
        {moonraker_event::reconnected(), MoonrakerEventType::RECONNECTED, false},
        {moonraker_event::connection_lost_reconnecting(), MoonrakerEventType::CONNECTION_LOST,
         false},
        {moonraker_event::reconnect_stalled(), MoonrakerEventType::CONNECTION_FAILED, true},
        {moonraker_event::rpc_failed("printer.info", "boom"), MoonrakerEventType::RPC_ERROR, true},
        {moonraker_event::request_timed_out("printer.info", 5000),
         MoonrakerEventType::REQUEST_TIMEOUT, false},
        {moonraker_event::subscribe_failed("{}"), MoonrakerEventType::DISCOVERY_FAILED, false},
    };

    ScopedLanguage de("de");
    for (const auto& c : cases) {
        INFO(c.evt.message);
        CHECK(c.evt.type == c.type);
        CHECK(c.evt.is_error == c.is_error);
        REQUIRE(c.evt.message_tag != nullptr);
        CHECK(c.evt.message == c.evt.render(c.evt.message_tag));
        CHECK(std::string(lv_tr(c.evt.message_tag)) != c.evt.message_tag);
    }
    CHECK(cases[4].evt.message == "Printer command 'printer.info' timed out after 5000ms");
    CHECK(cases[3].evt.details == "printer.info");
}

TEST_CASE("MoonrakerEvent::render tolerates a translation's placeholder count",
          "[api-error-i18n][i18n]") {
    const MoonrakerEvent evt = MoonrakerEvent::translatable(
        MoonrakerEventType::REQUEST_TIMEOUT, "cmd '{}' after {}ms", {"G28", "5000"}, false);
    CHECK(evt.message == "cmd 'G28' after 5000ms");
    CHECK(evt.render("only {}") == "only G28");
    CHECK(evt.render("{} {} {}") == "G28 5000 {}");
    CHECK(evt.render("{ not a placeholder }") == "{ not a placeholder }");
}

TEST_CASE_METHOD(LVGLTestFixture, "MoonrakerManager presents an event in the active language",
                 "[api-error-i18n][i18n]") {
    MoonrakerManager mgr;
    std::string shown;
    helix::ui::set_test_notification_error_hook([&](const std::string& m) { shown = m; });

    const MoonrakerEvent evt = MoonrakerEvent::translatable(
        MoonrakerEventType::RPC_ERROR, TR_NOOP("Printer command '{}' failed: {}"),
        {"printer.objects.query", "Klippy not ready"}, true, "printer.objects.query");
    {
        ScopedLanguage de("de");
        MoonrakerManagerTestAccess::present_event(mgr, evt);
    }
    helix::ui::set_test_notification_error_hook(nullptr);

    CHECK(shown.find("Druckerbefehl 'printer.objects.query' fehlgeschlagen: Klippy not ready") !=
          std::string::npos);
}
