// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/timer_guard.h"
#include "hold_repeat.h"

#include <lvgl.h>

namespace helix {

/** LVGL binding for HoldRepeat: one short-period lv_timer that polls the pure
 *  state machine and invokes the owner's fire callback on each repeat.
 *
 *  Both hold-to-repeat owners (the jog pad and the motion panel's Z buttons)
 *  share this class, so no timing logic exists twice; the poll period only
 *  has to divide both DELAY_MS and kIntervalMs for the repeats to land on a
 *  poll. Timing decisions themselves all live in HoldRepeat.
 *
 *  Lifecycle: begin() on press, release() on finger-up, cancel() on press
 *  lost or teardown, stop_ticking() when the repeated action itself refuses
 *  to continue (jog refused or clamped to zero). stop_ticking() keeps the
 *  swallow state, so the CLICKED that follows a refused hold is still
 *  swallowed.
 *
 *  poll() is public so tests can drive the repeat without a running periodic
 *  timer: the unit-test harness never executes periodic lv_timers.
 */
class HoldRepeatTimer {
  public:
    /// Runs one repeat; returns false when the owner cannot keep repeating
    /// (its jog was refused or fully clamped).
    using FireFn = bool (*)(void* user_data);

    /// Arm on press. Re-arms cleanly: any previous hold is replaced.
    void begin(FireFn fire, void* user_data) {
        fire_ = fire;
        user_data_ = user_data;
        hold_.press();
        press_tick_ = lv_tick_get();
        stop_timer();
        timer_ = lv_timer_create(&HoldRepeatTimer::on_timer, kPollMs, this);
    }

    /// Finger up: ticks stop; a fired repeat still swallows the click.
    void release() {
        hold_.release();
        stop_timer();
    }

    /// Press lost or teardown: ticks stop and the repeat is forgotten.
    void cancel() {
        hold_.cancel();
        stop_timer();
    }

    /// Halt repeats without touching the swallow state.
    void stop_ticking() {
        stop_timer();
    }

    /// One poll at ms-since-press: fires via the callback when the state
    /// machine says to, and halts ticking when the callback refuses. Returns
    /// whether a repeat fired.
    bool poll(uint32_t ms_since_press) {
        if (!timer_) {
            return false;
        }
        if (!hold_.on_elapsed(ms_since_press)) {
            return false;
        }
        if (fire_ && !fire_(user_data_)) {
            stop_timer();
        }
        return true;
    }

    bool ticking() const {
        return timer_ != nullptr;
    }

    bool swallow_click() const {
        return hold_.swallow_click();
    }

    /// Neuter the lv_timer so lv_timer_handler collects it: safe after
    /// lv_deinit() and from inside a timer callback. The pad's state struct
    /// is lv_free()d without running destructors, so its delete callback must
    /// call this explicitly.
    void teardown() {
        stop_timer();
    }

    ~HoldRepeatTimer() {
        teardown();
    }

    // Periodic by design; the test harness lends no pump, tests use poll().
    HoldRepeatTimer() = default;
    HoldRepeatTimer(const HoldRepeatTimer&) = delete;
    HoldRepeatTimer& operator=(const HoldRepeatTimer&) = delete;

  private:
    static void on_timer(lv_timer_t* timer) {
        auto* self = static_cast<HoldRepeatTimer*>(lv_timer_get_user_data(timer));
        if (self) {
            self->poll(lv_tick_elaps(self->press_tick_));
        }
    }

    void stop_timer() {
        if (timer_) {
            ui::lv_timer_cancel_safe(timer_);
            timer_ = nullptr;
        }
    }

    /// 50 ms divides both 400 and 150, so repeats land exactly on a poll.
    static constexpr uint32_t kPollMs = 50;

    HoldRepeat hold_;
    FireFn fire_ = nullptr;
    void* user_data_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t press_tick_ = 0;
};

} // namespace helix
