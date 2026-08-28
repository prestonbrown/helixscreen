// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_manager_auth_debounce.cpp
 * @brief Regression test for helixscreen#1050 — transient AUTH_FAILED debounce.
 *
 * Some adapters' wpa_supplicant emit a transient CTRL-EVENT-SSID-TEMP-DISABLED/
 * WRONG_KEY event mid-handshake on a connect that ultimately succeeds (a
 * CONNECTED arrives ~1-3s later). WiFiManager must NOT report failure for that
 * transient event when a CONNECTED follows within the grace window — otherwise
 * the first-run setup wizard's "completed" spinner hangs while WiFi is actually
 * up (Qidi Q2, #1050).
 *
 * The handlers normally fire from the backend event thread; here we drive them
 * directly via WiFiManagerTestAccess and pump the UpdateQueue + LVGL timers, so
 * the AUTH_FAILED -> CONNECTED ordering is deterministic rather than racing the
 * threaded mock backend.
 */

#include "ui_update_queue.h"

#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "runtime_config.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {
// Friend accessor — drives the private connection handlers and inspects the
// grace-timer state without going through the threaded mock backend.
class WiFiManagerTestAccess {
  public:
    static void begin_connect(WiFiManager& wm, std::function<void(bool, const std::string&)> cb) {
        // Simulate an in-flight connect() without invoking the backend.
        wm.connect_callback_ = std::move(cb);
        wm.connecting_in_progress_ = true;
    }
    static void fire_auth_failed(WiFiManager& wm, const std::string& data) {
        wm.handle_auth_failed(data);
    }
    static void fire_connected(WiFiManager& wm, const std::string& data) {
        wm.handle_connected(data);
    }
    static bool grace_pending(WiFiManager& wm) {
        return wm.auth_fail_grace_timer_ != nullptr;
    }
    static bool connecting(WiFiManager& wm) {
        return wm.connecting_in_progress_;
    }
};
} // namespace helix

namespace {

// Enables test mode (→ idle mock WiFi backend, no wpa_supplicant probing) and a
// headless LVGL display for the duration of the test. Restores test_mode on exit.
struct WifiDebounceFixture {
    ScopedRuntimeConfig scoped_config;

    WifiDebounceFixture() {
        auto* rc = get_runtime_config();
        rc->test_mode = true;      // should_mock_wifi() → true
        rc->use_real_wifi = false; // pick the idle mock backend
        lv_init_safe();
        ensure_headless_display();
        helix::ui::UpdateQueue::instance().init();
    }

    std::shared_ptr<WiFiManager> make_manager() {
        auto wm = std::make_shared<WiFiManager>(/*silent=*/true);
        wm->init_self_reference(wm); // required for the async callback weak_ptr
        return wm;
    }
};

} // namespace

TEST_CASE("WiFiManager: transient AUTH_FAILED preempted by CONNECTED delivers success",
          "[wifi][unit][1050]") {
    WifiDebounceFixture fx;
    auto wm = fx.make_manager();

    int calls = 0;
    bool last_success = false;
    WiFiManagerTestAccess::begin_connect(*wm, [&](bool ok, const std::string&) {
        calls++;
        last_success = ok;
    });

    // Transient wpa_supplicant auth failure mid-handshake.
    WiFiManagerTestAccess::fire_auth_failed(*wm, "WRONG_KEY");
    lv_timer_handler_safe(); // drain queue → arms the grace timer
    REQUIRE(WiFiManagerTestAccess::grace_pending(*wm));
    REQUIRE(WiFiManagerTestAccess::connecting(*wm)); // still in progress, not failed
    REQUIRE(calls == 0);                             // failure must NOT be delivered yet

    // The connection actually succeeds shortly after.
    WiFiManagerTestAccess::fire_connected(*wm, "");
    lv_timer_handler_safe(); // drain queue → cancels grace + delivers success
    REQUIRE(calls == 1);
    REQUIRE(last_success == true);
    REQUIRE_FALSE(WiFiManagerTestAccess::grace_pending(*wm));

    // Past the grace window, the cancelled failure must never fire (the bug).
    // Tests drive the LVGL clock explicitly — wait_ms() uses wall-clock and does
    // not advance lv_tick, so timer expiry must be simulated with lv_tick_inc().
    lv_tick_inc(4500);
    lv_timer_handler_safe();
    REQUIRE(calls == 1);
    REQUIRE(last_success == true);
}

TEST_CASE("WiFiManager: AUTH_FAILED with no CONNECTED surfaces failure after grace window",
          "[wifi][unit][1050]") {
    WifiDebounceFixture fx;
    auto wm = fx.make_manager();

    int calls = 0;
    bool last_success = true;
    std::string last_error;
    WiFiManagerTestAccess::begin_connect(*wm, [&](bool ok, const std::string& err) {
        calls++;
        last_success = ok;
        last_error = err;
    });

    WiFiManagerTestAccess::fire_auth_failed(*wm, "WRONG_KEY");
    lv_timer_handler_safe(); // arms the grace timer
    REQUIRE(WiFiManagerTestAccess::grace_pending(*wm));
    REQUIRE(calls == 0);

    // No CONNECTED arrives → after the grace window the failure is real.
    // Advance the LVGL clock past the grace window (lv_tick_inc, not wait_ms —
    // wall-clock sleep does not move lv_tick in tests).
    lv_tick_inc(4500);
    lv_timer_handler_safe();
    REQUIRE(calls == 1);
    REQUIRE(last_success == false);
    REQUIRE(last_error == "WRONG_KEY");
    REQUIRE_FALSE(WiFiManagerTestAccess::grace_pending(*wm));
}
