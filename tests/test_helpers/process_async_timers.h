// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "misc/lv_timer_private.h"

#include <set>
#include <vector>

/// True while @p timer is still in LVGL's timer list.
inline bool timer_is_live(const lv_timer_t* timer) {
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        if (t == timer) {
            return true;
        }
    }
    return false;
}

namespace helix::test {

/// Run @p timer once the way lv_timer_exec() does: spend one repeat, run its
/// callback, then, once its count is spent and the callback has not deleted it,
/// delete it if it auto-deletes or pause it if its owner keeps it
/// (lib/lvgl/src/misc/lv_timer.c#lv_timer_exec). A paused timer, one with no
/// callback and one with no repeats left are not run. Returns whether it ran.
///
/// A one-shot fired by hand and left armed fires again later, and one that
/// re-arms from its own callback then doubles on every pass (#2153).
inline bool fire_timer_once(lv_timer_t* timer) {
    if (timer->paused || !timer->timer_cb || timer->repeat_count == 0) {
        return false;
    }
    if (timer->repeat_count > 0) {
        timer->repeat_count--;
    }
    timer->last_run = lv_tick_get();
    timer->timer_cb(timer);
    if (timer_is_live(timer) && timer->repeat_count == 0) {
        if (timer->auto_delete) {
            lv_timer_delete(timer);
        } else {
            lv_timer_pause(timer);
        }
    }
    return true;
}

/// The timers in LVGL's list at one moment, so a test can tell which ones
/// something it did afterwards armed.
class TimerSnapshot {
  public:
    TimerSnapshot() {
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            timers_.insert(t);
        }
    }
    bool contains(const lv_timer_t* timer) const {
        return timers_.count(const_cast<lv_timer_t*>(timer)) != 0;
    }

  private:
    std::set<lv_timer_t*> timers_;
};

/// Fire, once each, the periodic timers armed since @p before. The test pump
/// never runs a periodic timer, so a test standing in for one main-loop tick
/// fires them by hand; one-shots are left to the pump, which runs each once
/// when it comes due. A timer an earlier callback cancelled is skipped.
inline void fire_new_periodic_timers(const TimerSnapshot& before) {
    std::vector<lv_timer_t*> armed;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        if (!before.contains(t) && t->timer_cb && t->repeat_count < 0) {
            armed.push_back(t);
        }
    }
    for (lv_timer_t* t : armed) {
        if (timer_is_live(t) && t->timer_cb && t->repeat_count < 0) {
            t->timer_cb(t);
        }
    }
}

} // namespace helix::test

/// Process pending lv_async_call / lv_obj_delete_async one-shot timers.
/// Unlike lv_timer_handler(), this only fires one-shot timers and avoids the
/// infinite-loop problem with display refresh timers in the test fixture:
/// lv_timer_handler() loops on them, so tests must drive one-shots by hand.
/// Restarts from the head of the timer list after each fire — callbacks may
/// have modified the list.
///
/// Each fire spends a repeat the way lv_timer_exec() does, and a timer whose
/// count is spent is deleted unless its callback already deleted it. A timer
/// left armed would fire again on the next pass, and a callback that frees its
/// user data on the first fire reads freed memory on the second.
inline void process_async_timers() {
    for (int safety = 0; safety < 100; safety++) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && helix::test::fire_timer_once(t)) {
                fired = true;
                break; // Restart — list may have changed
            }
            t = next;
        }
        if (!fired)
            break;
    }
}
