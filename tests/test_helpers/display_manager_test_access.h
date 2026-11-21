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

    static void set_use_hardware_blank(DisplayManager& dm, bool use_hw) {
        dm.m_use_hardware_blank = use_hw;
    }

    static void set_use_power_off(DisplayManager& dm, bool use_power_off) {
        dm.m_use_power_off = use_power_off;
    }

    /// The size width()/height() report while no lv_display_t is attached.
    static void set_size(DisplayManager& dm, int width, int height) {
        dm.m_width = width;
        dm.m_height = height;
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
        return dm.m_flush_suppressed_for_sleep;
    }

    // Run the SAME power-off gate that DisplayManager::init() applies, against the
    // manager's currently-injected backend/backlight/hardware-blank state. Lets a
    // test prove the gate's outcome (#1049 U1 regression guard) without a full
    // init(). Mirrors the init() expression exactly — if the gate is loosened,
    // this recomputes the loosened value and the guarding test fails.
    static bool compute_use_power_off(DisplayManager& dm) {
        bool has_usable_backlight = dm.m_backlight && dm.m_backlight->is_available();
        bool backend_can_power_off = dm.m_backend && dm.m_backend->supports_power_off();
        dm.m_use_power_off = DisplayManager::should_use_power_off(
            dm.m_use_hardware_blank, has_usable_backlight, backend_can_power_off);
        return dm.m_use_power_off;
    }

    // Deletes the current m_pointer/m_keyboard and recreates them from the
    // manager's backend through the same configure_pointer() init() calls.
    static void rebuild_input_after_backend_swap(DisplayManager& dm) {
        dm.rebuild_input_after_backend_swap();
    }

    // Publishes (or clears, with nullptr) the manager DisplayManager::instance()
    // returns, for a test that never runs init().
    static void set_active_instance(DisplayManager* dm) {
        DisplayManager::set_active_instance(dm);
    }

    // Deletes the pointer device a test-driven input rebuild created.
    static void delete_pointer_input(DisplayManager& dm) {
        if (dm.m_pointer) {
            lv_indev_delete(dm.m_pointer);
            dm.m_pointer = nullptr;
        }
    }

    static void set_display_sleeping(DisplayManager& dm, bool sleeping) {
        dm.m_display_sleeping = sleeping;
    }

    static void set_display_dimmed(DisplayManager& dm, bool dimmed) {
        dm.m_display_dimmed = dimmed;
    }

    // The pointer init() builds: the backend's device, then the pointer setup that
    // installs the sleep-aware wrapper on every backend but SDL.
    static void create_input_devices(DisplayManager& dm) {
        dm.m_pointer = dm.m_backend->create_input_pointer();
        dm.watch_pointer();
        dm.configure_pointer(25, 10);
    }

    // Whether a deleted pointer is waiting to be reopened (#2069).
    static bool pointer_reopen_pending(const DisplayManager& dm) {
        return dm.m_pointer_reopen_timer.get() != nullptr;
    }

    // Runs one retry of the pending pointer reopen directly, without going
    // through lv_timer_handler(), which would also run every other timer.
    static void run_pointer_reopen_tick(DisplayManager& dm) {
        DisplayManager::pointer_reopen_tick(dm.m_pointer_reopen_timer.get());
    }

    // Whether m_pointer reads through the sleep-aware wrapper init() installs.
    static bool pointer_is_sleep_wrapped(const DisplayManager& dm) {
        return dm.m_pointer != nullptr &&
               lv_indev_get_read_cb(dm.m_pointer) == &DisplayManager::sleep_aware_read_cb;
    }

    // shutdown() sets this before lv_deinit() deletes every remaining indev.
    static void set_shutting_down(DisplayManager& dm, bool shutting_down) {
        dm.m_shutting_down = shutting_down;
    }

#ifdef HELIX_ENABLE_SCREENSAVER
    static void set_screensaver_active(DisplayManager& dm, bool active) {
        dm.m_screensaver_active = active;
    }
#endif

    // Set by sleep_aware_read_cb when a press arrives while asleep or dimmed.
    static bool wake_requested(DisplayManager& dm) {
        return dm.m_wake_requested;
    }

    static void enter_sleep(DisplayManager& dm, int timeout_sec) {
        dm.enter_sleep(timeout_sec);
    }

    // Which branch the last enter_sleep() actually took (#1245). Not the same as
    // re-running select_sleep_mechanism(): the power-off branch can degrade to the
    // overlay at runtime, so this is the only way to prove enter_sleep() honored
    // the selector instead of re-deriving the branch itself.
    static DisplayManager::SleepMechanism last_sleep_mechanism(DisplayManager& dm) {
        return dm.m_last_sleep_mechanism;
    }

    // Mirror of the Android window's FLAG_KEEP_SCREEN_ON request (#1245). Must
    // stay true for the entire lifetime of a non-Android build.
    static bool keep_screen_on(DisplayManager& dm) {
        return dm.m_keep_screen_on;
    }

    // The software sleep overlay, so a test can assert a black rect was (or was
    // NOT) painted — the whole point of #1245 is that Android stops painting one.
    static lv_obj_t* sleep_overlay(DisplayManager& dm) {
        return dm.m_sleep_overlay;
    }

    // The press cancel wake_display() runs on every pointer (#1245). Private on the
    // manager and normally only reachable through a full wake, which also repaints;
    // driven directly here so the assertions are about the cancel alone.
    static void cancel_pointer_presses() {
        DisplayManager::cancel_pointer_presses();
    }

    // Exercises the wake-side panel restore (power-on / unblank / overlay removal)
    // WITHOUT the post-wake repaint. Production wake_display() runs this then
    // lv_refr_now().
    static void restore_display_output(DisplayManager& dm) {
        dm.m_display_sleeping = false;
        dm.restore_display_output();
    }
};
