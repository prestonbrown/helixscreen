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

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Enables test mode (idle mock WiFi backend, no wpa_supplicant probing)
// and a headless LVGL display for the duration of the test.
struct ObserverNotificationFixture : LVGLTestFixture {
    ScopedRuntimeConfig scoped_config;

    ObserverNotificationFixture() {
        auto* rc = get_runtime_config();
        rc->test_mode = true;
        rc->use_real_wifi = false;
    }

    std::shared_ptr<WiFiManager> make_manager() {
        auto wm = std::make_shared<WiFiManager>(/*silent=*/true);
        wm->init_self_reference(wm);
        return wm;
    }
};

} // namespace

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
