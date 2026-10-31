// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screen_hide_hold.h"

// Unhiding a screen marks its parent's layout dirty, and a screen has no parent. The
// LVGL patch that guards that call defines this marker in lv_obj.h.
#if !defined(HELIX_LV_OBJ_FLAG_SCREEN_PARENT_GUARD)
#error "lib/lvgl lacks lvgl_obj_flag_screen_parent_null_guard.patch: unhiding a screen derefs NULL"
#endif

namespace helix {

void ScreenHideHold::acquire(lv_obj_t* screen) {
    if (m_count++ > 0) {
        return;
    }
    m_screen = screen;
    m_hid_screen = screen != nullptr && !lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN);
    if (m_hid_screen) {
        // DECLARATIVE_OK: screen hidden under an opaque top-layer overlay
        lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    }
}

void ScreenHideHold::release() {
    if (m_count == 0 || --m_count > 0) {
        return;
    }
    show_hidden_screen();
}

void ScreenHideHold::show_hidden_screen() {
    if (m_hid_screen && m_screen && lv_is_initialized()) {
        // DECLARATIVE_OK: screen hidden under an opaque top-layer overlay
        lv_obj_remove_flag(m_screen, LV_OBJ_FLAG_HIDDEN);
    }
    m_screen = nullptr;
    m_hid_screen = false;
}

ScreenHideHold& active_screen_hide_hold() {
    static ScreenHideHold hold;
    return hold;
}

} // namespace helix
