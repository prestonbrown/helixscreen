// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "lvgl.h"

namespace helix::nav {

// "Loading..." pill on the TOP layer, painted before a (possibly multi-second)
// first build of a panel. STATIC label, NOT a spinner: the build blocks the LVGL
// thread, so no animation timer can run and a spinner would freeze and read as a
// hang. Small on purpose: painting or lifting a full-screen scrim re-renders the
// whole screen, which costs ~600ms on the ESP32; the pill costs a few dozen. Taps
// need no absorbing: touch is polled on the thread the build blocks.
lv_obj_t* make_loading_scrim();

// Back to the resting transform and opacity, so a hidden overlay shows up
// correctly the next time it is reused. Written unconditionally: reading the
// current transform and skipping the write when it looks clean leaves stale
// scale values behind under SDL's logical scaling on Android, which corrupts the
// display, and the four writes cost less than that.
void reset_overlay_transform(lv_obj_t* obj);

#if defined(HELIX_PLATFORM_ESP32)
// RAII busy indicator wrapping a panel transition (the deferred first-build now
// runs UNDER this scrim — one mechanism, not two). ctor shows the scrim and
// forces it to paint BEFORE the blocking transition body (the LVGL thread is
// about to block; a state that only appears after is useless). dtor paints the
// now-un-hidden new panel under the scrim, then lifts the scrim with
// safe_delete_deferred, an LVGL async delete: the transition can run inside a
// queued callback, where a sync delete can corrupt LVGL's event list (#776). The
// new panel is painted before the scrim lifts, so there is no old-panel flash.
// The async delete runs in the same or the next lv_timer_handler() pass, not at
// an UpdateQueue drain. Only the OUTERMOST transition owns a
// scrim — switch_to_panel_impl can cascade into handle_active_panel_change via
// the active_panel subject, and we must not nest two.
class NavTransitionScrim {
  public:
    // `needed`: the transition has a panel to build. Switching between built
    // panels only flips visibility, so a scrim would add two forced full renders
    // for nothing.
    NavTransitionScrim(bool& active, bool needed);
    ~NavTransitionScrim();
    NavTransitionScrim(const NavTransitionScrim&) = delete;
    NavTransitionScrim& operator=(const NavTransitionScrim&) = delete;

  private:
    bool& active_;
    bool owns_;
    helix::ui::WidgetRef scrim_;
};
#endif

} // namespace helix::nav
