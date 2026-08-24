// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file wifi_backend.h
 * @brief Abstract platform-independent interface for WiFi operations
 *
 * @pattern Pure virtual interface + static create()/create_auto() factory methods
 * @threading Implementation-dependent; see concrete implementations
 *
 * @see wifi_backend_wpa_supplicant.cpp, wifi_backend_networkmanager.cpp, wifi_backend_macos.cpp
 */

#pragma once

#include "wifi_interface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief WiFi operation result with detailed error information
 */
enum class WiFiResult {
    SUCCESS = 0,            ///< Operation succeeded
    PERMISSION_DENIED,      ///< Insufficient permissions (socket access, etc.)
    HARDWARE_NOT_AVAILABLE, ///< No WiFi hardware detected
    SERVICE_NOT_RUNNING,    ///< wpa_supplicant/network service not running
    INTERFACE_DOWN,         ///< WiFi interface is down/disabled
    RF_KILL_BLOCKED,        ///< WiFi blocked by RF-kill (hardware/software)
    CONNECTION_FAILED,      ///< Failed to connect to wpa_supplicant/service
    TIMEOUT,                ///< Operation timed out
    AUTHENTICATION_FAILED,  ///< Wrong password or authentication error
    NETWORK_NOT_FOUND,      ///< Specified network not in range
    INVALID_PARAMETERS,     ///< Invalid SSID, password, or other parameters
    BACKEND_ERROR,          ///< Internal backend error
    NOT_INITIALIZED,        ///< Backend not started/initialized
    UNKNOWN_ERROR           ///< Unexpected error condition
};

/**
 * @brief Detailed error information for WiFi operations
 */
struct WiFiError {
    WiFiResult result;         ///< Primary error code
    std::string technical_msg; ///< Technical details for logging/debugging
    std::string user_msg;      ///< User-friendly message for UI display
    std::string suggestion;    ///< Suggested action for user (optional)

    WiFiError(WiFiResult r = WiFiResult::SUCCESS, const std::string& tech = "",
              const std::string& user = "", const std::string& suggest = "")
        : result(r), technical_msg(tech), user_msg(user), suggestion(suggest) {}

    bool success() const {
        return result == WiFiResult::SUCCESS;
    }
    operator bool() const {
        return success();
    }
};

/**
 * @brief Utility class for creating user-friendly WiFi error messages
 */
class WiFiErrorHelper {
  public:
    /**
     * @brief Create permission denied error with helpful suggestions
     */
    static WiFiError permission_denied(const std::string& technical_detail) {
        return WiFiError(WiFiResult::PERMISSION_DENIED, technical_detail,
                         "Permission denied - unable to access WiFi controls",
                         "Try running as administrator or check user permissions");
    }

    /**
     * @brief Create hardware not available error
     */
    static WiFiError hardware_not_available() {
        return WiFiError(WiFiResult::HARDWARE_NOT_AVAILABLE, "No WiFi interfaces detected",
                         "No WiFi hardware found",
                         "Check that WiFi hardware is installed and enabled");
    }

    /**
     * @brief Create service not running error
     */
    static WiFiError service_not_running(const std::string& service_name) {
        return WiFiError(WiFiResult::SERVICE_NOT_RUNNING,
                         service_name + " service not running or not accessible",
                         "WiFi service unavailable",
                         "Check that WiFi services are enabled and running");
    }

    /**
     * @brief Create RF-kill blocked error
     */
    static WiFiError rf_kill_blocked() {
        return WiFiError(
            WiFiResult::RF_KILL_BLOCKED, "WiFi blocked by RF-kill (hardware or software switch)",
            "WiFi is disabled", "Check WiFi hardware switch or enable WiFi in system settings");
    }

    /**
     * @brief Create interface down error
     */
    static WiFiError interface_down(const std::string& interface_name) {
        return WiFiError(
            WiFiResult::INTERFACE_DOWN, "WiFi interface " + interface_name + " is down",
            "WiFi interface is disabled", "Enable the WiFi interface in network settings");
    }

    /**
     * @brief Create connection failed error
     */
    static WiFiError connection_failed(const std::string& technical_detail) {
        return WiFiError(WiFiResult::CONNECTION_FAILED, technical_detail,
                         "Failed to connect to WiFi system",
                         "Check that WiFi services are running and try again");
    }

    /**
     * @brief Create authentication failed error
     */
    static WiFiError authentication_failed(const std::string& ssid) {
        return WiFiError(WiFiResult::AUTHENTICATION_FAILED,
                         "Authentication failed for network: " + ssid,
                         "Incorrect password or network authentication failed",
                         "Verify the password and try again");
    }

    /**
     * @brief Create network not found error
     */
    static WiFiError network_not_found(const std::string& ssid) {
        return WiFiError(WiFiResult::NETWORK_NOT_FOUND, "Network not found: " + ssid,
                         "Network '" + ssid + "' is not in range",
                         "Move closer to the network or check the network name");
    }

    /**
     * @brief Create success result
     */
    static WiFiError success() {
        return WiFiError(WiFiResult::SUCCESS);
    }
};

/**
 * @brief Radio bands an SSID can be seen on, as bit flags
 *
 * A single SSID is routinely broadcast by several BSSes on different bands.
 * Collapsing those into one list row must not throw the band away, so the row
 * carries the OR of every band it was observed on.
 */
enum WiFiBandFlag : uint8_t {
    WIFI_BAND_NONE = 0,         ///< Band unknown (frequency not reported by the backend)
    WIFI_BAND_2_4GHZ = 1u << 0, ///< 2.4 GHz (channels 1-14)
    WIFI_BAND_5GHZ = 1u << 1,   ///< 5 GHz (UNII-1 through UNII-4)
    WIFI_BAND_6GHZ = 1u << 2,   ///< 6 GHz (Wi-Fi 6E)
};

/**
 * @brief Classify a channel centre frequency into a band flag
 *
 * @param frequency_mhz Centre frequency in MHz (0 or out-of-range = unknown)
 * @return One of WIFI_BAND_2_4GHZ / WIFI_BAND_5GHZ / WIFI_BAND_6GHZ, or
 *         WIFI_BAND_NONE when the frequency is unknown or not a WiFi band.
 */
uint8_t wifi_band_flag_from_frequency(int frequency_mhz);

/**
 * @brief Parse the output of `nmcli radio wifi` into a radio-on/off answer
 *
 * NetworkManager answers with a single word — "enabled" or "disabled" — and
 * decorates it with a trailing newline and (outside terse mode) leading
 * whitespace. Anything else is a broken/absent nmcli, and must NOT be guessed
 * at: an unparseable answer that defaulted to "enabled" is exactly how the
 * base-class default made the UI switch snap back on after a successful
 * radio-off.
 *
 * Declared here rather than in wifi_backend_networkmanager.h so it is
 * reachable (and testable) on platforms that do not build the NM backend.
 *
 * @param output Raw stdout from `nmcli radio wifi`
 * @return true/false for enabled/disabled, nullopt when unrecognized
 */
std::optional<bool> wifi_parse_nm_radio_state(const std::string& output);

/**
 * @brief WiFi network information
 */
struct WiFiNetwork {
    std::string ssid;          ///< Network name (SSID)
    int signal_strength;       ///< Signal strength (0-100 percentage)
    bool is_secured;           ///< True if network requires password
    std::string security_type; ///< Security type ("WPA2", "WPA3", "WEP", "Open")
    int frequency_mhz{0};      ///< Frequency of the strongest BSS in MHz (0 = unknown)
    uint8_t band_mask{0};      ///< OR of WiFiBandFlag for every band this SSID was seen on

    WiFiNetwork() : signal_strength(0), is_secured(false) {}

    WiFiNetwork(const std::string& ssid_, int strength, bool secured,
                const std::string& security = "", int freq_mhz = 0)
        : ssid(ssid_), signal_strength(strength), is_secured(secured), security_type(security),
          frequency_mhz(freq_mhz), band_mask(wifi_band_flag_from_frequency(freq_mhz)) {}
};

/**
 * @brief Collapse per-BSS scan results into one row per SSID, preserving bands
 *
 * Mesh systems and ordinary dual-band routers broadcast one SSID from several
 * BSSes. The picker shows one row per SSID, keeping the strongest signal — but
 * the surviving row inherits the union of every band the SSID was seen on, so a
 * 5 GHz BSS that loses on RSSI is still represented (helixscreen#1189).
 *
 * Result order is first-seen, making the output deterministic.
 *
 * @param networks Raw per-BSS scan results
 * @return One entry per unique SSID: strongest signal, merged band_mask
 */
std::vector<WiFiNetwork> wifi_merge_networks_by_ssid(const std::vector<WiFiNetwork>& networks);

/**
 * @brief Abstract WiFi backend interface
 *
 * Provides a clean, platform-agnostic API for WiFi operations.
 * Concrete implementations handle platform-specific details:
 * - WifiBackendWpaSupplicant: Linux wpa_supplicant integration
 * - WifiBackendMock: Simulator mode with fake data
 *
 * Design principles:
 * - Hide all backend-specific formats/commands from WiFiManager
 * - Provide async operations with event-based completion
 * - Thread-safe operations where needed
 * - Clean error handling with meaningful messages
 */
class WifiBackend {
  public:
    virtual ~WifiBackend() = default;

    /**
     * @brief Connection status information
     */
    struct ConnectionStatus {
        bool connected{false};   ///< True if connected to a network
        std::string ssid;        ///< Connected network name
        std::string bssid;       ///< Access point MAC address
        std::string ip_address;  ///< Current IP address
        std::string mac_address; ///< Device WiFi adapter MAC address
        int signal_strength{0};  ///< Signal strength (0-100%)
        int frequency_mhz{0};    ///< Connected frequency in MHz (0 = unknown)
    };

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * @brief Set silent mode (suppress error modals on startup)
     *
     * When silent mode is enabled, startup failures will be logged but
     * not displayed as modal dialogs to the user. Used when probing
     * WiFi availability for signal strength display rather than
     * explicit user-initiated WiFi configuration.
     *
     * @param silent true to suppress modals, false (default) to show them
     */
    void set_silent(bool silent) {
        silent_ = silent;
    }

    /**
     * @brief Check if silent mode is enabled
     */
    bool is_silent() const {
        return silent_;
    }

    /**
     * @brief Whether this is the NetworkManager (nmcli) backend
     *
     * WiFiManager needs it to decide whether an INIT_FAILED is recoverable by
     * falling back to wpa_supplicant — only the NM backend has that fallback.
     * A virtual query rather than a `dynamic_cast` to the concrete backend,
     * because the firmware builds -fno-rtti.
     */
    virtual bool is_network_manager() const {
        return false;
    }

    /**
     * @brief Initialize and start the WiFi backend
     *
     * Establishes connection to underlying WiFi system (wpa_supplicant, mock, etc.)
     * and starts any background processing threads.
     *
     * Note: This method is potentially BLOCKING on real backends (subprocess
     * probing, socket I/O). Prefer start_async() from UI threads.
     *
     * @return WiFiError with detailed status information
     */
    virtual WiFiError start() = 0;

    /**
     * @brief Non-blocking variant of start()
     *
     * Returns immediately. Concrete implementations perform deferred
     * initialization (subprocess probing, socket discovery, etc.) on a
     * worker thread. Completion is signalled via the event system:
     *   - "READY"        fires on successful initialization
     *   - "INIT_FAILED"  fires with an error message on failure
     *
     * After a READY event is received, is_running() will return true.
     *
     * Default implementation simply calls start() synchronously — backends
     * that need non-blocking behaviour must override.
     */
    virtual void start_async() {
        (void)start();
    }

    /**
     * @brief Stop the WiFi backend
     *
     * Cleanly shuts down background threads and connections.
     */
    virtual void stop() = 0;

    /**
     * @brief Check if backend is currently running/initialized
     *
     * @return true if backend is active and ready for operations
     */
    virtual bool is_running() const = 0;

    // ========================================================================
    // Event System
    // ========================================================================

    /**
     * @brief Register callback for WiFi events
     *
     * Events are delivered asynchronously and may arrive from background threads.
     * Ensure thread safety in callback implementations.
     *
     * Standard event types:
     * - "SCAN_COMPLETE" - Network scan finished
     * - "CONNECTED" - Successfully connected to network
     * - "DISCONNECTED" - Disconnected from network
     * - "AUTH_FAILED" - Authentication failed (wrong password, etc.)
     *
     * @param name Event type identifier
     * @param callback Handler function
     */
    virtual void register_event_callback(const std::string& name,
                                         std::function<void(const std::string&)> callback) = 0;

    // ========================================================================
    // Network Scanning
    // ========================================================================

    /**
     * @brief Trigger network scan (async)
     *
     * Initiates scan for available WiFi networks. Results delivered via
     * "SCAN_COMPLETE" event. Use get_scan_results() to retrieve networks.
     *
     * @return WiFiError with detailed status information
     */
    virtual WiFiError trigger_scan() = 0;

    /**
     * @brief Get scan results
     *
     * Returns networks discovered by the most recent scan.
     * Call after receiving "SCAN_COMPLETE" event for up-to-date results.
     *
     * @param[out] networks Vector to populate with discovered networks
     * @return WiFiError with detailed status information
     */
    virtual WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) = 0;

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * @brief Connect to network (async)
     *
     * Initiates connection to specified network. Results delivered via
     * "CONNECTED" event (success) or "AUTH_FAILED"/"DISCONNECTED" (failure).
     *
     * @param ssid Network name
     * @param password Password (empty string for open networks)
     * @return WiFiError with detailed status information
     */
    virtual WiFiError connect_network(const std::string& ssid, const std::string& password) = 0;

    /**
     * @brief Disconnect from current network
     *
     * @return WiFiError with detailed status information
     */
    virtual WiFiError disconnect_network() = 0;

    /**
     * @brief Enable or disable the WiFi radio itself
     *
     * Distinct from stop(), which only detaches this process from the WiFi
     * subsystem. Before this existed the UI toggle called stop() and the
     * station stayed associated and routed — a debug bundle uploaded over a
     * connection the UI was reporting as off (9GQXV5VN, v0.99.106).
     *
     * Implementations should stop association and, where the platform exposes
     * an rfkill switch, soft-block the radio. They must NOT take the network
     * interface down: that is the one step a user cannot undo without a root
     * shell, and it strands a WiFi-only printer whose UI is not running.
     *
     * Default implementation is a successful no-op, for platforms where this
     * toggle is not reachable.
     */
    virtual WiFiError set_radio_enabled(bool on) {
        (void)on;
        return WiFiErrorHelper::success();
    }

    /// Last state requested via set_radio_enabled(). Defaults to true.
    virtual bool is_radio_enabled() const {
        return true;
    }

    /// The interface identity this backend resolved (netdev, control socket,
    /// rfkill node — see wifi_interface.h), when resolution succeeded.
    ///
    /// Default returns nullopt — "inconclusive" — so a backend that has not
    /// implemented interface resolution behaves the same as one that tried
    /// and failed. Callers that gate a potentially device-stranding decision
    /// on this (WiFiManager's stored-radio-state reassert, Task 15) MUST
    /// treat nullopt as "unknown, fail safe", never as "definitely no wired
    /// fallback".
    virtual std::optional<helix::wifi::WifiInterface> resolved_interface() const {
        return std::nullopt;
    }

    /**
     * @brief Forget (permanently remove) a saved network
     *
     * Unlike the REMOVE_NETWORK calls issued internally as connect-failure
     * cleanup elsewhere in this codebase, this is a real, user-initiated
     * forget: it must remove the credential from every place this backend
     * persists it (vendor config, HelixScreen's own credential store, or
     * both), so the network does not reappear on its own.
     *
     * Default implementation returns BACKEND_ERROR, NOT a silent success.
     * This is deliberately the opposite choice from set_radio_enabled()'s
     * no-op default: a silent success here would tell the user a network
     * was forgotten when nothing happened — the exact class of lie this
     * feature exists to eliminate. Platforms that can forget a network must
     * override.
     *
     * @param ssid Network name to forget
     * @return WiFiResult::SUCCESS on success; WiFiResult::NETWORK_NOT_FOUND
     *         when @p ssid has no saved entry anywhere this backend looks
     *         (so callers can distinguish "nothing to forget" from "forget
     *         failed"); WiFiResult::BACKEND_ERROR when this backend does not
     *         support forgetting a network at all.
     */
    virtual WiFiError forget_network(const std::string& ssid) {
        (void)ssid;
        return WiFiError(WiFiResult::BACKEND_ERROR, "forget_network not supported by this backend",
                         "Cannot forget this network on this platform");
    }

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Get current connection status
     *
     * @return ConnectionStatus struct with current state
     */
    virtual ConnectionStatus get_status() = 0;

    /**
     * @brief Check if WiFi hardware supports 5GHz band
     *
     * Returns true if the WiFi adapter can connect to 5GHz networks.
     * Used to conditionally show "Only 2.4GHz networks" in the UI.
     *
     * @return true if 5GHz is supported, false if only 2.4GHz
     */
    virtual bool supports_5ghz() const = 0;

    // ========================================================================
    // Factory Methods
    // ========================================================================

    /**
     * @brief Create appropriate backend for current platform
     *
     * - Linux: WifiBackendNetworkManager (preferred) or WifiBackendWpaSupplicant
     * - macOS: WifiBackendMacOS (or mock in test mode)
     *
     * NON-BLOCKING CONTRACT: This factory MUST return quickly (< 50 ms)
     * and MUST NOT run any synchronous subprocess probing on the caller's
     * thread. The returned backend is constructed but may not yet be
     * initialized — start_async() is kicked off internally, and callers
     * should register for the "READY" event (or "INIT_FAILED") before
     * relying on is_running().
     *
     * Selection strategy on Linux: a cheap file-existence probe
     * (access("/usr/bin/nmcli", X_OK)) picks NetworkManager when present,
     * otherwise wpa_supplicant. No subprocesses are spawned during
     * selection.
     *
     * @param silent If true, suppress error modals on startup failures
     * @return Unique pointer to backend instance (non-null on supported
     *         platforms if any binary is available). The backend may be
     *         not-yet-running — watch for the READY event.
     */
    static std::unique_ptr<WifiBackend> create(bool silent = false);

  protected:
    bool silent_ = false; ///< When true, suppress error modals on startup
};

namespace helix {
/**
 * Platform-provided backend factory for embedded targets. NOT defined in
 * the desktop build — the ESP32 firmware tree implements it against
 * esp_wifi (WifiBackend::create() calls it when ESP_PLATFORM is defined).
 */
std::unique_ptr<WifiBackend> create_platform_wifi_backend(bool silent);
} // namespace helix