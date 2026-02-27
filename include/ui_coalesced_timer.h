// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <functional>

namespace helix::ui {

/**
 * @brief RAII one-shot timer that coalesces multiple rapid schedule() calls
 *
 * Multiple calls to schedule() within the timer period result in a single
 * callback firing after the period elapses with no new schedule() calls.
 * Each schedule() resets the timer, so the callback always reflects the
 * most recent request.
 *
 * Typical use: batching observer-driven rebuilds that fire many times
 * per LVGL tick during startup discovery.
 *
 * @code
 * CoalescedTimer timer(1);  // 1ms — coalesce within same LVGL frame
 * // In observer callbacks:
 * timer.schedule([this]() { rebuild(); });
 * @endcode
 */
class CoalescedTimer {
  public:
    /**
     * @brief Construct a coalesced timer
     * @param period_ms Quiet period before callback fires (default: 1ms)
     */
    explicit CoalescedTimer(uint32_t period_ms = 1);
    ~CoalescedTimer();

    CoalescedTimer(const CoalescedTimer&) = delete;
    CoalescedTimer& operator=(const CoalescedTimer&) = delete;
    CoalescedTimer(CoalescedTimer&& other) noexcept;
    CoalescedTimer& operator=(CoalescedTimer&& other) noexcept;

    /**
     * @brief Schedule a callback. Resets timer if already pending.
     *
     * If called multiple times before the timer fires, only the last
     * callback is invoked (after period_ms of quiet).
     */
    void schedule(std::function<void()> cb);

    /// Cancel any pending callback
    void cancel();

    /// @return true if a callback is scheduled but hasn't fired yet
    bool pending() const;

  private:
    static void timer_cb(lv_timer_t* t);

    lv_timer_t* timer_ = nullptr;
    std::function<void()> callback_;
    uint32_t period_ms_;
};

} // namespace helix::ui
