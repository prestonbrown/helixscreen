// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// After notify_klippy_disconnected the client polls printer.info until Klippy
// settles, because Moonraker announces a successful start but never a failed
// one: the poll is how a startup error reaches the klippy state.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Answers printer.info from a scripted result and counts the requests; every
/// other method goes to the mock as usual.
class PrinterInfoClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb,
                 uint32_t timeout_ms = 0, bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method != "printer.info") {
            return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                     std::move(error_cb), timeout_ms, silent,
                                                     intent);
        }
        ++info_requests;
        if (hold) {
            held = std::move(success_cb);
        } else if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", info}});
        }
        return 1;
    }

    /// Answer the held request now, with whatever `info` says by then.
    void answer_held() {
        auto cb = std::move(held);
        held = nullptr;
        cb(json{{"jsonrpc", "2.0"}, {"result", info}});
    }

    json info = {{"state", "startup"}, {"state_message", "Printer is starting up"}};
    int info_requests = 0;
    /// Keep the next printer.info's callback in `held` instead of answering it.
    bool hold = false;
    helix::SharedCallback<void(const json&)> held;
};

std::string klippy_notification(const char* method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", json::array()}}.dump();
}

const char* const kKey298 = "Internal error during connect: !{\"code\": \"key298\", \"msg\": "
                            "\"Can not update MCU rpi config as it is shutdown\"}";

class KlippyPollFixture : public LVGLTestFixture {
  public:
    KlippyPollFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        get_printer_state().init_subjects(false);
    }

    ~KlippyPollFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    KlippyState klippy_state() {
        return static_cast<KlippyState>(
            lv_subject_get_int(get_printer_state().network_state().get_klippy_state_subject()));
    }

    void disconnect_klippy() {
        client_.on_ws_message(klippy_notification("notify_klippy_disconnected"));
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(klippy_state() == KlippyState::SHUTDOWN);
    }

    void tick() {
        MoonrakerClientTestAccess::health_tick(client_);
        helix::ui::UpdateQueue::instance().drain();
    }

    PrinterInfoClient client_;
};

} // namespace

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: a startup error after disconnect reaches ERROR",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    REQUIRE(MoonrakerClientTestAccess::klippy_state_polling(client_));

    // Still on its way: nothing applied, the poll carries on.
    client_.info = {{"state", "disconnected"}, {"state_message", ""}};
    tick();
    client_.info = {{"state", "startup"}, {"state_message", "Printer is starting up"}};
    tick();
    REQUIRE(klippy_state() == KlippyState::SHUTDOWN);
    REQUIRE(MoonrakerClientTestAccess::klippy_state_polling(client_));

    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    tick();

    REQUIRE(klippy_state() == KlippyState::ERROR);
    REQUIRE(get_printer_state().network_state().get_klippy_fault_message() == kKey298);
    REQUIRE(client_.info_requests == 3);

    // Settled: the next tick asks nothing.
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 3);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: ready after disconnect reaches READY and stops",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();

    client_.info = {{"state", "ready"}, {"state_message", "Printer is ready"}};
    tick();

    REQUIRE(klippy_state() == KlippyState::READY);
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 1);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: notify_klippy_ready ends the poll",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    client_.on_ws_message(klippy_notification("notify_klippy_ready"));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 0);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: a closed socket ends the poll",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    MoonrakerClientTestAccess::fire_ws_close(client_);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    tick();
    REQUIRE(client_.info_requests == 0);
    REQUIRE(klippy_state() != KlippyState::ERROR);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: nothing polls while Klippy is connected",
                 "[klippy][klippy_poll]") {
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    tick();
    REQUIRE(client_.info_requests == 0);
}

TEST_CASE_METHOD(KlippyPollFixture,
                 "Klippy poll: an answer landing after notify_klippy_ready is dropped",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    client_.hold = true;
    tick();
    REQUIRE(client_.held);

    client_.on_ws_message(klippy_notification("notify_klippy_ready"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(klippy_state() == KlippyState::READY);

    // Asked before Klippy came up; its stale error must not override READY.
    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    client_.answer_held();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(klippy_state() == KlippyState::READY);
}

TEST_CASE_METHOD(KlippyPollFixture,
                 "Klippy poll: an answer landing after a socket close is dropped",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    client_.hold = true;
    tick();
    REQUIRE(client_.held);

    MoonrakerClientTestAccess::fire_ws_close(client_);
    helix::ui::UpdateQueue::instance().drain();
    const KlippyState after_close = klippy_state();

    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    client_.answer_held();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(klippy_state() == after_close);
    REQUIRE(after_close != KlippyState::ERROR);
}

TEST_CASE_METHOD(KlippyPollFixture, "Klippy poll: it gives up after its bound",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    tick();
    REQUIRE(client_.info_requests == 1);
    REQUIRE(MoonrakerClientTestAccess::klippy_state_polling(client_));

    MoonrakerClientTestAccess::age_klippy_state_poll(client_, std::chrono::seconds(121));
    tick();

    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    REQUIRE(client_.info_requests == 1);
    tick();
    REQUIRE(client_.info_requests == 1);
}

TEST_CASE_METHOD(KlippyPollFixture,
                 "Klippy poll: disconnect() ends the poll and drops a late answer",
                 "[klippy][klippy_poll]") {
    disconnect_klippy();
    client_.hold = true;
    tick();
    REQUIRE(client_.info_requests == 1);
    REQUIRE(client_.held);

    // The real client's disconnect, not the mock's simulation of one.
    client_.MoonrakerClient::disconnect();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(MoonrakerClientTestAccess::klippy_state_polling(client_));
    const KlippyState after_disconnect = klippy_state();

    client_.info = {{"state", "error"}, {"state_message", kKey298}};
    client_.answer_held();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(klippy_state() == after_disconnect);
    REQUIRE(after_disconnect != KlippyState::ERROR);

    client_.hold = false;
    tick();
    REQUIRE(client_.info_requests == 1);
}
