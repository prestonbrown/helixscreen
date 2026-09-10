// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_mock.h"

#include "ui_error_reporting.h"

#include "safe_log.h"
#include "spdlog/spdlog.h"
#include "wifi_saved_config.h"

#include <algorithm>
#include <chrono>

WifiBackendMock::WifiBackendMock()
    : running_(false), connected_(false), connected_signal_(0),
      rng_(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
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
    spdlog::debug("[WifiBackend] Mock backend started (simulator mode)");
    return WiFiErrorHelper::success();
}

void WifiBackendMock::start_async() {
    // Mock init is instantaneous — just run start() on the caller's thread
    // and fire READY so tests waiting on the event see it.
    WiFiError result = start();
    if (result.success()) {
        fire_event("READY");
    } else {
        fire_event("INIT_FAILED", result.technical_msg);
    }
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
    spdlog::trace("[WifiBackend] Mock: Registered callback for '{}'", name);
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

    // Extract public WiFiNetwork objects (without passwords), collapse per-BSS
    // rows to one per SSID, then sort by signal strength
    std::vector<WiFiNetwork> raw;
    raw.reserve(mock_networks_.size());
    for (const auto& mock_net : mock_networks_) {
        raw.push_back(mock_net.network);
    }

    networks = wifi_merge_networks_by_ssid(raw);

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
        LOG_WARN_INTERNAL("[WifiBackend] Mock: Network '{}' not found in scan results",
                          ssid); // PII_OK: mock backend, fixture SSIDs
        return WiFiErrorHelper::network_not_found(ssid);
    }

    // Validate password for secured networks
    if (it->network.is_secured && password.empty()) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: No password provided for secured network '{}'",
                          ssid); // PII_OK: mock backend, fixture SSIDs
        return WiFiError(
            WiFiResult::INVALID_PARAMETERS, "Password required for secured network: " + ssid,
            "This network requires a password", "Enter the network password and try again");
    }

    spdlog::info("[WifiBackend] Mock: Connecting to '{}'...",
                 ssid); // PII_OK: mock backend, fixture SSIDs

    connecting_ssid_ = ssid;
    connecting_password_ = password;

    // Record the network as saved immediately, mirroring the real backend's
    // SAVE_CONFIG happening before the CONNECTED event fires — a test does not
    // have to wait out the simulated connect delay to see forget_network()
    // find this SSID.
    saved_networks_.insert(ssid);

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

    spdlog::info("[WifiBackend] Mock: Disconnecting from '{}'",
                 connected_ssid_); // PII_OK: mock backend, fixture SSIDs

    connected_ = false;
    std::string old_ssid = connected_ssid_;
    connected_ssid_.clear();
    connected_ip_.clear();
    connected_signal_ = 0;

    fire_event("DISCONNECTED", "reason=user_request");
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::set_radio_enabled(bool on) {
    radio_enabled_ = on;
    if (!on) {
        connected_ = false;
        connected_ssid_.clear();
        connected_ip_.clear();
        connected_signal_ = 0;
        fire_event("DISCONNECTED", "");
    }
    return WiFiErrorHelper::success();
}

bool WifiBackendMock::is_radio_enabled() const {
    return radio_enabled_;
}

WiFiError WifiBackendMock::forget_network(const std::string& ssid) {
    if (!running_) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: forget_network called but not running");
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Mock backend not running",
                         "WiFi system not ready", "Initialize the WiFi system first");
    }

    const bool had_local = saved_networks_.erase(ssid) > 0;

    // Also check HelixScreen's own credential store — real devices can have a
    // credential ONLY there (SAVE_CONFIG never reached the vendor's config,
    // see wifi_saved_config.h), so a real forget must clear it too, not just
    // whatever this mock happens to track locally.
    const auto stored = helix::wifi::store::load();
    const bool had_store =
        std::any_of(stored.begin(), stored.end(), [&](const auto& n) { return n.ssid == ssid; });

    if (!had_local && !had_store) {
        LOG_WARN_INTERNAL("[WifiBackend] Mock: forget_network — no saved entry for '{}'",
                          ssid); // PII_OK: mock backend, fixture SSIDs
        return WiFiErrorHelper::network_not_found(ssid);
    }

    if (had_store)
        helix::wifi::store::remove(ssid);

    if (connected_ && connected_ssid_ == ssid) {
        connected_ = false;
        connected_ssid_.clear();
        connected_ip_.clear();
        connected_signal_ = 0;
        fire_event("DISCONNECTED", "reason=forgotten");
    }

    spdlog::info("[WifiBackend] Mock: Forgot network '{}'",
                 ssid); // PII_OK: mock backend, fixture SSIDs
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
                           connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
        fire_event("DISCONNECTED", "reason=network_not_found");
        return;
    }

    // Validate password for secured networks
    if (it->network.is_secured) {
        if (connecting_password_.empty()) {
            spdlog::info("[WifiBackend] Mock: Auth failed - no password for secured network '{}'",
                         connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
            fire_event("AUTH_FAILED", "reason=no_password");
            return;
        }

        // Check if password matches expected password
        if (connecting_password_ != it->password) {
            spdlog::debug("[WifiBackend] Mock: Auth failed - wrong password for '{}'",
                          connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
            fire_event("AUTH_FAILED", "reason=wrong_password");
            return;
        }

        spdlog::debug("[WifiBackend] Mock: Password correct for '{}'",
                      connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
    }

    // Connection successful!
    connected_ = true;
    connected_ssid_ = connecting_ssid_;
    connected_signal_ = it->network.signal_strength;

    // Generate mock IP address
    int subnet = 100 + (rng_() % 155); // 192.168.1.100-255
    connected_ip_ = "192.168.1." + std::to_string(subnet);

    spdlog::info("[WifiBackend] Mock: Connected to '{}', IP: {}", connected_ssid_,
                 connected_ip_); // PII_OK: mock backend, fixture SSIDs

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
    status.mac_address = "de:ad:be:ef:ca:fe"; // Mock device WiFi adapter MAC

    // Generate mock BSSID (access point MAC)
    if (connected_) {
        status.bssid = "aa:bb:cc:dd:ee:ff";
    }

    return status;
}

void WifiBackendMock::set_connected_state(bool connected, const std::string& ssid,
                                          const std::string& ip, int signal) {
    connected_ = connected;
    connected_ssid_ = ssid;
    connected_ip_ = ip;
    connected_signal_ = signal;
}

bool WifiBackendMock::supports_5ghz() const {
    // Mock simulates a dual-band adapter (AD5X, K2 Plus, Pi 4+): its scan list
    // contains 5GHz BSSes, which a 2.4GHz-only radio could not have seen.
    return true;
}

std::optional<helix::wifi::WifiInterface> WifiBackendMock::resolved_interface() const {
    return resolved_interface_;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void WifiBackendMock::init_mock_networks() {
    // Frequencies model a realistic mixed-band neighbourhood. "Office-Main"
    // deliberately appears twice — once per band, the 5GHz BSS weaker — which is
    // the dual-band-single-SSID case that used to erase the 5GHz AP entirely
    // (helixscreen#1189). get_scan_results() merges it back to one row.
    mock_networks_ = {
        MockWiFiNetwork("HomeNetwork-5G", 92, true, "WPA2", "12345678", 5180), // Strong, encrypted
        MockWiFiNetwork("Office-Main", 78, true, "WPA2", "12345678", 2437),    // Strong, encrypted
        MockWiFiNetwork("Office-Main", 61, true, "WPA2", "12345678", 5745),    // Same SSID, 5GHz
        MockWiFiNetwork("Printers-WiFi", 85, true, "WPA2", "12345678", 2412),  // Strong, encrypted
        MockWiFiNetwork("CoffeeShop_Free", 68, false, "Open", "", 2462),       // Medium, open
        MockWiFiNetwork("IoT-Devices", 55, true, "WPA", "12345678", 2412),     // Medium, encrypted
        MockWiFiNetwork("Guest-Access", 48, false, "Open", "", 5200),          // Medium, open
        MockWiFiNetwork("Neighbor-Network", 38, true, "WPA3", "12345678", 2432), // Weak, encrypted
        MockWiFiNetwork("Public-Hotspot", 25, false, "Open", "", 2417),          // Weak, open
        MockWiFiNetwork("SmartHome-Net", 32, true, "WPA3", "12345678", 2452),    // Weak, encrypted
        MockWiFiNetwork("Distant-Router", 18, true, "WPA2", "12345678", 5220)    // Weak, encrypted
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