// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <shared_mutex>
#include <thread>

namespace helix {

/**
 * @brief Wait for every shared-lock holder of @p m to release, bounded by @p timeout.
 *
 * The callback-lifecycle pattern: in-flight callbacks hold a shared lock for as
 * long as they run, so taking the exclusive lock — and dropping it again right
 * away — is how a teardown path waits for them to finish.
 *
 * The bound is the whole point. Callers on the UI thread (disconnect() is
 * reached from the display-wake path) cannot afford an unbounded acquire: a
 * single callback that never returns would park the main loop in
 * pthread_rwlock_wrlock forever, which presents as a lit screen that ignores
 * every touch.
 *
 * Polls try_lock() to a deadline rather than using shared_timed_mutex's
 * try_lock_for. The mutex type is declared in moonraker_client.h, which ~21
 * ESP32 firmware TUs include transitively, and std::shared_timed_mutex is not
 * worth betting the firmware build on for a wait this coarse.
 *
 * @return true when exclusive ownership was acquired, meaning all callbacks had
 *         drained. false on timeout, meaning at least one is still running and
 *         the caller is about to race it — callers must say so loudly rather
 *         than treating it as success.
 */
inline bool drain_shared_holders(std::shared_mutex& m, std::chrono::milliseconds timeout) {
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(5);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    do {
        if (m.try_lock()) {
            // Ownership is released immediately — callers want the barrier, not
            // continued exclusion.
            m.unlock();
            return true;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

} // namespace helix
