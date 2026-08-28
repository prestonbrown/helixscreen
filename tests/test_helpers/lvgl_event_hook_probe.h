// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

/**
 * @brief Is an event hook carrying @p owner still installed on @p obj?
 *
 * Reads LVGL's event list directly. A behavioural probe cannot answer this:
 * the invariant is that the hook must be gone BEFORE anything fires it, so
 * triggering it to find out is triggering the use-after-free under test. It
 * also means the check fails in a plain build, not only under a sanitizer.
 *
 * Matching on user_data alone is what a stale hook costs: a descriptor whose
 * user_data points at a freed owner is the crash regardless of which callback
 * it names.
 */
inline bool event_hook_installed(lv_obj_t* obj, const void* owner) {
    if (obj == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc != nullptr && lv_event_dsc_get_user_data(dsc) == owner) {
            return true;
        }
    }
    return false;
}
