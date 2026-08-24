// SPDX-License-Identifier: GPL-3.0-or-later
// include/app_motion_activity.h
#pragma once

#include <atomic>
#include <chrono>

namespace helix {

/**
 * Tracks app-initiated motion (jog) RPC activity so the discretionary-gcode
 * busy guard can tell self-inflicted busy (idle_timeout == "Printing" because
 * OUR jog is executing) from an external blocking op (calibration, console
 * gcode from another UI). Thread-safe: sends stamp from the main thread,
 * acks/errors from the websocket thread.
 *
 * Invariant: MoonrakerMotionAPI wraps BOTH the success and error callback of
 * every stamped send, and the request tracker guarantees one of them fires
 * (including on timeout) — so inflight_ cannot leak upward permanently.
 */
class AppMotionActivity {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds GRACE_WINDOW{2};

    void note_sent() {
        inflight_.fetch_add(1, std::memory_order_relaxed);
    }

    void note_done(clock::time_point now = clock::now()) {
        last_done_ns_.store(now.time_since_epoch().count(), std::memory_order_relaxed);
        // Clamp at zero if a done arrives without a matching send.
        int prev = inflight_.fetch_sub(1, std::memory_order_relaxed);
        if (prev <= 0) {
            inflight_.store(0, std::memory_order_relaxed);
        }
    }

    bool recently_active(clock::time_point now = clock::now()) const {
        if (inflight_.load(std::memory_order_relaxed) > 0) {
            return true;
        }
        const auto last_ns = last_done_ns_.load(std::memory_order_relaxed);
        if (last_ns == 0) {
            return false;
        }
        const clock::time_point last{clock::duration{last_ns}};
        return (now - last) < GRACE_WINDOW;
    }

  private:
    std::atomic<int> inflight_{0};
    std::atomic<long long> last_done_ns_{0};
};

} // namespace helix
