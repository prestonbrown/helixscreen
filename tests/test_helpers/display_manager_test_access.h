// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "backlight_backend.h"
#include "display_backend.h"
#include "display_manager.h"

#include <memory>

// Test-only seam (#1049). DisplayManager declares this class as a friend, so the
// statics below can reach its private members to exercise the sleep/wake/power-off
// paths without a full init() (LVGLTestFixture already owns LVGL). Keeping these
// out of the production header satisfies the "no _for_testing methods in headers"
// lint (L065/L088).
class DisplayManagerTestAccess {
  public:
    static void set_backend(DisplayManager& dm, std::unique_ptr<DisplayBackend> backend) {
        dm.m_backend = std::move(backend);
    }

    static bool try_drm_to_fbdev_fallback(DisplayManager& dm, lv_display_rotation_t rot) {
        return dm.try_drm_to_fbdev_fallback(rot, false);
    }

    static void set_use_hardware_blank(DisplayManager& dm, bool use_hw) {
        dm.m_sleep.m_use_hardware_blank = use_hw;
    }

    static void set_use_power_off(DisplayManager& dm, bool use_power_off) {
        dm.m_sleep.m_use_power_off = use_power_off;
    }

    static void schedule_brightness_reassert(DisplayManager& dm) {
        dm.schedule_brightness_reassert();
    }

    static void apply_startup_rotation(DisplayManager& dm, const DisplayManager::Config& config) {
        dm.apply_startup_rotation(config);
    }

    /// The size width()/height() report while no lv_display_t is attached.
    static void set_size(DisplayManager& dm, int width, int height) {
        dm.m_width = width;
        dm.m_height = height;
    }

    /// The screen the resize handler is installed on, or null once it is deleted.
    static lv_obj_t* resize_screen(const DisplayManager& dm) {
        return dm.m_resize_screen;
    }

    static void set_backlight(DisplayManager& dm, std::unique_ptr<BacklightBackend> backlight) {
        dm.m_backlight = std::move(backlight);
    }

    // Inject an lv_display_t so the flush-suppression path (#1049) has a real
    // display to act on. FakePowerOffBackend::create_display() returns nullptr, so
    // without this m_display stays null and suppress_flush_for_sleep() no-ops.
    static void set_display(DisplayManager& dm, lv_display_t* disp) {
        dm.m_display = disp;
    }

    // True while the power-off flush suppression is engaged (#1049 regression
    // guard: must be set after a real power-off enter_sleep, cleared on restore).
    static bool is_flush_suppressed(DisplayManager& dm) {
        return dm.m_sleep.m_flush_suppressed_for_sleep;
    }

    // Run the SAME power-off gate that DisplayManager::init() applies, against the
    // manager's currently-injected backend/backlight/hardware-blank state. Lets a
    // test prove the gate's outcome (#1049 U1 regression guard) without a full
    // init(). Mirrors the init() expression exactly — if the gate is loosened,
    // this recomputes the loosened value and the guarding test fails.
    static bool compute_use_power_off(DisplayManager& dm) {
        bool has_usable_backlight = dm.m_backlight && dm.m_backlight->is_available();
        bool backend_can_power_off = dm.m_backend && dm.m_backend->supports_power_off();
        dm.m_sleep.m_use_power_off = helix::should_use_power_off(
            dm.m_sleep.m_use_hardware_blank, has_usable_backlight, backend_can_power_off);
        return dm.m_sleep.m_use_power_off;
    }

    static void enter_sleep(DisplayManager& dm, int timeout_sec) {
        dm.m_sleep.enter_sleep(timeout_sec);
    }

    // Creates the pointer from the manager's backend and runs init()'s input
    // setup on it (the default scroll throw/limit), without init()'s LVGL and
    // device bring-up.
    static void create_input_devices(DisplayManager& dm) {
        dm.m_pointer = dm.m_backend->create_input_pointer();
        dm.finish_input_setup(25, 10);
    }

    static void create_keyboard_input(DisplayManager& dm) {
        dm.create_keyboard_input();
    }

    // Assigns m_pointer/m_keyboard directly, without creating a device, deleting
    // one, or registering a delete watch. Lets a test move the member to a second
    // live device while the first stays alive, which no production path does.
    static void set_pointer(DisplayManager& dm, lv_indev_t* pointer) {
        dm.m_pointer = pointer;
    }
    static void set_keyboard(DisplayManager& dm, lv_indev_t* keyboard) {
        dm.m_keyboard = keyboard;
    }

    // Registers the manager's current m_pointer/m_keyboard with its
    // IndevDeleteWatch, the same call init() makes right after creating the
    // device.
    static void watch_pointer(DisplayManager& dm) {
        dm.watch_pointer();
    }
    static void watch_keyboard(DisplayManager& dm) {
        dm.watch_keyboard();
    }

    // Creates the debug-touch ripple timer against the manager's current
    // m_pointer. init() calls this once; a test drives it directly so it can
    // exist against an injected pointer without a full init().
    static lv_timer_t* install_debug_touch_timer(DisplayManager& dm) {
        return dm.install_debug_touch_timer();
    }

    // Runs the debug-touch timer's tick directly, without going through
    // lv_timer_handler(), which would also run every other timer live in the
    // process.
    static void debug_touch_tick(lv_timer_t* t) {
        DisplayManager::debug_touch_tick(t);
    }

    // Publishes (or clears, with nullptr) the manager DisplayManager::instance()
    // returns. init()/shutdown() call this as part of a full lifecycle; a test
    // that never runs init() calls it directly so a lambda that can only reach
    // state through instance() (lv_timer_create takes a plain function
    // pointer, so it cannot capture a test's manager) reaches the right one.
    static void set_active_instance(DisplayManager* dm) {
        DisplayManager::set_active_instance(dm);
    }

    // Runs the interactive rotation-detection flow directly against the
    // manager's current backend/pointer, without a full init(). The probe
    // reads m_pointer's read callback itself (poll_pointer/drain_until_release
    // in run_rotation_probe()) rather than through LVGL's own indev polling, so
    // a scripted read callback drives it exactly as a real touch would.
    static void run_rotation_probe(DisplayManager& dm) {
        dm.run_rotation_probe();
    }

    // The rotate-then-cache step init() and the rotation probe both route
    // through, against the manager's current backend and display.
    static void settle_display_rotation(DisplayManager& dm, lv_display_rotation_t rot, int phys_w,
                                        int phys_h) {
        dm.settle_display_rotation(rot, phys_w, phys_h);
    }

    // Which branch the last enter_sleep() actually took (#1245). Not the same as
    // re-running select_sleep_mechanism(): the power-off branch can degrade to the
    // overlay at runtime, so this is the only way to prove enter_sleep() honored
    // the selector instead of re-deriving the branch itself.
    static helix::SleepMechanism last_sleep_mechanism(DisplayManager& dm) {
        return dm.m_sleep.m_last_sleep_mechanism;
    }

    // Mirror of the Android window's FLAG_KEEP_SCREEN_ON request (#1245). Must
    // stay true for the entire lifetime of a non-Android build.
    static bool keep_screen_on(DisplayManager& dm) {
        return dm.m_sleep.m_keep_screen_on;
    }

    // The software sleep overlay, so a test can assert a black rect was (or was
    // NOT) painted — the whole point of #1245 is that Android stops painting one.
    static lv_obj_t* sleep_overlay(DisplayManager& dm) {
        return dm.m_sleep.m_sleep_overlay;
    }

    // Exercises the wake-side panel restore (power-on / unblank / overlay removal)
    // WITHOUT the post-wake repaint. Production wake_display() runs this then
    // lv_refr_now().
    static void restore_display_output(DisplayManager& dm) {
        dm.m_sleep.m_display_sleeping = false;
        dm.m_sleep.restore_display_output();
    }

    // The software sleep overlay pair on its own, so a repeated create or destroy
    // can be checked against the screen hold the overlay takes.
    static void create_sleep_overlay(DisplayManager& dm) {
        dm.m_sleep.create_sleep_overlay();
    }

    static void destroy_sleep_overlay(DisplayManager& dm) {
        dm.m_sleep.destroy_sleep_overlay();
    }

    // Refresh pacing as init() would have read it from the environment.
    static void set_refresh_timing(DisplayManager& dm, const helix::RefreshTiming& timing) {
        dm.m_refresh_timing = timing;
    }

    static void set_display_sleeping(DisplayManager& dm, bool sleeping) {
        dm.m_sleep.m_display_sleeping = sleeping;
    }

    static void set_display_dimmed(DisplayManager& dm, bool dimmed) {
        dm.m_sleep.m_display_dimmed = dimmed;
    }

    // Set by sleep_aware_read_cb when a press arrives while asleep or dimmed.
    static bool wake_requested(DisplayManager& dm) {
        return dm.m_sleep.m_wake_requested;
    }

    // Deletes the pointer device create_input_devices() created.
    static void delete_pointer_input(DisplayManager& dm) {
        if (dm.m_pointer) {
            lv_indev_delete(dm.m_pointer);
            dm.m_pointer = nullptr;
        }
    }

#if HELIX_ENABLE_SCREENSAVER
    // What check_display_sleep() records when it starts a screensaver.
    static void set_screensaver_active(DisplayManager& dm, bool active) {
        dm.m_sleep.m_screensaver_active = active;
    }
#endif
};
