// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "refresh_period_hold.h"

#include "core/update_queue.h"
#include "lvgl/src/misc/lv_timer_private.h" // lv_timer_t::period; LVGL has no period getter
#include "refresh_timing_env.h"

namespace helix {

namespace {

bool display_is_live(const lv_display_t* disp) {
    for (lv_display_t* d = lv_display_get_next(nullptr); d != nullptr; d = lv_display_get_next(d)) {
        if (d == disp) {
            return true;
        }
    }
    return false;
}

} // namespace

void RefreshPeriodHold::acquire() {
    if (m_count++ > 0) {
        return;
    }
    take_timers();
}

void RefreshPeriodHold::follow(uint32_t saver_period_ms) {
    if (m_count == 0 || saver_period_ms == 0) {
        return;
    }
    m_saver_period_ms = saver_period_ms;
    if (!lv_is_initialized()) {
        return;
    }
    if (m_display == nullptr) {
        // acquire() had no period to run at and left the timers alone.
        take_timers();
        return;
    }
    if (display_is_live(m_display)) {
        if (lv_timer_t* refr = lv_display_get_refr_timer(m_display)) {
            lv_timer_set_period(refr, saver_period_ms);
        }
    }
    if (m_saved_anim) {
        if (lv_timer_t* anim = lv_anim_get_timer()) {
            lv_timer_set_period(anim, saver_period_ms);
        }
    }
}

void RefreshPeriodHold::take_timers() {
    const uint32_t period_ms = effective_period();
    if (period_ms == 0 || !lv_is_initialized()) {
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    lv_timer_t* refr = disp != nullptr ? lv_display_get_refr_timer(disp) : nullptr;
    if (refr == nullptr) {
        return;
    }
    m_display = disp;
    m_saved_refr_period_ms = refr->period;
    lv_timer_set_period(refr, period_ms);
    if (lv_timer_t* anim = lv_anim_get_timer()) {
        m_saved_anim_period_ms = anim->period;
        m_saved_anim = true;
        lv_timer_set_period(anim, period_ms);
    }
    spdlog::debug("[RefreshPeriodHold] Refresh period {} ms -> {} ms", m_saved_refr_period_ms,
                  period_ms);
}

void RefreshPeriodHold::release() {
    if (m_count == 0 || --m_count > 0) {
        return;
    }
    restore_timers();
    m_saver_period_ms = 0;
}

void RefreshPeriodHold::rebase(const std::function<void()>& set_baseline) {
    if (m_count == 0) {
        set_baseline();
        return;
    }
    restore_timers();
    set_baseline();
    take_timers();
}

void RefreshPeriodHold::restore_timers() {
    if (m_display != nullptr && lv_is_initialized()) {
        // A deleted display took its refresh timer with it.
        if (display_is_live(m_display)) {
            if (lv_timer_t* refr = lv_display_get_refr_timer(m_display)) {
                lv_timer_set_period(refr, m_saved_refr_period_ms);
            }
        }
        if (m_saved_anim) {
            if (lv_timer_t* anim = lv_anim_get_timer()) {
                lv_timer_set_period(anim, m_saved_anim_period_ms);
            }
        }
    }
    m_display = nullptr;
    m_saved_anim = false;
}

RefreshPeriodHold& active_refresh_period_hold() {
    static RefreshPeriodHold hold;
    return hold;
}

void apply_refresh_timing(const RefreshTiming& timing) {
    RefreshPeriodHold& hold = active_refresh_period_hold();
    hold.set_period(timing.screensaver_refr_period_ms);
    hold.set_loop_min_sleep(timing.screensaver_loop_min_sleep_ms);
    if (!lv_is_initialized()) {
        return;
    }
    const uint32_t period = timing.refr_period_ms;
    // A running screensaver keeps its own period; the global one becomes what it restores.
    hold.rebase([period] {
        if (period == 0) {
            return;
        }
        if (lv_display_t* disp = lv_display_get_default()) {
            if (lv_timer_t* refr = lv_display_get_refr_timer(disp)) {
                lv_timer_set_period(refr, period);
            }
        }
        if (lv_timer_t* anim = lv_anim_get_timer()) {
            lv_timer_set_period(anim, period);
        }
    });
    if (period == 0 || !timing.scope_all) {
        return;
    }
    // Looked up afresh on every call: devices come and go (unplug, hot-plug).
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_timer_t* read = lv_indev_get_read_timer(indev)) {
            lv_timer_set_period(read, period);
        }
    }
    if (lv_timer_t* queue = ui::UpdateQueue::instance().timer()) {
        lv_timer_set_period(queue, period);
    }
}

} // namespace helix
