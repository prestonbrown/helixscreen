// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file moonraker_client.cpp
 * @brief WebSocket client for Moonraker printer API communication
 *
 * @pattern libhv WebSocketClient with atomic state machine
 * @threading Callbacks run on libhv event loop thread - use helix::ui::async_call() for LVGL
 * @gotchas is_destroying_ flag blocks callbacks during destruction; skip cleanup during static
 * destruction
 *
 * @see moonraker_manager.cpp, printer_state.cpp
 */

#include "moonraker_client.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "callback_drain.h"
#include "helix_version.h"
#include "host_identity.h"
#include "printer_state.h"
#include "system/telemetry_manager.h"

using namespace helix;

using namespace hv;

// Anonymous namespace for file-scoped state
namespace {
// Rate limiting flags for reconnection notifications
std::atomic<bool> g_already_notified_max_attempts{false};
std::atomic<bool> g_already_notified_disconnect{false};

// How long disconnect() waits for in-flight callbacks to drain before giving up.
// Generous enough that a merely slow callback still wins the race, short enough
// that a wedged one costs a visible stutter instead of a dead touchscreen.
constexpr std::chrono::milliseconds CALLBACK_DRAIN_TIMEOUT{3000};

// "ws://192.168.1.171:7125/websocket" -> "192.168.1.171:7125". The bare
// host:port is what a user can act on: compare it against the printer, or find
// it under Settings > System > Printer Host. Falls back to the input unchanged
// if it does not look like a URL.
std::string format_ws_endpoint(const std::string& url) {
    std::string s = url;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        s.erase(0, scheme + 3);
    }
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        s.erase(slash);
    }
    return s.empty() ? url : s;
}

// "127.0.0.1:7125" -> "127.0.0.1". Handles the bracketed IPv6 literal form
// ("[::1]:7125") so a v6 loopback is not mistaken for a remote host, and leaves
// a bare v6 address (no brackets, no port) alone rather than truncating it at
// its first colon.
std::string host_of_endpoint(const std::string& endpoint) {
    if (!endpoint.empty() && endpoint.front() == '[') {
        const size_t close = endpoint.find(']');
        return (close != std::string::npos) ? endpoint.substr(1, close - 1) : endpoint;
    }
    const size_t colon = endpoint.find(':');
    if (colon == std::string::npos)
        return endpoint;
    // More than one colon and no brackets: an unbracketed IPv6 literal.
    if (endpoint.find(':', colon + 1) != std::string::npos)
        return endpoint;
    return endpoint.substr(0, colon);
}

// Reset notification flags on successful connection
void reset_notification_flags() {
    g_already_notified_max_attempts.store(false);
    g_already_notified_disconnect.store(false);
}

} // namespace

MoonrakerClient::MoonrakerClient(EventLoopPtr loop)
    : WebSocketClient(loop), discovery_(*this), was_connected_(false),
      connection_state_(ConnectionState::DISCONNECTED),
      connection_timeout_ms_(10000) // Default 10 seconds
      ,
      keepalive_interval_ms_(10000) // Default 10 seconds
      ,
      reconnect_min_delay_ms_(200) // Default 200ms
      ,
      reconnect_max_delay_ms_(2000) { // Default 2 seconds
}

void MoonrakerClient::start_health_timer() {
    if (health_timer_id_ != 0) {
        return; // Already running
    }
    auto l = loop();
    if (!l) {
        return;
    }
    health_timer_id_ = l->setInterval(HEALTH_CHECK_INTERVAL_MS, [this](hv::TimerID) {
        // Acquire shared lock to prevent destructor from proceeding while we execute.
        // try_to_lock ensures we never block if destructor holds the exclusive lock.
        // Without this lock, the destructor can destroy members (logger, tracker_)
        // while this callback is mid-execution — SIGBUS after long uptimes. (#717)
        std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
        if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire)) {
            return;
        }

        // Check pending request timeouts
        process_timeouts();

        // Check for stalled reconnection
        ConnectionState state = connection_state_.load();
        if (state == ConnectionState::RECONNECTING) {
            auto elapsed = steady_ms_since(reconnect_started_at_.load(std::memory_order_relaxed));
            if (elapsed > MAX_RECONNECT_STALL_MS) {
                spdlog::error("[Moonraker Client] Reconnection stalled for {}ms, giving up",
                              elapsed);
                set_connection_state(ConnectionState::FAILED);
                emit_event(MoonrakerEventType::CONNECTION_FAILED,
                           "Unable to reach printer. Check power and network connection.", true);
            }
        }
    });
    spdlog::debug("[Moonraker Client] Health check timer started ({}ms interval)",
                  HEALTH_CHECK_INTERVAL_MS);
}

void MoonrakerClient::stop_health_timer() {
    if (health_timer_id_ == 0) {
        return;
    }
    auto l = loop();
    if (l) {
        l->killTimer(health_timer_id_);
    }
    health_timer_id_ = 0;
    spdlog::debug("[Moonraker Client] Health check timer stopped");
}

MoonrakerClient::~MoonrakerClient() {
    // Set destroying flag FIRST so callbacks see it immediately
    is_destroying_.store(true, std::memory_order_release);

    // Stop health timer before waiting for callbacks (timer fires on event loop thread)
    stop_health_timer();

    // Wait for any in-flight callbacks to finish. Callbacks hold a shared lock;
    // acquiring an exclusive lock here blocks until all shared locks are released.
    // Once acquired, no new callback can start (try_to_lock will fail).
    {
        std::unique_lock<std::shared_mutex> lk(callback_lifecycle_mutex_);
    } // Release immediately — just needed to wait for in-flight callbacks

    // Now safe to reset lifetime guards (no callbacks can be mid-execution).
    // destruction_guard_ signals object destruction to SubscriptionGuard and
    // other external holders; lifetime_guard_ covers WS callbacks.
    lifetime_guard_.reset();
    destruction_guard_.reset();

    // Disable auto-reconnect BEFORE closing - prevents libhv from attempting
    // reconnection after we've started destruction (avoids stderr "No route to host")
    setReconnect(nullptr);

    // NOTE: We intentionally do NOT reassign onopen/onmessage/onclose to no-ops here.
    // Reassigning the inherited std::function members while the libhv event-loop thread
    // may be mid-invoke frees the running lambda's storage → UAF (the very bug this fix
    // addresses). The install-once trampolines self-cancel safely: destruction_guard_ was
    // reset above (before the base hv::WebSocketClient destructor runs), so any callback
    // firing during teardown sees dg.expired() and bails before touching `this`.

    // Under state_callback_mutex_ like every other write. Being in the destructor
    // is not an excuse to skip it: set_connection_state() copies this member on
    // the libhv event loop thread, and that thread is still alive here — the base
    // hv::WebSocketClient destructor, which stops the loop, has not run yet.
    // Holding the lock cannot deadlock, since the reader only copies under it and
    // invokes the callback after releasing.
    {
        std::lock_guard<std::mutex> lock(state_callback_mutex_);
        state_change_callback_ = nullptr;
    }

    // Pending requests are dropped without invoking error callbacks.
    // During destruction, callback targets (UI panels, file providers, etc.) may
    // already be destroyed — invoking them would be use-after-free.
    // The tracker_'s default destructor will clear the map automatically.

    // Clear method callbacks safely. Lambdas in this map may capture shared_ptrs
    // to objects (e.g. NoiseCheckCollector) whose destructors call
    // unregister_method_callback(). By moving the map to a local first, the member
    // is empty when those destructors fire, so unregister finds nothing and returns
    // harmlessly. The mutex is safe here since we're on the main thread and not
    // in static destruction (we got here through normal Application::shutdown()).
    if (callbacks_mutex_.try_lock()) {
        decltype(method_callbacks_) doomed_callbacks = std::move(method_callbacks_);
        method_callbacks_.clear();
        callbacks_mutex_.unlock();
        // doomed_callbacks destructs here - lambda destructors may call unregister,
        // but method_callbacks_ is now empty so they'll no-op
    }
}

void MoonrakerClient::set_last_url(const std::string& url) {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    last_url_ = url;
}

void MoonrakerClient::set_connection_state(ConnectionState new_state) {
    ConnectionState old_state = connection_state_.exchange(new_state);

    if (old_state != new_state) {
        const char* state_names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "RECONNECTING",
                                     "FAILED"};
        spdlog::debug("[Moonraker Client] Connection state: {} -> {}",
                      state_names[static_cast<int>(old_state)],
                      state_names[static_cast<int>(new_state)]);

        // Handle state-specific logic
        if (new_state == ConnectionState::RECONNECTING) {
            if (old_state != ConnectionState::RECONNECTING) {
                reconnect_started_at_.store(steady_ticks_now(), std::memory_order_relaxed);
            }
            reconnect_attempts_++;
            if (max_reconnect_attempts_ > 0 && reconnect_attempts_ >= max_reconnect_attempts_) {
                spdlog::error("[Moonraker Client] Max reconnect attempts ({}) exceeded",
                              max_reconnect_attempts_);
                TelemetryManager::instance().record_error(
                    "websocket", "reconnect_failed",
                    fmt::format("max attempts ({}) exceeded", max_reconnect_attempts_));

                // Emit event only once during reconnect sequence
                if (!g_already_notified_max_attempts.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_FAILED,
                               fmt::format("Unable to reach printer after {} attempts. "
                                           "Check power and network connection.",
                                           max_reconnect_attempts_),
                               true);
                    g_already_notified_max_attempts.store(true);
                }

                set_connection_state(ConnectionState::FAILED);
                return;
            }
        } else if (new_state == ConnectionState::CONNECTED) {
            reconnect_attempts_ = 0; // Reset on successful connection
        }

        // Copy callback under lock to prevent race with destructor clearing it
        // We invoke OUTSIDE the lock so we don't hold mutex during LVGL operations
        std::function<void(ConnectionState, ConnectionState)> callback_copy;
        {
            std::lock_guard<std::mutex> lock(state_callback_mutex_);
            if (state_change_callback_ && !is_destroying_.load()) {
                callback_copy = state_change_callback_;
            }
        }

        // Double-check is_destroying_ AFTER releasing lock but BEFORE invoking callback
        // This catches the race where destructor set the flag between our copy and invocation
        if (callback_copy && !is_destroying_.load()) {
            try {
                callback_copy(old_state, new_state);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] State change callback threw exception: {}",
                                   e.what());
            } catch (...) {
                LOG_ERROR_INTERNAL(
                    "[Moonraker Client] State change callback threw unknown exception");
            }
        }
    }
}

void MoonrakerClient::disconnect() {
    // Serialize with connect() — both modify callbacks and call close()
    std::lock_guard<std::recursive_mutex> connect_lock(connect_mutex_);

    ConnectionState current_state = connection_state_.load();

    // Only log if we're actually connected/connecting
    if (current_state != ConnectionState::DISCONNECTED &&
        current_state != ConnectionState::FAILED) {
        spdlog::debug("[Moonraker Client] Disconnecting from WebSocket server");
    }

    // Arm a short suppression window so the close that THIS intentional disconnect
    // triggers does not produce a "connection lost / reconnecting" toast. on_ws_close()
    // checks is_disconnect_modal_suppressed() to gate the reconnect side-effects. This
    // replicates the old lifetime_guard_-reset suppression for the common case.
    suppress_disconnect_modal(2000);

    // Disable auto-reconnect BEFORE invalidation to prevent spurious reconnection
    setReconnect(nullptr);

    // Invalidate lifetime guard so any in-flight or future callbacks on the event loop
    // thread will see weak_guard.lock() fail and early-return. Then create a fresh guard
    // for the next connect() call.
    // NOTE: Do NOT replace onopen/onmessage/onclose with no-op lambdas here — that's a
    // data race with the event loop thread which may be mid-call on the std::function.
    // The invalidated weak_ptr is the safe cancellation mechanism.
    lifetime_guard_.reset();
    lifetime_guard_ = std::make_shared<bool>(true);

    // Wait for any in-flight callbacks to finish before we modify shared state.
    // Callbacks hold a shared lock; acquiring exclusive waits until they complete.
    //
    // Bounded, unlike the destructor's drain. Callers reach this from the UI
    // thread (printer switch, wizard, force_reconnect), and blocking that thread
    // forever is a lit screen that ignores touch — a failure the watchdog cannot
    // even see, because the process is still alive and merely parked in
    // pthread_rwlock_wrlock.
    //
    // It is also the backstop for re-entry. A caller that already holds this
    // mutex shared and then reaches disconnect() self-deadlocks outright, since
    // std::shared_mutex is neither recursive nor upgradeable. on_ws_message()
    // used to do exactly that on an oversized frame and now defers instead, but
    // the bound is what keeps the next such caller to a logged stall rather than
    // a dead event loop.
    //
    // A callback still running after this long is wedged, not slow, and blocking
    // on it forever is the worse failure either way.
    //
    // Proceeding after a timeout does race that callback. lifetime_guard_ was
    // already invalidated above, so it early-returns at its next guard check, but
    // the window is real — hence the error log rather than a silent carry-on.
    if (!helix::drain_shared_holders(callback_lifecycle_mutex_, CALLBACK_DRAIN_TIMEOUT)) {
        spdlog::error("[Moonraker Client] In-flight callbacks still running after {}ms — "
                      "proceeding with disconnect anyway",
                      CALLBACK_DRAIN_TIMEOUT.count());
    }

    // Now safe to stop timer and close — no callbacks can restart the timer or
    // access our state because the lifetime guard is invalidated.
    stop_health_timer();
    close();

    // Clean up any pending requests (invokes error callbacks)
    tracker_.cleanup_all();

    // Reset discovery state for next connection
    discovery_.reset_identified();
    discovery_.reset_completion();

    // Reset connection state
    set_connection_state(ConnectionState::DISCONNECTED);
    reconnect_attempts_ = 0;
}

void MoonrakerClient::force_reconnect() {
    spdlog::info("[Moonraker Client] Force reconnect requested - full state reset");

    // Copy stored connection info under lock
    std::string url;
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void()> on_discovery_complete;
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        url = last_url_;
        on_connected = last_on_connected_;
        on_disconnected = last_on_disconnected_;
        on_discovery_complete = last_discovery_complete_;
    }

    // Verify we have stored connection info
    if (url.empty()) {
        spdlog::warn(
            "[Moonraker Client] force_reconnect() called but no previous connection info - "
            "call connect() first");
        return;
    }

    // 1. Disconnect cleanly (clears pending requests, resets state)
    disconnect();

    // 2. Connect using stored URL and callbacks
    int result = connect(url.c_str(), on_connected, on_disconnected);
    if (result != 0) {
        spdlog::error("[Moonraker Client] force_reconnect() connect failed: {}", result);
        return;
    }

    // 3. Re-run discovery if we have a stored callback
    //    Note: discover_printer() is typically called in on_connected callback,
    //    so it will be triggered automatically. But if the caller wants explicit
    //    discovery, we provide the mechanism.
    spdlog::debug("[Moonraker Client] force_reconnect() complete - connection initiated");
}

int MoonrakerClient::connect(const char* url, std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    // Serialize concurrent connect() calls — the close/callback-setup/open sequence
    // is not atomic and concurrent callers race on libhv internal state.
    std::lock_guard<std::recursive_mutex> connect_lock(connect_mutex_);

    // Reset WebSocket state from previous connection attempt BEFORE setting new callbacks.
    // This prevents libhv from rejecting the new open() call if we're already connecting/connected.
    // Note: close() is safe to call even if already closed (idempotent).
    close();

    // Apply connection timeout to libhv (must be called before open())
    setConnectTimeout(static_cast<int>(connection_timeout_ms_));

    spdlog::debug("[Moonraker Client] WebSocket connecting to {}", url);
    set_connection_state(ConnectionState::CONNECTING);
    connection_generation_.fetch_add(1);

    // Anchor the initial-connection window. libhv's auto-reconnect re-enters its
    // own startConnect(), never this function, so this timestamp stays put for
    // the whole never-connected streak — which is exactly what we want to
    // measure. A fresh connect() (new host, wizard, force_reconnect) re-arms it.
    connect_started_at_.store(steady_ticks_now(), std::memory_order_relaxed);
    initial_failure_notified_.store(false, std::memory_order_relaxed);

    // Install the inherited libhv callbacks exactly ONCE. Reassigning onopen/onmessage/
    // onclose per connect() races the libhv event-loop thread, which may be mid-invoke on
    // the old std::function — freeing its heap storage under the running lambda → UAF
    // (bundle UK9QCFY3). The install-once trampolines forward to on_ws_open/on_ws_message/
    // on_ws_close, which read per-connect state from last_url_/last_on_connected_/
    // last_on_disconnected_ (stored below under reconnect_mutex_) instead of captures.
    // Guarded by the already-held connect_mutex_.
    if (!ws_callbacks_installed_) {
        install_ws_callbacks();
        ws_callbacks_installed_ = true;
    }

    // WebSocket ping (keepalive) - use configured interval
    setPingInterval(static_cast<int>(keepalive_interval_ms_));

    // Automatic reconnection with exponential backoff - use configured values
    apply_reconnect_settings();

    // Store connection info for force_reconnect() AND for the install-once trampolines,
    // which snapshot these (under reconnect_mutex_) on every invocation rather than
    // capturing the per-connect callbacks/url by value.
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_url_ = url ? url : "";
        last_on_connected_ = on_connected;
        last_on_disconnected_ = on_disconnected;
    }

    // Connect
    http_headers headers;
    headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
    return open(url, headers);
}

void MoonrakerClient::apply_reconnect_settings() {
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = reconnect_min_delay_ms_;
    reconn.max_delay = reconnect_max_delay_ms_;
    reconn.delay_policy = 2; // Exponential backoff
    setReconnect(&reconn);
}

void MoonrakerClient::set_auto_reconnect(bool enabled) {
    if (enabled) {
        apply_reconnect_settings();
    } else {
        setReconnect(nullptr);
    }
}

void MoonrakerClient::install_ws_callbacks() {
    // Each trampoline captures ONLY [this, dg] — a weak_ptr to destruction_guard_, which
    // is reset only in the destructor. CRITICAL ordering: dg.lock() is checked FIRST,
    // before any `this` deref, so a callback firing during the hv::WebSocketClient base-class
    // destructor (after destruction_guard_.reset()) bails without touching destroyed members.
    onopen = [this, dg = std::weak_ptr<bool>(destruction_guard_)]() {
        // Liveness check that never dereferences `this`: a null lock() means the destructor
        // already ran (it resets destruction_guard_ before the base-class dtor). See above.
        auto live = dg.lock();
        if (!live)
            return;
        try {
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire))
                return;
            on_ws_open();
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen trampoline threw: {}", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen trampoline threw unknown");
        }
    };

    onmessage = [this, dg = std::weak_ptr<bool>(destruction_guard_)](const std::string& msg) {
        // Liveness check that never dereferences `this`: a null lock() means the destructor
        // already ran (it resets destruction_guard_ before the base-class dtor). See above.
        auto live = dg.lock();
        if (!live)
            return;
        try {
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire))
                return;
            on_ws_message(msg);
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onmessage trampoline threw: {}", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onmessage trampoline threw unknown");
        }
    };

    onclose = [this, dg = std::weak_ptr<bool>(destruction_guard_)]() {
        // Liveness check that never dereferences `this`: a null lock() means the destructor
        // already ran (it resets destruction_guard_ before the base-class dtor). See above.
        auto live = dg.lock();
        if (!live)
            return;
        try {
            std::shared_lock<std::shared_mutex> lk(callback_lifecycle_mutex_, std::try_to_lock);
            if (!lk.owns_lock() || is_destroying_.load(std::memory_order_acquire))
                return;
            on_ws_close();
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose trampoline threw: {}", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose trampoline threw unknown");
        }
    };
}

void MoonrakerClient::on_ws_open() {
    // Snapshot per-connect callback + url; the install-once trampoline no longer captures them.
    std::function<void()> on_connected;
    std::string url_owned;
    {
        std::lock_guard<std::mutex> lk(reconnect_mutex_);
        on_connected = last_on_connected_;
        url_owned = last_url_;
    }

    // Note: getHttpResponse() available here if needed for upgrade response inspection
    spdlog::debug("[Moonraker Client] WebSocket connected to {}", url_owned);

    // Check if this is a reconnection (was_connected_ is true from previous session)
    // Emit RECONNECTED event BEFORE updating was_connected_
    if (was_connected_.load()) {
        emit_event(MoonrakerEventType::RECONNECTED, "Connection restored", false);
    }

    was_connected_ = true;
    set_connection_state(ConnectionState::CONNECTED);

    // Start periodic health checks (timeout detection, reconnect staleness)
    start_health_timer();

    // Reset notification flags on successful connection
    reset_notification_flags();
    // Re-arm the initial-connection escalation. A session that came up and later
    // dies is the health timer's problem, but if this client is pointed at a new
    // host that never answers, that streak deserves its own notification.
    initial_failure_notified_.store(false, std::memory_order_relaxed);

    invoke_connected_callback(on_connected, "WebSocket opened");
}

void MoonrakerClient::on_ws_message(const std::string& msg) {
    // Snapshot per-connect callbacks; the install-once trampoline no longer captures them.
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    {
        std::lock_guard<std::mutex> lk(reconnect_mutex_);
        on_connected = last_on_connected_;
        on_disconnected = last_on_disconnected_;
    }

    // DEBUG: Log every raw message received to diagnose AD5M WebSocket issue
    spdlog::trace("[Moonraker Client] onmessage received {} bytes", msg.size());

    try {
        // Validate message size to prevent memory exhaustion
        static constexpr size_t MAX_MESSAGE_SIZE = 5 * 1024 * 1024; // 5 MB
        if (msg.size() > MAX_MESSAGE_SIZE) {
            spdlog::error("[Moonraker Client] Message too large: {} bytes (max: {})", msg.size(),
                          MAX_MESSAGE_SIZE);

            // Emit event - this indicates a protocol problem
            emit_event(MoonrakerEventType::MESSAGE_OVERSIZED,
                       fmt::format("Received oversized data from printer ({} bytes). "
                                   "This may indicate a communication error.",
                                   msg.size()),
                       true);

            // Deferred, not inline. disconnect() takes callback_lifecycle_mutex_
            // exclusively, and the onmessage trampoline that called us still holds
            // that same mutex SHARED on this very thread. std::shared_mutex is
            // neither recursive nor upgradeable, so acquiring exclusive here is an
            // unconditional self-deadlock of the event loop — the socket stops
            // being serviced and every later request times out with no clue why.
            //
            // queueInLoop() rather than runInLoop(): the latter runs inline when
            // already on the loop thread, which is exactly the case here and would
            // reproduce the deadlock. Queuing lands it on the next loop iteration,
            // after the trampoline has returned and dropped its shared lock.
            if (auto l = loop()) {
                l->queueInLoop([this, dg = std::weak_ptr<bool>(destruction_guard_)]() {
                    // Same liveness contract as the trampolines: a null lock() means
                    // the destructor already ran, so never touch `this`.
                    auto live = dg.lock();
                    if (!live || is_destroying_.load(std::memory_order_acquire)) {
                        return;
                    }
                    disconnect();
                });
            }
            return;
        }

        // Check for timed out requests on each message (opportunistic cleanup)
        process_timeouts();

        // DEBUG: Log large messages to help diagnose history issue
        if (msg.size() > 50000) {
            spdlog::debug("[Moonraker Client] Received large message: {} bytes", msg.size());
        }

        // Parse JSON message
        json j;
        try {
            j = json::parse(msg);
        } catch (const json::parse_error& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] JSON parse error: {}", e.what());
            TelemetryManager::instance().record_error("websocket", "parse_error",
                                                      fmt::format("JSON parse: {}", e.what()));
            return;
        }

        // Route responses with request IDs through the tracker
        if (j.contains("id")) {
            tracker_.route_response(
                j,
                [this](MoonrakerEventType type, const std::string& msg_str, bool is_error,
                       const std::string& details) {
                    emit_event(type, msg_str, is_error, details);
                },
                []() { return AbortManager::instance().is_handling_shutdown(); });
        }

        // Handle notifications (no request ID)
        if (j.contains("method")) {
            // Validate 'method' field type
            if (!j["method"].is_string()) {
                LOG_ERROR_INTERNAL("[Moonraker Client] Invalid 'method' type in notification: {}",
                                   j["method"].type_name());
                return;
            }

            std::string method = j["method"].get<std::string>();

            // Copy callbacks to invoke (to avoid holding lock during callback execution)
            std::vector<std::function<void(const json&)>> callbacks_to_invoke;

            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);

                // Printer status updates (most common)
                if (method == "notify_status_update" || method == "notify_filelist_changed") {
                    // Copy all notify callbacks from map
                    callbacks_to_invoke.reserve(notify_callbacks_.size());
                    for (const auto& [id, cb] : notify_callbacks_) {
                        callbacks_to_invoke.push_back(cb);
                    }
                }

                // Method-specific persistent callbacks
                auto method_it = method_callbacks_.find(method);
                if (method_it != method_callbacks_.end()) {
                    for (auto& [handler_name, cb] : method_it->second) {
                        callbacks_to_invoke.push_back(cb);
                    }
                }
            } // Release lock

            // Parse bed mesh updates before invoking user callbacks
            if (method == "notify_status_update" && j.contains("params") &&
                j["params"].is_array() && !j["params"].empty()) {
                const json& params = j["params"][0];
                if (params.contains("bed_mesh") && params["bed_mesh"].is_object()) {
                    parse_bed_mesh(params["bed_mesh"]);
                }
            }

            // Invoke callbacks outside lock to prevent deadlock
            for (auto& cb : callbacks_to_invoke) {
                // Defense-in-depth: a moved-from or empty std::function in the
                // registration map would SIGSEGV on invocation (#765 class).
                if (!cb)
                    continue;
                try {
                    cb(j);
                } catch (const std::exception& e) {
                    LOG_ERROR_INTERNAL("[Moonraker Client] Callback for {} threw exception: {}",
                                       method, e.what());
                } catch (...) {
                    LOG_ERROR_INTERNAL("[Moonraker Client] Callback for {} threw unknown exception",
                                       method);
                }
            }

            // Klippy disconnected from Moonraker
            if (method == "notify_klippy_disconnected") {
                spdlog::warn("[Moonraker Client] Klipper disconnected from Moonraker");

                // Update klippy state in PrinterState (SHUTDOWN = firmware disconnected)
                get_printer_state().set_klippy_state(KlippyState::SHUTDOWN);

                // Clear pending requests — Klippy can't process them anymore
                tracker_.cleanup_all();

                // Emit event for UI layer to handle
                emit_event(MoonrakerEventType::KLIPPY_DISCONNECTED,
                           "Klipper has disconnected from Moonraker. Check for errors in your "
                           "printer interface.",
                           true);

                // Invoke user callback with exception safety
                if (on_disconnected) {
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL("[Moonraker Client] Disconnection callback threw "
                                           "unknown exception");
                    }
                }
            }
            // Klippy entered shutdown state (M112, thermal runaway, config error)
            // Distinct from disconnect — Klipper is still running but in shutdown mode
            else if (method == "notify_klippy_shutdown") {
                spdlog::warn("[Moonraker Client] Klipper entered shutdown state");

                helix::ui::queue_update("MoonrakerClient::notify_klippy_shutdown", []() {
                    get_printer_state().set_klippy_state_sync(KlippyState::SHUTDOWN);
                });

                // Emit event for UI layer — recovery dialog will show
                emit_event(MoonrakerEventType::KLIPPY_SHUTDOWN,
                           "Klipper has entered shutdown state.", true);

                // Shutdown is a valid gate state for discovery. If we never
                // completed one on this connection (Klippy was unreachable
                // at WS-connect time), retry now (#802).
                if (!discovery_.is_completed()) {
                    spdlog::info("[Moonraker Client] Retrying discovery after Klippy shutdown");
                    invoke_connected_callback(on_connected, "Klippy shutdown");
                }
            }
            // Klippy reconnected to Moonraker
            else if (method == "notify_klippy_ready") {
                spdlog::info("[Moonraker Client] Klipper ready");

                helix::ui::queue_update("MoonrakerClient::notify_klippy_ready", []() {
                    get_printer_state().set_klippy_state_sync(KlippyState::READY);
                });

                // The event drives internal consumers (rediscovery below,
                // connected observers) and routes to Ignore in the UI layer;
                // the user-facing ready signal is the klippy_state observer
                // in ui_emergency_stop.cpp.
                emit_event(MoonrakerEventType::KLIPPY_READY, "Klipper ready", false);

                // Unconditional retrigger (unlike notify_klippy_shutdown which
                // checks !discovery_.is_completed()): a transition INTO ready
                // may follow a FIRMWARE_RESTART after config edits where the
                // hardware shape changed, so we always rediscover.
                invoke_connected_callback(on_connected, "Klippy ready");
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("[Moonraker Client] onmessage callback threw unexpected exception: {}",
                           e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("[Moonraker Client] onmessage callback threw unknown exception");
    }
}

void MoonrakerClient::on_ws_close() {
    // Snapshot per-connect callback; the install-once trampoline no longer captures it.
    std::function<void()> on_disconnected;
    {
        std::lock_guard<std::mutex> lk(reconnect_mutex_);
        on_disconnected = last_on_disconnected_;
    }

    try {
        spdlog::debug("[Moonraker Client] onclose callback invoked");

        ConnectionState current = connection_state_.load();

        // Best-effort suppression of "connection lost / reconnecting" side-effects when the
        // close was triggered by an intentional teardown (disconnect() arms a short suppress
        // window). The unconditional cleanup below always runs. A rare residual race during
        // change-host-to-unreachable-host may still surface one rate-limited notification —
        // this is accepted.
        const bool suppressed = is_disconnect_modal_suppressed();

        // Cleanup all pending requests (invoke error callbacks) — unconditional.
        tracker_.cleanup_all();

        // Drop the klippy-state freshness watermark, also unconditionally — this
        // is the one point every close funnels through, intentional teardown and
        // dropped link alike. Klipper's eventtime is monotonic within one host
        // uptime, so a reboot (or a switch to a different printer) restarts it near
        // zero; carrying the old watermark across would make the next session's
        // genuinely-current frames look older than the last session's and be
        // rejected for the life of the process. Touches two POD members under
        // PrinterState's own mutex — no LVGL, safe from this event-loop thread.
        get_printer_state().reset_klippy_state_freshness();

        if (was_connected_) {
            spdlog::warn("[Moonraker Client] WebSocket connection closed");
            TelemetryManager::instance().record_error("websocket", "disconnected",
                                                      "connection closed unexpectedly");
            was_connected_ = false;
            // Reset so re-identification + retry-on-klippy-state happen on reconnect
            discovery_.reset_identified();
            discovery_.reset_completion();

            if (!suppressed) {
                // Emit event with rate limiting to prevent spam during reconnect loop
                if (!g_already_notified_disconnect.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_LOST,
                               "Connection to printer lost - attempting to reconnect...", false);
                    g_already_notified_disconnect.store(true);
                }

                // Check if this is a reconnection scenario
                if (current != ConnectionState::FAILED) {
                    set_connection_state(ConnectionState::RECONNECTING);
                }

                // Invoke user callback with exception safety
                if (on_disconnected) {
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw unknown exception");
                    }
                }
            }
        } else {
            spdlog::debug("[Moonraker Client] WebSocket connection failed (printer not available)");

            // Initial connection failed. NOT suppressed — we want failed-new-host
            // notifications to surface (this is the initial-connection branch, not a
            // reconnect of a previously-established session).
            if (current == ConnectionState::CONNECTING) {
                set_connection_state(ConnectionState::DISCONNECTED);
            }

            // Escalate a connection that has NEVER opened. Both existing
            // escalations are unreachable here: the health timer that owns the
            // stall check is started from on_ws_open(), and that check requires
            // RECONNECTING while this path sets DISCONNECTED. So without this,
            // a stale/wrong host retries silently forever — bundle XRK8KPTF sat
            // in exactly that state, and the only feedback was a disconnected
            // icon that never names the address being dialed.
            //
            // Latched: libhv retries roughly every 3s and re-opening the modal
            // on each one would be unusable. Retries continue regardless, so
            // the app still heals itself if the printer comes up later.
            const auto connect_anchor = connect_started_at_.load(std::memory_order_relaxed);
            if (!initial_failure_notified_.load(std::memory_order_relaxed) && connect_anchor != 0) {
                const auto elapsed = steady_ms_since(connect_anchor);
                if (elapsed > static_cast<long long>(initial_connect_failure_ms_)) {
                    initial_failure_notified_.store(true, std::memory_order_relaxed);

                    std::string endpoint;
                    {
                        std::lock_guard<std::mutex> lk(reconnect_mutex_);
                        endpoint = last_url_;
                    }
                    endpoint = format_ws_endpoint(endpoint);

                    spdlog::error("[Moonraker Client] Never reached {} after {}ms — giving up on "
                                  "the initial connection (retries continue)",
                                  endpoint, elapsed);
                    set_connection_state(ConnectionState::FAILED);
                    // "Check that the printer is powered on and that this
                    // address is correct" is wrong advice when the address is
                    // this machine: HelixScreen is running on the printer, so
                    // it is demonstrably powered on and the address is not in
                    // question — Moonraker is simply not up. The AD5X bundles
                    // TAU4PW4H / 865DXBQ7 are exactly that: instant connection
                    // refusals on 127.0.0.1:7125 for two boots running, and a
                    // dialog telling the user to check the address and offering
                    // to change it.
                    emit_event(
                        MoonrakerEventType::CONNECTION_FAILED,
                        helix::is_moonraker_on_same_host(host_of_endpoint(endpoint))
                            ? fmt::format("Moonraker is not responding at {}. It runs on this "
                                          "printer, so check that the Klipper and Moonraker "
                                          "services started.",
                                          endpoint)
                            : fmt::format("Unable to reach printer at {}. Check that the printer "
                                          "is powered on and that this address is correct.",
                                          endpoint),
                        true);
                }
            }

            // Call on_disconnected() to notify about connection failure
            // Callers can use their own state tracking (e.g. connection_testing flag)
            // to distinguish initial connection failures from reconnection scenarios
            if (on_disconnected) {
                try {
                    on_disconnected();
                } catch (const std::exception& e) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw exception: {}", e.what());
                } catch (...) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw unknown exception");
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unexpected exception: {}",
                           e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unknown exception");
    }
}

SubscriptionId MoonrakerClient::register_notify_update(std::function<void(const json&)> cb) {
    if (!cb) {
        spdlog::warn("[Moonraker Client] register_notify_update called with null callback");
        return INVALID_SUBSCRIPTION_ID;
    }

    SubscriptionId id = next_subscription_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        notify_callbacks_.emplace(id, cb);
    }
    spdlog::trace("[Moonraker Client] Registered notify callback with ID {}", id);
    return id;
}

bool MoonrakerClient::unsubscribe_notify_update(SubscriptionId id) {
    if (id == INVALID_SUBSCRIPTION_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = notify_callbacks_.find(id);
    if (it != notify_callbacks_.end()) {
        notify_callbacks_.erase(it);
        spdlog::debug("[Moonraker Client] Unsubscribed notify callback ID {}", id);
        return true;
    }
    spdlog::debug("[Moonraker Client] Unsubscribe failed: notify callback ID {} not found", id);
    return false;
}

void MoonrakerClient::register_event_handler(MoonrakerEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_handler_mutex_);
    event_handler_ = std::move(cb);
    spdlog::debug("[Moonraker Client] Event handler {}",
                  event_handler_ ? "registered" : "unregistered");
}

void MoonrakerClient::suppress_disconnect_modal(uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(suppress_mutex_);
    suppress_disconnect_modal_until_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    spdlog::info("[Moonraker Client] Suppressing disconnect modal for {}ms", duration_ms);
}

bool MoonrakerClient::is_disconnect_modal_suppressed() const {
    std::lock_guard<std::mutex> lock(suppress_mutex_);
    return std::chrono::steady_clock::now() < suppress_disconnect_modal_until_;
}

void MoonrakerClient::emit_event(MoonrakerEventType type, const std::string& message, bool is_error,
                                 const std::string& details) {
    MoonrakerEventCallback handler;
    {
        std::lock_guard<std::mutex> lock(event_handler_mutex_);
        handler = event_handler_;
    }

    if (handler) {
        MoonrakerEvent evt{type, message, details, is_error};
        try {
            handler(evt);
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] Event handler threw exception: {}", e.what());
        }
    } else {
        // No handler registered - just log the event
        if (is_error) {
            spdlog::error("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        } else {
            spdlog::warn("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        }
    }
}

void MoonrakerClient::dispatch_status_update(const json& status, bool from_cached_snapshot) {
    // Parse bed mesh data before dispatching (mirrors WebSocket handler behavior)
    // This ensures bed mesh is populated on initial subscription response,
    // not just on subsequent notify_status_update messages
    if (status.contains("bed_mesh") && status["bed_mesh"].is_object()) {
        parse_bed_mesh(status["bed_mesh"]);
        // NOTE: Do NOT set build_volume from mesh bounds here!
        // Mesh bounds represent the probe area, not bed dimensions.
        // Actual bed dimensions come from stepper config in moonraker_api_motion.cpp.
    }

    // Extract kinematics type from toolhead data (for printer detection)
    if (status.contains("toolhead") && status["toolhead"].is_object()) {
        const json& toolhead = status["toolhead"];
        if (toolhead.contains("kinematics") && toolhead["kinematics"].is_string()) {
            auto kinematics = toolhead["kinematics"].get<std::string>();
            discovery_.modify_hardware(
                [&](PrinterDiscovery& hw) { hw.set_kinematics(kinematics); });
            spdlog::debug("[Moonraker Client] Kinematics type: {}", kinematics);
        }
    }

    // Wrap raw status into notify_status_update format. There is no eventtime to
    // carry — a synthetic dispatch is not a Klipper frame — so 0.0 stands for
    // "untimestamped", which is why replay provenance has to be stated separately
    // rather than inferred from the clock value.
    json notification = {
        {"method", "notify_status_update"},
        {"params", json::array({status, 0.0})} // [status, eventtime]
    };
    if (from_cached_snapshot) {
        notification[CACHED_SNAPSHOT_MARKER] = true;
    }

    // Dispatch to all registered callbacks
    // Two-phase: copy under lock, invoke outside to avoid deadlock
    std::vector<std::function<void(const json&)>> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy.reserve(notify_callbacks_.size());
        for (const auto& [id, cb] : notify_callbacks_) {
            callbacks_copy.push_back(cb);
        }
    }

    // Wrap each callback in try/catch — this path delivers the initial subscription
    // snapshot synchronously on the main thread (Application::on_discovery_complete →
    // dispatch_status_update). An unhandled exception here unwinds straight through
    // run_main_loop into main()'s top-level catch, exiting 134 and triggering a crash
    // loop the watchdog can't break out of (#filament_motion_sensor null fields,
    // f75b961d8). The onmessage path already wraps each callback (line 533); this
    // mirrors that contract for the initial-state path.
    for (const auto& cb : callbacks_copy) {
        if (!cb)
            continue;
        try {
            cb(notification);
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] dispatch_status_update callback threw exception: {}", e.what());
            TelemetryManager::instance().record_error("websocket", "status_dispatch_exception",
                                                      e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] dispatch_status_update callback threw unknown exception");
        }
    }

    spdlog::trace(
        "[Moonraker Client] Dispatched status update to {} callbacks (has print_stats: {})",
        callbacks_copy.size(), status.contains("print_stats"));
}

void MoonrakerClient::register_method_callback(const std::string& method,
                                               const std::string& handler_name,
                                               std::function<void(const json&)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Registering new method callback: {} (handler: {})",
                      method, handler_name);
        std::map<std::string, std::function<void(const json&)>> handlers;
        handlers.insert({handler_name, cb});
        method_callbacks_.insert({method, handlers});
    } else {
        spdlog::debug("[Moonraker Client] Adding handler to existing method {}: {}", method,
                      handler_name);
        it->second.insert({handler_name, cb});
    }
}

bool MoonrakerClient::unregister_method_callback(const std::string& method,
                                                 const std::string& handler_name) {
    // During destruction, method_callbacks_ may already be cleared or mid-destruction.
    // Skip the erase to avoid use-after-free on the map's internal tree.
    if (is_destroying_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto method_it = method_callbacks_.find(method);
    if (method_it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Unregister failed: method '{}' not found", method);
        return false;
    }

    auto handler_it = method_it->second.find(handler_name);
    if (handler_it == method_it->second.end()) {
        spdlog::debug(
            "[Moonraker Client] Unregister failed: handler '{}' not found for method '{}'",
            handler_name, method);
        return false;
    }

    method_it->second.erase(handler_it);
    spdlog::debug("[Moonraker Client] Unregistered handler '{}' from method '{}'", handler_name,
                  method);

    // Clean up empty method entries to avoid memory leaks
    if (method_it->second.empty()) {
        method_callbacks_.erase(method_it);
        spdlog::debug("[Moonraker Client] Removed empty method entry for '{}'", method);
    }

    return true;
}

// libhv's WebSocketClient::send only checks `channel != NULL`, NOT the WS
// protocol state. Sending while the channel exists but state ∈ {CONNECTING,
// WS_UPGRADING, WS_CLOSED} writes WS frame bytes into the wrong phase of the
// underlying TCP stream — Moonraker silently drops the malformed bytes and the
// request sits pending until 60 s timeout (or never, if the panel cleared its
// in-flight flag and stopped expecting a response). Surfaced as #909 (K2 Plus
// startup race: get_directory issued before onopen → request lost, panel stuck
// in refresh_in_flight_ for hours, no timeout warning fired because the timer
// was scheduled but the request wasn't where the timer expected).
//
// Fail fast unless our connection_state_ is CONNECTED — that flag flips ONLY
// in the onopen callback after WS_OPENED, and back out on disconnect/close.
bool MoonrakerClient::ready_to_send(const char* method) const {
    auto state = connection_state_.load(std::memory_order_acquire);
    if (state == ConnectionState::CONNECTED) {
        return true;
    }
    spdlog::debug("[Moonraker Client] Refusing send for '{}' — connection_state={}",
                  method ? method : "?", static_cast<int>(state));
    return false;
}

int MoonrakerClient::send_jsonrpc(const std::string& method) {
    if (!ready_to_send(method.c_str())) {
        return -1;
    }
    return tracker_.send_fire_and_forget(*this, method, json());
}

int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    if (!ready_to_send(method.c_str())) {
        return -1;
    }
    return tracker_.send_fire_and_forget(*this, method, params);
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(const json&)> cb) {
    // Forward to new overload with null error callback
    return send_jsonrpc(method, params, cb, nullptr, 0);
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(const json&)> success_cb,
                                        std::function<void(const MoonrakerError&)> error_cb,
                                        uint32_t timeout_ms, bool silent,
                                        std::optional<rpc_error_policy::CallerIntent> intent) {
    if (!ready_to_send(method.c_str())) {
        // Invoke error callback synchronously so callers (panels with
        // in_flight flags, etc.) see immediate failure instead of a stuck
        // request that never times out (#909).
        if (error_cb) {
            try {
                error_cb(MoonrakerError::connection_lost(method));
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker Client] Pre-send error cb for '{}' threw: {}", method,
                              e.what());
            }
        }
        return INVALID_REQUEST_ID;
    }
    return tracker_.send(*this, method, params, success_cb, error_cb, timeout_ms, silent, intent);
}

int MoonrakerClient::gcode_script(const std::string& gcode) {
    // Transmitted VERBATIM — see moonraker_gcode_guards.h.
    json params = {{"script", gcode}};
    int result = send_jsonrpc("printer.gcode.script", params);
    // send() returns bytes sent (positive) on success, negative on error.
    // Normalize to match API contract: 0 = success, negative = error.
    return result < 0 ? result : 0;
}

void MoonrakerClient::get_gcode_store(
    int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
    std::function<void(const MoonrakerError&)> on_error) {
    json params = {{"count", count}};

    send_jsonrpc(
        "server.gcode_store", params,
        [on_success](json response) {
            std::vector<GcodeStoreEntry> entries;

            // Parse response: {"result": {"gcode_store": [...]}}
            if (response.contains("result") && response["result"].contains("gcode_store")) {
                const auto& store = response["result"]["gcode_store"];
                entries.reserve(store.size());

                for (const auto& item : store) {
                    GcodeStoreEntry entry;
                    entry.message = item.value("message", "");
                    entry.time = item.value("time", 0.0);
                    entry.type = item.value("type", "response");
                    entries.push_back(entry);
                }
            }

            if (on_success) {
                on_success(entries);
            }
        },
        on_error);
}

TemperatureStore MoonrakerClient::parse_temperature_store(const json& result) {
    TemperatureStore store;
    if (!result.is_object()) {
        return store;
    }

    // Each entry is a sensor name → {temperatures:[], targets:[], powers:[]}.
    // Any array may be absent (a temperature_sensor has no targets/powers).
    for (const auto& [key, series_json_binding] : result.items()) {
        if (!series_json_binding.is_object()) {
            continue;
        }
        // Alias the structured binding to a real local; C++17 forbids capturing
        // a structured binding directly in a lambda (allowed only from C++20).
        const json& series_json = series_json_binding;
        TemperatureStoreSeries series;
        auto load_array = [&series_json](const char* field, std::vector<float>& out) {
            if (series_json.contains(field) && series_json[field].is_array()) {
                const auto& arr = series_json[field];
                out.reserve(arr.size());
                for (const auto& v : arr) {
                    if (v.is_number()) {
                        out.push_back(v.get<float>());
                    }
                }
            }
        };
        load_array("temperatures", series.temperatures);
        load_array("targets", series.targets);
        load_array("powers", series.powers);
        store.emplace(key, std::move(series));
    }

    return store;
}

void MoonrakerClient::get_temperature_store(std::function<void(const TemperatureStore&)> on_success,
                                            std::function<void(const MoonrakerError&)> on_error) {
    json params = {{"include_monitors", false}};

    send_jsonrpc(
        "server.temperature_store", params,
        [on_success](json response) {
            TemperatureStore store;
            if (response.contains("result")) {
                store = parse_temperature_store(response["result"]);
            }
            if (on_success) {
                on_success(store);
            }
        },
        on_error);
}

void MoonrakerClient::invoke_connected_callback(const std::function<void()>& cb,
                                                const char* cause) {
    if (cb) {
        try {
            cb();
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] {} callback threw exception: {}", cause,
                               e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] {} callback threw unknown exception", cause);
        }
    }

    // Fan out to any registered observers. Copy under lock to avoid holding the
    // mutex while user code runs.
    std::vector<std::pair<std::string, std::function<void()>>> observers_copy;
    {
        std::lock_guard<std::mutex> lock(connected_observers_mutex_);
        observers_copy.reserve(connected_observers_.size());
        for (const auto& [name, fn] : connected_observers_) {
            observers_copy.emplace_back(name, fn);
        }
    }
    for (auto& [name, fn] : observers_copy) {
        if (!fn)
            continue;
        try {
            fn();
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] {} observer '{}' threw exception: {}", cause,
                               name, e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] {} observer '{}' threw unknown exception", cause,
                               name);
        }
    }
}

void MoonrakerClient::add_connected_observer(const std::string& handler_name,
                                             std::function<void()> cb) {
    bool fire_immediately = false;
    std::function<void()> immediate_cb;
    {
        std::lock_guard<std::mutex> lock(connected_observers_mutex_);
        connected_observers_[handler_name] = cb;
        if (connection_state_.load(std::memory_order_acquire) == ConnectionState::CONNECTED) {
            fire_immediately = true;
            immediate_cb = cb;
        }
    }
    if (fire_immediately && immediate_cb) {
        // Already-connected handshake: fire on the caller's thread (typically
        // the main/UI thread). Observers that need to touch LVGL should defer
        // via UpdateQueue / AsyncLifetimeGuard::bg_cb themselves.
        try {
            immediate_cb();
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] connected observer '{}' immediate-fire threw: {}", handler_name,
                e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] connected observer '{}' immediate-fire threw unknown",
                handler_name);
        }
    }
}

bool MoonrakerClient::remove_connected_observer(const std::string& handler_name) {
    std::lock_guard<std::mutex> lock(connected_observers_mutex_);
    return connected_observers_.erase(handler_name) > 0;
}

void MoonrakerClient::discover_printer(std::function<void()> on_complete,
                                       std::function<void(const std::string& reason)> on_error) {
    // Store callback for force_reconnect()
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_discovery_complete_ = on_complete;
    }

    discovery_.start(on_complete, on_error);
}

// Discovery methods (continue_discovery, complete_discovery_subscription, parse_objects,
// parse_bed_mesh) moved to MoonrakerDiscoverySequence
// Request tracking methods (check_request_timeouts, cleanup_pending_requests) moved to
// MoonrakerRequestTracker
// cancel_request() is now an inline delegation in the header
