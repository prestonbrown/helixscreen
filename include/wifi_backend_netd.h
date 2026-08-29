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
 * - Commands from other threads park ONE pending-ack slot. The wait is an
 *   early-reject window only: on timeout the op is treated as accepted and
 *   any late ack arrives unsolicited and drives events.
 * - stop() closes the connection but keeps the event loop alive (libhv
 *   loops cannot restart); the destructor does the full stop+join+cleanup.
 *
 * The constructor's millisecond knobs are injectable so tests can shrink
 * the reconnect/watchdog/ack timings; production uses the defaults.
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
     * @param ack_wait_ms       How long a sent command waits for its OK/ERR
     *                         ack before being treated as accepted (default
     *                         2 s). The daemon acks operation COMPLETION,
     *                         not acceptance — CONNECT_WIFI's OK can lag by
     *                         tens of seconds, so this is a reject window,
     *                         never a verdict.
     */
    explicit WifiBackendNetd(int reconnect_ms = 5000, int scan_watchdog_ms = 15000,
                             int ack_wait_ms = 2000);

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

    /// ::socket + ::connect to helix::netd::socket_path(), register the fd
    /// with libhv, send SUBSCRIBE + GET. Loop thread. @p error_out receives
    /// a log-safe errno description on failure.
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
    void handle_line(const std::string& line);

    /// Apply an OK/ERR line. @p pending_cmd is the command whose parked slot
    /// this ack resolved ("" when the ack was unsolicited — a late or
    /// daemon-initiated completion).
    void handle_ack(const helix::netd::Ack& ack, const std::string& pending_cmd);

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

    /// Blocking-send @p line (which must already end in '\n') on the daemon
    /// socket under cmd_mutex_. Bounded: partial writes poll for writability
    /// with a hard attempt cap, so a wedged peer cannot pin a caller thread.
    /// Callable from the loop thread (handshake, liveness probe) too.
    bool write_line_raw(const std::string& line);

    /// Outcome of a parked command send.
    struct SendOutcome {
        enum class Kind {
            NotSent, ///< The socket was down; the daemon never saw the command.
            Ok,      ///< OK ack arrived within ack_wait_ms_.
            Err,     ///< ERR ack arrived within ack_wait_ms_ (text = reason).
            Timeout, ///< No ack in the window; the op is treated as accepted.
        };
        Kind kind{Kind::NotSent};
        std::string text; ///< ERR reason, when kind == Err.
    };

    /// Send @p cmd (no trailing newline) and park the pending-ack slot while
    /// waiting, up to ack_wait_ms_. EXTERNAL THREADS ONLY — it blocks on a
    /// condvar that the event loop resolves, so calling it on the loop
    /// thread would self-deadlock. Loop-thread sends use write_line_raw().
    SendOutcome send_request(const std::string& cmd);

    /// If nothing arrives from the daemon while an op is outstanding, probe
    /// with GET, then force a reconnect — the first-party client's watchdog.
    void arm_liveness_probe();

    /// Resolve the parked slot (if any) with @p ack and wake its waiter.
    /// Returns the command whose slot was resolved ("" when the ack was
    /// unsolicited — a late or daemon-initiated completion).
    std::string resolve_pending(const helix::netd::Ack& ack);

    // ========================================================================
    // Helpers
    // ========================================================================

    /// Read the station MAC from sysfs once per init, via the interface
    /// probe (never a hardcoded netdev name). Empty when unresolvable.
    void read_mac_address_once();

    bool event_loop_active() const {
        return const_cast<WifiBackendNetd*>(this)->hv::EventLoopThread::isRunning();
    }

    void signal_init_complete(const std::string& error);

    // ========================================================================
    // State
    // ========================================================================

    /// The parked pending-ack slot. Dismissed = released without a verdict
    /// (displaced by a newer op, or teardown): the waiter treats its op as
    /// accepted — losing the slot never cancels a daemon-owned operation.
    struct AckWait {
        enum class Result { Waiting, Ok, Err, Dismissed };
        std::mutex mtx;
        std::condition_variable cv;
        Result result{Result::Waiting};
        std::string text; ///< ERR reason / OK text, once resolved.
    };

    const int reconnect_ms_;
    const int scan_watchdog_ms_;
    const int ack_wait_ms_;

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

    std::string mac_address_; ///< Station MAC, read once at init.

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
    bool was_connected_ = false; ///< loop thread only (snapshot state diff)

    // Timers and liveness bookkeeping. Loop thread only.
    static constexpr hv::TimerID kNoTimer{0};
    hv::TimerID reconnect_timer_{kNoTimer};
    hv::TimerID scan_watchdog_timer_{kNoTimer};
    std::chrono::steady_clock::time_point last_line_at_;

    std::mutex pending_mutex_;
    std::shared_ptr<AckWait> pending_ack_; ///< Parked slot, or null.
    std::string pending_cmd_;              ///< Command that parked it.

    // Async init worker (start_async()); joined in stop() and the dtor.
    std::thread async_init_thread_;
    std::atomic<bool> async_init_in_progress_{false};
};

#endif // __APPLE__
