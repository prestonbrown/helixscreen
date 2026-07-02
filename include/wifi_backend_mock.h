// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#include <atomic>
#include <map>
#include <random>
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
                    const std::string& security, const std::string& pass = "")
        : network(ssid, strength, secured, security), password(pass) {}
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
    ConnectionStatus get_status() override;
    bool supports_5ghz() const override;

    // Test helpers — allow test code to drive state directly without going
    // through the simulated async connect/disconnect flow.
    void set_connected_state(bool connected, const std::string& ssid = "",
                             const std::string& ip = "", int signal = 0);

  private:
    // ========================================================================
    // Internal State
    // ========================================================================

    bool running_;
    bool connected_;
    std::string connected_ssid_;
    std::string connected_ip_;
    int connected_signal_;

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