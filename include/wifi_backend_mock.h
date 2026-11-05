// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <vector>

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

    WiFiError trigger_scan() override;
    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override;
    WiFiError connect_network(const std::string& ssid, const std::string& password,
                              bool is_hidden) override;
    WiFiError disconnect_network() override;
    WiFiError set_radio_enabled(bool on) override;
    bool is_radio_enabled() const override;
    ConnectionStatus get_status() override;
    bool supports_5ghz() const override;
    WiFiError forget_network(const std::string& ssid) override;
    bool supports_forget() const override {
        return true;
    }
    std::optional<helix::wifi::WifiInterface> resolved_interface() const override;

    /// Country starts as HELIX_MOCK_WIFI_COUNTRY ("" = world). set_country()
    /// dispatches REGDOM_CHANGE the way wpa_supplicant does.
    bool supports_country() const override {
        return true;
    }
    WiFiError get_country(std::string& code) override;
    WiFiError set_country(const std::string& code) override;

    /// In memory; HELIX_MOCK_IPV4_APPLY_FAIL=1 makes applies fail.
    bool supports_ipv4_config() const override {
        return true;
    }
    helix::net::Ipv4Result get_ipv4_config(helix::net::Ipv4Config& out) override {
        return ipv4_.get(out);
    }
    helix::net::Ipv4Result apply_ipv4_config(const helix::net::Ipv4Config& config) override {
        return ipv4_.apply(config);
    }

    /// Test helpers: stand in for a country the device's own configuration
    /// set, and count the set_country() calls that reached the radio.
    void set_country_for_test(const std::string& code);
    int set_country_calls() const;

    // Test helpers — allow test code to drive state directly without going
    // through the simulated async connect/disconnect flow.
    void set_connected_state(bool connected, const std::string& ssid = "",
                             const std::string& ip = "", int signal = 0);

    /// Whether the most recent connect_network() request asked for a hidden
    /// association. Lets tests observe the flag WiFiManager threaded through,
    /// which the mock otherwise drops on the floor.
    bool last_connect_hidden() const {
        return last_connect_hidden_;
    }

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

    /// Test helpers for callers that must never block on a status read: the
    /// threads get_status() ran on since the last clear, and a gate that makes
    /// the next get_status() take its answer, then wait for release.
    std::vector<std::thread::id> status_callers();
    void clear_status_callers();
    void hold_next_status();
    void release_held_status();

  private:
    helix::net::MockIpv4Config ipv4_;
    // ========================================================================
    // Internal State
    // ========================================================================

    bool running_;
    std::mutex status_mutex_; ///< get_status() runs on workers; tests write the state
    std::condition_variable status_cv_;
    std::vector<std::thread::id> status_callers_;
    bool hold_next_status_{false};
    bool status_held_{false};
    bool connected_;
    std::string connected_ssid_;
    std::string connected_ip_;
    int connected_signal_;
    bool radio_enabled_{true};
    bool join_displaces_wired_link_{false};
    std::optional<helix::wifi::WifiInterface> resolved_interface_;
    mutable std::mutex country_mutex_; ///< get/set_country() run on the control lane
    std::string country_;
    int set_country_calls_{0};

    // Async timers for scan/connect simulation (std::thread based - no LVGL dependency)
    std::thread scan_thread_;
    std::thread connect_thread_;
    std::atomic<bool> scan_active_{false};
    std::atomic<bool> connect_active_{false};
    std::mutex sim_wait_mutex_; ///< Pairs with sim_wait_cv_ so a cancel wakes a simulated delay
    std::condition_variable sim_wait_cv_;

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

    // Thread functions for async scan/connect simulation
    void scan_thread_func();
    void connect_thread_func();

    /// Clears @p active and wakes any simulated delay waiting on it.
    void cancel_sim(std::atomic<bool>& active);
    /// Sleeps @p delay_ms unless @p active is cleared first; true when still active.
    bool sim_delay(const std::atomic<bool>& active, int delay_ms);

    // Connection simulation state
    std::string connecting_ssid_;
    std::string connecting_password_;
    bool last_connect_hidden_ = false;
    std::function<void(bool, const std::string&)> connect_callback_;
};