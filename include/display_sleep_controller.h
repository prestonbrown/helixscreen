// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "backlight_backend.h"
#include "display_backend.h"
#include "display_idle_decision.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <vector>

class DisplayManagerTestAccess; // NAMESPACE_OK: the test seam is a global class

namespace helix {

/// What the controller reaches on its owner. The references bind to the owner's members,
/// so a backend, backlight or display the owner replaces is the one the controller sees
/// on its next call; there is no copy to go stale.
struct DisplaySleepHost {
    const std::unique_ptr<DisplayBackend>& backend;
    const std::unique_ptr<BacklightBackend>& backlight;
    lv_display_t* const& display;
    const bool& shutting_down;
    /// Ends the press LVGL is tracking on every pointer and makes each wait for its release.
    std::function<void()> cancel_pointer_presses;
    /// Re-installs a flush callback on the display and forces a full upload.
    std::function<void(lv_display_flush_cb_t)> restore_flush_cb;
    /// Someone is watching through the remote screen server; empty means nobody can.
    std::function<bool()> remote_viewer_active;
};

/// How the panel is darkened and dimmed, resolved by the owner at init.
struct DisplaySleepConfig {
    bool use_hardware_blank = false;
    /// Real panel power-off (fbdev FB_BLANK_POWERDOWN / DRM DPMS) for devices with no
    /// hardware blank and no usable backlight. Falls back to the overlay when the backend
    /// cannot power off.
    bool use_power_off = false;
    /// False on platforms where powering the backlight off prevents wake-on-touch.
    bool sleep_backlight_off = true;
    int dim_timeout_sec = 600;
    int dim_brightness_percent = 30;
};

/**
 * @brief Dim, screensaver, sleep and wake: the idle half of the display.
 *
 * Main thread only. Owns the idle state, the software sleep overlay, the power-off flush
 * suppression and the sleep callbacks, and is the only code that blanks, powers or dims the
 * panel for idleness. The owner keeps the backend, the backlight and the input devices.
 */
class DisplaySleepController {
  public:
    explicit DisplaySleepController(DisplaySleepHost host);
    ~DisplaySleepController();

    DisplaySleepController(const DisplaySleepController&) = delete;
    DisplaySleepController& operator=(const DisplaySleepController&) = delete;

    void configure(const DisplaySleepConfig& config);

    /// One idle-check tick: reads the idle clock and applies helix::decide_idle().
    void tick();

    /// Wakes from sleep, dim or a screensaver. A no-op when already awake.
    void wake();

    /// Forces the display awake at startup whatever the previous run left behind.
    void ensure_on();

    /// Leaves the panel usable for the next process.
    void restore_on_shutdown();

    /// Stops LVGL from drawing for the rest of this process: no wake, touch or
    /// shutdown path turns the flush back on. For a host power-off already under
    /// way, so the black frame written after it holds until init stops us.
    void hold_flush_for_power_off();

    /// Drops state that points into LVGL objects lv_deinit() is about to free.
    void abandon_lvgl_state();

    void set_dim_timeout(int seconds);

    void register_callback(std::function<void(bool sleeping)> cb) {
        m_callbacks.push_back(std::move(cb));
    }

    bool is_sleeping() const {
        return m_display_sleeping;
    }
    bool is_dimmed() const {
        return m_display_dimmed;
    }
    bool uses_hardware_blank() const {
        return m_use_hardware_blank;
    }

    /// A press arrived. While asleep, dimmed or showing a screensaver it requests a wake and
    /// returns true: the press is the wake touch, which the caller hides from LVGL until it
    /// lifts (helix::WakeTouchLatch).
    bool note_press(int x, int y);

#if HELIX_ENABLE_SCREENSAVER
    /// Starts @p type as a settings preview; the next touch dismisses it without auto-lock.
    void preview_screensaver(int type);
#endif

  private:
    friend class ::DisplayManagerTestAccess;

    void enter_sleep(int timeout_sec);

    /// Undoes enter_sleep()'s panel output (unblank / power on / overlay removal), before
    /// the post-wake repaint so the framebuffer is ready when LVGL paints.
    void restore_display_output();

    /// Disables invalidation and swaps the flush callback for a no-op while the panel is
    /// powered off, so no page-flip re-asserts DPMS-on. Idempotent.
    void suppress_flush_for_sleep();

    /// Undoes suppress_flush_for_sleep(). Safe to call when nothing was suppressed.
    void restore_flush_after_sleep();

    void create_sleep_overlay();
    void destroy_sleep_overlay();

    /// Hides the active screen while the hardware blank or the host has the panel dark,
    /// and shows it again. Each is idempotent.
    void hide_screen_for_dark_panel();
    void show_screen_after_dark_panel();

    /// Asserts or releases the host window's keep-screen-on request. Android only, and
    /// transition-guarded so JNI is crossed only on a real change.
    void set_keep_screen_on(bool keep_on);

    DisplaySleepHost m_host;

    bool m_display_sleeping = false;
    bool m_display_dimmed = false;
    bool m_wake_requested = false;     // set by note_press() while asleep or dimmed
    bool m_remote_sleep_held = false;  // a remote viewer is keeping an overdue sleep at dim
    bool m_remote_viewer_seen = false; // the previous tick saw a remote viewer
    bool m_wake_from_input = false;    // wake() is running for the user's touch or activity
    int m_dim_timeout_sec = 600;
    int m_dim_brightness_percent = 30;

    bool m_use_hardware_blank = false;
    bool m_use_power_off = false;
    bool m_sleep_backlight_off = true;
    lv_obj_t* m_sleep_overlay = nullptr;
    bool m_screen_hidden_for_dark_panel = false;

    /// The branch the most recent enter_sleep() actually took, which can differ from the
    /// selector's answer: power_off() may refuse at runtime and degrade to the overlay.
    /// The Android self-wake fires only for a sleep that really handed the panel to the OS.
    SleepMechanism m_last_sleep_mechanism = SleepMechanism::SoftwareOverlay;

    /// Mirror of the Android window's FLAG_KEEP_SCREEN_ON. SDL asserts it at video init, so
    /// true is the startup truth. Always true off Android.
    bool m_keep_screen_on = true;

    /// HelixActivity.onResume() counter captured at host-sleep entry.
    int m_resume_seq_at_sleep = 0;

    lv_display_flush_cb_t m_saved_flush_cb_for_sleep = nullptr;
    bool m_flush_suppressed_for_sleep = false;
    bool m_flush_held_for_power_off = false;

#if HELIX_ENABLE_SCREENSAVER
    bool m_screensaver_active = false;
    bool m_screensaver_is_preview = false;
    /// Tick the preview started at, so the click that launched it cannot dismiss it.
    uint32_t m_preview_start_tick_ms = 0;
    /// True while a lifecycle suspend this controller requested is outstanding.
    /// NavigationManager's suspend latch has a second owner, Application's
    /// background/foreground pair, so a wake resumes only a suspend taken here.
    bool m_lifecycle_suspended = false;
#endif

    std::vector<std::function<void(bool sleeping)>> m_callbacks;
};

} // namespace helix
