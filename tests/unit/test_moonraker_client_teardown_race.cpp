// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_teardown_race.cpp
 * @brief Regression tests for the client/event-loop teardown SIGSEGV (#1212)
 *
 * libhv arms an auto-reconnect timer whenever a connect attempt fails, and
 * never stores its TimerID — so nothing could cancel it. Two consequences,
 * one test each:
 *
 * 1. The armed timer calls TcpClientEventLoopTmpl::startConnect() on the event
 *    loop thread long after the client has been disconnected.
 *
 * 2. startConnect() -> createsocket() calls hio_get(loop_->loop(), fd), and
 *    hv::EventLoop::stop() nulls its hloop_t* on the CALLING thread while
 *    hloop_run() is still executing on the loop thread. A timer that fires in
 *    that window dereferences a null hloop:
 *
 *      thread #9, EXC_BAD_ACCESS (code=1, address=0x130)
 *      frame #0: hio_get + 28
 *      ->  ldr x8, [x0, #0x130]        ; x0 == NULL, 0x130 == loop->ios.maxsize
 *
 * Reachable outside tests: ~MoonrakerClient stops and joins the loop the same
 * way, so shutting down while the printer is unreachable (reconnect timer
 * armed) hits the same window.
 */

#include "../../include/moonraker_client.h"
#include "../helix_test_fixture.h"

#include <chrono>
#include <memory>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// A port nothing listens on, so connect() fails fast and libhv arms its
// auto-reconnect timer. Deliberately distinct from the 19999 other client
// tests share, so a stray listener there cannot influence these.
constexpr const char* DEAD_URL = "ws://127.0.0.1:19997/websocket";

// MoonrakerClient's reconnect_min_delay_ms_ default (moonraker_client.cpp).
constexpr int RECONNECT_MIN_DELAY_MS = 200;

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClient connect() on a stopped event loop does not deref a null hloop",
                 "[connection][eventloop][1212]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());

    // Stop the loop FIRST. hv::EventLoop::stop() sets its hloop_t* to NULL on
    // the calling thread (evpp/EventLoop.h), which is exactly the state a timer
    // firing during teardown observes.
    loop_thread->stop(true);
    REQUIRE(loop_thread->loop()->loop() == nullptr); // precondition: null window is real

    // Reaches createsocket() -> hio_get(NULL, fd). Without the null guard this
    // is the SIGSEGV above, so a regression kills the process rather than
    // failing the assertion.
    int rc = client.connect(DEAD_URL, []() {}, []() {});

    REQUIRE(rc < 0);
    REQUIRE(client.get_connection_state() != ConnectionState::CONNECTED);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClient disconnect() stops a pending libhv auto-reconnect timer",
                 "[connection][eventloop][slow][1212]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());

    client.connect(DEAD_URL, []() {}, []() {});

    // Long enough for the connect to be refused and libhv to arm the retry,
    // short enough that the retry has not yet fired.
    std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_MIN_DELAY_MS / 2));

    client.disconnect();
    const hv::WebSocketChannel* channel_at_disconnect = client.channel.get();

    // Well past the retry delay. A live timer runs startConnect(), whose
    // createsocket() installs a brand-new channel — observable proof that the
    // cancelled reconnect ran anyway.
    std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_MIN_DELAY_MS * 3));

    CHECK(client.channel.get() == channel_at_disconnect);
    CHECK(client.get_connection_state() == ConnectionState::DISCONNECTED);

    loop_thread->stop(true);
}
