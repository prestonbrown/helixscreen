// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "wifi_backend.h"
#include "wifi_scan_scheduler.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief WiFi Manager - Clean interface using backend system
 *
 * Provides network scanning, connection management, and status monitoring.
 * Uses pluggable backend system:
 * - Linux: WifiBackendWpaSupplicant for real wpa_supplicant integration
 * - macOS: WifiBackendMock for simulator testing
 *
 * Key improvements over old implementation:
 * - No platform ifdefs in manager code
 * - Event-driven architecture with proper callbacks
 * - Thread-safe communication between backend and UI
 * - Cleaner separation between WiFi operations and UI timer management
 */
class WiFiManager {
  public:
    /**
     * @brief Initialize WiFi manager with appropriate backend
     *
     * Automatically selects platform-appropriate backend and starts it.
     *
     * @param silent If true, suppress error modals on startup (used when WiFi
     *               wasn't previously configured and we're just probing availability)
     */
    explicit WiFiManager(bool silent = false);

    /**
     * @brief Initialize WiFi manager with an injected backend (test seam)
     *
     * Bypasses WifiBackend::create() platform selection and runs the same
     * callback-registration + start_async() bringup as the default
     * constructor against the given backend. Lets tests exercise
     * WiFiManager against a WifiBackendMock instance they retain a raw
     * pointer to, instead of going through RuntimeConfig's mock-selection
     * plumbing.
     *
     * @param backend Backend to own and drive; must be non-null
     * @param silent  If true, suppress error modals (defaults to true here —
     *                tests are the only caller and generally don't want
     *                modal side effects)
     */
    explicit WiFiManager(std::unique_ptr<WifiBackend> backend, bool silent = true);

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~WiFiManager();

    // ========================================================================
    // Network Scanning
    // ========================================================================

    /**
     * @brief Perform a single network scan (synchronous)
     *
     * Triggers scan and returns results immediately.
     * Uses backend's get_scan_results() after triggering scan.
     *
     * @return Vector of discovered WiFi networks
     */
    std::vector<WiFiNetwork> scan_once();

    /**
     * @brief Start periodic network scanning
     *
     * Scans for available networks and invokes callback with results.
     * Scanning continues automatically, on an interval that backs off from
     * ScanScheduler::BASE_INTERVAL_MS up to ScanScheduler::MAX_INTERVAL_MS as
     * results stay unchanged (and suppresses entirely once connected and
     * stable), until stop_scan() is called. See ScanScheduler.
     *
     * @param on_networks_updated Callback invoked with scan results
     */
    void start_scan(std::function<void(const std::vector<WiFiNetwork>&)> on_networks_updated);

    /**
     * @brief Stop periodic network scanning
     *
     * Cancels auto-refresh timer and any pending scan operations.
     */
    void stop_scan();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * @brief Connect to WiFi network
     *
     * Attempts to connect to the specified network. Operation is asynchronous;
     * callback invoked when connection succeeds or fails.
     *
     * @param ssid Network name
     * @param password Network password (empty for open networks)
     * @param on_complete Callback with (success, error_message)
     */
    void connect(const std::string& ssid, const std::string& password,
                 std::function<void(bool success, const std::string& error)> on_complete);

    /**
     * @brief Disconnect from current network
     */
    void disconnect();

    /**
     * @brief Forget (permanently remove) a saved network
     *
     * Unlike the REMOVE_NETWORK cleanup issued internally elsewhere as a
     * connect-failure rollback, this is a real, user-initiated forget:
     * removes the credential from wherever the backend persists it (vendor
     * config, HelixScreen's own credential store, or both), synchronously,
     * then reports the outcome.
     *
     * @param ssid Network name to forget
     * @param on_complete Callback with (success, error). error is empty on
     *                     success.
     */
    void forget(const std::string& ssid,
                std::function<void(bool success, const std::string& error)> on_complete);

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Check if connected to any network
     *
     * @return true if connected
     */
    bool is_connected();

    /**
     * @brief Get currently connected network name
     *
     * @return SSID of connected network, or empty string if not connected
     */
    std::string get_connected_ssid();

    /**
     * @brief Get current IP address
     *
     * @return IP address string (e.g., "192.168.1.100"), or empty if not connected
     */
    std::string get_ip_address();

    /**
     * @brief Get WiFi adapter MAC address
     *
     * @return MAC address string (e.g., "aa:bb:cc:dd:ee:ff"), or empty if unavailable
     */
    std::string get_mac_address();

    /**
     * @brief Get signal strength of connected network
     *
     * @return Signal strength 0-100%, or 0 if not connected
     */
    int get_signal_strength();

    /**
     * @brief Check if WiFi hardware supports 5GHz band
     *
     * Returns true if the WiFi adapter can connect to 5GHz networks.
     * Used to conditionally show "Only 2.4GHz networks" in the UI.
     *
     * @return true if 5GHz is supported, false if only 2.4GHz
     */
    bool supports_5ghz();

    // ========================================================================
    // Hardware Detection (Legacy Compatibility)
    // ========================================================================

    /**
     * @brief Check if WiFi hardware is available
     *
     * Always returns true - backend creation handles hardware availability.
     * Kept for compatibility with existing UI code.
     *
     * @return true if WiFi backend is available
     */
    bool has_hardware();

    /**
     * @brief Check if WiFi is currently enabled
     *
     * @return true if the backend is running AND the radio is on (not
     *         rfkill-blocked / soft-disabled) — see WifiBackend::is_running()
     *         and WifiBackend::is_radio_enabled().
     */
    bool is_enabled();

    /**
     * @brief Enable or disable WiFi radio (BLOCKING — not for UI callbacks)
     *
     * Runs the backend radio change on the calling thread. On wpa_supplicant
     * that is two control commands per direction, each of which can spend up
     * to 5s retrying the send and then up to 10s waiting for a reply, so this
     * can stall its caller for tens of seconds. Anything reachable from an
     * LVGL event callback must use set_enabled_async() instead.
     *
     * @param enabled true to enable, false to disable
     * @return true on success
     */
    bool set_enabled(bool enabled);

    /**
     * @brief Non-blocking radio on/off for UI callers
     *
     * Dispatches the blocking backend work to an HttpExecutor worker and
     * returns immediately, so a switch can flip optimistically and reconcile
     * when the real outcome lands (see helix::wifi::reconcile_radio_toggle).
     *
     * `on_complete` is invoked on the main/LVGL thread through `token`, so its
     * body may touch widgets and subjects directly and needs no expired()
     * check of its own. It receives:
     *  - `success` — whether the backend reported the change as applied
     *  - `actual_enabled` — the radio state read back afterwards, which is the
     *    value the UI and the persisted setting must follow
     *
     * A failure is reported to the user by this method (the same toast
     * set_enabled() raises, minus the suppression case for an unmanageable
     * -but-up link), so callers only handle the state reconciliation.
     *
     * The manager's destructor blocks until any in-flight radio op finishes,
     * so the worker can never outlive the backend it is driving.
     *
     * @param enabled     true to enable, false to disable
     * @param token       Caller's LifetimeToken — expires with the owning object
     * @param on_complete Invoked on the UI thread; may be null
     */
    void set_enabled_async(bool enabled, helix::LifetimeToken token,
                           std::function<void(bool success, bool actual_enabled)> on_complete);

    /**
     * @brief Non-blocking re-attempt of backend bringup.
     *
     * The backend's initial start_async() (kicked from the constructor) can fail
     * on a fresh boot if the system WiFi service (wpa_supplicant) hasn't created
     * its control socket yet. Without a retry the backend stays permanently dead
     * and every scan reports "Backend not started" (helixscreen#1036). This kicks
     * a fresh start_async() on a worker thread; on success the backend fires
     * READY, which wakes state observers and the periodic scan. Safe to call
     * repeatedly — concurrent attempts are de-duplicated by the backend, and a
     * call while already connected is a cheap no-op (re-fires READY).
     */
    void retry_async();

    /**
     * @brief Subscribe to backend state changes (READY / CONNECTED / DISCONNECTED)
     *
     * Fires when the backend's connection state transitions — including the
     * initial READY event that lands after async init completes. The callback
     * is marshalled to the UI thread via the caller's LifetimeToken and is
     * silently skipped if the token has expired by the time it runs.
     *
     * Use this to react to state changes without polling (e.g. to unstick UI
     * queried before async backend init landed).
     *
     * @param token   Caller's LifetimeToken — expires with the owning object
     * @param on_change Callback invoked on the UI thread after state changes
     */
    void add_state_observer(helix::LifetimeToken token, std::function<void()> on_change);

    /**
     * @brief Initialize self-reference for async callback safety
     *
     * MUST be called immediately after construction when using shared_ptr.
     * Enables async callbacks to safely check if manager still exists.
     *
     * @param self Shared pointer to this WiFiManager instance
     */
    void init_self_reference(std::shared_ptr<WiFiManager> self);

    /**
     * @brief True when this device has a working non-WiFi network path (wired
     *        or otherwise) it could fall back to if the radio were turned off.
     *
     * The same safety check the backend's READY handler uses to decide
     * whether a stored "off" is safe to reassert at startup (see
     * register_backend_callbacks()) — exposed here so UI callers can gate a
     * *live* radio-off toggle the same way and warn the user instead of
     * silently stranding a WiFi-only device (the CC1 incident, Task 15).
     * Returns false whenever interface resolution is inconclusive: fail
     * safe, same as the startup path.
     */
    bool has_non_wifi_fallback();

  private:
    // Grants the auth-failure-debounce regression test direct access to the
    // connection handlers and grace-timer state (helixscreen#1050).
    friend class WiFiManagerTestAccess;

    std::unique_ptr<WifiBackend> backend_;

    /// Expires the backend-swap callback deferred out of the NetworkManager init
    /// worker thread. Declared after `backend_` so reverse-order member
    /// destruction expires the guard before the backend that callback touches.
    /// Like IMoonrakerAPI, this class has no deinit_subjects() — it owns no
    /// subjects — so the destructor really is the only teardown point, and the
    /// guard's own dtor covers it (#1165).
    helix::AsyncLifetimeGuard async_lifetime_;

    // Self-reference for async callback safety: the source a callback copies a
    // weak_ptr from so it can check whether the manager still exists. NON-OWNING
    // by necessity -- an owning self-reference is a cycle nothing can break, so
    // ~WiFiManager never runs and the backend threads it stops there stay live
    // for the life of the process.
    std::weak_ptr<WiFiManager> self_;

    // Guards the callback/flag state below, which is read on the libhv backend
    // thread (handle_scan_complete / handle_connected / handle_disconnected /
    // handle_auth_failed) and written on the main/LVGL thread (start_scan /
    // stop_scan / connect / scan_timer_callback / deliver_auth_failure). Reading
    // or reassigning a std::function concurrently is a data race (UB). Callbacks
    // are copied under the lock and invoked OUTSIDE it to avoid re-entrant
    // deadlock and holding the lock across arbitrary UI code.
    mutable std::mutex callback_mutex_;

    // Scanning state
    lv_timer_t* scan_timer_;
    std::function<void(const std::vector<WiFiNetwork>&)>
        scan_callback_; // guarded by callback_mutex_
    bool scan_pending_; // guarded by callback_mutex_; true when scan triggered, cleared after first
                        // SCAN_COMPLETE processed — dedupes duplicate SCAN_COMPLETE events for
                        // the same trigger. Distinct concern from scan_scheduler_ below (which
                        // decides whether to START a new scan); the two cannot disagree because
                        // scan_pending_ is only ever read/written on the backend thread while
                        // scan_scheduler_ is only ever touched on the main/LVGL thread.

    // Scan cadence policy (no-overlap / backoff / suppression). Pure state
    // machine — see wifi_scan_scheduler.h. Touched ONLY on the main/LVGL
    // thread: from start_scan()/scan_timer_callback() directly (already
    // main-thread), and from handle_scan_complete()/handle_disconnected()
    // indirectly via helix::ui::queue_update() dispatch, since those two
    // fire on the backend thread and scan_scheduler_ is not mutex-guarded.
    helix::wifi::ScanScheduler scan_scheduler_;

    // Connection state
    std::function<void(bool, const std::string&)> connect_callback_; // guarded by callback_mutex_
    bool connecting_in_progress_ = false; // guarded by callback_mutex_; true during connect
                                          // attempt, prevents false failure on DISCONNECTED

    // Auth-failure debounce (helixscreen#1050). Some adapters' wpa_supplicant emit a
    // transient CTRL-EVENT-SSID-TEMP-DISABLED/WRONG_KEY mid-handshake on a connect that
    // ultimately succeeds (CONNECTED follows ~1-3s later). Treating that AUTH_FAILED as
    // terminal latched failure into the wizard while WiFi was actually up. Instead the
    // failure is deferred for a grace window; a CONNECTED arriving within it preempts and
    // delivers success. Only a real wrong password (no CONNECTED) surfaces the error.
    // Touched on the UI thread only (the timer and the queue_update apply lambdas).
    lv_timer_t* auth_fail_grace_timer_ = nullptr;
    std::string pending_auth_error_;
    void start_auth_fail_grace(const std::string& error); // arm/restart grace window
    void cancel_auth_fail_grace();                        // CONNECTED preempted the failure
    void deliver_auth_failure();                          // grace elapsed — failure is real
    static void auth_fail_grace_timer_cb(lv_timer_t* timer);

    // Connect watchdog. The wpa_supplicant backend's connect_network() returns as soon as
    // SELECT_NETWORK is accepted; whether the attempt ever resolves depends entirely on a
    // CONNECTED or AUTH_FAILED arriving on the monitor socket. Events that map to neither
    // (CTRL-EVENT-ASSOC-REJECT, CTRL-EVENT-NETWORK-NOT-FOUND, a bare 4-way-handshake-timeout
    // DISCONNECTED) leave the attempt pending forever — and handle_disconnected() deliberately
    // swallows DISCONNECTED while connecting_in_progress_ is set, so nothing else can clear it.
    // That hung the first-run wizard on "Connecting" on AD5X. NetworkManager cannot hit this:
    // nmcli carries its own timeout. Touched on the UI thread only (connect()/disconnect(), the
    // queue_update apply lambdas, and the timer callback).
    lv_timer_t* connect_timeout_timer_ = nullptr;
    void start_connect_timeout();   // arm once the async connect path is entered
    void cancel_connect_timeout();  // attempt resolved, superseded, or aborted
    void deliver_connect_timeout(); // watchdog elapsed — report failure to the caller
    static void connect_timeout_timer_cb(lv_timer_t* timer);

    // Event handling
    void handle_scan_complete(const std::string& event_data);
    void handle_connected(const std::string& event_data);
    void handle_disconnected(const std::string& event_data);
    void handle_auth_failed(const std::string& event_data);
    void handle_init_failed(bool silent, const std::string& msg);

    // Registers SCAN_COMPLETE/CONNECTED/DISCONNECTED/AUTH_FAILED/INIT_FAILED/READY
    // handlers on the current backend_. Called once from the constructor and
    // again after swapping backends during INIT_FAILED auto-failover.
    void register_backend_callbacks(bool silent);

    // On Linux, WiFiManager attempts a one-shot NetworkManager -> wpa_supplicant
    // fallback when NM's INIT_FAILED fires (daemon dead despite nmcli present).
    // This flag prevents infinite loops if wpa_supplicant also fails.
    bool tried_fallback_ = false;

    // Observers of backend state transitions — each entry is a {token, cb} pair.
    // Fans out READY/CONNECTED/DISCONNECTED so UI consumers (home-panel network
    // widget) can unstick themselves when they queried before async init landed.
    struct StateObserver {
        helix::LifetimeToken token;
        std::function<void()> callback;
    };
    std::mutex state_observers_mutex_;
    std::vector<StateObserver> state_observers_;
    void notify_state_observers();

    // Timer callbacks (must be static for LVGL)
    static void scan_timer_callback(lv_timer_t* timer);

    // True when the OS reports a live wireless link even though the managed
    // backend (wpa_supplicant) is unreachable. In that state the link is
    // genuinely up (printer reachable by IP, no managed control), so the
    // "scan failed" / "service unavailable" warnings are demoted to debug logs
    // rather than nagging the user (helixscreen#1059, Qidi Q2). Defaults to the
    // real sysfs/proc probe; tests inject a stub via WiFiManagerTestAccess.
    static std::function<bool()> os_link_probe_;
    static bool os_link_up();

    // A scan trigger that fails immediately after WE tore the association down
    // is our own doing, not a fault the user can act on. Forgetting the
    // connected network disassociates, and start_scan() runs again right after,
    // so wpa_supplicant answers FAIL while the link is mid-teardown and
    // os_link_up() is legitimately false — bundle TAU4PW4H shows "WiFi scan
    // failed. Try again." 48 ms after the user's own Forget tap. Suppress the
    // toast for a short window after any association change we initiated; the
    // periodic timer (10 s base) surfaces a genuinely broken scan on its next
    // tick. Main-thread only: connect/forget/disconnect and start_scan() are
    // all UI-initiated, as is the scan timer callback.
    static constexpr auto ASSOCIATION_GRACE = std::chrono::seconds(5);
    std::chrono::steady_clock::time_point last_association_change_{};
    void mark_association_change();
    bool in_association_grace() const;

    // Drives the backend radio change. Blocking, and safe to call from any
    // thread — it touches only backend_, which the destructor barrier below
    // keeps alive for the duration.
    WiFiError apply_radio_enabled(bool enabled);

    // Surfaces a radio failure to the user. Static (and therefore free of any
    // `this` access) so the async path can hand it to a deferred callback
    // without tying that callback's safety to the manager's lifetime.
    // Main-thread only — NOTIFY_ERROR builds widgets.
    static void report_radio_result(bool enabled, const WiFiError& result);

    // Barrier for set_enabled_async() workers. The worker runs on an
    // HttpExecutor thread and dereferences `this` (backend_) for the whole of
    // apply_radio_enabled(), so the destructor waits here before any member is
    // torn down. Unbounded on purpose: a timeout that expired would hand the
    // worker a freed backend, and the backend calls carry their own deadlines.
    std::mutex radio_op_mutex_;
    std::condition_variable radio_op_cv_;
    int radio_ops_inflight_ = 0;
    void wait_for_radio_ops();

    // Sysfs root used by has_non_wifi_network_path() to gate the stored-radio
    // -state reassert (Task 15: never disable the radio on a device whose
    // only network path is that radio). Defaults to "/sys"; tests point it at
    // a fixture tree via WiFiManagerTestAccess.
    static std::string sys_root_;
};

/**
 * @brief Get the global WiFiManager instance
 *
 * Returns a lazily-created singleton WiFiManager. Use this from all
 * components (wizard, home panel, etc.) rather than creating instances.
 *
 * @return Shared pointer to the global WiFiManager instance
 */
std::shared_ptr<WiFiManager> get_wifi_manager();

} // namespace helix
