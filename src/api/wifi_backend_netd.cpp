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

// Liveness watchdog constants (the first-party client's cadence): if an op
// is outstanding and the daemon says nothing for this long, ask for a
// snapshot; if even that draws no line for this much longer, the connection
// is wedged.
constexpr int kLivenessProbeMs = 20000;
constexpr int kLivenessGiveUpMs = 5000;

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

WifiBackendNetd::WifiBackendNetd(int reconnect_ms, int scan_watchdog_ms, int ack_wait_ms)
    : hv::EventLoopThread(nullptr), reconnect_ms_(reconnect_ms),
      scan_watchdog_ms_(scan_watchdog_ms), ack_wait_ms_(ack_wait_ms) {
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
    if (event_loop_active() && loop()) {
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

    hv::EventLoopThread::stop();
    hv::EventLoopThread::join();
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
                // start() failed before init_netd() ran, so no INIT_FAILED
                // was dispatched there; emit it here. When init_netd() did
                // run and fail it already dispatched — re-dispatching would
                // double-notify the manager.
                dispatch_event("INIT_FAILED", result.technical_msg);
            }
        });
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackendNetd] Failed to spawn init thread: {}", e.what());
        async_init_in_progress_.store(false);
        dispatch_event("INIT_FAILED", "system busy");
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

    if (!init_complete_.load()) {
        spdlog::trace("[WifiBackendNetd] stop(): never initialized");
        return;
    }

    spdlog::info("[WifiBackendNetd] Stopping (keeping the event loop alive)");

    init_succeeded_.store(false);
    init_complete_.store(false);

    // hio/timer manipulation MUST run on the loop thread. Schedule via
    // runInLoop with a shared-promise sync — the shared_ptr keeps the
    // promise alive if this wait times out and the deferred cleanup runs
    // later (the wpa backend's use-after-free fix).
    if (event_loop_active() && loop()) {
        auto cleanup_done = std::make_shared<std::promise<void>>();
        std::future<void> cleanup_future = cleanup_done->get_future();
        loop()->runInLoop([this, cleanup_done]() {
            cleanup_netd();
            cleanup_done->set_value();
        });
        if (cleanup_future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            spdlog::warn("[WifiBackendNetd] Cleanup timed out after 2 seconds");
        }
    } else {
        // Loop not running: no I/O callbacks can fire, cleanup directly.
        cleanup_netd();
    }

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
    read_mac_address_once();

    if (shutdown_requested_.load()) {
        signal_init_complete("shutdown requested during init");
        return;
    }

    std::string error;
    if (!open_connection(error)) {
        spdlog::warn("[WifiBackendNetd] Init failed: {}", error);
        dispatch_event("INIT_FAILED", error);
        signal_init_complete(error);
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
    scan_pending_.store(false);
    connect_in_flight_.store(false);

    // Dismiss any parked sender: its op reads as a timeout (accepted
    // semantics — losing the socket never cancels a daemon-owned op).
    std::shared_ptr<AckWait> slot;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        slot = pending_ack_;
        pending_ack_.reset();
        pending_cmd_.clear();
    }
    if (slot) {
        {
            std::lock_guard<std::mutex> lk(slot->mtx);
            slot->result = AckWait::Result::Dismissed;
        }
        slot->cv.notify_all();
    }

    close_connection();
    spdlog::debug("[WifiBackendNetd] Daemon connection cleaned up");
}

bool WifiBackendNetd::open_connection(std::string& error_out) {
    const std::string path = helix::netd::socket_path();

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

    // A blocking connect is bounded here: the peer is local, so connect()
    // returns immediately with a verdict (same trade the wpa backend makes
    // with wpa_ctrl_open2()).
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_out = "connect(\"" + path + "\"): " + std::strerror(errno);
        ::close(fd);
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    last_line_at_ = std::chrono::steady_clock::now();

    // Subscribe for pushes, then GET to seed the snapshot: the daemon only
    // pushes on CHANGE, so without the seed a connected printer shows blank
    // until the next state change.
    if (!write_line_raw(helix::netd::encode_subscribe() + "\n") ||
        !write_line_raw(helix::netd::encode_get() + "\n")) {
        error_out = std::string("handshake write failed: ") + std::strerror(errno);
        close_connection();
        return false;
    }
    return true;
}

void WifiBackendNetd::close_connection() {
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
        close_connection();
        if (want_connection_.load() && !shutdown_requested_.load()) {
            arm_reconnect_timer();
        }
        return;
    }

    for (const std::string& line :
         assembler_.feed(std::string_view(static_cast<const char*>(data), nbyte))) {
        handle_line(line);
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

    spdlog::debug("[WifiBackendNetd] Daemon connection lost");
    if (want_connection_.load() && !shutdown_requested_.load()) {
        arm_reconnect_timer();
    }
}

void WifiBackendNetd::handle_line(const std::string& line) {
    last_line_at_ = std::chrono::steady_clock::now();

    // Dispatch order per netd_protocol.h: ack -> scan row -> snapshot field
    // -> ignore.
    const helix::netd::Ack ack = helix::netd::parse_ack(line);
    if (ack.kind != helix::netd::Ack::Kind::None) {
        handle_ack(ack, resolve_pending(ack));
        return;
    }

    if (const auto row = helix::netd::parse_scan_row(line)) {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        scan_rows_.push_back(*row);
        return;
    }

    helix::netd::NetdSnapshot after;
    bool carried_field = false;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        carried_field = helix::netd::parse_snapshot_line(line, snapshot_);
        after = snapshot_;
    }
    if (carried_field) {
        handle_snapshot_diff(after);
    }
    // Anything else is a newer daemon's addition: ignored (forward compat).
}

std::string WifiBackendNetd::resolve_pending(const helix::netd::Ack& ack) {
    std::shared_ptr<AckWait> slot;
    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_ack_) {
            slot = pending_ack_;
            cmd = pending_cmd_;
            pending_ack_.reset();
            pending_cmd_.clear();
        }
    }
    if (slot) {
        {
            std::lock_guard<std::mutex> lk(slot->mtx);
            slot->result = ack.ok() ? AckWait::Result::Ok : AckWait::Result::Err;
            slot->text = ack.text;
        }
        slot->cv.notify_all();
    }
    return cmd;
}

void WifiBackendNetd::handle_ack(const helix::netd::Ack& ack, const std::string& pending_cmd) {
    const bool resolved = !pending_cmd.empty();

    if (ack.kind == helix::netd::Ack::Kind::Ok) {
        // An OK completing a scan ends it NOW, whether it resolved the parked
        // SCAN (rows precede the OK, so the results are already cached) or
        // arrived unsolicited after the slot timed out. Waiting out the
        // watchdog instead would add its full bound to every scan's latency;
        // the watchdog remains the backstop for a daemon that never acks.
        const bool completes_scan =
            scan_pending_.load() && (!resolved || pending_cmd == helix::netd::encode_scan());
        if (completes_scan) {
            finish_scan();
        }
        return;
    }

    // ERR: map by reason. The ack protocol carries no op id, so a parked
    // slot's command is the one disambiguation available; unsolicited errors
    // are attributed by reason alone.
    const std::string& reason = ack.text;
    const bool auth_failure =
        reason == "WRONG_KEY" || reason == "AUTH_FAILED" || reason == "INVALID_PSK";
    if (auth_failure) {
        // Terminal join failure: fire AUTH_FAILED once per failure, never per
        // RETRYING push or repeated error. The latch clears when a new join
        // is accepted or a CONNECTED lands.
        connect_in_flight_.store(false);
        if (!auth_failure_latched_.exchange(true)) {
            dispatch_event("AUTH_FAILED", reason);
        }
    } else if (connect_in_flight_.exchange(false)) {
        // Any other failure while a join is user-visible (parked or
        // unsolicited) ends that attempt as a disconnect.
        dispatch_event("DISCONNECTED", reason);
    }

    // An unsolicited ERR also closes out an accepted-but-unacked scan: a
    // successful trigger_scan() obligates an eventual SCAN_COMPLETE.
    if (!resolved && scan_pending_.load()) {
        finish_scan();
    }
}

void WifiBackendNetd::handle_snapshot_diff(const helix::netd::NetdSnapshot& after) {
    const bool now_connected = after.connected_state() && after.mode == "WIFI";
    if (now_connected && !was_connected_) {
        // Association counts even before an address arrives (wpa COMPLETED
        // parity). Never fire CONNECTED for mode==ETHERNET.
        auth_failure_latched_.store(false);
        connect_in_flight_.store(false);
        dispatch_event("CONNECTED", after.state);
    } else if (!now_connected && was_connected_ &&
               (after.state == "DISCONNECTED" || after.state == "OFFLINE" ||
                after.state == "CANCELLED")) {
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

bool WifiBackendNetd::write_line_raw(const std::string& line) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    if (io_ == nullptr || fd_ < 0)
        return false;

    const char* data = line.data();
    size_t remaining = line.size();
    int waits = 0;
    while (remaining > 0) {
        const ssize_t n = ::send(fd_, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits < 20) {
            // Bounded wait for buffer space: a wedged peer must not pin the
            // caller (or the loop thread) forever. 20 x 100 ms is the cap.
            pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            ++waits;
            if (::poll(&pfd, 1, 100) > 0)
                continue;
        }
        spdlog::debug("[WifiBackendNetd] Daemon write failed: {}", std::strerror(errno));
        return false;
    }
    return true;
}

WifiBackendNetd::SendOutcome WifiBackendNetd::send_request(const std::string& cmd) {
    spdlog::debug("[WifiBackendNetd] Sending '{}'", command_name(cmd));

    auto slot = std::make_shared<AckWait>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        // One slot: parking a second op displaces the first, whose waiter
        // reads Dismissed and treats its op as accepted — the same semantics
        // as its own timeout.
        pending_ack_ = slot;
        pending_cmd_ = cmd;
    }

    if (!write_line_raw(cmd + "\n")) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_ack_ == slot) {
            pending_ack_.reset();
            pending_cmd_.clear();
        }
        return SendOutcome{SendOutcome::Kind::NotSent, {}};
    }

    arm_liveness_probe();

    // Early-reject window ONLY: OK/ERR here are operation COMPLETION acks,
    // so a timeout means "accepted, the daemon owns the rest" — the late ack
    // arrives unsolicited and drives events.
    std::unique_lock<std::mutex> lock(slot->mtx);
    const bool resolved = slot->cv.wait_for(lock, std::chrono::milliseconds(ack_wait_ms_), [&slot] {
        return slot->result != AckWait::Result::Waiting;
    });
    if (!resolved)
        return SendOutcome{SendOutcome::Kind::Timeout, {}};
    switch (slot->result) {
    case AckWait::Result::Ok:
        return SendOutcome{SendOutcome::Kind::Ok, slot->text};
    case AckWait::Result::Err:
        return SendOutcome{SendOutcome::Kind::Err, slot->text};
    case AckWait::Result::Dismissed:
    case AckWait::Result::Waiting:
        break;
    }
    return SendOutcome{SendOutcome::Kind::Timeout, {}};
}

void WifiBackendNetd::arm_liveness_probe() {
    loop()->setTimeout(kLivenessProbeMs, [this](hv::TimerID) {
        if (shutdown_requested_.load() || io_ == nullptr)
            return;
        if (std::chrono::steady_clock::now() - last_line_at_ <
            std::chrono::milliseconds(kLivenessProbeMs))
            return;
        spdlog::debug("[WifiBackendNetd] Daemon silent while an op is pending; sending GET");
        (void)write_line_raw(helix::netd::encode_get() + "\n");
        loop()->setTimeout(kLivenessGiveUpMs, [this](hv::TimerID) {
            if (shutdown_requested_.load() || io_ == nullptr)
                return;
            if (std::chrono::steady_clock::now() - last_line_at_ <
                std::chrono::milliseconds(kLivenessProbeMs + kLivenessGiveUpMs))
                return;
            spdlog::warn("[WifiBackendNetd] Daemon unresponsive; forcing reconnect");
            close_connection();
            arm_reconnect_timer();
        });
    });
}

// ============================================================================
// WifiBackend Interface Implementation
// ============================================================================

WiFiError WifiBackendNetd::trigger_scan() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // Clear the row cache as the request goes out: the daemon sends a scan's
    // rows BEFORE the OK that completes it, so clearing after the ack would
    // wipe the very rows that ack completes. A refused scan leaving the cache
    // empty is correct (nothing new was found).
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        scan_rows_.clear();
    }
    scan_pending_.store(true);

    const SendOutcome outcome = send_request(helix::netd::encode_scan());
    switch (outcome.kind) {
    case SendOutcome::Kind::NotSent:
        scan_pending_.store(false);
        return WiFiErrorHelper::connection_failed(
            "netd connection is down; scan was not requested");
    case SendOutcome::Kind::Err:
        scan_pending_.store(false);
        // SCAN_FAILED / BUSY / SCAN_IN_PROGRESS all reject synchronously;
        // the manager resolves this through its failure path.
        return WiFiError(WiFiResult::BACKEND_ERROR, "SCAN refused: " + outcome.text,
                         "Failed to start network scan",
                         "Check the printer's network service and try again");
    case SendOutcome::Kind::Ok:
    case SendOutcome::Kind::Timeout:
        break;
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

    // A new join attempt re-arms the auth latch: a fresh wrong password must
    // fire AUTH_FAILED again even if the previous attempt already had.
    auth_failure_latched_.store(false);
    connect_in_flight_.store(true);

    const SendOutcome outcome = send_request(helix::netd::encode_connect_wifi(ssid, password));
    switch (outcome.kind) {
    case SendOutcome::Kind::NotSent:
        connect_in_flight_.store(false);
        return WiFiErrorHelper::connection_failed(
            "netd connection is down; join was not requested");
    case SendOutcome::Kind::Err: {
        // The AUTH_FAILED/DISCONNECTED events for this failure are fired from
        // the loop thread by handle_ack(); this maps the synchronous return.
        const std::string& reason = outcome.text;
        if (reason == "WRONG_KEY" || reason == "AUTH_FAILED" || reason == "INVALID_PSK")
            return WiFiErrorHelper::authentication_failed(ssid);
        if (reason == "NOT_FOUND")
            return WiFiErrorHelper::network_not_found(ssid);
        return WiFiErrorHelper::connection_failed("netd refused the join: " + reason);
    }
    case SendOutcome::Kind::Ok:
    case SendOutcome::Kind::Timeout:
        // Accepted: the daemon owns the join; its outcome arrives as
        // CONNECTED / DISCONNECTED / AUTH_FAILED events. NEVER auto-CANCEL.
        break;
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
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot = snapshot_;
    }
    // Association counts even with an empty IP (wpa COMPLETED parity), and
    // ethernet mode is not "connected wifi". Reads never touch the socket.
    status.connected = snapshot.connected_state() && snapshot.mode == "WIFI";
    status.ssid = snapshot.ssid;
    status.ip_address = snapshot.ip;
    status.signal_strength = parse_signal_percent(snapshot.signal);
    status.mac_address = mac_address_;
    status.frequency_mhz = 0; // the protocol carries no association frequency
    return status;
}

bool WifiBackendNetd::supports_5ghz() const {
    // This backend drives a 2.4 GHz-only radio; the picker shows the
    // "only 2.4 GHz networks" hint.
    return false;
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

void WifiBackendNetd::read_mac_address_once() {
    if (!mac_address_.empty())
        return;
    // Resolve the netdev through the sysfs probe — never a hardcoded name;
    // iface is filled even when the link is down.
    const helix::ui::wifi::OsWifiLink link = helix::ui::wifi::probe_os_wifi_link();
    if (link.iface.empty())
        return;
    std::ifstream file("/sys/class/net/" + link.iface + "/address");
    std::string mac;
    if (!file.is_open() || !std::getline(file, mac))
        return;
    while (!mac.empty() && std::isspace(static_cast<unsigned char>(mac.back())))
        mac.pop_back();
    mac_address_ = mac;
    spdlog::trace("[WifiBackendNetd] Station address from {}: {}", link.iface, mac_address_);
}

#endif // !__APPLE__ && !__ANDROID__
