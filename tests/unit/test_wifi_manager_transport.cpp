// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WiFiManager's handling of a backend whose Wi-Fi join takes the link away
 * from a wired transport (a single-transport printer network daemon downs eth0
 * and moves the address to wlan0). The manager warns about the cost and sends
 * the join anyway, and the failure message a caller receives carries the
 * backend's own reason rather than a canned one.
 */

#include "../test_fixtures.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "wifi_backend.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Captures the toasts the manager raises for the duration of one test.
struct ToastCapture {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    ToastCapture() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
    }
    ~ToastCapture() {
        helix::ui::set_test_notification_warning_hook(nullptr);
        helix::ui::set_test_notification_error_hook(nullptr);
    }

    bool warned(const std::string& needle) const {
        for (const auto& w : warnings) {
            if (w.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "WiFiManager still sends a join that displaces the wired link",
                 "[1542][1398][wifi]") {
    ToastCapture toasts;

    auto backend = std::make_unique<WifiBackendMock>();
    REQUIRE(backend->start().success());
    backend->set_join_displaces_wired_link_for_test(true);
    auto* mock = backend.get();

    helix::WiFiManager manager(std::move(backend), /*silent=*/true);

    // The SSID is unknown to the mock, so the backend refuses it for its own
    // reason. What matters is that the manager REACHED the backend: a local
    // refusal would have answered before connect_network ever ran, and would
    // report the transport instead of the mock's not-found reason.
    bool success = true;
    std::string error;
    WiFiResult result = WiFiResult::SUCCESS;
    manager.connect("NotInScanResults", "pw", [&](bool ok, const std::string& msg, WiFiResult r) {
        success = ok;
        error = msg;
        result = r;
    });

    REQUIRE_FALSE(success);
    REQUIRE(result == WiFiResult::NETWORK_NOT_FOUND);

    // And the user was told what the join costs, immediately — not left to
    // the 45s watchdog.
    REQUIRE(toasts.warned(lv_tr("Joining Wi-Fi disconnects the wired network.")));
    REQUIRE(mock->join_displaces_wired_link());
}

TEST_CASE_METHOD(LVGLTestFixture, "WiFiManager stays silent when no wired link is displaced",
                 "[1542][wifi]") {
    ToastCapture toasts;

    auto backend = std::make_unique<WifiBackendMock>();
    REQUIRE(backend->start().success());
    backend->set_join_displaces_wired_link_for_test(false);

    helix::WiFiManager manager(std::move(backend), /*silent=*/true);
    manager.connect("NotInScanResults", "pw", [](bool, const std::string&, WiFiResult) {});

    REQUIRE_FALSE(toasts.warned(lv_tr("Joining Wi-Fi disconnects the wired network.")));
}

TEST_CASE_METHOD(LVGLTestFixture, "WiFiManager reports the backend's own reason for a failed join",
                 "[1542][wifi]") {
    ToastCapture toasts;

    auto backend = std::make_unique<WifiBackendMock>();
    REQUIRE(backend->start().success());

    helix::WiFiManager manager(std::move(backend), /*silent=*/true);

    std::string error;
    manager.connect("NotInScanResults", "",
                    [&](bool, const std::string& msg, WiFiResult) { error = msg; });

    // Whatever the mock said, it is not empty and it is not a canned message —
    // the reason is the only thing the user can act on.
    REQUIRE_FALSE(error.empty());
    REQUIRE(error != lv_tr("Connection failed. Check password."));
    REQUIRE(error != lv_tr("Connection failed. Check credentials."));
    // The toast carries it too.
    REQUIRE_FALSE(toasts.errors.empty());
    REQUIRE(toasts.errors.front().find(error) != std::string::npos);
}

// ============================================================================
// The failure-message decision, shared by every consumer of connect().
// ============================================================================

TEST_CASE("connect_failure_message names the password only for a credential rejection",
          "[1542][wifi]") {
    const std::string auth = "Check your password";

    // The one case canned text beats the backend's words.
    REQUIRE(helix::connect_failure_message(WiFiResult::AUTHENTICATION_FAILED, "WRONG_KEY", auth) ==
            auth);

    // Everything else carries a reason the user has to read. A wrong-password
    // message here is what tells someone whose password is correct to retype
    // it indefinitely.
    REQUIRE(helix::connect_failure_message(WiFiResult::TIMEOUT, "Connection timeout", auth) ==
            "Connection timeout");
    REQUIRE(helix::connect_failure_message(WiFiResult::NETWORK_NOT_FOUND, "Network not in range",
                                           auth) == "Network not in range");
    REQUIRE(helix::connect_failure_message(WiFiResult::PERMISSION_DENIED, "permission denied",
                                           auth) == "permission denied");
    REQUIRE(helix::connect_failure_message(WiFiResult::CONNECTION_FAILED, "netd socket is down",
                                           auth) == "netd socket is down");

    // A reasonless non-auth failure gets a neutral message, never the
    // password one.
    REQUIRE(helix::connect_failure_message(WiFiResult::UNKNOWN_ERROR, "", auth) ==
            lv_tr("Connection failed"));
}

// ============================================================================
// The result a caller receives is what classifies the failure — the manager
// must not leave every consumer sniffing the error string for a substring.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "WiFiManager delivers AUTHENTICATION_FAILED as a result",
                 "[1542][wifi][slow]") {
    auto backend = std::make_unique<WifiBackendMock>();
    REQUIRE(backend->start().success());
    auto manager = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
    manager->init_self_reference(manager);

    int calls = 0;
    WiFiResult result = WiFiResult::SUCCESS;
    std::string error;
    helix::WiFiManagerTestAccess::begin_connect(*manager,
                                                [&](bool, const std::string& msg, WiFiResult r) {
                                                    calls++;
                                                    error = msg;
                                                    result = r;
                                                });

    helix::WiFiManagerTestAccess::fire_auth_failed(*manager, "WRONG_KEY");
    lv_timer_handler_safe(); // drain the queue → arms the grace timer
    REQUIRE(calls == 0);

    lv_tick_inc(5000); // past AUTH_FAIL_GRACE_MS with no CONNECTED
    lv_timer_handler_safe();

    REQUIRE(calls == 1);
    REQUIRE(result == WiFiResult::AUTHENTICATION_FAILED);
    REQUIRE(error == "WRONG_KEY");
}

TEST_CASE_METHOD(LVGLTestFixture, "WiFiManager delivers a watchdog timeout as TIMEOUT",
                 "[1542][wifi][slow]") {
    auto backend = std::make_unique<WifiBackendMock>();
    REQUIRE(backend->start().success());
    auto manager = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
    manager->init_self_reference(manager);

    int calls = 0;
    WiFiResult result = WiFiResult::SUCCESS;
    helix::WiFiManagerTestAccess::begin_connect(*manager,
                                                [&](bool, const std::string&, WiFiResult r) {
                                                    calls++;
                                                    result = r;
                                                });
    helix::WiFiManagerTestAccess::arm_connect_watchdog(*manager);

    lv_tick_inc(46000); // past CONNECT_TIMEOUT_MS
    lv_timer_handler_safe();

    REQUIRE(calls == 1);
    REQUIRE(result == WiFiResult::TIMEOUT);
}
