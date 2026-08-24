// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#ifndef __APPLE__
// ============================================================================
// Linux Implementation: NetworkManager fallback via nmcli
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief NetworkManager WiFi backend using nmcli command-line interface
 *
 * Provides WiFi functionality on systems where NetworkManager manages WiFi
 * (e.g., MainsailOS on Raspberry Pi) and wpa_supplicant has no user-accessible
 * control socket. Uses `nmcli --terse` for stable, machine-parseable output.
 *
 * Architecture:
 * - All nmcli commands run via popen() (read-only) or fork/exec (connect)
 * - std::thread for async scan/connect (same pattern as WifiBackendMock)
 * - Event callbacks broadcast to registered handlers
 * - Zero external dependencies beyond nmcli binary
 *
 * This is a fallback backend - the factory tries wpa_supplicant first.
 * Only used when wpa_supplicant sockets are unavailable (NM manages them).
 *
 * @see wifi_backend_wpa_supplicant.h (primary Linux backend)
 * @see wifi_backend_mock.h (similar threading pattern)
 */
class WifiBackendNetworkManager : public WifiBackend {
    friend class TestableNMBackend; // Unit test access to private parsing methods

  public:
    WifiBackendNetworkManager();
    ~WifiBackendNetworkManager();

    bool is_network_manager() const override {
        return true;
    }

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

  private:
    // ========================================================================
    // Internal State
    // ========================================================================

    std::atomic<bool> running_{false};
    std::mutex start_mutex_;     // Serializes start() against concurrent start_async()
    std::string wifi_interface_; ///< Detected WiFi interface (e.g., "wlan0")

    // Event system (thread-safe)
    std::mutex callbacks_mutex_;
    std::map<std::string, std::function<void(const std::string&)>> callbacks_;

    // Async threads for scan/connect (same pattern as mock backend)
    std::thread scan_thread_;
    std::thread connect_thread_;
    std::atomic<bool> scan_active_{false};
    std::atomic<bool> connect_active_{false};

    // Cached scan results (protected by mutex)
    std::mutex networks_mutex_;
    std::vector<WiFiNetwork> cached_networks_;

    // Async init thread (used by start_async())
    std::thread init_thread_;
    std::atomic<bool> init_in_progress_{false};

    // Background status polling thread
    std::thread status_thread_;
    std::mutex status_mutex_;    // Protects cached_status_ only
    std::mutex status_cv_mutex_; // Dedicated mutex for condvar wait
    std::condition_variable status_cv_;
    std::atomic<bool> status_running_{false};
    std::atomic<bool> status_refresh_requested_{false};
    ConnectionStatus cached_status_{};        // Protected by status_mutex_
    std::atomic<bool> prev_connected_{false}; // Track transitions for event firing

    // 5GHz support — computed once at start(), never changes
    std::atomic<bool> supports_5ghz_cached_{false};
    std::atomic<bool> supports_5ghz_resolved_{false};

    // Cached `nmcli radio wifi` state. is_radio_enabled() is polled from the
    // UI thread (WiFiManager::is_enabled), so it must not shell out; the cache
    // is seeded at start(), written by set_radio_enabled(), and refreshed by
    // the background status poll so an external `nmcli radio wifi off` is
    // eventually reflected. Defaults to true — same as the base class — but
    // start() replaces that guess with the real answer.
    std::atomic<bool> radio_enabled_{true};

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    /**
     * @brief Result from exec_nmcli_full() capturing both stdout and stderr
     */
    struct NmcliResult {
        std::string stdout_output;
        std::string stderr_output;
        int exit_code;
    };

    /**
     * @brief Execute an nmcli command and return stdout
     *
     * Uses popen() for read-only commands. Safe because command strings
     * are constructed internally (no user input in command).
     * stderr is suppressed (redirected to /dev/null).
     *
     * @param args Arguments to pass after "nmcli"
     * @return stdout output, or empty string on failure
     */
    std::string exec_nmcli(const std::string& args);

    /**
     * @brief Execute an nmcli command capturing stdout, stderr, and exit code
     *
     * Used for commands where error diagnostics matter (e.g., detecting
     * polkit permission errors). stderr is merged with stdout via 2>&1.
     *
     * @param args Arguments to pass after "nmcli"
     * @return NmcliResult with stdout, stderr, and exit code
     */
    NmcliResult exec_nmcli_full(const std::string& args);

    /**
     * @brief Check that NetworkManager is running and nmcli is available
     * @return WiFiError with status
     */
    WiFiError check_system_prerequisites();

    /**
     * @brief Check if stderr output indicates a polkit permission error
     * @param stderr_output stderr text from nmcli
     * @return true if permission/polkit error detected
     */
    static bool is_polkit_permission_error(const std::string& stderr_output);

    /**
     * @brief Find the WiFi device managed by NetworkManager
     * @return Interface name (e.g., "wlan0") or empty string
     */
    std::string detect_wifi_interface();

    /**
     * @brief Parse nmcli terse scan output into WiFiNetwork vector
     *
     * Handles nmcli's colon-separated terse format, including escaped colons
     * in SSIDs (\:), empty/hidden SSIDs, and malformed lines.
     *
     * @param output Raw nmcli -t output from "device wifi list"
     * @return Parsed networks
     */
    std::vector<WiFiNetwork> parse_scan_output(const std::string& output);

    /**
     * @brief Parse a single nmcli terse-mode line, respecting escaped colons
     *
     * nmcli -t uses ':' as field separator but escapes literal colons as '\:'.
     * This splits correctly on unescaped colons only.
     *
     * @param line Single line of nmcli -t output
     * @return Vector of unescaped field values
     */
    std::vector<std::string> split_nmcli_fields(const std::string& line);

    /**
     * @brief Validate SSID/password for shell safety and sanity
     *
     * Rejects control characters, null bytes, excessive length.
     * Same validation as wpa_supplicant backend's validate_wpa_string().
     *
     * @param input String to validate
     * @param field_name "SSID" or "password" for error messages
     * @return Validated string, or empty on failure
     */
    std::string validate_input(const std::string& input, const std::string& field_name);

    /**
     * @brief Fire a registered event callback
     * @param event_name Event type (e.g., "SCAN_COMPLETE", "CONNECTED")
     * @param data Optional event data
     */
    void fire_event(const std::string& event_name, const std::string& data = "");

    // Thread functions for async operations
    void scan_thread_func();
    void connect_thread_func(std::string ssid, std::string password);

    // Result of one nmcli connect attempt (fork/exec, captures stderr).
    struct ConnectAttempt {
        int exit_code = -1;     // -1 = internal failure (fork/pipe/timeout)
        bool timed_out = false; // true => killed after CONNECT_TIMEOUT_SECONDS
        std::string stderr_out;
    };

    // Single fork/exec of `nmcli device wifi connect`. Returns exit_code + stderr.
    // Honours connect_active_ for cancellation.
    ConnectAttempt try_nmcli_connect(const std::string& ssid, const std::string& password);

    // Delete a saved connection profile by id. Best-effort, fork/exec to avoid
    // shell injection with caller-supplied SSIDs. Returns true if nmcli exited 0.
    bool delete_connection_profile(const std::string& profile_id);

    /**
     * @brief Ask NetworkManager whether the WiFi radio is on
     *
     * Blocking (popen). Returns nullopt when nmcli's answer is unparseable —
     * a transient nmcli failure must not be mistaken for "radio on", which is
     * how the inherited base default made the UI switch snap back.
     */
    std::optional<bool> query_radio_enabled();

    // Status polling
    void status_thread_func();
    // Actual nmcli calls (background thread only). Returns nullopt when the
    // underlying `device show` query fails/returns nothing — a transient nmcli
    // or popen error, distinct from a genuine disconnected state — so callers
    // can keep the last-known status instead of reporting a false disconnect
    // (prestonbrown/helixscreen#1059).
    std::optional<ConnectionStatus> poll_status_now();
    void request_status_refresh(); // Wake status thread for immediate poll
};

#endif // __APPLE__
