// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "misc/lv_timer_private.h"

/// Process pending lv_async_call / lv_obj_delete_async one-shot timers.
/// Unlike lv_timer_handler(), this only fires one-shot timers and avoids the
/// infinite-loop problem with display refresh timers in the test fixture:
/// lv_timer_handler() loops on them, so tests must drive one-shots by hand.
/// Restarts from the head of the timer list after each fire — callbacks may
/// have modified the list.
inline void process_async_timers() {
    for (int safety = 0; safety < 100; safety++) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break; // Restart — list may have changed
            }
            t = next;
        }
        if (!fired)
            break;
    }
}
