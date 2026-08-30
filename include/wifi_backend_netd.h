// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#ifndef __APPLE__
// ============================================================================
// Linux Implementation: netd line-protocol client
// ============================================================================

#include "hv/EventLoop.h"
#include "hv/EventLoopThread.h"
#include "hv/hloop.h"
#include "netd_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief WiFi backend driving the printer's network daemon over its line
 *        protocol
 *
 * helix::netd (netd_protocol.h) owns the wire format; this class owns the
 * connection lifecycle and translates between daemon pushes and the
 * WifiBackend event/callback surface. The daemon — not this class — owns
 * retry/recovery for network operations; losing the socket never cancels a
 * daemon-owned op, and the only CANCEL ever sent is an explicit
 * disconnect_network() call.
 *
 * Architecture (mirrors WifiBackendWpaSupplicant):
 * - Inherits privately from hv::EventLoopThread; all socket I/O callbacks,
 *   timers, and event dispatch run on the loop thread.
 * - One stream connection to helix::netd::socket_path(); SUBSCRIBE at
 *   connect, then GET to seed the snapshot (the daemon only pushes on
 *   change). The connection is re-established by a timer whenever it drops
 *   while the backend is running.
 * - Commands are FIRE-AND-FORGET: the daemon acks operation COMPLETION, not
 *   acceptance (a join's OK can trail by tens of seconds), so parking the
 *   caller for a verdict would freeze the UI thread for the length of a
 *   radio operation. Verdicts arrive as daemon lines and drive events;
 *   attribution is by outstanding work (a pending scan claims the next
 *   ack; everything else maps to the join in flight).
 * - stop() closes the connection but keeps the event loop alive (libhv
 *   loops cannot restart); the destructor does the full stop+join+cleanup.
 *
 * The constructor's millisecond knobs are injectable so tests can shrink
 * the reconnect/watchdog timings; production uses the defaults.
 */
class WifiBackendNetd : public WifiBackend, private hv::EventLoopThread {
  public:
    /**
     * @param reconnect_ms      Delay before re-establishing a dropped daemon
     *                         connection (default 5 s).
     * @param scan_watchdog_ms  Upper bound from an accepted SCAN to the
     *                         SCAN_COMPLETE event (default 15 s). A success
     *                         return from trigger_scan() OBLIGATES an
     *                         eventual SCAN_COMPLETE, or WiFiManager's scan
     *                         scheduler latches forever.
     */
    explicit WifiBackendNetd(int reconnect_ms = 5000, int scan_watchdog_ms = 15000);

    /**
     * @brief Destructor - full teardown (stop loop, join threads, close socket)
     */
    ~WifiBackendNetd() override;

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
    WiFiError set_radio_enabled(bool on) override;
    bool is_radio_enabled() const override;

  private:
    // ========================================================================
    // Connection lifecycle (loop thread unless noted)
    // ========================================================================

    /// First-time init: open the daemon connection, subscribe, seed. Runs on
    /// the loop thread (as the EventLoopThread pre-functor or a runInLoop
    /// task) and signals init_complete_ whatever the outcome.
    void init_netd();

    /// Tear the connection down and kill every timer. Runs on the loop
    /// thread; called from stop() (scheduled, loop kept alive) and the
    /// destructor (scheduled before the loop is stopped).
    void cleanup_netd();

    /// ::socket + ::connect to helix::netd::socket_path() (non-blocking,
    /// bounded), register the fd with libhv, send SUBSCRIBE + GET. Loop
    /// thread. @p error_out receives a log-safe errno description on failure.
    bool open_connection(std::string& error_out);

    /// hio_close() the current connection (which closes the fd — this class
    /// never ::close()s it separately) and reset the line assembler. Loop
    /// thread; takes cmd_mutex_ so an in-flight writer can never race the
    /// close.
    void close_connection();

    /// Reconnect-timer callback: re-open the connection, re-arm on failure.
    void try_reconnect();
    void arm_reconnect_timer();

    // ========================================================================
    // Read path (loop thread)
    // ========================================================================

    static void _on_read(hio_t* io, void* data, int nbyte);
    void on_read(void* data, int nbyte);

    /// libhv never delivers the read callback on EOF — it closes the hio
    /// directly (nio.c: nread==0 -> hio_close), so socket loss is detected
    /// HERE. Fires for deliberate closes too; those have already nulled io_,
    /// so the identity check filters them out.
    static void _on_close(hio_t* io);
    void on_socket_closed(hio_t* io);

    /// Dispatch one complete daemon line: ack, then scan row, then snapshot
    /// field, then ignore (the order documented in netd_protocol.h).
    /// Returns true when a snapshot field merged — the caller batches the
    /// state-diff to once per read batch.
    bool handle_line(const std::string& line);

    /// Apply an OK/ERR line with no outstanding scan (the scan path in
    /// handle_line owns the outstanding-scan case): late or daemon-initiated
    /// join verdicts, mapped to events.
    void handle_ack(const helix::netd::Ack& ack);

    /// State-diff event logic over the just-merged snapshot.
    void handle_snapshot_diff(const helix::netd::NetdSnapshot& after);

    /// End the in-progress scan: kill the watchdog, fire SCAN_COMPLETE from
    /// the row cache. Loop thread.
    void finish_scan();

    /// Fire a registered callback by name (copy under the map mutex, invoke
    /// outside it — same deadlock-avoidance shape as the wpa backend).
    void dispatch_event(const std::string& event_name, const std::string& message);

    // ========================================================================
    // Write path (any thread)
    // ========================================================================

    /// Non-blocking send under cmd_mutex_ for EXTERNAL threads. Commands are
    /// tiny, so a send that cannot complete immediately means a wedged
    /// daemon: report failure on the spot and leave recovery to the
    /// liveness/reconnect machinery — the caller (the UI thread) is never
    /// parked.
    bool write_line_raw(const std::string& line);

    /// Non-blocking send for the loop thread's own lines (handshake, liveness
    /// GET). No mutex (the loop serializes against the connection mutators)
    /// and the same fail-fast policy as write_line_raw().
    bool write_line_from_loop(const std::string& line);

    /// Fire-and-forget command send from an external thread: send, arm the
    /// liveness probe, return. No verdict is waited for — see the class doc.
    bool send_line(const std::string& cmd);

    /// If nothing arrives from the daemon while an op is outstanding, probe
    /// with GET, then force a reconnect — the first-party client's watchdog.
    void arm_liveness_probe();

    // ========================================================================
    // Helpers
    // ========================================================================

    /// Read the station MAC from sysfs via the interface probe (never a
    /// hardcoded netdev name); no-op once non-empty. Called at init AND on a
    /// CONNECTED transition: a box booted in ETHERNET mode has no wlan0 up
    /// when init runs, so the first read legitimately finds nothing and the
    /// MAC would stay blank for the whole session without the retry
    /// (prestonbrown/helixscreen#1399). Empty when unresolvable.
    void read_mac_address_if_empty();

    bool event_loop_active() {
        return hv::EventLoopThread::isRunning();
    }

    /// Schedule cleanup_netd() on the loop and wait for it, bounded at 2 s.
    /// The shared_ptr keeps the promise alive if the wait times out and the
    /// cleanup runs later — the wpa backend's use-after-free fix, extracted
    /// here as ONE copy shared by stop() and the destructor.
    void schedule_cleanup_bounded();

    void signal_init_complete(const std::string& error);

    /// Dispatch INIT_FAILED at most once per init attempt: the async worker
    /// (on start() timeout) and a late-running init_netd() can both observe
    /// the same failure, and double-dispatching would double-notify the
    /// manager.
    void emit_init_failed_once(const std::string& error);

    // ========================================================================
    // State
    // ========================================================================

    const int reconnect_ms_;
    const int scan_watchdog_ms_;

    // Connection. io_ / fd_ are written under cmd_mutex_; fd_ is owned by
    // io_ once registered (hio_close closes it — never ::close(fd_) too).
    hio_t* io_{nullptr};
    int fd_{-1};
    helix::netd::LineAssembler assembler_; ///< loop thread only

    std::mutex cmd_mutex_;       ///< Serializes writers against the closer.
    std::mutex callbacks_mutex_; ///< Protects the callbacks map.
    std::map<std::string, std::function<void(const std::string&)>> callbacks_;

    std::mutex snapshot_mutex_;
    helix::netd::NetdSnapshot snapshot_; ///< Last merged daemon state.

    std::mutex scan_mutex_;
    std::vector<helix::netd::ScanRow> scan_rows_; ///< Rows of the current/last scan.

    /// Station MAC. Written at init and on the CONNECTED retry (loop thread),
    /// read by get_status() from any thread — guard with snapshot_mutex_.
    std::string mac_address_;

    // start()/start_async() synchronization (same shape as the wpa backend).
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    std::atomic<bool> init_complete_{false};  ///< An init attempt finished.
    std::atomic<bool> init_succeeded_{false}; ///< The connection is live.
    std::string init_error_;                  ///< Guarded by init_mutex_.

    std::atomic<bool> shutdown_requested_{false};
    /// True between a successful init and stop(): the reconnect timer may
    /// fire while it is set. Socket loss alone never clears it.
    std::atomic<bool> want_connection_{false};
    std::atomic<bool> scan_pending_{false};
    std::atomic<bool> connect_in_flight_{false};
    /// Auth failures fire AUTH_FAILED once per failure, never per RETRYING
    /// push; cleared when a new join is accepted or a CONNECTED lands.
    std::atomic<bool> auth_failure_latched_{false};
    /// Set when any scan row arrives on the 5 GHz band — a stronger answer
    /// to supports_5ghz() than a hardcoded claim.
    std::atomic<bool> seen_5ghz_network_{false};
    /// Guards emit_init_failed_once()'s at-most-once dispatch per attempt.
    std::atomic<bool> init_failed_dispatched_{false};
    bool was_connected_ = false; ///< loop thread only (snapshot state diff)

    // Timers and liveness bookkeeping. Loop thread only.
    static constexpr hv::TimerID kNoTimer{0};
    hv::TimerID reconnect_timer_{kNoTimer};
    hv::TimerID scan_watchdog_timer_{kNoTimer};
    std::chrono::steady_clock::time_point last_line_at_;

    // Async init worker (start_async()); joined in stop() and the dtor.
    std::thread async_init_thread_;
    std::atomic<bool> async_init_in_progress_{false};
};

#endif // __APPLE__
