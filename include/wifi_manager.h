// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "helix_callback.h"
#include "helix_shared_callback.h"
#include "lvgl/lvgl.h"
#include "wifi_backend.h"
#include "wifi_control_breaker.h"
#include "wifi_scan_scheduler.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// How a join attempt resolved. `result` carries WHY it failed, which the
/// error string alone cannot: a backend reason is free-form text, so sniffing
/// it for a substring misclassifies every reason nobody thought to look for.
using ConnectCallback =
    helix::SharedCallback<void(bool success, const std::string& error, WiFiResult result)>;

/// The message to put in front of a user whose Wi-Fi join failed.
///
/// A genuine credential rejection is the ONE case where canned text beats the
/// backend's own words: the password is the only thing the user can act on,
/// and a supplicant reason code is not actionable. Every other result carries
/// a reason the user needs to read - a daemon that is down, a network out of
/// range, a transport that took the link - and substituting "check your
/// password" for those tells someone whose password is correct to retype it
/// indefinitely.
///
/// @param result       How the attempt resolved.
/// @param reason       What the backend or manager said, already translated.
/// @param auth_message Canned text for a credential rejection, already
///                     translated (callers differ: a password modal says
///                     "password", a hidden-network modal says "credentials").
std::string connect_failure_message(WiFiResult result, const std::string& reason,
                                    const std::string& auth_message);

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
 *
 * Threading: every backend control call a UI entry point makes (scan trigger,
 * connect, disconnect, forget, radio, status read) runs on one serial lane
 * over HttpExecutor::fast(), never on the LVGL thread. A wpa_supplicant
 * request can wait 15 s on a wedged driver, so a call from the LVGL thread
 * freezes the screen for that long. One lane, not one task per call, so a
 * wedged daemon holds at most one of the pool's four workers and its
 * requests run in the order the user made them. The ControlBreaker
 * (wifi_control_breaker.h) stops the periodic scans and status polls once the
 * daemon stops answering in time. A user action (connect, disconnect, forget,
 * radio) always reports: by its result, by its own deadline, or at once with
 * "WiFi isn't responding" while the lane is stalled or the breaker is open.
 * Results reach the LVGL thread through LifetimeTokens. With the pool never
 * started (ESP32, whose backend answers from memory) the lane runs each call
 * inline; once the pool has stopped, calls are dropped.
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
     * Scans for available networks and invokes callback with results. The
     * trigger runs on the control lane; a trigger already queued or running
     * there is reused rather than sent again. While the control breaker is
     * open and no probe is due, or the lane has stalled, no scan is sent. The
     * callback always hears back: with fresh results, or with the last list a
     * scan delivered when the trigger is refused, fails, or is stuck behind a
     * stalled lane.
     * Scanning continues automatically, on an interval that backs off from
     * ScanScheduler::kBaseIntervalMs up to ScanScheduler::kMaxIntervalMs as
     * results stay unchanged (and suppresses entirely once connected and
     * stable), until stop_scan() is called. See ScanScheduler.
     *
     * @param on_networks_updated Callback invoked with scan results
     */
    void
    start_scan(helix::SharedCallback<void(const std::vector<WiFiNetwork>&)> on_networks_updated);

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
     * callback invoked on the LVGL thread when connection succeeds or fails.
     * A later connect() or disconnect() supersedes this attempt: its result is
     * dropped. Fails at once while WiFi control is unresponsive, and with a
     * timeout if the join is not sent and answered within the connect window.
     *
     * @param ssid Network name
     * @param password Network password (empty for open networks)
     * @param on_complete Callback with (success, error_message, result)
     * @param is_hidden true when the SSID was typed rather than picked from the
     *        scan list; forwarded to the backend, which must not require the
     *        network to be present in its scan cache
     */
    void connect(const std::string& ssid, const std::string& password, ConnectCallback on_complete,
                 bool is_hidden = false);

    /**
     * @brief Disconnect from current network (on the control lane)
     *
     * Warns the user when the backend refuses, or when WiFi control does not
     * answer (at once while unresponsive, else at the user-op deadline).
     */
    void disconnect();

    /**
     * @brief Forget (permanently remove) a saved network
     *
     * Unlike the REMOVE_NETWORK cleanup issued internally elsewhere as a
     * connect-failure rollback, this is a real, user-initiated forget:
     * removes the credential from wherever the backend persists it (vendor
     * config, HelixScreen's own credential store, or both), on the control
     * lane, then reports the outcome on the LVGL thread: a failure when WiFi
     * control does not answer by the user-op deadline. Not reported if the
     * manager is destroyed first.
     *
     * @param ssid Network name to forget
     * @param on_complete Callback with (success, error). error is empty on
     *                     success.
     */
    void forget(const std::string& ssid,
                helix::SharedCallback<void(bool success, const std::string& error)> on_complete);

    // ========================================================================
    // Status Queries
    // ========================================================================
    //
    // The synchronous queries below each ask the backend, which on
    // wpa_supplicant is a control-socket round trip that can wait up to 10s.
    // UI code uses get_status_async() instead.

    /**
     * @brief Non-blocking connection status for UI callers
     *
     * Reads on an HttpExecutor worker; `on_done` runs on the main/LVGL thread
     * through `token`. One read is in flight at a time: a caller arriving
     * during it is answered by the next read, so no answer predates its
     * request. Call from the main thread: the read is registered there, so a
     * backend swap or the destructor waits for it. While the control breaker
     * is open and no probe is due, or the lane's current call has already
     * taken the breaker's slow-call time, the answer is the last good status
     * with `unresponsive` set, without asking the backend. With the pool never
     * started (ESP32; its backend answers from memory) `on_done` runs before
     * this returns; once the pool has stopped, the answer is the cached one.
     */
    void
    get_status_async(helix::LifetimeToken token,
                     helix::SharedCallback<void(const WifiBackend::ConnectionStatus&)> on_done);

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

    /// WifiBackend::reports_frequency().
    bool reports_frequency();

    /// Managed network interface name ("wlan0"), empty when the backend cannot say.
    std::string netdev_name();

    /// Whether the backend can forget a saved network (WifiBackend::supports_forget()).
    bool supports_forget();

    /// Whether the backend can move the radio (WifiBackend::supports_radio_toggle()).
    bool supports_radio_toggle();

    // ========================================================================
    // Regulatory Country
    // ========================================================================

    /// The one toast every WiFi action shows when control is not answering,
    /// in place of that action's own failure toast. LVGL thread.
    static void notify_unresponsive();

    /// Whether the backend can read and set the radio's regulatory country.
    bool supports_country();

    struct CountryState {
        bool read_ok = false;  ///< The radio answered; false keeps `system` empty
        std::string system;    ///< What the radio reports ("" = world)
        std::string stored;    ///< The saved choice ("" = never chosen)
        std::string last_seen; ///< The last real country the radio reported
        std::string display;   ///< What the UI shows as current (country::plan())
    };

    /**
     * @brief Read the radio's country on the control lane
     *
     * @p on_done runs on the LVGL thread, and only while @p token lives.
     * A backend without country support answers at once with read_ok false.
     */
    void get_country_async(helix::LifetimeToken token,
                           helix::SharedCallback<void(const CountryState&)> on_done);

    /**
     * @brief Save the user's country choice and apply it to the radio
     *
     * LVGL thread. @p code "" clears the saved choice and leaves the radio as
     * it is, so the device's own configuration governs it again. A non-empty
     * code is saved only once the radio accepts it, and reapplied at the next
     * start. @p on_done(success) runs on the LVGL thread while @p token lives.
     */
    void set_country_async(const std::string& code, helix::LifetimeToken token,
                           helix::SharedCallback<void(bool success)> on_done);

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
     * When WiFi control does not answer (at once while unresponsive, else at
     * the user-op deadline) `on_complete` gets (false, current radio state).
     *
     * The manager's destructor blocks until any in-flight radio op finishes,
     * so the worker can never outlive the backend it is driving.
     *
     * @param enabled     true to enable, false to disable
     * @param token       Caller's LifetimeToken — expires with the owning object
     * @param on_complete Invoked on the UI thread; may be null
     */
    void
    set_enabled_async(bool enabled, helix::LifetimeToken token,
                      helix::SharedCallback<void(bool success, bool actual_enabled)> on_complete);

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
    void add_state_observer(helix::LifetimeToken token, helix::SharedCallback<void()> on_change);

    /**
     * @brief Subscribe to the moments the regulatory country may have changed
     *
     * Fires on READY (the backend can now be asked) and on a regulatory
     * domain change, and on nothing else: reading the country is a control
     * lane op, so it must not ride every connection transition. Delivered
     * like add_state_observer().
     */
    void add_country_observer(helix::LifetimeToken token, helix::SharedCallback<void()> on_change);

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
    // or reassigning a callback concurrently is a data race (UB). Callbacks
    // are copied under the lock and invoked OUTSIDE it to avoid re-entrant
    // deadlock and holding the lock across arbitrary UI code.
    mutable std::mutex callback_mutex_;

    // Scanning state
    lv_timer_t* scan_timer_;
    helix::SharedCallback<void(const std::vector<WiFiNetwork>&)>
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
    ConnectCallback connect_callback_;    // guarded by callback_mutex_
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
    // LVGL thread: logs the change, remembers a real country, and rescans
    // while a scan session is open, since the new domain can enable channels
    // the last scan could not use.
    void handle_regdom_change(const std::string& event_data);
    // LVGL thread, once the backend is READY: sends the country
    // country::plan() picks: the saved one when the radio disagrees, else the
    // remembered one when the radio is at world. A country the radio reports
    // without a saved choice is left alone.
    void apply_stored_country();
    // Any thread: saves @p system as the last real country the radio
    // reported, on the LVGL thread. World and junk are ignored.
    void remember_radio_country(const std::string& system);

    // Registers SCAN_COMPLETE/CONNECTED/DISCONNECTED/AUTH_FAILED/INIT_FAILED/READY
    // handlers on the current backend_. Called once from the constructor and
    // again after swapping backends during INIT_FAILED auto-failover.
    void register_backend_callbacks(bool silent);

    // On Linux, WiFiManager attempts a one-shot swap to wpa_supplicant when a
    // backend reports INIT_FAILED and answers supports_wpa_supplicant_fallback().
    // This flag prevents infinite loops if wpa_supplicant also fails.
    bool tried_fallback_ = false;

    // Observers of backend state transitions — each entry is a {token, cb} pair.
    // Fans out READY/CONNECTED/DISCONNECTED so UI consumers (home-panel network
    // widget) can unstick themselves when they queried before async init landed.
    struct StateObserver {
        helix::LifetimeToken token;
        helix::SharedCallback<void()> callback;
        bool country_only = false; ///< add_country_observer(): READY and REGDOM_CHANGE only
    };
    std::mutex state_observers_mutex_;
    std::vector<StateObserver> state_observers_;
    /// @p country_may_have_changed also wakes the country observers.
    void notify_state_observers(bool country_may_have_changed = false);

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
    static constexpr auto kAssociationGrace = std::chrono::seconds(5);
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
    // has_wired_fallback is computed by the caller (which still holds a live
    // `this`) so this stays free of member access.
    static void report_radio_result(bool enabled, const WiFiError& result, bool has_wired_fallback);

    // Records the radio state a toggle actually reached, so the stored
    // expectation is never left at the value the caller assumed when it
    // flipped its switch. Static for the same reason as report_radio_result,
    // and deferred on the MANAGER's token rather than the caller's: a screen
    // that starts a toggle and is dismissed inside the radio's multi-second
    // window must not take the record of the outcome down with it. Only a
    // toggle that actually reached the backend calls this — with nothing
    // attempted there is no outcome to record. Main-thread only.
    static void persist_radio_expectation(bool requested, bool success, bool actual);

    // Barrier for the control lane's drain. It runs on an HttpExecutor thread
    // and dereferences `this` (backend_) for the whole of its backend call, so
    // the destructor waits here before touching backend_, and the backend swap
    // does not start until the count is zero. Unbounded in the destructor on
    // purpose: a timeout that expired would hand the worker a freed backend,
    // and the backend calls carry their own deadlines.
    std::mutex radio_op_mutex_;
    std::condition_variable radio_op_cv_;
    int radio_ops_inflight_ = 0;
    void begin_radio_op();
    void end_radio_op();
    void wait_for_radio_ops();

    // Callers waiting on the status read in flight (get_status_async).
    struct StatusWaiter {
        helix::LifetimeToken token;
        helix::SharedCallback<void(const WifiBackend::ConnectionStatus&)> on_done;
    };
    std::mutex status_waiters_mutex_;
    std::vector<StatusWaiter> status_waiters_;
    bool status_read_inflight_ = false;
    // The last status a backend read returned in time, adjusted by
    // CONNECTED/DISCONNECTED events. Guarded by status_waiters_mutex_. Answers
    // callers while the breaker is open, and the scan scheduler's "connected?"
    // question, which must not cost a backend round trip on the LVGL thread.
    WifiBackend::ConnectionStatus cached_status_;
    bool cached_connected();
    void run_status_reads();

    // The control lane: backend control calls run here one at a time, in
    // submission order, on an HttpExecutor::fast() worker (see the class
    // comment). The running drain holds a radio op, so the destructor and the
    // backend swap wait for the call in progress; the destructor drops what is
    // still queued. `on_dropped` runs for an op that never runs.
    // A health call (status read, scan trigger) is one short request on a
    // healthy daemon; a user op (connect, disconnect, forget, radio) can
    // legitimately take tens of seconds (NetworkManager joins its previous
    // nmcli worker), so the two are held to different stall limits.
    enum class ControlKind { Health, User };
    struct ControlOp {
        const char* tag = nullptr;
        ControlKind kind = ControlKind::Health;
        helix::Callback<void()> work;
        helix::Callback<void()> on_dropped;
    };
    std::mutex control_mutex_;
    std::deque<ControlOp> control_queue_; // guarded by control_mutex_
    bool control_lane_active_ = false;    // guarded by control_mutex_
    bool control_shutdown_ = false;       // guarded by control_mutex_
    // Set while a backend swap waits for the lane to empty: new calls are
    // dropped and the drain stops after the call in progress.
    bool control_paused_ = false; // guarded by control_mutex_
    // The pool has run this manager's calls. Once it has, a pool that is not
    // running has been stopped (app exit), not never started (ESP32), and a
    // call must not fall back to running inline on the LVGL thread.
    // LVGL thread only, like HttpExecutor::running().
    bool lane_pooled_ = false;
    void submit_control(const char* tag, ControlKind kind, helix::Callback<void()> work,
                        helix::Callback<void()> on_dropped = nullptr);
    void run_control_ops();
    void drop_control_ops();

    // Deadlines for user ops, and how long one may run before the lane counts
    // as stalled. A member so tests can shorten them. LVGL thread only.
    struct UserOpTimeouts {
        uint32_t connect_ms = 45000; // the connect watchdog's window
        uint32_t other_ms = 40000;   // disconnect, forget, radio: two wpa_ctrl
                                     // round trips at their 15 s worst, plus margin
        uint32_t stall_ms = 60000;   // past every deadline above, and nmcli's 30 s
    };
    UserOpTimeouts user_op_timeouts_;

    // Sends a user op down the lane and guarantees its caller one answer:
    // `work` runs on the lane and returns what to do on the LVGL thread with
    // its result (may be empty); `on_unanswered` runs on the LVGL thread
    // instead when the lane is unresponsive at submit, the op is dropped unrun,
    // or `deadline_ms` passes first. An op whose deadline passes while still
    // queued is never sent. Nothing runs once the manager is destroyed.
    void submit_user_op(const char* tag, uint32_t deadline_ms,
                        helix::Callback<helix::Callback<void()>()> work,
                        helix::SharedCallback<void()> on_unanswered);
    // The answer a user op gets when WiFi control is not answering, how to
    // recognise it, and the one toast every action shows for it, in place of
    // that action's own failure toast. LVGL thread for the toast.
    static WiFiError unresponsive_error();
    static bool is_unresponsive_error(const WiFiError& error);
    // Runs `fn` on the LVGL thread after `ms`, unless the manager is gone by
    // then. The timer frees itself, so nothing is left for the destructor.
    void after_ms(uint32_t ms, helix::Callback<void()> fn);

    // Swaps in wpa_supplicant once the lane is idle; retries until it is.
    // LVGL thread only, with control_paused_ set.
    void swap_to_wpa_supplicant(bool silent);
    // wait_for_radio_ops(), giving up after `timeout`. True once none remain.
    bool wait_for_radio_ops_for(std::chrono::milliseconds timeout);

    // Health of the control interface (wifi_control_breaker.h). record() runs
    // on the lane, allow_call() on the LVGL thread.
    std::mutex breaker_mutex_;
    helix::wifi::ControlBreaker breaker_; // guarded by breaker_mutex_
    static uint64_t monotonic_ms();
    // Feeds the breaker one health call that started at `started`. Returns
    // whether it answered in time. Lane thread.
    bool record_health_call(std::chrono::steady_clock::time_point started);
    // Claims a breaker admission for a health call about to be sent. LVGL thread.
    bool breaker_allows_call();
    bool breaker_open();
    // A health call admitted by breaker_allows_call() will never run.
    void breaker_abandon_probe();

    // When the call now running on the lane started, and its kind; unset
    // while idle. Guarded by control_mutex_. The breaker only hears about
    // calls that finish, and a send blocked on a daemon that stopped reading
    // never does.
    std::optional<std::chrono::steady_clock::time_point> control_op_started_;
    ControlKind control_op_kind_ = ControlKind::Health;
    // The call now running has outlived its kind's limit (the breaker's
    // slow-call time for a health call, user_op_timeouts_.stall_ms for a user
    // op), so anything queued behind it will wait at least that long. LVGL thread.
    bool control_stalled();
    // control_stalled() or the breaker is open: a user op would not be answered.
    bool control_unresponsive();

    // A scan trigger is queued or running on the lane. LVGL thread only.
    bool scan_trigger_inflight_ = false;
    // start_scan()'s caller is still owed an answer. LVGL thread only.
    bool user_scan_waiting_ = false;
    // Bumped whenever the owed answer is settled, so a stale watch stops.
    uint64_t scan_wait_generation_ = 0;
    // The last list a scan delivered: what a refused, failed or stuck scan
    // answers with, so the visible list is kept. LVGL thread only.
    std::vector<WiFiNetwork> last_scan_results_;
    // Hands start_scan()'s caller last_scan_results_ if it is still waiting.
    void answer_scan_waiter();
    // Hands the scan callback last_scan_results_. LVGL thread only.
    void deliver_last_scan_results();
    // Answers the waiter once the trigger it waits on is stuck behind a
    // stalled lane; polls until the trigger leaves the lane.
    void watch_scan_waiter(uint64_t generation);
    enum class ScanRequest { Sent, Coalesced, Refused };
    // Sends one scan trigger down the lane unless one is already in flight or
    // the breaker refuses. `user_refresh` selects start_scan()'s failure
    // reporting over the periodic tick's. LVGL thread only.
    ScanRequest request_scan(bool user_refresh);
    void apply_scan_trigger_result(const WiFiError& result, bool user_refresh);

    // Bumped by connect() and disconnect(), so a connect result arriving after
    // the attempt was superseded is dropped. LVGL thread only.
    uint64_t connect_generation_ = 0;
    void apply_connect_result(uint64_t generation, const std::string& ssid,
                              const WiFiError& result);

    // Toasts a forget outcome and hands it to the caller. Static: it runs
    // after the lane call and needs nothing of the manager. LVGL thread.
    static void report_forget_result(
        const std::string& ssid, const WiFiError& result,
        const helix::SharedCallback<void(bool success, const std::string& error)>& cb);

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
