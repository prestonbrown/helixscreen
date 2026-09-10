// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#include <atomic>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <thread>

/**
 * @brief Mock WiFi network with password for testing
 *
 * Extends public WiFiNetwork info (SSID, signal, security type) with mock-specific
 * data (expected password). Real backends don't store passwords - they're only
 * needed for mock authentication simulation.
 */
struct MockWiFiNetwork {
    WiFiNetwork network;  ///< Public network info (SSID, signal, is_secured, security_type: "WPA2",
                          ///< "WPA3", "Open", etc.)
    std::string password; ///< Expected password for authentication (empty for open networks)

    MockWiFiNetwork(const std::string& ssid, int strength, bool secured,
                    const std::string& security, const std::string& pass = "", int freq_mhz = 0)
        : network(ssid, strength, secured, security, freq_mhz), password(pass) {}
};

/**
 * @brief Mock WiFi backend for simulator and testing
 *
 * Provides fake WiFi functionality with realistic behavior:
 * - Static list of mock networks with varying signal strength
 * - Simulated scan delays
 * - Simulated connection delays with success/failure scenarios
 * - Random signal strength variations for realism
 * - LVGL timer integration for async events
 *
 * Perfect for:
 * - macOS/simulator development
 * - UI testing without real WiFi hardware
 * - Automated testing scenarios
 */
class WifiBackendMock : public WifiBackend {
  public:
    WifiBackendMock();
    ~WifiBackendMock();

    // ========================================================================
    // WifiBackend Interface Implementation
    // ========================================================================

    WiFiError start() override;
    void start_async() override;
    void stop() override;
    bool is_running() const override;

    void register_event_callback(const std::string& name,
                                 std::function<void(const std::string&)> callback) override;

    WiFiError trigger_scan() override;
    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override;
    WiFiError connect_network(const std::string& ssid, const std::string& password) override;
    WiFiError disconnect_network() override;
    WiFiError set_radio_enabled(bool on) override;
    bool is_radio_enabled() const override;
    ConnectionStatus get_status() override;
    bool supports_5ghz() const override;
    WiFiError forget_network(const std::string& ssid) override;
    std::optional<helix::wifi::WifiInterface> resolved_interface() const override;

    // Test helpers — allow test code to drive state directly without going
    // through the simulated async connect/disconnect flow.
    void set_connected_state(bool connected, const std::string& ssid = "",
                             const std::string& ip = "", int signal = 0);

    /// Test helper — stands in for real interface resolution (which this
    /// backend never performs on its own) so tests can exercise callers that
    /// branch on resolved_interface(), e.g. WiFiManager's stranding-prevention
    /// gate (Task 15). Defaults to nullopt, same as the base class.
    void set_resolved_interface_for_test(std::optional<helix::wifi::WifiInterface> iface) {
        resolved_interface_ = std::move(iface);
    }

    /// Test helper — makes join_displaces_wired_link() answer true, mirroring
    /// a single-transport daemon holding the link on Ethernet so the
    /// manager's advisory can be tested without a netd daemon.
    void set_join_displaces_wired_link_for_test(bool displaces) {
        join_displaces_wired_link_ = displaces;
    }

    bool join_displaces_wired_link() override {
        return join_displaces_wired_link_;
    }

  private:
    // ========================================================================
    // Internal State
    // ========================================================================

    bool running_;
    bool connected_;
    std::string connected_ssid_;
    std::string connected_ip_;
    int connected_signal_;
    bool radio_enabled_{true};
    bool join_displaces_wired_link_{false};
    std::optional<helix::wifi::WifiInterface> resolved_interface_;

    // Event system
    std::map<std::string, std::function<void(const std::string&)>> callbacks_;

    // Async timers for scan/connect simulation (std::thread based - no LVGL dependency)
    std::thread scan_thread_;
    std::thread connect_thread_;
    std::atomic<bool> scan_active_{false};
    std::atomic<bool> connect_active_{false};

    // Mock networks (realistic variety with passwords)
    std::vector<MockWiFiNetwork> mock_networks_;
    std::mt19937 rng_; // Random number generator for signal variations

    // SSIDs "saved" by a connect_network() call, standing in for wpa_supplicant's
    // own network list — forget_network() checks and clears this set the same
    // way the real backend checks LIST_NETWORKS. Recorded synchronously in
    // connect_network() rather than only after the simulated connect delay
    // completes, mirroring how the real backend's SAVE_CONFIG happens before
    // the CONNECTED event.
    std::set<std::string> saved_networks_;

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    void init_mock_networks();
    void vary_signal_strengths(); // Add realism with signal variations
    void fire_event(const std::string& event_name, const std::string& data = "");

    // Thread functions for async scan/connect simulation
    void scan_thread_func();
    void connect_thread_func();

    // Connection simulation state
    std::string connecting_ssid_;
    std::string connecting_password_;
    std::function<void(bool, const std::string&)> connect_callback_;
};