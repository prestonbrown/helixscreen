// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h" // Base class
#include "wifi_interface.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace helix::wifi::detail {
/// Locate an existing network id for @p ssid in a raw LIST_NETWORKS reply
/// ("network id / ssid / bssid / flags\n" header, then tab-separated
/// "id\tssid\tbssid\tflags" per line). Returns "" when @p ssid has no saved
/// entry (including when @p list_networks_reply is empty, e.g. the control
/// connection is down). Splitting is tab-only — SSIDs may contain spaces, and
/// splitting on generic whitespace would break "my home net" apart.
///
/// Used by connect_network() to reuse a saved entry instead of stacking a
/// fresh ADD_NETWORK on every connect attempt: a real user's wpa_supplicant
/// had reached network id 7 for a handful of networks, and duplicate
/// all-enabled entries give wpa_supplicant more candidates to roam between
/// after a reboot. Exposed for unit testing.
std::string find_network_id(const std::string& list_networks_reply, const std::string& ssid);

/// Character/length rules for a value that will be spliced into a
/// wpa_supplicant SET_NETWORK command (SSID or PSK): no double quote,
/// backslash, control character, and non-empty within 255 bytes. Pure
/// predicate, no logging — this is the injection barrier between untrusted
/// input and a command protocol, so it is the single rule set both
/// validate_wpa_string() (below) and any PSK-only caller must share rather
/// than reimplement. Exposed for unit testing.
bool wpa_string_is_valid(const std::string& input);

/// Validate @p input against wpa_string_is_valid() and additionally log the
/// specific violation on failure — so this must only be called with values
/// safe to echo into a log line (an SSID, never a PSK or other secret).
/// Returns @p input unchanged on success, "" on failure. Exposed for unit
/// testing.
std::string validate_wpa_string(const std::string& input, const std::string& field_name);
} // namespace helix::wifi::detail

#ifndef __APPLE__
// ============================================================================
// Linux Implementation: Full wpa_supplicant integration
// ============================================================================

#include "hv/EventLoop.h"
#include "hv/EventLoopThread.h"
#include "hv/hloop.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

// Forward declaration - avoid including wpa_ctrl.h in header
struct wpa_ctrl;

namespace helix::wifi::detail {
// Parse a wpa_supplicant config file's `ctrl_interface=` directive and return
// the control-socket directory it points to ("" if absent, unreadable, or not
// an absolute path). Exposed for unit testing; used by the /proc cmdline scan
// to honour `wpa_supplicant -c <conf>` launches whose ctrl_interface lives in
// the config file rather than on the command line.
std::string read_ctrl_interface_from_conf(const std::string& conf_path);

/// True if @p config_contents declares a `network={...}` block whose `ssid=`
/// matches @p ssid. Used to confirm a SAVE_CONFIG actually landed on disk.
/// Exposed for unit testing.
bool wpa_config_has_network(const std::string& config_contents, const std::string& ssid);

enum class SavePersistence {
    Persisted,    ///< Credentials are on disk; they will survive a reboot.
    NotPersisted, ///< They are not. The user's WiFi dies at the next power-off.
};

/// Decide whether a SAVE_CONFIG actually persisted @p ssid.
///
/// An "OK" reply is NOT proof. The Snapmaker U1's wpa_supplicant answers OK to
/// SAVE_CONFIG and never writes the file — device-verified 2026-07-29: reply
/// "OK", config mtime unchanged, zero `network={` blocks, path provably
/// writable. Trusting the reply is why WiFi there dies on every power-off.
/// Only the config file's actual contents settle it.
///
/// @param save_reply       Raw SAVE_CONFIG reply ("OK\n", "FAIL\n", …).
/// @param config_contents  The wpa config file re-read AFTER the save.
/// @param ssid             The SSID that was supposed to be written.
SavePersistence classify_save_result(const std::string& save_reply,
                                     const std::string& config_contents, const std::string& ssid);

enum class RemovalPersistence {
    Verified,     ///< Read the config; the SSID is gone. Survives a reboot.
    StillListed,  ///< Read the config; the SSID is still there. It comes back.
    Unverifiable, ///< No config path, or we could not read it. Unknown.
};

/// Decide whether a REMOVE_NETWORK + SAVE_CONFIG actually removed @p ssid.
///
/// The mirror image of classify_save_result(): a forget wants the SSID ABSENT.
/// The reason this is not merely "is the SSID in the contents" is
/// @p conf_readable — an unreadable config yields empty contents, which looks
/// identical to "successfully removed" and is not. That conflation reported a
/// verified removal on every forget on an AD5X whose vendor config the app
/// cannot open, while the network reappeared at the next boot (bundles
/// TAU4PW4H / 865DXBQ7).
///
/// @param conf_path_known  A `-c <conf>` path was resolved for the daemon.
/// @param conf_readable    That file was opened and read to completion.
/// @param conf_contents    The config re-read AFTER the save.
/// @param ssid             The SSID that was supposed to be removed.
RemovalPersistence classify_removal_result(bool conf_path_known, bool conf_readable,
                                           const std::string& conf_contents,
                                           const std::string& ssid);

enum class ScanTrigger {
    Started,     ///< A fresh scan is underway.
    AlreadyBusy, ///< One was already in flight; its results still arrive.
    NoReply,     ///< The control socket gave us nothing.
    Failed,      ///< wpa_supplicant refused.
};

/// Classify a wpa_supplicant `SCAN` reply.
///
/// AlreadyBusy exists because "FAIL-BUSY" is not a failure: wpa_supplicant is
/// already scanning and will still emit SCAN_COMPLETE, so there is nothing for
/// the caller to recover from and nothing to tell the user. Lumping it in with
/// FAIL booked a phantom failure in the scheduler and, whenever the link
/// happened to be down at that instant, raised "WiFi scan failed. Try again."
/// over a healthy radio.
ScanTrigger classify_scan_reply(const std::string& reply);
} // namespace helix::wifi::detail

/**
 * @brief wpa_supplicant backend using libhv async event loop
 *
 * Provides asynchronous communication with wpa_supplicant daemon via
 * Unix socket control interface. Uses libhv's EventLoopThread for
 * non-blocking socket I/O.
 *
 * Architecture:
 * - Inherits privately from hv::EventLoopThread for async I/O
 * - Dual wpa_ctrl connections: control (commands) + monitor (events)
 * - Event callbacks broadcast to registered handlers
 * - Commands sent synchronously via wpa_ctrl_request()
 *
 * Usage:
 * @code
 *   WifiBackendWpaSupplicant backend;
 *   backend.register_callback("scan", [](const std::string& event) {
 *       // Handle scan complete events
 *   });
 *   backend.start();  // Connects to wpa_supplicant, starts event loop
 *   std::string result = backend.send_command("SCAN");
 *   backend.stop();   // Clean shutdown
 * @endcode
 */
class WifiBackendWpaSupplicant : public WifiBackend, private hv::EventLoopThread {
  public:
    /**
     * @brief Construct WiFi backend
     *
     * Does NOT connect to wpa_supplicant. Call start() to initialize.
     */
    WifiBackendWpaSupplicant();

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~WifiBackendWpaSupplicant();

    // ========================================================================
    // WifiBackend Interface Implementation
    // ========================================================================

    /**
     * @brief Initialize and start wpa_supplicant backend
     *
     * Discovers wpa_supplicant socket, establishes dual connections
     * (control + monitor), and starts libhv event loop thread.
     *
     * @return true if initialization succeeded
     */
    WiFiError start() override;

    /**
     * @brief Non-blocking start — runs start() on a worker thread and
     * fires "READY" or "INIT_FAILED" events on completion.
     */
    void start_async() override;

    /**
     * @brief Stop wpa_supplicant backend
     *
     * Blocks until event loop thread terminates.
     */
    void stop() override;

    /**
     * @brief Check if backend is running
     *
     * @return true if event loop is active
     */
    bool is_running() const override;

    /**
     * @brief Register event callback
     *
     * Translates standard event names to wpa_supplicant-specific events:
     * - "SCAN_COMPLETE" → "CTRL-EVENT-SCAN-RESULTS"
     * - "CONNECTED" → "CTRL-EVENT-CONNECTED"
     * - "DISCONNECTED" → "CTRL-EVENT-DISCONNECTED"
     * - "AUTH_FAILED" → "CTRL-EVENT-SSID-TEMP-DISABLED"
     *
     * @param name Standard event name
     * @param callback Handler function
     */
    void register_event_callback(const std::string& name,
                                 std::function<void(const std::string&)> callback) override;

    /**
     * @brief Send synchronous command to wpa_supplicant
     *
     * Blocks until response received or timeout (usually <100ms).
     *
     * Common commands:
     * - "SCAN" - Trigger network scan
     * - "SCAN_RESULTS" - Get scan results (tab-separated format)
     * - "ADD_NETWORK" - Add network configuration (returns network ID)
     * - "SET_NETWORK <id> ssid \"<ssid>\"" - Set network SSID
     * - "SET_NETWORK <id> psk \"<password>\"" - Set WPA password
     * - "ENABLE_NETWORK <id>" - Connect to network
     * - "STATUS" - Get connection status
     *
     * @param cmd Command string (see wpa_supplicant control interface docs)
     * @return Response string (may contain newlines), or empty on error
     */
    std::string send_command(const std::string& cmd);

    // ========================================================================
    // Clean Abstraction API - Hides wpa_supplicant ugliness
    // ========================================================================

    WiFiError trigger_scan() override;
    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override;
    WiFiError connect_network(const std::string& ssid, const std::string& password) override;
    WiFiError disconnect_network() override;
    ConnectionStatus get_status() override;
    bool supports_5ghz() const override;
    WiFiError set_radio_enabled(bool on) override;
    bool is_radio_enabled() const override;
    WiFiError forget_network(const std::string& ssid) override;

  private:
    // ========================================================================
    // System Validation and Permission Checking
    // ========================================================================

    /**
     * @brief Check system prerequisites before starting backend
     *
     * Performs comprehensive validation:
     * - WiFi hardware detection
     * - wpa_supplicant socket availability
     * - Permission checking for socket access
     * - RF-kill status validation
     *
     * @return WiFiError with detailed status
     */
    WiFiError check_system_prerequisites();

    /**
     * @brief Check if user has permission to access wpa_supplicant sockets
     *
     * @param socket_path Path to test socket access
     * @return WiFiError indicating permission status
     */
    WiFiError check_socket_permissions(const std::string& socket_path);

    /**
     * @brief Detect WiFi hardware interfaces
     *
     * @return WiFiError with hardware status
     */
    WiFiError check_wifi_hardware();

    // ========================================================================
    // wpa_supplicant Communication
    // ========================================================================

    /**
     * @brief Initialize wpa_supplicant connection (runs in event loop thread)
     *
     * Called by start() in the context of the libhv event loop thread.
     * Discovers socket, opens connections, registers I/O callbacks.
     */
    void init_wpa();

    /**
     * @brief Cleanup wpa_supplicant connections
     *
     * Closes both control and monitor connections, detaches from events.
     * Called from destructor to prevent resource leaks.
     */
    void cleanup_wpa();

    /**
     * @brief Handle incoming wpa_supplicant events
     *
     * Broadcasts event to all registered callbacks.
     *
     * @param data Raw event data from wpa_supplicant
     * @param len Length of event data in bytes
     */
    void handle_wpa_events(void* data, int len);

    /**
     * @brief Static trampoline for C callback compatibility
     *
     * libhv uses C-style function pointers for I/O callbacks.
     * This static method extracts the instance pointer from hio_context()
     * and forwards to the member function handle_wpa_events().
     *
     * @param io libhv I/O handle
     * @param data Event data buffer
     * @param readbyte Number of bytes read
     */
    static void _handle_wpa_events(hio_t* io, void* data, int readbyte);

    /**
     * @brief Dispatch a synthetic event to a specific registered callback
     *
     * Used for internal events like INIT_FAILED that don't come from wpa_supplicant.
     *
     * @param event_name Name of the callback to dispatch to
     * @param message Message to pass to the callback
     */
    void dispatch_event(const std::string& event_name, const std::string& message);

    // Helper methods for clean API (encapsulate wpa_supplicant ugliness)
    std::vector<WiFiNetwork> parse_scan_results(const std::string& raw);
    std::vector<std::string> split_by_tabs(const std::string& str);
    int dbm_to_percentage(int dbm);
    std::string detect_security_type(const std::string& flags, bool& is_secured);

    /**
     * @brief Re-read the wpa_supplicant config file after a SAVE_CONFIG
     *
     * Shared by connect_network() and forget_network() — both issue
     * SAVE_CONFIG and then need the config path plus its just-written
     * contents to judge whether the write actually reached disk (a reply of
     * "OK" is not proof; see classify_save_result()).
     *
     * `readable` is what separates "read the file, the SSID is not in it" from
     * "never got to look". Both arrive here as empty contents, and conflating
     * them let forget_network() report a removal it had not verified on the
     * reporter's AD5X, whose config the app cannot open at all (bundles
     * TAU4PW4H / 865DXBQ7: the same session logged "Removal verified on disk"
     * and "did not record this network" about the same path).
     */
    struct WpaConfSnapshot {
        std::string path;      ///< "" when the -c path could not be resolved
        std::string contents;  ///< Empty when unresolved OR unreadable
        bool readable = false; ///< The file was opened and read to completion
    };
    WpaConfSnapshot read_wpa_conf_after_save();

    /**
     * @brief Mirror @p conf_path onto its remembered persistent target, if any
     *
     * A no-op unless @p conf_path lives on volatile storage AND
     * remember_persistent_target() captured a durable target for it at
     * startup (see wifi_saved_config.h). Shared by connect_network() (mirrors
     * a newly-written credential) and forget_network() (mirrors a removal) —
     * whichever side just confirmed the on-disk config matches what it
     * expected calls this to propagate that state onto the durable copy.
     */
    void mirror_if_volatile(const std::string& conf_path);

    /**
     * @brief Check if the libhv event loop thread is active
     *
     * Distinct from is_running() which checks if WiFi is logically enabled.
     * The thread may be active but WiFi disabled (after stop()).
     */
    bool event_loop_active() const {
        return const_cast<WifiBackendWpaSupplicant*>(this)->hv::EventLoopThread::isRunning();
    }

    /**
     * @brief Map raw wpa_supplicant event to callback name
     *
     * Parses the event string to determine which callback should handle it.
     * Returns empty string for informational events that don't need handling.
     *
     * @param event Raw wpa_supplicant event string
     * @return Callback name ("SCAN_COMPLETE", "CONNECTED", etc.) or empty string
     */
    std::string map_event_to_callback(const std::string& event);

    // 5GHz support — computed once during init, never changes
    std::atomic<bool> supports_5ghz_cached_{false};
    std::atomic<bool> supports_5ghz_resolved_{false};

    void resolve_5ghz_support();

    struct wpa_ctrl* conn;     ///< Control connection for sending commands
    struct wpa_ctrl* mon_conn; ///< Monitor connection for receiving events (FIXED LEAK)
    hio_t* mon_io_{nullptr};   ///< libhv I/O handle for monitor socket (must cleanup on re-init)

    // Thread safety
    std::mutex cmd_mutex_;       ///< Protects conn from concurrent send_command() calls
    std::mutex callbacks_mutex_; ///< Protects callbacks map from race conditions
    std::map<std::string, std::function<void(const std::string&)>>
        callbacks; ///< Registered event handlers

    // Change detection for status logging (reduces log noise)
    ConnectionStatus last_logged_status_; ///< Previous status for change detection

    // Init synchronization - ensures init_wpa() completes before start() returns
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    // init_complete_: an init *attempt* finished (success OR failure). Used only
    // to wake start()'s condition-variable wait — NOT a "backend is usable" flag.
    std::atomic<bool> init_complete_{false};
    // init_succeeded_: init_wpa() ran to completion with live control + monitor
    // connections. This is the real "backend is up" signal — is_running(),
    // is_enabled(), and the start()/start_async() retry guards key off it so a
    // failed init (e.g. a fresh-boot race where wpa_supplicant's control socket
    // isn't up yet) does not read as "running" and does not block a later retry.
    std::atomic<bool> init_succeeded_{false};

    // Resolved WiFi interface identity (netdev, ctrl socket, conf path, daemon
    // pid, rfkill node) — computed once in init_wpa() via
    // resolve_and_store_interface(). std::nullopt means resolution was
    // inconclusive; callers fall back to legacy first-match detection.
    mutable std::mutex iface_mutex_;
    std::optional<helix::wifi::WifiInterface> iface_;

    /// Resolve the managed interface and store it. Runs on the event loop
    /// thread during init_wpa(), after the control connection is live.
    void resolve_and_store_interface();

    /// Re-add any network in HelixScreen's own store (helix::wifi::store)
    /// that is missing from wpa_supplicant's LIST_NETWORKS, then re-issue
    /// SAVE_CONFIG. Runs on the event loop thread during init_wpa(), after
    /// interface resolution — the same thread connect_network() already does
    /// blocking send_command() I/O on.
    ///
    /// This is the other half of the fix for credentials that SAVE_CONFIG
    /// claims to persist but does not: connect_network() records every
    /// successful connect in the store regardless of what happened to the
    /// vendor's own config file, and this reconciliation restores them into
    /// wpa_supplicant on the next boot even when the underlying persistence
    /// problem (wrong daemon verified, or a write that lands somewhere
    /// non-durable) was never diagnosed.
    void reconcile_saved_networks();

  public:
    /// The resolved interface, or nullopt when resolution was inconclusive and
    /// callers must fall back to legacy first-match behaviour.
    std::optional<helix::wifi::WifiInterface> resolved_interface() const override;

  private:
    // Last state requested via set_radio_enabled(). Defaults to true (radio on).
    std::atomic<bool> radio_enabled_{true};

    /// Write "0"/"1" to the resolved rfkill node's `soft` attribute.
    /// @return false when there is no node or the write failed (not fatal —
    ///         DISCONNECT + DISABLE_NETWORK already stopped association).
    bool set_rfkill_soft_block(bool blocked);

    // Shutdown coordination - prevents use-after-free when start() times out
    // (GitHub issue #8: thread still in wpa_ctrl_attach when destructor runs)
    std::atomic<bool> shutdown_requested_{false};

    // Async init worker (used by start_async())
    std::thread async_init_thread_;
    std::atomic<bool> async_init_in_progress_{false};
};

#endif // __APPLE__
