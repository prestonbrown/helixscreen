// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_oversized_frame.cpp
 * @brief What on_ws_message() does with a frame over MAX_MESSAGE_SIZE.
 *
 * The onmessage trampoline holds callback_lifecycle_mutex_ SHARED for as long as
 * on_ws_message() runs, so anything there that takes that mutex EXCLUSIVE (as
 * disconnect() does) blocks the libhv loop on itself. An oversized frame is dropped
 * and the connection kept: the frame is usually Moonraker's valid reply to a large
 * request, and disconnect() would also switch off auto-reconnect.
 */

#include "../../include/moonraker_client.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// on_ws_message()'s MAX_MESSAGE_SIZE is 5 MB; comfortably over it.
constexpr size_t OVERSIZED_BYTES = 6 * 1024 * 1024;

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "MoonrakerClient drops an oversized frame without deadlocking or disconnecting",
                 "[connection][eventloop][drain]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());

    // The onmessage trampoline is the half that takes the shared lock; without it
    // there is no lock to collide with and the test proves nothing. Installed
    // without connecting, so nothing but this frame can change the connection.
    MoonrakerClientTestAccess::install_ws_callbacks(client);
    REQUIRE(client.onmessage != nullptr);
    MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);

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

    // The connection survives the dropped frame: no teardown is queued for later either.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(client.get_connection_state() == ConnectionState::CONNECTED);

    loop_thread->stop(true);
}
