// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_observer_notification.cpp
 * @brief Tests for WiFiManager state observer notification on CONNECTED
 *        and DISCONNECTED events — regression coverage for the NetworkWidget
 *        race where DISCONNECTED events were silently ignored and CONNECTED
 *        events arriving before the backend's first status poll were missed
 *        (prestonbrown/helixscreen#1059).
 *
 * The handlers normally fire from the backend event thread; here we drive
 * them directly via WiFiManagerTestAccess and pump the UpdateQueue so the
 * observer callback ordering is deterministic.
 */

#include "ui_update_queue.h"

#include "../ui_test_utils.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {
// Friend accessor — drives the private connection/disconnection handlers
// and state observers without going through the threaded mock backend.
class WiFiManagerTestAccess {
  public:
    static void fire_connected(WiFiManager& wm, const std::string& data = "") {
        // Match production: backend updates its state before firing CONNECTED.
        // The mock's connect_thread_func() sets connected_=true before
        // fire_event("CONNECTED"); the NM backend's status_thread_func()
        // updates cached_status_ before fire_event().  Our test helper must
        // mirror this ordering so is_connected() returns true when the state
        // observer runs (prestonbrown/helixscreen#1059).
        if (auto* mock = dynamic_cast<WifiBackendMock*>(wm.backend_.get())) {
            mock->set_connected_state(true, "TestSSID", "192.168.1.100", 75);
        }
        wm.handle_connected(data);
    }
    static void fire_disconnected(WiFiManager& wm, const std::string& data = "") {
        // Match production: backend updates its state before firing DISCONNECTED.
        // The mock's disconnect_network() sets connected_=false before
        // fire_event("DISCONNECTED").
        if (auto* mock = dynamic_cast<WifiBackendMock*>(wm.backend_.get())) {
            mock->set_connected_state(false);
        }
        wm.handle_disconnected(data);
    }
    static void add_observer(WiFiManager& wm, helix::LifetimeToken token,
                             std::function<void()> cb) {
        wm.add_state_observer(std::move(token), std::move(cb));
    }
};
} // namespace helix

namespace {

// Enables test mode (idle mock WiFi backend, no wpa_supplicant probing)
// and a headless LVGL display for the duration of the test.
struct ObserverNotificationFixture {
    RuntimeConfig* rc;
    bool prev_test_mode;
    bool prev_use_real_wifi;

    ObserverNotificationFixture()
        : rc(get_runtime_config()), prev_test_mode(rc->test_mode),
          prev_use_real_wifi(rc->use_real_wifi) {
        rc->test_mode = true;
        rc->use_real_wifi = false;
        lv_init_safe();
        ensure_display();
        helix::ui::UpdateQueue::instance().init();
    }

    ~ObserverNotificationFixture() {
        rc->test_mode = prev_test_mode;
        rc->use_real_wifi = prev_use_real_wifi;
    }

    static void ensure_display() {
        static bool created = false;
        if (created)
            return;
        auto* disp = lv_display_create(480, 320);
        alignas(64) static lv_color_t buf[480 * 10];
        lv_display_set_buffers(disp, buf, nullptr, sizeof(buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(
            disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
        created = true;
    }

    std::shared_ptr<WiFiManager> make_manager() {
        auto wm = std::make_shared<WiFiManager>(/*silent=*/true);
        wm->init_self_reference(wm);
        return wm;
    }
};

} // namespace

TEST_CASE("WiFiManager: state observer fires on CONNECTED event",
          "[wifi][unit][1059][observers]") {
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    helix::AsyncLifetimeGuard lifetime;
    int fires = 0;
    WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&fires]() { fires++; });

    WiFiManagerTestAccess::fire_connected(*wm);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(fires == 1);
}

TEST_CASE("WiFiManager: state observer fires on DISCONNECTED event",
          "[wifi][unit][1059][observers]") {
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    helix::AsyncLifetimeGuard lifetime;
    int fires = 0;
    WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&fires]() { fires++; });

    WiFiManagerTestAccess::fire_disconnected(*wm);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(fires == 1);
}

TEST_CASE("WiFiManager: state observer fires on both CONNECTED and DISCONNECTED",
          "[wifi][unit][1059][observers]") {
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    helix::AsyncLifetimeGuard lifetime;
    int fires = 0;
    WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&fires]() { fires++; });

    WiFiManagerTestAccess::fire_connected(*wm);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fires == 1);

    WiFiManagerTestAccess::fire_disconnected(*wm);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fires == 2);
}

TEST_CASE("WiFiManager: observer fires via notify_state_observers from handle_connected",
          "[wifi][unit][1059][observers]") {
    // The production code path: handle_connected() calls notify_state_observers(),
    // which locks state_observers_mutex_, snapshots the list, and defers each
    // observer callback via tok.defer(). The deferred callback ultimately calls
    // NetworkWidget::detect_network_type(true).
    //
    // This test verifies that the full chain — handle_connected → observer
    // callback — works with the mock backend's connected flow.
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    // Replay: simulate what NetworkWidget::attach() registers.
    helix::AsyncLifetimeGuard lifetime;
    bool backend_ready = false;
    bool wifi_connected = false;
    int call_count = 0;

    WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&]() {
        call_count++;
        backend_ready = true;
        // Simulate detect_network_type(true):
        wifi_connected = wm->is_connected();
    });

    // Fire CONNECTED as the backend would.
    WiFiManagerTestAccess::fire_connected(*wm);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(call_count == 1);
    REQUIRE(backend_ready);
}

TEST_CASE("WiFiManager: observer callback ordering — CONNECTED before DISCONNECTED",
          "[wifi][unit][1059][observers]") {
    // Verify that when the NetworkWidget observer pattern is used, a DISCONNECTED
    // event that arrives after CONNECTED is correctly delivered — this was broken
    // before the fix because detect_network_type()'s condition skipped the WiFi
    // re-check when current_network_ was already Wifi.
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    helix::AsyncLifetimeGuard lifetime;
    std::vector<std::string> events;
    WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&]() {
        events.push_back(wm->is_connected() ? "CONNECTED" : "DISCONNECTED");
    });

    WiFiManagerTestAccess::fire_connected(*wm);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0] == "CONNECTED");

    WiFiManagerTestAccess::fire_disconnected(*wm);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(events.size() == 2);
    REQUIRE(events[1] == "DISCONNECTED");
}

TEST_CASE("WiFiManager: expired lifetime token suppresses observer callback",
          "[wifi][unit][1059][observers]") {
    // Regression: if the NetworkWidget is detached before a pending event
    // arrives, the token is expired and the observer callback must be skipped.
    ObserverNotificationFixture fx;
    auto wm = fx.make_manager();

    int fires = 0;
    {
        helix::AsyncLifetimeGuard lifetime;
        WiFiManagerTestAccess::add_observer(*wm, lifetime.token(), [&fires]() { fires++; });

        // Expire the token — simulates detach().
        lifetime.invalidate();

        WiFiManagerTestAccess::fire_connected(*wm);
        helix::ui::UpdateQueue::instance().drain();
    }

    // Token was expired before the event, so the observer should NOT fire.
    REQUIRE(fires == 0);
}
