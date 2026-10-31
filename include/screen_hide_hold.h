// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include <lvgl.h>

namespace helix {

class ScreenHideHoldTestAccess;

/**
 * @brief Reference-counted hold that keeps a screen hidden under an opaque overlay
 *
 * A full-screen opaque overlay on lv_layer_top() does not stop LVGL redrawing the
 * panel beneath it: the cover search walks only the active screen, and the top
 * layer is drawn on top of whatever that search found. A hidden screen is skipped
 * by refresh, and invalidations from its descendants are dropped, so hiding it
 * removes that cost. Hiding the screen object does not reflow its children.
 *
 * - The first acquire() records the screen and hides it. A screen that is already
 *   hidden is left as it is and never unhidden by this hold.
 * - Nested acquire() calls only count; the screen they pass is ignored.
 * - The final release() unhides only the screen the first acquire() hid, and only
 *   while it is still alive. A screen loaded mid-hold is never touched.
 * - release() without a matching acquire() does nothing.
 *
 * Defined in display_manager.cpp, which every build compiles; the screensaver
 * sources that also take this hold are optional. Main thread only.
 */
class ScreenHideHold {
  public:
    void acquire(lv_obj_t* screen);
    void release();

    /// True between the first acquire() and the final release().
    bool is_held() const {
        return m_count > 0;
    }

  private:
    friend class ScreenHideHoldTestAccess;

    /// Unhides the screen the first acquire() hid, if it is still alive, and
    /// forgets it.
    void show_hidden_screen();

    helix::ui::WidgetRef m_screen;
    bool m_hid_screen = false;
    int m_count = 0;
};

/// The hold shared by every opaque full-screen overlay (screensavers, software sleep).
ScreenHideHold& active_screen_hide_hold();

} // namespace helix
