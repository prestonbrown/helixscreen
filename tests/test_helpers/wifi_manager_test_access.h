// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file wifi_manager_test_access.h
 * @brief The shared friend accessor for driving WiFiManager's private
 *        connection handlers from tests.
 *
 * One copy (previously three hand-written variants in
 * test_wifi_observer_notification.cpp, test_wifi_manager_auth_debounce.cpp,
 * and test_network_settings_transport_refresh.cpp). The handlers under test
 * are production's own; all this shim reimplements is the BACKEND-side state
 * write a real nmcli or wpa_supplicant poll would have made before the event
 * fired.
 */

#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <functional>
#include <string>

namespace helix {

class WiFiManagerTestAccess {
  public:
    /// Drive handle_connected() with production's ordering: the backend
    /// updates its state BEFORE firing CONNECTED, so is_connected() is
    /// already true when a state observer's refresh runs
    /// (prestonbrown/helixscreen#1059). When the manager's backend is not
    /// the mock, this is the raw handler call.
    static void fire_connected(WiFiManager& wm, const std::string& data = "") {
        if (auto* mock = dynamic_cast<WifiBackendMock*>(wm.backend_.get())) {
            mock->set_connected_state(true, "TestSSID", "192.168.1.100", 75);
        }
        wm.handle_connected(data);
    }

    /// Drive handle_disconnected() with the same ordering: the mock's
    /// disconnect path clears its connected state before firing.
    static void fire_disconnected(WiFiManager& wm, const std::string& data = "") {
        if (auto* mock = dynamic_cast<WifiBackendMock*>(wm.backend_.get())) {
            mock->set_connected_state(false);
        }
        wm.handle_disconnected(data);
    }

    /// Simulate an in-flight connect() without invoking the backend.
    static void begin_connect(WiFiManager& wm, std::function<void(bool, const std::string&)> cb) {
        wm.connect_callback_ = std::move(cb);
        wm.connecting_in_progress_ = true;
    }

    static void fire_auth_failed(WiFiManager& wm, const std::string& data) {
        wm.handle_auth_failed(data);
    }

    /// Drive handle_init_failed() exactly as the backend's INIT_FAILED event
    /// would. The NM->wpa fallback it schedules runs on the next queue drain.
    static void fire_init_failed(WiFiManager& wm, bool silent, const std::string& msg) {
        wm.handle_init_failed(silent, msg);
    }

    /// Whether the scan scheduler would allow another periodic scan — the
    /// latch prestonbrown/helixscreen#1405 is about. False while a scan is
    /// outstanding; permanently false once a backend swap strands one.
    static bool scan_should_trigger(const WiFiManager& wm) {
        return wm.scan_scheduler_.should_trigger();
    }

    static bool grace_pending(WiFiManager& wm) {
        return wm.auth_fail_grace_timer_ != nullptr;
    }

    static bool connecting(WiFiManager& wm) {
        return wm.connecting_in_progress_;
    }

    static void add_observer(WiFiManager& wm, helix::LifetimeToken token,
                             std::function<void()> cb) {
        wm.add_state_observer(std::move(token), std::move(cb));
    }
};

} // namespace helix
