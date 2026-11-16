// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// What the WebSocket client reports to telemetry. A dropped session is a
// websocket/disconnected error; a close this app asked for (disconnect(), an
// expected Klippy restart) is not. A frame Moonraker wrote with a bare NaN is read,
// not reported as a parse error.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/telemetry_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class CloseTelemetryFixture : public LVGLTestFixture, public TelemetryTestFixture {
  public:
    CloseTelemetryFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        get_printer_state().init_subjects(false);
        TelemetryManager::instance().set_enabled(true);
        TelemetryManager::instance().clear_queue();
    }

    ~CloseTelemetryFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    int websocket_error_events(const char* code) {
        int n = 0;
        for (const auto& e : TelemetryManager::instance().get_queue_snapshot()) {
            if (e.value("event", "") == "error_encountered" &&
                e.value("category", "") == "websocket" && e.value("code", "") == code) {
                ++n;
            }
        }
        return n;
    }

    int websocket_disconnect_events() {
        return websocket_error_events("disconnected");
    }

    void close_open_session() {
        client_.on_ws_open();
        client_.on_ws_close();
        helix::ui::UpdateQueue::instance().drain();
    }

    MoonrakerClientMock client_;
};

} // namespace

TEST_CASE_METHOD(CloseTelemetryFixture, "Close telemetry: a dropped session is reported",
                 "[telemetry][error][moonraker_ws_telemetry]") {
    close_open_session();
    REQUIRE(websocket_disconnect_events() == 1);
}

TEST_CASE_METHOD(CloseTelemetryFixture,
                 "Close telemetry: a close inside the suppression window is not reported",
                 "[telemetry][error][moonraker_ws_telemetry]") {
    client_.suppress_disconnect_modal(2000);
    close_open_session();
    REQUIRE(websocket_disconnect_events() == 0);
}

TEST_CASE_METHOD(CloseTelemetryFixture,
                 "Close telemetry: a connect attempt that never opened is not reported",
                 "[telemetry][error][moonraker_ws_telemetry]") {
    client_.on_ws_close();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(websocket_disconnect_events() == 0);
}

TEST_CASE_METHOD(CloseTelemetryFixture,
                 "WS telemetry: a status frame carrying a bare NaN is delivered with null",
                 "[telemetry][error][moonraker_ws_telemetry]") {
    nlohmann::json received;
    int calls = 0;
    client_.register_method_callback("notify_proc_stat_update", "nan_probe",
                                     [&](const nlohmann::json& msg) {
                                         received = msg;
                                         ++calls;
                                     });

    client_.on_ws_message(
        R"({"jsonrpc": "2.0", "method": "notify_proc_stat_update", "params": [)"
        R"({"temperature": NaN, "low": -Infinity, "high": Infinity, "name": "NaN probe"}]})");
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(calls == 1);
    const auto& p = received["params"][0];
    CHECK(p["temperature"].is_null());
    CHECK(p["low"].is_null());
    CHECK(p["high"].is_null());
    CHECK(p["name"] == "NaN probe");
    CHECK(websocket_error_events("parse_error") == 0);
    client_.unregister_method_callback("notify_proc_stat_update", "nan_probe");
}

TEST_CASE_METHOD(CloseTelemetryFixture, "WS telemetry: a frame that is not JSON is still reported",
                 "[telemetry][error][moonraker_ws_telemetry]") {
    client_.on_ws_message(R"({"jsonrpc": "2.0", "method": )");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(websocket_error_events("parse_error") == 1);
}
