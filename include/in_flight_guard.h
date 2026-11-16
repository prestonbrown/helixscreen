// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace helix {

/**
 * @file in_flight_guard.h
 * @brief Single-flight guard for async RPCs with a stuck-response self-heal.
 *
 * Several panels gate an async Moonraker RPC behind an "in-flight" bool that is
 * cleared only in the success/error callback. If the response is silently lost
 * (WebSocket drop, server never replies), the flag wedges forever and the panel
 * stops refreshing. This helper folds that pattern — the boolean plus an optional
 * timestamp-based self-heal — into one place so every caller gets the same
 * behavior (#910/#911).
 *
 * @c now is injectable so unit tests drive stuck-detection deterministically.
 *
 * Not thread-safe: acquire on the main thread, release from a callback that has
 * already been marshalled back to the main thread (the panels do this via
 * tok.defer / queue_update).
 */
class InFlightGuard {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Outcome of a try_acquire() call.
    enum class AcquireResult {
        Acquired,       ///< Newly acquired (was idle, or force) — proceed.
        RecoveredStuck, ///< Prior request stuck past threshold; re-acquired — proceed.
        Skipped         ///< A healthy request is still in flight — do NOT proceed.
    };

    explicit InFlightGuard(
        std::chrono::milliseconds stuck_threshold = std::chrono::milliseconds(30000))
        : stuck_threshold_(stuck_threshold) {}

    /**
     * @brief Attempt to acquire the guard for a new request.
     *
     * Preserves PrintSelectPanel::refresh_should_skip() semantics exactly:
     *   - force            -> Acquired (bumps started_at)
     *   - !active          -> Acquired
     *   - active && stuck  -> RecoveredStuck (re-acquire, bump started_at)
     *   - active && !stuck -> Skipped (state unchanged)
     *
     * @param force  Bypass the in-flight check (e.g. user-initiated refresh).
     * @param now    Injected clock reading (defaults to the steady clock).
     */
    AcquireResult try_acquire(bool force = false, TimePoint now = Clock::now()) {
        if (force) {
            in_flight_ = true;
            started_at_ = now;
            return AcquireResult::Acquired;
        }
        if (!in_flight_) {
            in_flight_ = true;
            started_at_ = now;
            return AcquireResult::Acquired;
        }
        if ((now - started_at_) >= stuck_threshold_) {
            started_at_ = now; // re-base; stays in-flight for the fresh request
            return AcquireResult::RecoveredStuck;
        }
        return AcquireResult::Skipped;
    }

    /// Release the guard (call from the request's success/error callback).
    void release() {
        in_flight_ = false;
    }

    /// True while a request is considered in flight.
    bool active() const {
        return in_flight_;
    }

    /// True if a request has been in flight for at least the stuck threshold.
    bool is_stuck(TimePoint now = Clock::now()) const {
        return in_flight_ && (now - started_at_) >= stuck_threshold_;
    }

    std::chrono::milliseconds stuck_threshold() const {
        return stuck_threshold_;
    }

  private:
    bool in_flight_ = false;
    TimePoint started_at_{};
    std::chrono::milliseconds stuck_threshold_;
};

} // namespace helix
