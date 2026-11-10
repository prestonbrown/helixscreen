// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

namespace helix {

/**
 * @brief Marks a flow as owing the printer a cleanup that only this app will send
 *
 * A heater target the flow set and must clear, or a printer state it changed
 * and must restore. While any hold is set, SIGTERM takes the graceful path so
 * teardown can send that cleanup (sigterm_action()). A holder must therefore
 * send it from before_disconnect(), while the client is still connected. Take
 * a hold only when the flow will send the cleanup. Several flows can hold at
 * once.
 */
class CleanupHold {
  public:
    CleanupHold() = default;
    ~CleanupHold() {
        set(false);
    }
    CleanupHold(const CleanupHold&) = delete;
    CleanupHold& operator=(const CleanupHold&) = delete;

    /// Idempotent per object. One thread per object; distinct objects may race.
    void set(bool held) {
        if (held != held_) {
            held_ = held;
            s_count.fetch_add(held ? 1 : -1, std::memory_order_acq_rel);
        }
    }

    [[nodiscard]] bool held() const {
        return held_;
    }

    /// Async-signal-safe: a lock-free atomic load.
    [[nodiscard]] static bool any_held_signal_safe() {
        return s_count.load(std::memory_order_acquire) > 0;
    }

  private:
#ifndef ESP_PLATFORM // Firmware has no POSIX signals, so no handler reads this.
    static_assert(std::atomic<int>::is_always_lock_free);
#endif
    inline static std::atomic<int> s_count{0};
    bool held_ = false;
};

} // namespace helix
