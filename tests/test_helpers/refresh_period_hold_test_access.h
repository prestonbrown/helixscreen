// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/update_queue.h"
#include "lvgl/src/misc/lv_timer_private.h"
#include "refresh_period_hold.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace helix {

// Test-only seam. The shared hold is a process-wide static that no production path
// resets, so a test that fails between acquire and release would leave the display
// refreshing at the fast period, and configured, for every test after it.
class RefreshPeriodHoldTestAccess {
  public:
    // Drops every outstanding acquire, puts back the timers the hold changed under the
    // same checks as release(), and forgets the configured period.
    static void reset(RefreshPeriodHold& hold) {
        hold.m_count = 0;
        hold.restore_timers();
        hold.m_period_ms = 0;
        hold.m_saver_period_ms = 0;
    }
};

inline uint32_t default_refr_timer_period() {
    lv_display_t* disp = lv_display_get_default();
    const lv_timer_t* refr = disp ? lv_display_get_refr_timer(disp) : nullptr;
    return refr ? refr->period : 0;
}

inline uint32_t anim_timer_period() {
    const lv_timer_t* anim = lv_anim_get_timer();
    return anim ? anim->period : 0;
}

/// Puts back, however the test exits, the period of every timer apply_refresh_timing() can
/// change, as each was at construction: the default display's refresh timer, the animation
/// timer, the UpdateQueue drain timer, and the read timer of every input device that existed
/// then. Input devices outlive tests (UITest's virtual pointer is never deleted), so a read
/// period left set would reach every later test in the process.
class ScopedTimerPeriods {
  public:
    ScopedTimerPeriods()
        : display_(lv_display_get_default()), refr_(default_refr_timer_period()),
          anim_(anim_timer_period()), drain_timer_(ui::UpdateQueue::instance().timer()),
          drain_(drain_timer_ != nullptr ? drain_timer_->period : 0) {
        for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
             indev = lv_indev_get_next(indev)) {
            if (const lv_timer_t* read = lv_indev_get_read_timer(indev)) {
                reads_.emplace_back(indev, read->period);
            }
        }
    }

    ~ScopedTimerPeriods() {
        if (lv_timer_t* refr = display_ ? lv_display_get_refr_timer(display_) : nullptr) {
            lv_timer_set_period(refr, refr_);
        }
        if (lv_timer_t* anim = lv_anim_get_timer()) {
            lv_timer_set_period(anim, anim_);
        }
        // A queue shut down and started again mid-test has a new timer at its default period.
        if (drain_timer_ != nullptr && ui::UpdateQueue::instance().timer() == drain_timer_) {
            lv_timer_set_period(drain_timer_, drain_);
        }
        // Only devices still registered are touched: a deleted one took its timer with it.
        for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
             indev = lv_indev_get_next(indev)) {
            for (const auto& [saved, period] : reads_) {
                if (saved != indev) {
                    continue;
                }
                if (lv_timer_t* read = lv_indev_get_read_timer(indev)) {
                    lv_timer_set_period(read, period);
                }
            }
        }
    }

    ScopedTimerPeriods(const ScopedTimerPeriods&) = delete;
    ScopedTimerPeriods& operator=(const ScopedTimerPeriods&) = delete;

    /// Sets both timers for the rest of the test.
    static void set(uint32_t refr_ms, uint32_t anim_ms) {
        lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), refr_ms);
        lv_timer_set_period(lv_anim_get_timer(), anim_ms);
    }

  private:
    lv_display_t* display_;
    uint32_t refr_;
    uint32_t anim_;
    lv_timer_t* drain_timer_;
    uint32_t drain_;
    std::vector<std::pair<lv_indev_t*, uint32_t>> reads_;
};

} // namespace helix
