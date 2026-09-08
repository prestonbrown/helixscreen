// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system/telemetry_manager.h"

#include <chrono>
#include <mutex>

// Test-only seam. TelemetryManager declares this class as a friend, so a test
// can observe whether try_send() cleared its send-interval gate.
//
// The gate's three inputs are all private: the backoff multiplier says whether
// the previous attempt failed, last_send_time_ says how long ago it was, and
// the pass/skip decision leaves no other trace — a cleared gate stamps
// last_send_time_, a skipped one does not. Without these, the spacing between
// a failed send and its retry is unreachable from a test.
//
// disable_network() is what makes calling try_send() safe: it puts the send
// path into the state a device with no CA bundle is already in, so do_send()
// returns before it builds a request. The gate still runs, which is the part
// under test.
//
// Kept out of the production header so no shipped code path can suppress a
// send or rewrite the send clock (the "no _for_testing methods in headers"
// lint, L065/L088).
class TelemetryManagerTestAccess {
  public:
    /// Make do_send() a no-op so try_send() can be called without HTTP. Leaves
    /// the send-interval gate, the thread spawn and the queue untouched.
    static void disable_network(TelemetryManager& t) {
        t.ssl_verified_ = true;
        t.send_disabled_ = true;
    }

    /// Pretend the previous attempt failed @p backoff doublings ago. 1 means
    /// the last send succeeded, which is the healthy 24-hour cadence.
    static void set_backoff(TelemetryManager& t, int backoff) {
        t.backoff_multiplier_.store(backoff);
    }

    static int backoff(const TelemetryManager& t) {
        return t.backoff_multiplier_.load();
    }

    /// Move the send clock so a test can choose how long ago the last attempt
    /// was without waiting.
    static void set_last_send_time(TelemetryManager& t,
                                   std::chrono::steady_clock::time_point when) {
        t.last_send_time_ = when;
    }

    /// try_send() stamps this when it clears the interval gate and leaves it
    /// alone when it skips, so it is the observable for "did the gate pass".
    static std::chrono::steady_clock::time_point last_send_time(const TelemetryManager& t) {
        return t.last_send_time_;
    }

    /// Events discarded by enqueue_event() since the last batch was accepted.
    /// The queue size alone cannot distinguish a window that fit from one that
    /// overflowed, because both leave it at MAX_QUEUE_SIZE.
    static size_t events_dropped_since_send(const TelemetryManager& t) {
        std::lock_guard<std::mutex> lock(t.mutex_);
        return t.events_dropped_since_send_;
    }
};
