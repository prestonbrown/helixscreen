// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <lvgl.h>
#include <string>

namespace helix::ui {

/**
 * @brief Safely cancel an LVGL timer without modifying the timer linked list.
 *
 * Calling lv_timer_delete() from within lv_timer_handler iteration (e.g., during
 * input event processing in a click callback) can corrupt the linked list and
 * cause SIGSEGV in node_set_next/lv_ll_remove (#750, #751).
 *
 * This function neuters the timer (nulls callback, sets repeat_count=0) so that
 * lv_timer_handler auto-deletes it safely on the next pass.
 */
inline void lv_timer_cancel_safe(lv_timer_t* timer) {
    if (!timer || !lv_is_initialized())
        return;
    lv_timer_set_cb(timer, nullptr);
    lv_timer_set_repeat_count(timer, 0);
    lv_timer_set_period(timer, 0);
    lv_timer_reset(timer);
}

/**
 * @brief RAII wrapper for LVGL timers - automatically deletes timer on destruction
 *
 * Handles the edge case where LVGL may be deinitialized before the timer owner,
 * preventing crashes during shutdown.
 */
class LvglTimerGuard {
  public:
    LvglTimerGuard() = default;
    explicit LvglTimerGuard(lv_timer_t* timer) : timer_(timer) {}

    ~LvglTimerGuard() {
        reset();
    }

    // Move-only (no copies)
    LvglTimerGuard(const LvglTimerGuard&) = delete;
    LvglTimerGuard& operator=(const LvglTimerGuard&) = delete;

    LvglTimerGuard(LvglTimerGuard&& other) noexcept : timer_(other.timer_) {
        other.timer_ = nullptr;
    }

    LvglTimerGuard& operator=(LvglTimerGuard&& other) noexcept {
        if (this != &other) {
            reset();
            timer_ = other.timer_;
            other.timer_ = nullptr;
        }
        return *this;
    }

    void reset(lv_timer_t* timer = nullptr) {
        if (timer_ && lv_is_initialized()) {
            lv_timer_cancel_safe(timer_);
        }
        timer_ = timer;
    }

    lv_timer_t* get() const {
        return timer_;
    }
    lv_timer_t* release() {
        lv_timer_t* t = timer_;
        timer_ = nullptr;
        return t;
    }

    explicit operator bool() const {
        return timer_ != nullptr;
    }

  private:
    lv_timer_t* timer_ = nullptr;
};

/**
 * @brief 1 Hz timer that refreshes a string subject with elapsed seconds
 *
 * For long offline phases that report no percentage (input-shaper analysis,
 * PID settling): the bar is swapped for a spinner and this timer keeps a
 * "Phase... Ns" label moving so the screen never looks frozen. Extracted from
 * InputShaperPanel's analysis elapsed timer, which was a structural twin of
 * PIDCalibrationPanel's eta timer; the PID panel is the intended follow-up
 * consumer and is deliberately not migrated in the branch that introduced
 * this class.
 *
 * Threading/lifecycle contract:
 * - begin()/cancel() must run on the main thread (they touch LVGL + subjects)
 * - cancel() neuters via lv_timer_cancel_safe(), so it is safe from inside
 *   lv_timer_handler, from a destructor, and after lv_deinit()
 * - the destructor cancels, so a member held by value cannot leave an armed
 *   timer behind
 * - elapsed time reads lv_tick_get() (the same clock the timer fires on), not
 *   a wall clock
 *
 * The unit-test harness only executes timers with a finite repeat count, and
 * this timer is periodic by design; tests lend it one through timer_for_test()
 * and restore -1 afterwards (see InputShaperPanel's analysis tests).
 */
class ElapsedLabelTimer {
  public:
    /// Formats the label for an elapsed-seconds count (e.g. "Analyzing data... 4s")
    using Formatter = std::function<std::string(uint32_t elapsed_seconds)>;

    ElapsedLabelTimer() = default;

    // A periodic lv_timer plus an lv_subject_t pointer: no meaningful move or
    // copy semantics.
    ElapsedLabelTimer(const ElapsedLabelTimer&) = delete;
    ElapsedLabelTimer& operator=(const ElapsedLabelTimer&) = delete;

    ~ElapsedLabelTimer() {
        cancel();
    }

    /**
     * @brief Arm the timer and write the first (0s) label immediately
     *
     * Idempotent: while armed, a second begin() keeps the original start stamp
     * and just refreshes the label (repeat heartbeat reports from the phase
     * must not restart the elapsed count).
     *
     * @param subject String subject the label is copied into
     * @param formatter Renders elapsed seconds to label text
     */
    void begin(lv_subject_t* subject, Formatter formatter) {
        subject_ = subject;
        formatter_ = std::move(formatter);
        if (!timer_) {
            // First report fixes the timestamp the label counts from.
            start_tick_ = lv_tick_get();
            timer_ = lv_timer_create(&ElapsedLabelTimer::on_tick, 1000, this);
        }
        format();
    }

    /**
     * @brief Stop the timer; the label keeps its last text
     */
    void cancel() {
        if (!timer_) {
            return;
        }
        // Neuter rather than delete: this can run from contexts (queued
        // callbacks, destructors) where unlinking from the timer list
        // mid-batch corrupts it. lv_timer_handler collects the neutered timer
        // on its next pass.
        lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }

    /// Seconds elapsed since begin() armed the timer (virtual LVGL clock)
    [[nodiscard]] uint32_t elapsed_seconds() const {
        return (lv_tick_get() - start_tick_) / 1000;
    }

    /// Test seam: raw handle, so the harness can lend a finite repeat count
    [[nodiscard]] lv_timer_t* timer_for_test() const {
        return timer_;
    }

  private:
    void format() {
        if (subject_ && formatter_) {
            const std::string text = formatter_(elapsed_seconds());
            lv_subject_copy_string(subject_, text.c_str());
        }
    }

    static void on_tick(lv_timer_t* timer) {
        auto* self = static_cast<ElapsedLabelTimer*>(lv_timer_get_user_data(timer));
        if (!self) {
            return;
        }
        self->format();
    }

    lv_subject_t* subject_ = nullptr;
    Formatter formatter_;
    lv_timer_t* timer_ = nullptr;
    uint32_t start_tick_ = 0;
};

} // namespace helix::ui
