// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

namespace helix {

/**
 * @brief Marks a flow as owning a heater target that only this app will clear
 *
 * While any hold is set, SIGTERM takes the graceful path so teardown can send
 * the off (sigterm_action()). A holder must therefore send that off from
 * before_disconnect(), while the client is still connected. Take a hold only
 * when the flow will clear the heater. Several flows can hold at once.
 */
class HeaterHold {
  public:
    HeaterHold() = default;
    ~HeaterHold() {
        set(false);
    }
    HeaterHold(const HeaterHold&) = delete;
    HeaterHold& operator=(const HeaterHold&) = delete;

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
