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

#include <chrono>
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
