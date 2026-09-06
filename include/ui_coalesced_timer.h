// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <functional>

namespace helix::ui {

/**
 * @brief RAII one-shot timer that collapses a burst of requests into one callback
 *
 * Two coalescing policies, and picking the wrong one is the whole hazard:
 *
 * - schedule() is a **trailing-edge debounce**. Every call resets the timer and
 *   replaces the callback, so the work fires `period_ms` after the burst *stops*
 *   and reflects the most recent request. A caller that re-requests faster than
 *   the period never lets the timer come due — the work is starved indefinitely.
 * - schedule_once() is **leading-edge**. The first call arms the timer; calls
 *   while that one is pending are dropped without touching the timer or the
 *   callback. The work fires `period_ms` after the *first* request, at a bounded
 *   rate, whatever the request rate.
 *
 * Anything driven by a render or animation path wants schedule_once(): those
 * re-request every frame, which is exactly the pattern schedule() starves on.
 * schedule() is for bursty-then-quiet producers (observer storms during startup
 * discovery) where the last value is the only one that matters.
 *
 * The timer is owned by this object and cancelled by its destructor, so a
 * pending callback cannot outlive whatever the timer is a member of.
 *
 * @code
 * CoalescedTimer timer(1);  // 1ms — coalesce within same LVGL frame
 * // In observer callbacks:
 * timer.schedule([this]() { rebuild(); });
 * // In a per-frame draw callback:
 * timer.schedule_once([this]() { recompute_cache(); });
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
     * @brief Trailing-edge debounce: schedule @p cb, resetting the timer if one
     *        is already pending.
     *
     * Called n times before the timer fires, only the last @p cb runs, and it
     * runs period_ms after the *last* call. A caller that re-schedules faster
     * than period_ms defers the work forever — use schedule_once() there.
     */
    void schedule(std::function<void()> cb);

    /**
     * @brief Leading-edge coalescing: schedule @p cb only if nothing is pending.
     *
     * Called n times before the timer fires, only the first @p cb runs, and it
     * runs period_ms after the *first* call. Calls made while pending() are
     * dropped: neither the timer nor the stored callback is touched, so the
     * fire time cannot be pushed out by a caller that requests every frame.
     */
    void schedule_once(std::function<void()> cb);

    /// Cancel any pending callback
    void cancel();

    /// @return true if a callback is scheduled but hasn't fired yet
    bool pending() const;

  private:
    static void timer_cb(lv_timer_t* t);
    /// Create the one-shot LVGL timer. Callers must have checked !timer_.
    void arm();

    lv_timer_t* timer_ = nullptr;
    std::function<void()> callback_;
    uint32_t period_ms_;
};

} // namespace helix::ui
