// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "wifi_backend_mock.h"

#include "ui_error_reporting.h"

#include "safe_log.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <chrono>

WifiBackendMock::WifiBackendMock()
    : running_(false), connected_(false), connected_signal_(0),
      rng_(std::chrono::steady_clock::now().time_since_epoch().count()) {
    spdlog::debug("[WifiBackend] Mock backend initialized");
    init_mock_networks();
}

WifiBackendMock::~WifiBackendMock() {
    stop();
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WifiBackend] Mock backend destroyed\n");
}

// ============================================================================
// Lifecycle Management
// ============================================================================

WiFiError WifiBackendMock::start() {
    if (running_) {
        spdlog::debug("[WifiBackend] Mock backend already running");
        return WiFiErrorHelper::success();
    }

    running_ = true;
    spdlog::info("[WifiBackend] Mock backend started (simulator mode)");
    return WiFiErrorHelper::success();
}

void WifiBackendMock::stop() {
    if (!running_)
        return;

    // Signal threads to stop
    scan_active_ = false;
    connect_active_ = false;

    // Wait for threads to complete
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    running_ = false;
    connected_ = false;
    connected_ssid_.clear();
    connected_ip_.clear();

    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WifiBackend] Mock backend stopped\n");
}

bool WifiBackendMock::is_running() const {
    return running_;
}

// ============================================================================
// Event System
// ============================================================================

void WifiBackendMock::register_event_callback(const std::string& name,
                                              std::function<void(const std::string&)> callback) {
    callbacks_[name] = callback;
    spdlog::debug("[WifiBackend] Mock: Registered callback for '{}'", name);
}

void WifiBackendMock::fire_event(const std::string& event_name, const std::string& data) {
    spdlog::debug("[WifiBackend] fire_event ENTRY: event_name='{}'", event_name);
    auto it = callbacks_.find(event_name);
    if (it != callbacks_.end()) {
        spdlog::debug("[WifiBackend] fire_event: found callback for '{}', about to invoke",
                      event_name);
        it->second(data);
        spdlog::debug("[WifiBackend] fire_event: callback returned");
    } else {
        spdlog::debug("[WifiBackend] fire_event: no callback registered for '{}'", event_name);
    }
    spdlog::debug("[WifiBackend] fire_event EXIT");
}

// ============================================================================
// Network Scanning
// ============================================================================

WiFiError WifiBackendMock::trigger_scan() {
    if (!running_) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: trigger_scan called but not running");
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Mock backend not running",
                         "WiFi scanner not ready", "Initialize the WiFi system first");
    }

    spdlog::debug("[WifiBackend] Mock: Triggering network scan");

    // Clean up any existing scan thread
    scan_active_ = false;
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }

    // Launch async scan thread (simulates 2-second scan delay)
    scan_active_ = true;
    scan_thread_ = std::thread(&WifiBackendMock::scan_thread_func, this);

    spdlog::debug("[WifiBackend] Mock: Scan thread started");
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!running_) {
        networks.clear();
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Mock backend not running",
                         "WiFi scanner not ready", "Initialize the WiFi system first");
    }

    // Add some realism - vary signal strengths slightly
    vary_signal_strengths();

    // Extract public WiFiNetwork objects (without passwords) and sort by signal strength
    networks.clear();
    networks.reserve(mock_networks_.size());
    for (const auto& mock_net : mock_networks_) {
        networks.push_back(mock_net.network);
    }

    std::sort(networks.begin(), networks.end(), [](const WiFiNetwork& a, const WiFiNetwork& b) {
        return a.signal_strength > b.signal_strength;
    });

    spdlog::debug("[WifiBackend] Mock: Returning {} scan results", networks.size());
    return WiFiErrorHelper::success();
}

void WifiBackendMock::scan_thread_func() {
    // Simulate 2-second scan delay
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Check if still active (not canceled)
    if (!scan_active_) {
        spdlog::debug("[WifiBackend] Mock: Scan thread canceled");
        return;
    }

    spdlog::debug("[WifiBackend] Mock: Scan completed");
    fire_event("SCAN_COMPLETE");
}

// ============================================================================
// Connection Management
// ============================================================================

WiFiError WifiBackendMock::connect_network(const std::string& ssid, const std::string& password) {
    if (!running_) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: connect_network called but not running");
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Mock backend not running",
                         "WiFi system not ready", "Initialize the WiFi system first");
    }

    // Check if network exists in our mock list
    auto it = std::find_if(
        mock_networks_.begin(), mock_networks_.end(),
        [&ssid](const MockWiFiNetwork& mock_net) { return mock_net.network.ssid == ssid; });

    if (it == mock_networks_.end()) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: Network '{}' not found in scan results", ssid);
        return WiFiErrorHelper::network_not_found(ssid);
    }

    // Validate password for secured networks
    if (it->network.is_secured && password.empty()) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: No password provided for secured network '{}'",
                          ssid);
        return WiFiError(
            WiFiResult::INVALID_PARAMETERS, "Password required for secured network: " + ssid,
            "This network requires a password", "Enter the network password and try again");
    }

    spdlog::info("[WifiBackend] Mock: Connecting to '{}'...", ssid);

    connecting_ssid_ = ssid;
    connecting_password_ = password;

    // Cancel and wait for any existing connect thread
    // IMPORTANT: Must join, not detach - detached threads cause use-after-free during destruction
    connect_active_ = false;
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    // Launch async connect thread (simulates 2-3 second delay)
    connect_active_ = true;
    connect_thread_ = std::thread(&WifiBackendMock::connect_thread_func, this);

    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::disconnect_network() {
    if (!connected_) {
        spdlog::debug("[WifiBackend] Mock: disconnect_network called but not connected");
        return WiFiErrorHelper::success(); // Not an error - idempotent operation
    }

    spdlog::info("[WifiBackend] Mock: Disconnecting from '{}'", connected_ssid_);

    connected_ = false;
    std::string old_ssid = connected_ssid_;
    connected_ssid_.clear();
    connected_ip_.clear();
    connected_signal_ = 0;

    fire_event("DISCONNECTED", "reason=user_request");
    return WiFiErrorHelper::success();
}

void WifiBackendMock::connect_thread_func() {
    // Simulate connection delay (2-3 seconds)
    int delay_ms = 2000 + (rng_() % 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    // Check if still active (not canceled)
    if (!connect_active_) {
        spdlog::debug("[WifiBackend] Mock: Connect thread canceled");
        return;
    }

    // Simulate timeout for very weak signals (<20%) - 30% chance of timeout
    auto it_timeout_check = std::find_if(mock_networks_.begin(), mock_networks_.end(),
                                         [this](const MockWiFiNetwork& mock_net) {
                                             return mock_net.network.ssid == connecting_ssid_;
                                         });

    if (it_timeout_check != mock_networks_.end()) {
        if (it_timeout_check->network.signal_strength < 20 && (rng_() % 100) < 30) {
            spdlog::info("[WifiBackend] Mock: Connection timeout - weak signal ({}%)",
                         it_timeout_check->network.signal_strength);
            fire_event("DISCONNECTED", "reason=timeout");
            return;
        }
    }

    // Find the network we're trying to connect to
    auto it = std::find_if(mock_networks_.begin(), mock_networks_.end(),
                           [this](const MockWiFiNetwork& mock_net) {
                               return mock_net.network.ssid == connecting_ssid_;
                           });

    if (it == mock_networks_.end()) {
        LOG_ERROR_INTERNAL("[WifiBackend] Mock: Network '{}' disappeared during connection",
                           connecting_ssid_);
        fire_event("DISCONNECTED", "reason=network_not_found");
        return;
    }

    // Validate password for secured networks
    if (it->network.is_secured) {
        if (connecting_password_.empty()) {
            spdlog::info("[WifiBackend] Mock: Auth failed - no password for secured network '{}'",
                         connecting_ssid_);
            fire_event("AUTH_FAILED", "reason=no_password");
            return;
        }

        // Check if password matches expected password
        if (connecting_password_ != it->password) {
            spdlog::debug("[WifiBackend] Mock: Auth failed - wrong password for '{}'",
                          connecting_ssid_);
            fire_event("AUTH_FAILED", "reason=wrong_password");
            return;
        }

        spdlog::debug("[WifiBackend] Mock: Password correct for '{}'", connecting_ssid_);
    }

    // Connection successful!
    connected_ = true;
    connected_ssid_ = connecting_ssid_;
    connected_signal_ = it->network.signal_strength;

    // Generate mock IP address
    int subnet = 100 + (rng_() % 155); // 192.168.1.100-255
    connected_ip_ = "192.168.1." + std::to_string(subnet);

    spdlog::info("[WifiBackend] Mock: Connected to '{}', IP: {}", connected_ssid_, connected_ip_);

    fire_event("CONNECTED", "ip=" + connected_ip_);
}

// ============================================================================
// Status Queries
// ============================================================================

WifiBackend::ConnectionStatus WifiBackendMock::get_status() {
    ConnectionStatus status = {};
    status.connected = connected_;
    status.ssid = connected_ssid_;
    status.ip_address = connected_ip_;
    status.signal_strength = connected_signal_;

    // Generate mock BSSID
    if (connected_) {
        status.bssid = "aa:bb:cc:dd:ee:ff"; // Mock MAC address
    }

    return status;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void WifiBackendMock::init_mock_networks() {
    mock_networks_ = {
        MockWiFiNetwork("HomeNetwork-5G", 92, true, "WPA2", "12345678"), // Strong, encrypted
        MockWiFiNetwork("Office-Main", 78, true, "WPA2", "12345678"),    // Strong, encrypted
        MockWiFiNetwork("Printers-WiFi", 85, true, "WPA2", "12345678"),  // Strong, encrypted
        MockWiFiNetwork("CoffeeShop_Free", 68, false, "Open", ""),   // Medium, open (no password)
        MockWiFiNetwork("IoT-Devices", 55, true, "WPA", "12345678"), // Medium, encrypted
        MockWiFiNetwork("Guest-Access", 48, false, "Open", ""),      // Medium, open (no password)
        MockWiFiNetwork("Neighbor-Network", 38, true, "WPA3", "12345678"), // Weak, encrypted
        MockWiFiNetwork("Public-Hotspot", 25, false, "Open", ""),       // Weak, open (no password)
        MockWiFiNetwork("SmartHome-Net", 32, true, "WPA3", "12345678"), // Weak, encrypted
        MockWiFiNetwork("Distant-Router", 18, true, "WPA2", "12345678") // Weak, encrypted
    };

    spdlog::debug("[WifiBackend] Mock: Initialized {} mock networks", mock_networks_.size());
}

void WifiBackendMock::vary_signal_strengths() {
    for (auto& mock_net : mock_networks_) {
        // Vary signal strength by ±5% for realism
        int original = mock_net.network.signal_strength;
        int variation = (rng_() % 11) - 5; // -5 to +5
        mock_net.network.signal_strength = std::max(0, std::min(100, original + variation));
    }
}