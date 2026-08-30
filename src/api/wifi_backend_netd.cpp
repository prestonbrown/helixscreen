// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_netd.h"

#include "spdlog/spdlog.h"
#include "wifi_ui_utils.h"

#if !defined(__APPLE__) && !defined(__ANDROID__)
// ============================================================================
// Linux Implementation: netd line-protocol client
// ============================================================================

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Snapshot values are raw strings; a missing or malformed SIGNAL reads as 0
// (absent), never as garbage.
int parse_signal_percent(const std::string& text) {
    if (text.empty())
        return 0;
    char* end = nullptr;
    const long dbm = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str())
        return 0;
    return wifi_signal_percent_from_dbm(static_cast<int>(dbm));
}

// The first token of a wire command ("SCAN", "CONNECT_WIFI", ...). Commands
// are logged by NAME only — a CONNECT_WIFI line carries base64 ssid/psk,
// which is decodable PII and must never reach a log.
std::string command_name(const std::string& cmd) {
    const size_t space = cmd.find(' ');
    return cmd.substr(0, space);
}

// Production MAC source for the injectable reader: resolve the netdev through
// the sysfs probe — never a hardcoded name; iface is filled even when the link
// is down — then read the address through the shared, redaction-aware helper.
std::string default_mac_reader() {
    const helix::ui::wifi::OsWifiLink link = helix::ui::wifi::probe_os_wifi_link();
    if (link.iface.empty())
        return std::string();
    return helix::ui::wifi::wifi_get_device_mac(link.iface);
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

WifiBackendNetd::WifiBackendNetd(int reconnect_ms, int scan_watchdog_ms, MacReader mac_reader,
                                 int liveness_probe_ms, int liveness_giveup_ms)
    : hv::EventLoopThread(nullptr), reconnect_ms_(reconnect_ms),
      scan_watchdog_ms_(scan_watchdog_ms), liveness_probe_ms_(liveness_probe_ms),
      liveness_giveup_ms_(liveness_giveup_ms),
      mac_reader_(mac_reader ? std::move(mac_reader) : MacReader(default_mac_reader)) {
    spdlog::debug("[WifiBackendNetd] Initialized (netd mode)");
}

WifiBackendNetd::~WifiBackendNetd() {
    spdlog::trace("[WifiBackendNetd] Destructor called");

    shutdown_requested_.store(true);
    want_connection_.store(false);

    // Join the async init worker so it cannot call start() into us mid-teardown.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Clean up while the loop still runs: the hio belongs to the loop's
    // lifetime, so the close must be scheduled (and finish) BEFORE the loop
    // stops. Same ordering rationale as the wpa backend's destructor.
    schedule_cleanup_bounded();

    hv::EventLoopThread::stop();
    hv::EventLoopThread::join();
}

void WifiBackendNetd::schedule_cleanup_bounded() {
    if (!event_loop_active() || !loop()) {
        // Loop not running: no I/O callbacks can fire, cleanup directly.
        cleanup_netd();
        return;
    }
    auto cleanup_done = std::make_shared<std::promise<void>>();
    std::future<void> cleanup_future = cleanup_done->get_future();
    loop()->runInLoop([this, cleanup_done]() {
        cleanup_netd();
        cleanup_done->set_value();
    });
    if (cleanup_future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        spdlog::warn("[WifiBackendNetd] Cleanup timed out after 2 seconds");
    }
}

void WifiBackendNetd::emit_init_failed_once(const std::string& error) {
    bool expected = false;
    if (!init_failed_dispatched_.compare_exchange_strong(expected, true))
        return;
    dispatch_event("INIT_FAILED", error);
}

void WifiBackendNetd::signal_init_complete(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_error_ = error;
    }
    init_complete_.store(true);
    init_cv_.notify_all();
}

WiFiError WifiBackendNetd::start() {
    spdlog::debug("[WifiBackendNetd] Starting...");
    shutdown_requested_.store(false);
    init_failed_dispatched_.store(false);

    if (event_loop_active()) {
        // stop() deliberately leaves the event loop alive (libhv loops cannot
        // restart), so a restart re-runs the init on the live loop. Keyed on
        // init_succeeded_ so a FAILED prior init still retries here.
        if (init_complete_.load() && init_succeeded_.load()) {
            spdlog::debug("[WifiBackendNetd] Already running and initialized");
            return WiFiErrorHelper::success();
        }
        init_complete_.store(false);
        loop()->runInLoop([this]() { init_netd(); });
    } else {
        init_complete_.store(false);
        try {
            spdlog::info("[WifiBackendNetd] Starting event loop thread");
            hv::EventLoopThread::start(true, [this]() -> int {
                init_netd();
                return 0;
            });
        } catch (const std::exception& e) {
            return WiFiErrorHelper::connection_failed("Failed to start event loop: " +
                                                      std::string(e.what()));
        }
    }

    {
        std::unique_lock<std::mutex> lock(init_mutex_);
        if (!init_cv_.wait_for(lock, std::chrono::seconds(5),
                               [this] { return init_complete_.load(); })) {
            spdlog::error("[WifiBackendNetd] Initialization timed out after 5 seconds");
            return WiFiError(WiFiResult::TIMEOUT, "Backend initialization timed out",
                             "WiFi system took too long to start");
        }
        if (!init_succeeded_.load()) {
            // Connect failures carry strerror(errno) — socket permissions on
            // this platform are a real risk and the errno must be visible.
            const std::string error = init_error_;
            return WiFiError(WiFiResult::CONNECTION_FAILED,
                             error.empty() ? "netd init did not complete" : error,
                             "Could not reach the printer's network service",
                             "Check that the printer's network service is running");
        }
    }

    spdlog::info("[WifiBackendNetd] Backend initialized successfully");
    return WiFiErrorHelper::success();
}

void WifiBackendNetd::start_async() {
    // Non-blocking variant: run start() on a worker thread and fire READY on
    // success or INIT_FAILED on failure (the wpa backend's exact shape).
    bool expected = false;
    if (!async_init_in_progress_.compare_exchange_strong(expected, true)) {
        spdlog::debug("[WifiBackendNetd] start_async already in progress");
        return;
    }
    if (init_complete_.load() && init_succeeded_.load()) {
        async_init_in_progress_.store(false);
        dispatch_event("READY", "");
        return;
    }

    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Wrap — EAGAIN under thread exhaustion throws std::system_error.
    try {
        async_init_thread_ = std::thread([this]() {
            const WiFiError result = start();
            const bool ran_init = init_complete_.load();
            async_init_in_progress_.store(false);
            if (result.success()) {
                dispatch_event("READY", "");
            } else if (!ran_init) {
                // start() timed out before init_netd() ran; the late init
                // may still dispatch its own failure, so route through the
                // at-most-once guard.
                emit_init_failed_once(result.technical_msg);
            }
        });
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackendNetd] Failed to spawn init thread: {}", e.what());
        async_init_in_progress_.store(false);
        emit_init_failed_once("system busy");
    }
}

void WifiBackendNetd::stop() {
    // Join any outstanding async init worker so it cannot race teardown.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // NO CANCEL is ever sent here: stop() detaches this process from the
    // daemon; it does not abort a daemon-owned operation.
    want_connection_.store(false);
    // Guard a still-QUEUED init_netd(): if start() timed out waiting for it,
    // the task is pending on the loop, and without this flag it would later
    // open a connection and resurrect a backend the caller just stopped
    // (re-arming want_connection_ and is_running_). start() clears the flag
    // again on the next explicit start.
    shutdown_requested_.store(true);

    if (!init_complete_.load()) {
        spdlog::trace("[WifiBackendNetd] stop(): never initialized");
        return;
    }

    spdlog::info("[WifiBackendNetd] Stopping (keeping the event loop alive)");

    init_succeeded_.store(false);
    init_complete_.store(false);

    schedule_cleanup_bounded();

    spdlog::debug("[WifiBackendNetd] Stopped");
}

bool WifiBackendNetd::is_running() const {
    // Mirrors init success only: a dropped daemon socket does NOT flip this
    // (the reconnect timer owns that); the last snapshot keeps being served.
    return init_succeeded_.load();
}

void WifiBackendNetd::register_event_callback(const std::string& name,
                                              std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    const auto& entry = callbacks_.find(name);
    if (entry == callbacks_.end()) {
        callbacks_.insert({name, callback});
        spdlog::debug("[WifiBackendNetd] Registered callback '{}'", name);
    } else {
        spdlog::warn("[WifiBackendNetd] Callback '{}' already registered (not replacing)", name);
    }
}

// ============================================================================
// Init / connection management (loop thread)
// ============================================================================

void WifiBackendNetd::init_netd() {
    read_mac_address_if_empty();

    if (shutdown_requested_.load()) {
        signal_init_complete("shutdown requested during init");
        return;
    }

    std::string error;
    if (!open_connection(error)) {
        spdlog::warn("[WifiBackendNetd] Init failed: {}", error);
        emit_init_failed_once(error);
        signal_init_complete(error);
        return;
    }

    // stop() early-returns without cleanup when init has not completed yet,
    // so it can have run entirely inside open_connection(): without this
    // re-check the completing init would resurrect a stopped backend with a
    // live socket.
    if (shutdown_requested_.load()) {
        close_connection();
        signal_init_complete("shutdown requested during init");
        return;
    }

    want_connection_.store(true);
    // MODE=ETHERNET or NO_CONFIG at this point is STILL SUCCESS: the socket
    // is up and wifi is simply unconfigured. init_succeeded_ mirrors the
    // connection, never the wifi state.
    init_succeeded_.store(true);
    spdlog::info("[WifiBackendNetd] Connected to the network daemon");
    signal_init_complete({});
}

void WifiBackendNetd::cleanup_netd() {
    spdlog::trace("[WifiBackendNetd] Cleaning up daemon connection");

    if (reconnect_timer_ != kNoTimer) {
        loop()->killTimer(reconnect_timer_);
        reconnect_timer_ = kNoTimer;
    }
    if (scan_watchdog_timer_ != kNoTimer) {
        loop()->killTimer(scan_watchdog_timer_);
        scan_watchdog_timer_ = kNoTimer;
    }
    cancel_liveness_probe();
    // trigger_scan() returning success OBLIGATES an eventual SCAN_COMPLETE;
    // tearing down with a scan outstanding would latch the manager's
    // scheduler forever (stop-then-start is a supported reuse path). Resolve
    // from the cached rows — a listener still registered gets its event; a
    // torn-down manager unregistered first and this is a no-op.
    finish_scan();
    connect_in_flight_.store(false);

    close_connection();
    spdlog::debug("[WifiBackendNetd] Daemon connection cleaned up");
}

bool WifiBackendNetd::open_connection(std::string& error_out) {
    const std::string path = helix::netd::socket_path();

    // A previous init that outlived its caller's start() timeout may have
    // left a connection open; never overwrite a live hio (its fd would
    // leak until the loop dies).
    if (io_ != nullptr) {
        close_connection();
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error_out = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        error_out = "socket path too long (" + std::to_string(path.size()) + " bytes)";
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // Non-blocking connect with a bounded wait. A local connect usually
    // completes instantly, but a daemon wedged with a full accept backlog
    // would BLOCK the loop thread in ::connect() forever — killing every
    // timer and read on the loop and hanging the destructor's join. One
    // second to connect, then the reconnect cadence owns the retry.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    const int connect_rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (connect_rc != 0) {
        if (errno != EINPROGRESS) {
            error_out = "connect(\"" + path + "\"): " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        pollfd connect_poll{};
        connect_poll.fd = fd;
        connect_poll.events = POLLOUT;
        if (::poll(&connect_poll, 1, 1000) <= 0) {
            error_out = "connect(\"" + path + "\"): timed out after 1 s";
            ::close(fd);
            return false;
        }
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
            socket_error != 0) {
            error_out = "connect(\"" + path +
                        "\"): " + std::strerror(socket_error != 0 ? socket_error : errno);
            ::close(fd);
            return false;
        }
    }

    hio_t* io = hio_get(hv::EventLoopThread::loop()->loop(), fd);
    if (io == nullptr) {
        error_out = "hio_get() failed for the daemon connection";
        ::close(fd);
        return false;
    }
    hio_set_context(io, this);
    hio_setcb_read(io, &WifiBackendNetd::_on_read);
    hio_setcb_close(io, &WifiBackendNetd::_on_close);
    hio_read_start(io);

    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        io_ = io;
        fd_ = fd;
    }
    assembler_.reset();
    // A scan pending from the OLD connection cannot be answered on this one:
    // the handshake's seed acks would be consumed as an instant empty-scan
    // completion. The request died with the old connection; the scan watchdog
    // owns resolving an orphaned scan from whatever rows arrived.
    scan_pending_.store(false);
    last_line_at_ = std::chrono::steady_clock::now();

    // Subscribe for pushes, then GET to seed the snapshot: the daemon only
    // pushes on CHANGE, so without the seed a connected printer shows blank
    // until the next state change. Loop-thread sends: no mutex, no waiting.
    if (!write_line_from_loop(helix::netd::encode_subscribe() + "\n") ||
        !write_line_from_loop(helix::netd::encode_get() + "\n")) {
        error_out = std::string("handshake write failed: ") + std::strerror(errno);
        close_connection();
        return false;
    }
    return true;
}

void WifiBackendNetd::close_connection() {
    // The closed connection's liveness question is moot: a stale probe or
    // give-up must not fire into whatever connection comes next.
    cancel_liveness_probe();
    hio_t* io = nullptr;
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        io = io_;
        io_ = nullptr;
        fd_ = -1;
    }
    if (io != nullptr) {
        // hio_close() IS the fd close for this connection — never also
        // ::close(fd_). Safe to call from inside the read callback (the
        // documented libhv teardown chain).
        hio_read_stop(io);
        hio_close(io);
    }
    assembler_.reset();
}

void WifiBackendNetd::arm_reconnect_timer() {
    if (reconnect_timer_ != kNoTimer)
        return;
    spdlog::debug("[WifiBackendNetd] Reconnecting to the daemon in {} ms", reconnect_ms_);
    reconnect_timer_ = loop()->setTimeout(reconnect_ms_, [this](hv::TimerID) {
        reconnect_timer_ = kNoTimer;
        try_reconnect();
    });
}

void WifiBackendNetd::try_reconnect() {
    if (shutdown_requested_.load() || !want_connection_.load())
        return;
    if (io_ != nullptr)
        return; // already re-established

    std::string error;
    if (open_connection(error)) {
        spdlog::info("[WifiBackendNetd] Reconnected to the network daemon");
        return;
    }
    spdlog::debug("[WifiBackendNetd] Reconnect failed: {}", error);
    arm_reconnect_timer();
}

// ============================================================================
// Read path (loop thread)
// ============================================================================

void WifiBackendNetd::_on_read(hio_t* io, void* data, int nbyte) {
    // Static trampoline: extract the instance and forward.
    auto* instance = static_cast<WifiBackendNetd*>(hio_context(io));
    if (instance != nullptr) {
        instance->on_read(data, nbyte);
    } else {
        spdlog::error("[WifiBackendNetd] Read callback invoked with null context");
    }
}

void WifiBackendNetd::on_read(void* data, int nbyte) {
    if (nbyte <= 0) {
        // 0 = orderly EOF, <0 = error: either way the daemon went away. The
        // last snapshot keeps being served; a timer re-establishes the
        // connection. LOSING THE SOCKET NEVER CANCELS a daemon-owned op.
        // (libhv normally closes the hio without delivering this case —
        // on_socket_closed() is the real detection path; this is defense.)
        spdlog::debug("[WifiBackendNetd] Daemon connection lost (read returned {})", nbyte);
        if (scan_pending_.load()) {
            finish_scan(); // the in-flight scan died with the connection
        }
        close_connection();
        if (want_connection_.load() && !shutdown_requested_.load()) {
            arm_reconnect_timer();
        }
        return;
    }

    // Apply the whole read batch, then diff once. A daemon push is one line
    // per field; merging field-by-field and diffing per line would fire
    // events off half-merged snapshots (CONNECTED before the SSID/IP lines
    // of the same push have landed).
    const std::vector<std::string> lines =
        assembler_.feed(std::string_view(static_cast<const char*>(data), nbyte));
    bool merged_any_field = false;
    for (const std::string& line : lines) {
        merged_any_field = handle_line(line) || merged_any_field;
    }
    if (merged_any_field) {
        helix::netd::NetdSnapshot after;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            after = snapshot_;
        }
        handle_snapshot_diff(after);
    }
}

void WifiBackendNetd::_on_close(hio_t* io) {
    auto* instance = static_cast<WifiBackendNetd*>(hio_context(io));
    if (instance != nullptr)
        instance->on_socket_closed(io);
}

void WifiBackendNetd::on_socket_closed(hio_t* io) {
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        if (io_ != io) {
            // A deliberate close already nulled io_ (stop(), reconnect,
            // teardown), or this is a stale hio. Nothing to recover from.
            return;
        }
        io_ = nullptr;
        fd_ = -1;
    }
    assembler_.reset();
    // The old connection's liveness question died with it (same rationale as
    // close_connection): a pending probe must not occupy the slot across the
    // reconnect, or the first command on the new connection goes unwatched.
    cancel_liveness_probe();

    spdlog::debug("[WifiBackendNetd] Daemon connection lost");
    // The in-flight scan's answers died with the connection: complete it
    // from whatever rows arrived before the drop, so the scheduler never
    // latches. (Leaving it armed would let the reconnect handshake's seed
    // acks complete it over an emptied cache instead.)
    if (scan_pending_.load()) {
        finish_scan();
    }
    if (want_connection_.load() && !shutdown_requested_.load()) {
        arm_reconnect_timer();
    }
}

bool WifiBackendNetd::handle_line(const std::string& line) {
    // Any daemon line proves the daemon alive: refresh the liveness clock and
    // retire a pending probe — its question ("is the daemon silent?") is
    // answered.
    last_line_at_ = std::chrono::steady_clock::now();
    cancel_liveness_probe();

    // Dispatch order per netd_protocol.h: ack -> scan row -> snapshot field
    // -> ignore. Returns true when a snapshot field merged; the caller diffs
    // once per read batch, not per line.
    const helix::netd::Ack ack = helix::netd::parse_ack(line);
    if (ack.kind != helix::netd::Ack::Kind::None) {
        // Sends are fire-and-forget with no request id on the wire, so an ack
        // is attributed by outstanding work: while a scan is pending, an OK
        // (its rows precede it) or an ERR from the scan's own reason family
        // completes that scan. Every other ERR is a join verdict — including
        // an auth-failure ERR while a scan is pending: netd retries a
        // wrong-password join for tens of seconds while the manager's scan
        // timer keeps firing, so the collision is routine, and eating the
        // join's ERR as the scan's completion would turn a wrong password
        // into a generic connect timeout (#1398). When both are outstanding
        // the scan loses the tie and its watchdog resolves it from the row
        // cache. BUSY can deny either op, so it owns the scan only when no
        // join is in flight.
        const bool scan_ack =
            scan_pending_.load() &&
            (ack.kind == helix::netd::Ack::Kind::Ok ||
             (ack.kind == helix::netd::Ack::Kind::Err &&
              (helix::netd::is_scan_failure_reason(ack.text) ||
               (helix::netd::is_busy_reason(ack.text) && !connect_in_flight_.load()))));
        if (scan_ack) {
            if (ack.kind == helix::netd::Ack::Kind::Err) {
                spdlog::debug("[WifiBackendNetd] Outstanding scan failed: {}", ack.text);
            }
            finish_scan();
        } else {
            handle_ack(ack);
        }
        return false;
    }

    if (const auto row = helix::netd::parse_scan_row(line)) {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        // The daemon streams a scan's rows BEFORE the OK that completes it,
        // so the first row is the moment the previous cache is obsolete. A
        // refused scan delivers no rows and the old list stands.
        if (!scan_rows_reset_) {
            scan_rows_reset_ = true;
            scan_rows_.clear();
        }
        scan_rows_.push_back(*row);
        if (row->frequency_mhz >= 4900) {
            // A 5 GHz BSS answers supports_5ghz() better than any static
            // claim: the radio demonstrably sees the band.
            seen_5ghz_network_.store(true);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (helix::netd::parse_snapshot_line(line, snapshot_))
            return true;
    }
    // Anything else is a newer daemon's addition: ignored (forward compat).
    return false;
}

void WifiBackendNetd::handle_ack(const helix::netd::Ack& ack) {
    if (ack.kind == helix::netd::Ack::Kind::Ok) {
        // An OK with no outstanding scan is a join's late completion (or a
        // reconnect-handshake seed ack) — the CONNECTED it implies arrives
        // separately as snapshot pushes.
        return;
    }

    // ERR: a late or daemon-initiated join verdict, mapped by reason (the
    // protocol module owns the vocabulary).
    const std::string& reason = ack.text;
    if (helix::netd::is_auth_failure_reason(reason)) {
        // Terminal join failure: fire AUTH_FAILED once per failure, never per
        // RETRYING push or repeated error. The latch clears when a new join
        // is accepted or a CONNECTED lands.
        connect_in_flight_.store(false);
        if (!auth_failure_latched_.exchange(true)) {
            dispatch_event("AUTH_FAILED", reason);
        }
    } else if (connect_in_flight_.exchange(false)) {
        // Any other failure while a join is user-visible ends that attempt
        // as a disconnect.
        dispatch_event("DISCONNECTED", reason);
    }
}

void WifiBackendNetd::handle_snapshot_diff(const helix::netd::NetdSnapshot& after) {
    const bool now_connected = after.connected_state() && after.mode == "WIFI";
    if (now_connected && !was_connected_) {
        // Association counts even before an address arrives (wpa COMPLETED
        // parity). Never fire CONNECTED for mode==ETHERNET.
        auth_failure_latched_.store(false);
        connect_in_flight_.store(false);
        // Reaching WIFI means the transport flipped and wlan0 exists now; on a
        // box that booted in ETHERNET mode the init-time MAC read found
        // nothing. The empty guard makes this a no-op once resolved.
        read_mac_address_if_empty();
        dispatch_event("CONNECTED", after.state);
    } else if (!now_connected && was_connected_) {
        // ANY loss of the connected state is a disconnect — including drops
        // that pass through an intermediate state first (RETRYING on beacon
        // loss, DHCP_WAIT, or the MODE flipping to ETHERNET). Gating on
        // terminal states only would consume was_connected_ on the
        // intermediate push and swallow the drop entirely: CONNECTED ->
        // RETRYING -> DISCONNECTED must still surface as one DISCONNECTED.
        dispatch_event("DISCONNECTED", after.state);
    }
    was_connected_ = now_connected;
}

void WifiBackendNetd::finish_scan() {
    if (scan_watchdog_timer_ != kNoTimer) {
        loop()->killTimer(scan_watchdog_timer_);
        scan_watchdog_timer_ = kNoTimer;
    }
    if (!scan_pending_.exchange(false))
        return;
    size_t rows = 0;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        rows = scan_rows_.size();
    }
    spdlog::debug("[WifiBackendNetd] Scan complete ({} rows)", rows);
    dispatch_event("SCAN_COMPLETE", "");
}

void WifiBackendNetd::dispatch_event(const std::string& event_name, const std::string& message) {
    // Copy the callback out under the mutex, release BEFORE invoking —
    // holding callbacks_mutex_ across the callback invites deadlock if a
    // handler re-enters the backend.
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        const auto it = callbacks_.find(event_name);
        if (it == callbacks_.end())
            return;
        cb = it->second;
    }
    spdlog::debug("[WifiBackendNetd] Dispatching '{}'", event_name);
    try {
        cb(message);
    } catch (const std::exception& e) {
        spdlog::error("[WifiBackendNetd] Exception in callback '{}': {}", event_name, e.what());
    } catch (...) {
        spdlog::error("[WifiBackendNetd] Unknown exception in callback '{}'", event_name);
    }
}

// ============================================================================
// Write path (any thread)
// ============================================================================

bool WifiBackendNetd::write_line_from_loop(const std::string& line) {
    // LOOP THREAD ONLY. io_/fd_ are mutated only by open_connection() and
    // close_connection(), both of which also run on the loop thread — the
    // loop itself serializes this write against them, so no mutex is needed.
    // Never wait here: a wedged peer's full buffer must not stall the loop
    // (timers, reads, the liveness watchdog all share it) — drop the line
    // and let the watchdog force the reconnect.
    if (io_ == nullptr || fd_ < 0)
        return false;
    const char* data = line.data();
    size_t remaining = line.size();
    while (remaining > 0) {
        const ssize_t n = ::send(fd_, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break; // buffer full: daemon wedged; do not park the loop
        spdlog::debug("[WifiBackendNetd] Daemon write failed: {}", std::strerror(errno));
        return false;
    }
    return remaining == 0;
}

bool WifiBackendNetd::write_line_raw(const std::string& line) {
    // EXTERNAL THREADS (the LVGL thread's fire-and-forget sends). Same
    // fail-fast policy as the loop path: commands are tiny, so a full
    // buffer means a wedged daemon — report failure immediately and let
    // the liveness/reconnect machinery own recovery. The caller (the UI
    // thread) is never parked.
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    if (io_ == nullptr || fd_ < 0)
        return false;

    const char* data = line.data();
    size_t remaining = line.size();
    while (remaining > 0) {
        const ssize_t n = ::send(fd_, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        spdlog::debug("[WifiBackendNetd] Daemon write failed: {}", std::strerror(errno));
        return false;
    }
    return true;
}

// Fire-and-forget send from an external thread: the daemon acks operation
// COMPLETION (an OK can trail a join by tens of seconds), so parking the
// caller for a verdict would freeze the UI thread for the length of a radio
// operation. Every command goes out, the caller returns immediately, and
// verdicts arrive as daemon lines that drive events. A failed or wedged
// send reports false on the spot; the liveness/reconnect machinery owns
// daemon-side recovery.
bool WifiBackendNetd::send_line(const std::string& cmd) {
    spdlog::debug("[WifiBackendNetd] Sending '{}'", command_name(cmd));
    if (!write_line_raw(cmd + "\n"))
        return false;
    arm_liveness_probe();
    return true;
}

void WifiBackendNetd::arm_liveness_probe() {
    // One chain at a time: a pending probe (or its give-up) already covers
    // "is the daemon silent while work is outstanding" — stacking one per
    // command only multiplies GETs against a quiet daemon.
    if (liveness_probe_timer_ != kNoTimer || liveness_giveup_timer_ != kNoTimer)
        return;
    liveness_probe_timer_ = loop()->setTimeout(liveness_probe_ms_, [this](hv::TimerID) {
        liveness_probe_timer_ = kNoTimer;
        if (shutdown_requested_.load() || io_ == nullptr)
            return;
        if (std::chrono::steady_clock::now() - last_line_at_ <
            std::chrono::milliseconds(liveness_probe_ms_))
            return;
        spdlog::debug("[WifiBackendNetd] Daemon silent while an op is pending; sending GET");
        (void)write_line_from_loop(helix::netd::encode_get() + "\n");
        liveness_giveup_timer_ = loop()->setTimeout(liveness_giveup_ms_, [this](hv::TimerID) {
            liveness_giveup_timer_ = kNoTimer;
            if (shutdown_requested_.load() || io_ == nullptr)
                return;
            if (std::chrono::steady_clock::now() - last_line_at_ <
                std::chrono::milliseconds(liveness_probe_ms_ + liveness_giveup_ms_))
                return;
            spdlog::warn("[WifiBackendNetd] Daemon unresponsive; forcing reconnect");
            close_connection();
            arm_reconnect_timer();
        });
    });
}

void WifiBackendNetd::cancel_liveness_probe() {
    if (liveness_probe_timer_ != kNoTimer) {
        loop()->killTimer(liveness_probe_timer_);
        liveness_probe_timer_ = kNoTimer;
    }
    if (liveness_giveup_timer_ != kNoTimer) {
        loop()->killTimer(liveness_giveup_timer_);
        liveness_giveup_timer_ = kNoTimer;
    }
}

// ============================================================================
// WifiBackend Interface Implementation
// ============================================================================

WiFiError WifiBackendNetd::trigger_scan() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // The daemon runs one op at a time: a scan fired mid-join is rejected
    // ERR BUSY on the wire AND collides with the join's ack attribution.
    // Refusing synchronously keeps the manager on its on_scan_failed() path —
    // the current list stays on screen and the scheduler retries after the
    // join resolves. (The manager suppresses the failure toast inside the
    // association grace window the join itself opened.)
    if (connect_in_flight_.load()) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "daemon busy with a join; scan deferred",
                         "WiFi is connecting — list refreshes after it finishes");
    }

    // The previous scan's rows stay cached until this scan's FIRST row
    // arrives (rows precede the completing OK): a refused scan never
    // delivers rows, so the old list stands instead of blanking.
    scan_rows_reset_ = false;
    scan_pending_.store(true);

    // Fire-and-forget: an immediate ERR (BUSY, SCAN_FAILED) arrives as a
    // daemon line and completes the scan through the same attribution as the
    // OK; the caller never waits.
    if (!send_line(helix::netd::encode_scan())) {
        scan_pending_.store(false);
        return WiFiErrorHelper::connection_failed(
            "netd connection is down; scan was not requested");
    }

    // The scan obligation: a SUCCESS here obligates an eventual
    // SCAN_COMPLETE, or WiFiManager's scan scheduler latches forever. The
    // watchdog is armed on the loop thread so its id is touched there only.
    loop()->runInLoop([this]() {
        if (!scan_pending_.load() || scan_watchdog_timer_ != kNoTimer)
            return;
        scan_watchdog_timer_ = loop()->setTimeout(scan_watchdog_ms_, [this](hv::TimerID) {
            scan_watchdog_timer_ = kNoTimer;
            spdlog::debug("[WifiBackendNetd] Scan watchdog fired after {} ms", scan_watchdog_ms_);
            finish_scan();
        });
    });
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendNetd::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::vector<WiFiNetwork> per_bss;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        per_bss.reserve(scan_rows_.size());
        for (const auto& row : scan_rows_) {
            per_bss.emplace_back(row.ssid, wifi_signal_percent_from_dbm(row.signal_dbm),
                                 row.secured, row.secured ? "PSK" : "", row.frequency_mhz);
        }
    }
    // One row per SSID: mesh and dual-band APs broadcast several BSSes, and
    // the picker shows the strongest BSS with the union of the bands.
    networks = wifi_merge_networks_by_ssid(per_bss);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendNetd::connect_network(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS, "SSID must not be empty",
                         "Enter a network name");
    }
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // Joining the network we are already on: the daemon accepts the bytes but
    // never processes a redundant CONNECT_WIFI, so the manager's connect
    // watchdog would be the only exit and the user would wait out a timeout
    // for a network they are on. The snapshot already answers the question —
    // resolve from it, nothing on the wire.
    helix::netd::NetdSnapshot current;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        current = snapshot_;
    }
    if (current.connected_state() && current.mode == "WIFI" && current.ssid == ssid) {
        spdlog::debug("[WifiBackendNetd] Join target is the current network; resolving from "
                      "the snapshot");
        connect_in_flight_.store(false);
        dispatch_event("CONNECTED", current.state);
        return WiFiErrorHelper::success();
    }

    // A new join attempt re-arms the auth latch: a fresh wrong password must
    // fire AUTH_FAILED again even if the previous attempt already had.
    auth_failure_latched_.store(false);
    connect_in_flight_.store(true);

    // Fire-and-forget: the daemon owns the join, and its verdict arrives as
    // CONNECTED / DISCONNECTED / AUTH_FAILED events (the ERR reasons map
    // through helix::netd::is_auth_failure_reason on the loop thread). The
    // caller never waits; NEVER auto-CANCEL.
    if (!send_line(helix::netd::encode_connect_wifi(ssid, password))) {
        connect_in_flight_.store(false);
        return WiFiErrorHelper::connection_failed(
            "netd connection is down; join was not requested");
    }
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendNetd::disconnect_network() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }
    connect_in_flight_.store(false);

    // Timeout-tolerant by design: CANCEL's ack can lag well past any window
    // (the daemon acks once the disconnect completes), so park no slot —
    // fire the line and return. This is the ONLY CANCEL sender.
    if (!write_line_raw(helix::netd::encode_cancel() + "\n")) {
        return WiFiErrorHelper::connection_failed(
            "netd connection is down; disconnect was not requested");
    }
    return WiFiErrorHelper::success();
}

WifiBackend::ConnectionStatus WifiBackendNetd::get_status() {
    ConnectionStatus status;
    helix::netd::NetdSnapshot snapshot;
    {
        // mac_address_ shares this lock: the loop thread can refresh it on a
        // CONNECTED transition while a reader is here.
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot = snapshot_;
        status.mac_address = mac_address_;
    }
    // Association counts even with an empty IP (wpa COMPLETED parity), and
    // ethernet mode is not "connected wifi". Reads never touch the socket.
    status.connected = snapshot.connected_state() && snapshot.mode == "WIFI";
    status.ssid = snapshot.ssid;
    status.ip_address = snapshot.ip;
    status.signal_strength = parse_signal_percent(snapshot.signal);
    status.frequency_mhz = 0; // the protocol carries no association frequency
    return status;
}

bool WifiBackendNetd::supports_5ghz() const {
    // Derived from evidence, not asserted: the moment any scan row lands on
    // the 5 GHz band, the radio demonstrably sees it. Until a scan has been
    // seen, report the conservative 2.4 GHz-only default for this hardware.
    return seen_5ghz_network_.load();
}

WiFiError WifiBackendNetd::set_radio_enabled(bool on) {
    (void)on;
    return WiFiError(WiFiResult::BACKEND_ERROR,
                     "set_radio_enabled not supported: the network daemon owns the radio",
                     "WiFi radio is managed by the printer's network daemon");
}

bool WifiBackendNetd::is_radio_enabled() const {
    // The daemon owns the radio; we never disable it, so report enabled.
    return true;
}

void WifiBackendNetd::read_mac_address_if_empty() {
    if (!mac_address_.empty())
        return;
    // The reader owns netdev resolution (sysfs probe in production, injected
    // stub in tests); empty means "no interface yet", which the CONNECTED
    // retry turns into a recoverable condition.
    const std::string mac = mac_reader_();
    if (mac.empty())
        return;
    // get_status() reads this from any thread; init and the loop-thread retry
    // are the only writers.
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    mac_address_ = mac;
    spdlog::trace("[WifiBackendNetd] Station address resolved: {}", mac_address_);
}

#endif // !__APPLE__ && !__ANDROID__
