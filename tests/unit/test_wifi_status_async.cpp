// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFiManager::get_status_async(): the non-blocking status read UI callers use
// instead of a wpa_supplicant round trip on the LVGL thread.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "async_lifetime_guard.h"
#include "core/http_executor.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::WiFiManager;
using helix::WiFiManagerTestAccess;
using helix::http::HttpExecutor;

namespace {

struct StatusAsyncFixture : LVGLTestFixture {
    WifiBackendMock* mock = nullptr;
    std::shared_ptr<WiFiManager> wm;
    helix::AsyncLifetimeGuard guard;

    StatusAsyncFixture() {
        auto backend = std::make_unique<WifiBackendMock>();
        mock = backend.get();
        wm = std::make_shared<WiFiManager>(std::move(backend));
        wm->init_self_reference(wm);
        HttpExecutor::fast().start();
    }

    ~StatusAsyncFixture() override {
        // Never leave a worker parked in a held read, or the pool stopped for
        // the next test.
        mock->release_held_status();
        HttpExecutor::fast().start();
    }
};

} // namespace

TEST_CASE_METHOD(StatusAsyncFixture, "get_status_async answers inline when the pool is not running",
                 "[wifi][wifi_status_async]") {
    // ESP32 never starts the pool, and submit() on a pool that is not running
    // drops the work, so the answer would otherwise never come.
    HttpExecutor::fast().stop();
    mock->set_connected_state(true, "InlineNet", "192.168.1.100", 75);

    std::string ssid;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus& s) { ssid = s.ssid; });

    CHECK(ssid == "InlineNet");
    CHECK(WiFiManagerTestAccess::radio_ops_inflight(*wm) == 0);
}

TEST_CASE_METHOD(StatusAsyncFixture,
                 "get_status_async runs one read at a time and answers late callers with the next",
                 "[wifi][wifi_status_async]") {
    mock->set_connected_state(true, "Old", "192.168.1.100", 75);
    mock->clear_status_callers();
    mock->hold_next_status();

    std::vector<std::string> first, second, third;
    auto record = [](std::vector<std::string>& into) {
        return [&into](const WifiBackend::ConnectionStatus& s) { into.push_back(s.ssid); };
    };
    wm->get_status_async(guard.token(), record(first));
    REQUIRE(wait_until([&]() { return !mock->status_callers().empty(); }));

    // Both arrive while the first read is parked with "Old" already taken.
    mock->set_connected_state(true, "New", "192.168.1.100", 75);
    wm->get_status_async(guard.token(), record(second));
    wm->get_status_async(guard.token(), record(third));
    CHECK_FALSE(wait_until([&]() { return mock->status_callers().size() > 1; }, 300));

    mock->release_held_status();
    REQUIRE(
        wait_until([&]() { return first.size() == 1 && second.size() == 1 && third.size() == 1; }));
    CHECK(first[0] == "Old");
    CHECK(second[0] == "New");
    CHECK(third[0] == "New");
    CHECK(mock->status_callers().size() == 2);
}

TEST_CASE_METHOD(StatusAsyncFixture,
                 "a status read the executor drops unrun releases its op and its waiters",
                 "[wifi][wifi_status_async]") {
    auto& fast = HttpExecutor::fast();
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    std::atomic<int> parked{0};
    for (int i = 0; i < 4; ++i) {
        fast.submit([open, &parked]() {
            parked++;
            open.wait();
        });
    }
    REQUIRE(wait_until([&]() { return parked.load() == 4; }));

    bool dropped_answered = false;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus&) { dropped_answered = true; });
    CHECK(WiFiManagerTestAccess::radio_ops_inflight(*wm) == 1);

    // Every worker is parked, so the read is still queued: stop() drops it unrun.
    fast.stop(std::chrono::milliseconds(20));
    gate.set_value();
    CHECK(WiFiManagerTestAccess::radio_ops_inflight(*wm) == 0);
    CHECK_FALSE(dropped_answered);

    // The next caller starts a fresh read that also answers the stranded waiter.
    fast.start();
    bool next_answered = false;
    wm->get_status_async(guard.token(),
                         [&](const WifiBackend::ConnectionStatus&) { next_answered = true; });
    REQUIRE(wait_until([&]() { return dropped_answered && next_answered; }));
    CHECK(WiFiManagerTestAccess::radio_ops_inflight(*wm) == 0);
}
