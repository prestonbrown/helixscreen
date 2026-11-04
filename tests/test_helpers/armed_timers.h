// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "misc/lv_timer_private.h"

#include <vector>

namespace helix::test {

/// Timers in LVGL's list that carry @p user_data and can still fire.
///
/// lv_timer_cancel_safe() leaves a cancelled timer linked, with its old
/// user_data, a null callback and no repeats left, until lv_timer_handler()
/// reaps it, and the test pump never runs that handler over it. An object
/// allocated later at the same address would otherwise match a dead timer.
/// Compares pointers only, so it is safe to ask about a destroyed object.
inline std::vector<lv_timer_t*> armed_timers_for(const void* user_data) {
    std::vector<lv_timer_t*> armed;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        if (t->user_data == user_data && t->timer_cb && t->repeat_count != 0) {
            armed.push_back(t);
        }
    }
    return armed;
}

inline int armed_timer_count(const void* user_data) {
    return static_cast<int>(armed_timers_for(user_data).size());
}

} // namespace helix::test
