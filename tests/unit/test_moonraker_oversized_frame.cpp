// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_oversized_frame.cpp
 * @brief Regression test for the oversized-frame self-deadlock in on_ws_message().
 *
 * The onmessage trampoline holds callback_lifecycle_mutex_ SHARED for as long as
 * on_ws_message() runs. on_ws_message() used to react to a frame over
 * MAX_MESSAGE_SIZE by calling disconnect() inline, and disconnect() takes that
 * same mutex EXCLUSIVE. std::shared_mutex is neither recursive nor upgradeable,
 * so the thread blocked handing the lock to itself: the libhv event loop stopped
 * being serviced entirely and every later request timed out with no clue why.
 *
 * The teardown is deferred through queueInLoop() now, so it lands on the next
 * loop iteration with the trampoline's shared lock already released.
 */

#include "../../include/moonraker_client.h"
#include "../helix_test_fixture.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Nothing listens here, so connect() fails fast. Distinct from the ports the
// other client tests use so a stray listener cannot influence this one.
constexpr const char* DEAD_URL = "ws://127.0.0.1:19993/websocket";

// on_ws_message()'s MAX_MESSAGE_SIZE is 5 MB; comfortably over it.
constexpr size_t OVERSIZED_BYTES = 6 * 1024 * 1024;

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClient oversized frame does not self-deadlock the event loop",
                 "[connection][eventloop][drain]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());

    // connect() is what installs the onmessage trampoline (install-once), which
    // is the half that takes the shared lock. Without it there is no lock to
    // collide with and the test proves nothing.
    client.connect(DEAD_URL, []() {}, []() {});
    REQUIRE(client.onmessage != nullptr);

    const std::string huge(OVERSIZED_BYTES, 'x');

    // Drive the trampoline from a helper thread so a regression fails the
    // assertion instead of hanging the whole suite.
    std::atomic<bool> returned{false};
    std::atomic<long long> elapsed_ms{0};
    std::thread driver([&client, &huge, &returned, &elapsed_ms]() {
        const auto started = std::chrono::steady_clock::now();
        client.onmessage(huge);
        elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count());
        returned.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool came_back = returned.load();
    if (came_back) {
        driver.join();
    } else {
        // Deadlocked: the thread owns a lock on `client` and will never release
        // it, so joining would hang the suite and destroying `client` under it
        // would be a use-after-free. Detach and let the process exit carry it.
        driver.detach();
    }
    REQUIRE(came_back);

    // "Came back at all" is too weak on its own: disconnect()'s drain barrier is
    // bounded, so an inline disconnect() self-deadlocks and then gets rescued by
    // that timeout, returning in ~3s and passing a mere liveness check. The
    // deferral is what makes this return immediately, so hold it to that — the
    // bound is the backstop, not the mechanism under test here.
    CAPTURE(elapsed_ms.load());
    REQUIRE(elapsed_ms.load() < 1000);

    // The teardown still has to actually happen, just later — a deferral that
    // silently dropped the disconnect would pass the deadlock check above while
    // leaving the client wired to a peer that just violated the protocol.
    const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client.get_connection_state() != ConnectionState::DISCONNECTED &&
           std::chrono::steady_clock::now() < disconnect_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(client.get_connection_state() == ConnectionState::DISCONNECTED);

    loop_thread->stop(true);
}
