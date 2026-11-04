// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_mock.h"

#include "ui_error_reporting.h"

#include "helix_thread.h"
#include "safe_log.h"
#include "spdlog/spdlog.h"
#include "wifi_country.h"
#include "wifi_saved_config.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

WifiBackendMock::WifiBackendMock()
    : running_(false), connected_(false), connected_signal_(0),
      rng_(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
    spdlog::debug("[WifiBackend] Mock backend initialized");
    init_mock_networks();
    if (const char* country = std::getenv("HELIX_MOCK_WIFI_COUNTRY")) {
        country_ = helix::wifi::country::normalize(country);
    }
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
        dispatch_event("READY");
    } else {
        dispatch_event("INIT_FAILED", result.technical_msg);
    }
}

void WifiBackendMock::stop() {
    if (!running_)
        return;

    // Signal threads to stop
    cancel_sim(scan_active_);
    cancel_sim(connect_active_);

    // Wait for threads to complete
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    running_ = false;
    set_connected_state(false);

    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WifiBackend] Mock backend stopped\n");
}

bool WifiBackendMock::is_running() const {
    return running_;
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
    cancel_sim(scan_active_);
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }

    // Launch async scan thread (simulates 2-second scan delay)
    scan_active_ = true;
    scan_thread_ = helix::make_thread(&WifiBackendMock::scan_thread_func, this);

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

void WifiBackendMock::cancel_sim(std::atomic<bool>& active) {
    {
        std::lock_guard<std::mutex> lock(sim_wait_mutex_);
        active = false;
    }
    sim_wait_cv_.notify_all();
}

bool WifiBackendMock::sim_delay(const std::atomic<bool>& active, int delay_ms) {
    std::unique_lock<std::mutex> lock(sim_wait_mutex_);
    sim_wait_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                          [&active] { return !active.load(); });
    return active.load();
}

void WifiBackendMock::scan_thread_func() {
    // Simulate 2-second scan delay; a cancel ends it at once
    if (!sim_delay(scan_active_, 2000)) {
        spdlog::debug("[WifiBackend] Mock: Scan thread canceled");
        return;
    }

    spdlog::debug("[WifiBackend] Mock: Scan completed");
    dispatch_event("SCAN_COMPLETE");
}

// ============================================================================
// Connection Management
// ============================================================================

WiFiError WifiBackendMock::connect_network(const std::string& ssid, const std::string& password,
                                           bool is_hidden) {
    // Recorded before any early return: a hidden SSID is by definition absent
    // from the scan list the lookup below rejects on, so the observable ask
    // must not depend on that lookup's outcome.
    last_connect_hidden_ = is_hidden;

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
    cancel_sim(connect_active_);
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    // Launch async connect thread (simulates 2-3 second delay)
    connect_active_ = true;
    connect_thread_ = helix::make_thread(&WifiBackendMock::connect_thread_func, this);

    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::disconnect_network() {
    if (!connected_) {
        spdlog::debug("[WifiBackend] Mock: disconnect_network called but not connected");
        return WiFiErrorHelper::success(); // Not an error - idempotent operation
    }

    spdlog::info("[WifiBackend] Mock: Disconnecting from '{}'",
                 connected_ssid_); // PII_OK: mock backend, fixture SSIDs

    set_connected_state(false);

    dispatch_event("DISCONNECTED", "reason=user_request");
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::set_radio_enabled(bool on) {
    radio_enabled_ = on;
    if (!on) {
        set_connected_state(false);
        dispatch_event("DISCONNECTED", "");
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
        set_connected_state(false);
        dispatch_event("DISCONNECTED", "reason=forgotten");
    }

    spdlog::info("[WifiBackend] Mock: Forgot network '{}'",
                 ssid); // PII_OK: mock backend, fixture SSIDs
    return WiFiErrorHelper::success();
}

void WifiBackendMock::connect_thread_func() {
    // Simulate connection delay (2-3 seconds)
    int delay_ms = 2000 + (rng_() % 1000);
    if (!sim_delay(connect_active_, delay_ms)) {
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
            dispatch_event("DISCONNECTED", "reason=timeout");
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
        dispatch_event("DISCONNECTED", "reason=network_not_found");
        return;
    }

    // Validate password for secured networks
    if (it->network.is_secured) {
        if (connecting_password_.empty()) {
            spdlog::info("[WifiBackend] Mock: Auth failed - no password for secured network '{}'",
                         connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
            dispatch_event("AUTH_FAILED", "reason=no_password");
            return;
        }

        // Check if password matches expected password
        if (connecting_password_ != it->password) {
            spdlog::debug("[WifiBackend] Mock: Auth failed - wrong password for '{}'",
                          connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
            dispatch_event("AUTH_FAILED", "reason=wrong_password");
            return;
        }

        spdlog::debug("[WifiBackend] Mock: Password correct for '{}'",
                      connecting_ssid_); // PII_OK: mock backend, fixture SSIDs
    }

    // Connection successful!
    // Generate mock IP address
    int subnet = 100 + (rng_() % 155); // 192.168.1.100-255
    const std::string ip = "192.168.1." + std::to_string(subnet);
    set_connected_state(true, connecting_ssid_, ip, it->network.signal_strength);

    spdlog::info("[WifiBackend] Mock: Connected to '{}', IP: {}", connecting_ssid_,
                 ip); // PII_OK: mock backend, fixture SSIDs

    dispatch_event("CONNECTED", "ip=" + ip);
}

// ============================================================================
// Status Queries
// ============================================================================

WifiBackend::ConnectionStatus WifiBackendMock::get_status() {
    ConnectionStatus status = {};
    std::unique_lock<std::mutex> lock(status_mutex_);
    status_callers_.push_back(std::this_thread::get_id());
    status.connected = connected_;
    status.ssid = connected_ssid_;
    status.ip_address = connected_ip_;
    status.signal_strength = connected_signal_;
    status.mac_address = "de:ad:be:ef:ca:fe"; // Mock device WiFi adapter MAC

    // Generate mock BSSID (access point MAC)
    if (connected_) {
        status.bssid = "aa:bb:cc:dd:ee:ff";
    }

    if (hold_next_status_) {
        hold_next_status_ = false;
        status_held_ = true;
        status_cv_.wait(lock, [this] { return !status_held_; });
    }
    return status;
}

std::vector<std::thread::id> WifiBackendMock::status_callers() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_callers_;
}

void WifiBackendMock::clear_status_callers() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_callers_.clear();
}

void WifiBackendMock::hold_next_status() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    hold_next_status_ = true;
}

void WifiBackendMock::release_held_status() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    hold_next_status_ = false;
    status_held_ = false;
    status_cv_.notify_all();
}

void WifiBackendMock::set_connected_state(bool connected, const std::string& ssid,
                                          const std::string& ip, int signal) {
    std::lock_guard<std::mutex> lock(status_mutex_);
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

WiFiError WifiBackendMock::get_country(std::string& code) {
    std::lock_guard<std::mutex> lock(country_mutex_);
    code = country_;
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendMock::set_country(const std::string& code) {
    const std::string clean = helix::wifi::country::normalize(code);
    if (clean.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS, "Not an ISO 3166-1 alpha-2 country");
    }
    {
        std::lock_guard<std::mutex> lock(country_mutex_);
        country_ = clean;
        ++set_country_calls_;
    }
    spdlog::info("[WifiBackend] Mock: Country set to {}", clean);
    dispatch_event("REGDOM_CHANGE",
                   "<3>CTRL-EVENT-REGDOM-CHANGE init=USER type=COUNTRY alpha2=" + clean);
    return WiFiErrorHelper::success();
}

void WifiBackendMock::set_country_for_test(const std::string& code) {
    std::lock_guard<std::mutex> lock(country_mutex_);
    country_ = code;
}

int WifiBackendMock::set_country_calls() const {
    std::lock_guard<std::mutex> lock(country_mutex_);
    return set_country_calls_;
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