// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_callback_drain.cpp
 * @brief Tests for the bounded in-flight-callback drain barrier.
 *
 * MoonrakerClient::disconnect() has to wait for callbacks that are mid-flight
 * on the libhv event-loop thread before it tears down the connection. Those
 * callbacks hold a shared lock; the drain takes the exclusive one.
 *
 * The wait MUST be bounded. disconnect() runs on the UI thread (the display
 * wake path reaches it via force_reconnect), so an unbounded acquire turns one
 * wedged callback into a permanently frozen touchscreen — the whole app parked
 * in pthread_rwlock_wrlock with the main loop never ticking again.
 */

#include "../../include/callback_drain.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace std::chrono_literals;

TEST_CASE("drain_shared_holders returns immediately when nobody holds the lock",
          "[connection][drain]") {
    std::shared_mutex m;

    auto start = std::chrono::steady_clock::now();
    bool drained = helix::drain_shared_holders(m, 2000ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(drained);
    // Nothing to wait for, so this must not have burned the timeout.
    REQUIRE(elapsed < 500ms);
}

TEST_CASE("drain_shared_holders releases the lock again so the next drain can acquire it",
          "[connection][drain]") {
    std::shared_mutex m;

    REQUIRE(helix::drain_shared_holders(m, 1000ms));
    // If the first call leaked exclusive ownership, this second one deadlocks
    // or reports failure.
    REQUIRE(helix::drain_shared_holders(m, 1000ms));
}

TEST_CASE("drain_shared_holders gives up instead of hanging on a wedged callback",
          "[connection][drain]") {
    std::shared_mutex m;
    std::atomic<bool> release{false};
    std::atomic<bool> holding{false};

    // Stand in for a callback that took the shared lock and never finished.
    std::thread wedged([&] {
        std::shared_lock<std::shared_mutex> lk(m);
        holding.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(5ms);
        }
    });

    while (!holding.load()) {
        std::this_thread::sleep_for(1ms);
    }

    auto start = std::chrono::steady_clock::now();
    bool drained = helix::drain_shared_holders(m, 300ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    // The point of the whole exercise: it came back, and it reported failure
    // rather than pretending the callbacks had drained.
    REQUIRE_FALSE(drained);
    REQUIRE(elapsed >= 250ms); // actually waited
    REQUIRE(elapsed < 5000ms); // but did not hang

    release.store(true);
    wedged.join();
}

TEST_CASE("drain_shared_holders succeeds once the in-flight callback finishes",
          "[connection][drain]") {
    std::shared_mutex m;
    std::atomic<bool> holding{false};

    // A callback that is slow but not wedged — the drain must wait for it and
    // then succeed, not time out early.
    std::thread slow([&] {
        std::shared_lock<std::shared_mutex> lk(m);
        holding.store(true);
        std::this_thread::sleep_for(200ms);
    });

    while (!holding.load()) {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(helix::drain_shared_holders(m, 5000ms));
    slow.join();
}
