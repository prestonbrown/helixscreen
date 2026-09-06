// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_reconnect.cpp
 * @brief Regression tests for install-once WebSocket callbacks (bundle UK9QCFY3).
 *
 * Root cause being guarded against: connect() used to reassign the inherited libhv
 * std::function callbacks (onopen/onmessage/onclose) on EVERY call. During a
 * change-host reconnect (disconnect() then connect()), the libhv event-loop thread
 * could be mid-invoke on the OLD onclose while the main thread reassigned it — freeing
 * the running lambda's heap storage → use-after-free → SIGSEGV with a garbage `this`.
 *
 * Fix: install the three callbacks exactly ONCE (install_ws_callbacks(), guarded by
 * ws_callbacks_installed_ under connect_mutex_). The trampolines read per-connect state
 * from last_url_/last_on_connected_/last_on_disconnected_ instead of captures, and
 * self-cancel via a weak_ptr to destruction_guard_.
 *
 * These tests drive repeated connect()/disconnect() cycles against unreachable URLs and
 * assert (a) no crash and (b) the install-once invariant holds: callbacks are installed
 * exactly once and never reassigned across reconnect cycles. ws_callbacks_installed_ is
 * observed via MoonrakerClientTestAccess.
 *
 * The URLs point at loopback ports nothing listens on, so open() fails fast with no
 * external dependency. A short, deterministic settle() between cycles avoids a
 * pre-existing libhv-internal race under rapid open()/close() churn (see the cadence note
 * below) — that race is inside libhv, not this fix.
 */

#include "../../include/moonraker_client.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
// Unreachable loopback port — open() fails fast, no external dependency.
constexpr const char* BAD_URL = "ws://127.0.0.1:19998/websocket";
constexpr const char* BAD_URL2 = "ws://127.0.0.1:19997/websocket"; // simulate change-host
} // namespace

// NOTE on cadence: connect() calls close() then open() on the inherited
// hv::WebSocketClient, which drives reconnection on libhv's own internal thread. Hammering
// connect()/disconnect() back-to-back races libhv's hio/onCustomEvent machinery and
// SIGSEGVs *inside libhv* (crash backtrace is entirely hio_get / eventfd_read_cb /
// onCustomEvent on the libhv loop thread — no HelixScreen frame, and the install-once
// trampolines are not even on the stack). That is a pre-existing libhv limitation, not a
// defect in this fix, and it is why the existing connection tests insert settle delays
// between connect attempts. We follow the same convention with a short (non-blocking,
// deterministic) settle between cycles. The install-once invariant (ws_callbacks_installed_
// flips true exactly once and is never cleared) is what these tests assert.
namespace {
// Brief settle to let libhv's internal loop drain the prior close before the next open().
inline void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}
} // namespace

TEST_CASE("MoonrakerClient install-once: callbacks installed on first connect, not before",
          "[moonraker][connection][reconnect][eventloop][slow]") {
    // Default-construct: supplying a loop sets is_loop_owner=false, so the
    // connect() below spawns an event-loop thread that ~WebSocketClient never
    // joins, freeing the client's members under it (#1146).
    MoonrakerClient client;

    // No callbacks installed until the first connect().
    REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == false);

    client.connect(BAD_URL, []() {}, []() {});
    REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == true);

    client.disconnect();
    // Disconnect must NOT tear down the install-once state — the trampolines stay
    // installed for the lifetime of the client.
    REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == true);
}

TEST_CASE("MoonrakerClient install-once: repeated connect/disconnect never reinstalls or crashes",
          "[moonraker][connection][reconnect][eventloop][slow]") {
    // Default-construct — see the note in the previous TEST_CASE (#1146).
    MoonrakerClient client;

    // Repeated connect/disconnect against unreachable hosts, alternating URLs to mimic a
    // change-host reconnect. Pre-fix, each connect() reassigned the inherited onclose/
    // onopen/onmessage std::functions; doing that while the libhv thread serviced the prior
    // close freed the running lambda's storage → UAF (bundle UK9QCFY3). Post-fix,
    // install_ws_callbacks() runs exactly once (guarded by ws_callbacks_installed_) and the
    // std::functions are never reassigned. We assert the guard flips true exactly once and
    // stays true across every cycle.
    for (int i = 0; i < 25; ++i) {
        const char* url = (i % 2 == 0) ? BAD_URL : BAD_URL2;
        REQUIRE_NOTHROW(client.connect(url, []() {}, []() {}));
        // After the very first connect the flag is set and must never clear — proving
        // install_ws_callbacks() is gated and won't reassign on subsequent connects.
        REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == true);
        REQUIRE_NOTHROW(client.disconnect());
        REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == true);
        settle();
    }
}

TEST_CASE("MoonrakerClient install-once: destruction during pending connect is safe",
          "[moonraker][connection][reconnect][cleanup][eventloop][slow]") {
    // Construct, connect to an unreachable host, then destroy immediately while the
    // event loop thread may still be servicing the failing connection. The install-once
    // trampolines must self-cancel via destruction_guard_ (reset in the dtor before the
    // base hv::WebSocketClient destructor runs) and not touch a destroyed `this`.
    SECTION("destroy right after connect") {
        auto loop_thread = std::make_shared<hv::EventLoopThread>();
        loop_thread->start();
        {
            MoonrakerClient client(loop_thread->loop());
            client.connect(BAD_URL, []() {}, []() {});
            settle(); // let the failing connect start churning on the libhv thread
            // Client destroyed here at scope exit — the dtor resets destruction_guard_
            // before the base hv::WebSocketClient destructor, so any in-flight trampoline
            // sees dg.expired() and bails. No crash expected.
        }
        // JOIN before scope exit. The client was destroyed above mid-connect; a
        // bare stop() leaves the loop thread running a reconnect timer that fires
        // startConnect() on freed io in a later test. stop(true) drains the loop
        // (the dtor's setReconnect(nullptr) keeps the drained timer from
        // reconnecting).
        loop_thread->stop(true);
        REQUIRE(true);
    }

    SECTION("destroy after a connect/disconnect churn") {
        auto loop_thread = std::make_shared<hv::EventLoopThread>();
        loop_thread->start();
        {
            MoonrakerClient client(loop_thread->loop());
            for (int i = 0; i < 5; ++i) {
                client.connect(BAD_URL, []() {}, []() {});
                client.disconnect();
                settle();
            }
            client.connect(BAD_URL2, []() {}, []() {});
            settle();
            // Destroyed while last connect is still pending — dtor must self-cancel the
            // install-once trampolines via destruction_guard_.
        }
        // JOIN before scope exit (see the section above) — stop(true) drains the
        // loop so no reconnect timer fires on freed io in a later test.
        loop_thread->stop(true);
        REQUIRE(true);
    }
}

// ============================================================================
// What disconnect() does and does not stop
// ============================================================================
//
// disconnect() drops a WebSocket connection; it does not retire the client. The three
// trampolines gate on destruction_guard_, callback_lifecycle_mutex_ and is_destroying_,
// and disconnect() changes none of the three, so it is not a barrier for the message
// path: only destruction is. These cases pin that scope in both directions — what still
// gets through, and what the bounded drain does when a callback outlasts it — so a
// rework of teardown that moves the boundary has to move them with it.

TEST_CASE("disconnect() leaves the guard the WS trampolines gate on intact",
          "[moonraker][connection][reconnect][disconnect][1474]") {
    auto client = std::make_unique<MoonrakerClient>();

    // The trampolines capture a weak_ptr to this guard and check it before any
    // `this` deref; SubscriptionGuard reaches the same guard via lifetime_weak().
    std::weak_ptr<bool> guard = client->lifetime_weak();
    REQUIRE_FALSE(guard.expired());

    client->disconnect();

    // Disconnect is a connection event, not an object event. A callback arriving now
    // still finds the guard alive and proceeds.
    REQUIRE_FALSE(guard.expired());

    // Destruction is what retires it — the single event that stops dispatch.
    client.reset();
    REQUIRE(guard.expired());
}

TEST_CASE("a message arriving after disconnect() still reaches its registered handler",
          "[moonraker][connection][reconnect][disconnect][1474]") {
    MoonrakerClient client;

    // Install the real trampolines, then drive onmessage through them. No socket is
    // involved: the point is what the trampoline itself lets through.
    MoonrakerClientTestAccess::install_ws_callbacks(client);
    REQUIRE(MoonrakerClientTestAccess::callbacks_installed(client) == true);
    REQUIRE(static_cast<bool>(client.onmessage));

    int handler_calls = 0;
    client.register_method_callback("notify_gcode_response", "disconnect_scope_probe",
                                    [&handler_calls](const nlohmann::json&) { ++handler_calls; });

    const std::string notification =
        R"({"jsonrpc":"2.0","method":"notify_gcode_response","params":["ok"]})";

    client.onmessage(notification);
    REQUIRE(handler_calls == 1);

    client.disconnect();

    // Registrations survive disconnect and the trampoline still admits the dispatch, so
    // unregistering a handler on a teardown path is load-bearing work that disconnect()
    // does not do on the caller's behalf.
    client.onmessage(notification);
    REQUIRE(handler_calls == 2);
}

TEST_CASE("disconnect()'s callback drain is bounded by a running callback, not blocked by it",
          "[moonraker][connection][reconnect][disconnect][1474][slow]") {
    using namespace std::chrono;

    MoonrakerClient client;

    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        auto lk = MoonrakerClientTestAccess::hold_callback_lock(client);
        holding.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(milliseconds(2));
        }
    });
    while (!holding.load()) {
        std::this_thread::sleep_for(milliseconds(1));
    }

    // disconnect() runs on its own thread: a shared_mutex is not upgradeable, so the
    // holder cannot be the thread that calls it. The future is the failure channel — an
    // unbounded drain would park here forever rather than reporting anything.
    auto elapsed_ms = std::async(std::launch::async, [&] {
        auto started = steady_clock::now();
        client.disconnect();
        return duration_cast<milliseconds>(steady_clock::now() - started).count();
    });

    const auto status = elapsed_ms.wait_for(seconds(30));

    // Release the holder before asserting, so a failure reports instead of hanging.
    release.store(true);
    holder.join();

    REQUIRE(status == std::future_status::ready);
    const auto waited = elapsed_ms.get();

    // It waits for the callback to finish...
    REQUIRE(waited >= 1000);
    // ...and then proceeds anyway, because a UI thread parked in pthread_rwlock_wrlock
    // is a lit screen that ignores touch.
    REQUIRE(waited < 20000);
}
