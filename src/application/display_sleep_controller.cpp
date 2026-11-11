// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_sleep_controller.h"

#include "ui_lock_screen.h"
#include "ui_nav_manager.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "display/lv_display_private.h" // flush_cb access
#include "display_awake_hold.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lock_manager.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "screen_hide_hold.h"

#if HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL_system.h>
#include <jni.h>

// ---------------------------------------------------------------------------
// JNI bridge to HelixActivity's window flags (#1245)
//
// Mirrors android_set_navbar_always_visible() in display_settings_manager.cpp —
// same guard shape, same ExceptionClear() on every failure path, and the same
// shared helix_activity_class() for class resolution. It lives HERE rather than
// being exported from display_settings_manager.h because DisplaySleepController is the
// only caller: which mechanism cuts the panel is display-output policy, not a
// persisted setting.
// Putting an Android-only declaration in the settings header to reach it would
// file the API under the wrong owner. There is exactly one copy of each helper.
//
// Note we do NOT use SDL_EnableScreenSaver()/SDL_DisableScreenSaver(), which
// reach the same window flag via COMMAND_SET_KEEP_SCREEN_ON: they early-return
// when SDL's cached suspend_screensaver already matches, so a re-assert after
// Android recreates the window is silently dropped. HelixActivity keeps the
// desired state in a static and re-applies it from onResume(), which is the
// behaviour we actually need.
// ---------------------------------------------------------------------------

/// Ask HelixActivity to add/clear FLAG_KEEP_SCREEN_ON (applied on the UI thread).
static void android_set_keep_screen_on(bool keep_on) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;

    // Cached global ref owned by helix_activity_class() — never released here.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return;

    jmethodID method = env->GetStaticMethodID(cls, "setKeepScreenOn", "(Z)V");
    if (!method) {
        env->ExceptionClear();
        return;
    }

    env->CallStaticVoidMethod(cls, method, static_cast<jboolean>(keep_on));
}

/// Read HelixActivity's onResume counter. Returns 0 when the bridge is
/// unavailable; both the sleep-entry capture and the idle poll go through this
/// same function, so a bridge that is uniformly broken reads 0 == 0 and simply
/// never self-wakes. A bridge that breaks *between* the two reads costs one
/// spurious wake, which is the harmless direction (the panel is already lit).
static int android_get_resume_seq() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return 0;

    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return 0;

    jmethodID method = env->GetStaticMethodID(cls, "getResumeSeq", "()I");
    if (!method) {
        env->ExceptionClear();
        return 0;
    }

    jint seq = env->CallStaticIntMethod(cls, method);
    return static_cast<int>(seq);
}
#endif // __ANDROID__

namespace helix {

DisplaySleepController::DisplaySleepController(DisplaySleepHost host) : m_host(std::move(host)) {}

// A controller that slept without LVGL teardown abandoning its state still owns a
// full-screen black overlay on the top layer and a hold that hides the active screen.
DisplaySleepController::~DisplaySleepController() {
    if (lv_is_initialized()) {
        destroy_sleep_overlay();
        show_screen_after_dark_panel();
    }
}

void DisplaySleepController::configure(const DisplaySleepConfig& config) {
    m_use_hardware_blank = config.use_hardware_blank;
    m_use_power_off = config.use_power_off;
    m_sleep_backlight_off = config.sleep_backlight_off;
    m_dim_timeout_sec = config.dim_timeout_sec;
    m_dim_brightness_percent = config.dim_brightness_percent;
}

// ============================================================================
// Sleep entry
// ============================================================================

void DisplaySleepController::enter_sleep(int timeout_sec) {
    DisplayBackend* backend = m_host.backend.get();
    BacklightBackend* backlight = m_host.backlight.get();
#if HELIX_ENABLE_SCREENSAVER
    // Stop screensaver before entering full sleep
    if (m_screensaver_active) {
        ScreensaverManager::instance().stop();
        m_screensaver_active = false;
    }
#endif
    m_display_sleeping = true;

    SleepMechanism mechanism =
        helix::select_sleep_mechanism(helix::platform_is_android(), m_use_hardware_blank,
                                      m_use_power_off && backend != nullptr, timeout_sec);

    switch (mechanism) {
    case SleepMechanism::HardwareBlank:
        if (backend) {
            backend->blank_display();
        }
        hide_screen_for_dark_panel();
        break;

    case SleepMechanism::PanelPowerOff:
        // Real panel power-off (fbdev FB_BLANK_POWERDOWN / DRM DPMS off) for
        // HDMI/fbdev devices with no hardware backlight blank (#1049). The panel
        // is actually powered down, so no software overlay is needed. wake()
        // restores power BEFORE lv_refr_now() to honor the #303 wake-race.
        if (backend->power_off()) {
            // Neutralize the flush so the next page-flip can't re-assert DPMS-on
            // and relight the panel on the home screen. Stopping the screensaver
            // above, or any later Klipper-driven invalidation, renders a frame
            // whose DRM commit turns the connector back ON.
            suppress_flush_for_sleep();
        } else {
            // The capability probe disagreed with reality; degrade to the overlay
            // rather than leaving a lit panel with no visual sleep at all.
            mechanism = SleepMechanism::SoftwareOverlay;
            create_sleep_overlay();
        }
        break;

    case SleepMechanism::HostSleep:
        // Android (#1245): no backlight sysfs and no backend blank/power-off, so
        // the only way to genuinely darken the panel is to stop asserting
        // FLAG_KEEP_SCREEN_ON and let Android's own display timeout run. No
        // overlay: a lit black rect would keep the panel on and block the device
        // from ever sleeping, and the app is paused along with the panel.
        //
        // The resume counter tells the pause/resume round trip that follows apart
        // from "still waiting for Android's timeout".
#ifdef __ANDROID__
        m_resume_seq_at_sleep = android_get_resume_seq();
#endif
        set_keep_screen_on(false);
        hide_screen_for_dark_panel();
        break;

    case SleepMechanism::SoftwareOverlay:
        // Software overlay path: do NOT call FBIOBLANK. The overlay alone is
        // sufficient, and FBIOBLANK can race on wake: the framebuffer isn't ready
        // before LVGL renders, leaving a black screen even after the overlay is
        // removed (#303).
        create_sleep_overlay();
        break;
    }
    m_last_sleep_mechanism = mechanism;

    if (backlight && backlight->is_available() && m_sleep_backlight_off) {
        backlight->set_brightness(0);
    }
    spdlog::info("[DisplaySleep] Display sleeping ({}{}) after {}s",
                 helix::sleep_mechanism_name(mechanism),
                 m_sleep_backlight_off ? "" : ", backlight kept on", timeout_sec);

    // Notify subscribers (camera stream, etc.) to suspend background work
    for (auto& cb : m_callbacks) {
        cb(true);
    }
}

// ============================================================================
// Host keep-screen-on (Android, #1245)
// ============================================================================

void DisplaySleepController::set_keep_screen_on(bool keep_on) {
#ifdef __ANDROID__
    if (m_keep_screen_on == keep_on) {
        return; // transition-guarded: don't cross JNI to re-say the same thing
    }
    m_keep_screen_on = keep_on;
    android_set_keep_screen_on(keep_on);
    spdlog::info("[DisplaySleep] Android keep-screen-on: {}", keep_on);
#else
    // Nothing else runs a display timeout behind our back — we own the panel on
    // every non-Android target, so the flag has no meaning and stays asserted.
    (void)keep_on;
#endif
}

// ============================================================================
// Software sleep overlay
// ============================================================================

void DisplaySleepController::create_sleep_overlay() {
    if (m_sleep_overlay) {
        return;
    }
    m_sleep_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m_sleep_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(m_sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_sleep_overlay, 0, 0);
    lv_obj_set_style_pad_all(m_sleep_overlay, 0, 0);
    lv_obj_remove_flag(m_sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    helix::active_screen_hide_hold().acquire(lv_screen_active());
    spdlog::debug("[DisplaySleep] Software sleep overlay created");
}

void DisplaySleepController::destroy_sleep_overlay() {
    if (!m_sleep_overlay) {
        return;
    }
    lv_obj_delete(m_sleep_overlay);
    m_sleep_overlay = nullptr;
    helix::active_screen_hide_hold().release();
    spdlog::debug("[DisplaySleep] Software sleep overlay destroyed");
}

// A panel the controller or the OS has darkened still receives every frame LVGL draws.
// Hiding the active screen drops those invalidations and skips the drawing.
void DisplaySleepController::hide_screen_for_dark_panel() {
    if (m_screen_hidden_for_dark_panel) {
        return;
    }
    helix::active_screen_hide_hold().acquire(lv_screen_active());
    m_screen_hidden_for_dark_panel = true;
}

void DisplaySleepController::show_screen_after_dark_panel() {
    if (!m_screen_hidden_for_dark_panel) {
        return;
    }
    m_screen_hidden_for_dark_panel = false;
    helix::active_screen_hide_hold().release();
}

// ============================================================================
// Power-off flush suppression (#1049)
// ============================================================================

namespace {
// No-op flush used while the panel is powered off: it must still signal "ready"
// or LVGL stalls waiting for the flush to complete, but it commits nothing to the
// framebuffer — so no DRM page-flip happens to re-assert DPMS-on.
void sleep_noop_flush_cb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px*/) {
    lv_display_flush_ready(disp);
}
} // namespace

void DisplaySleepController::suppress_flush_for_sleep() {
    lv_display_t* display = m_host.display;
    if (m_flush_suppressed_for_sleep || !display) {
        return;
    }
    // Stop invalidations from scheduling renders AND swap the flush callback for a
    // no-op. Pausing the refresh timer alone is insufficient: any invalidation
    // fires LV_EVENT_REFR_REQUEST, which resumes the timer (see the identical note
    // in Application's splash suppression). With the flush neutralized, even a
    // render that slips through commits nothing, so the powered-off panel can't be
    // relit by a stray page-flip (#1049).
    lv_display_enable_invalidation(display, false);
    m_saved_flush_cb_for_sleep = display->flush_cb;
    lv_display_set_flush_cb(display, sleep_noop_flush_cb);
    m_flush_suppressed_for_sleep = true;
    // Nothing on the screen is drawn now, so it is hidden: widgets that wait on
    // their own draw (the G-code viewer's stall watchdog) read it as not visible.
    helix::active_screen_hide_hold().acquire(lv_screen_active());
    spdlog::debug("[DisplaySleep] Flush suppressed while panel powered off");
}

void DisplaySleepController::hold_flush_for_power_off() {
    suppress_flush_for_sleep();
    m_flush_held_for_power_off = true;
}

void DisplaySleepController::restore_flush_after_sleep() {
    if (!m_flush_suppressed_for_sleep || m_flush_held_for_power_off) {
        return;
    }
    m_flush_suppressed_for_sleep = false;
    if (m_host.display) {
        m_host.restore_flush_cb(m_saved_flush_cb_for_sleep);
        lv_display_enable_invalidation(m_host.display, true);
    }
    m_saved_flush_cb_for_sleep = nullptr;
    helix::active_screen_hide_hold().release();
    spdlog::debug("[DisplaySleep] Flush restored on wake");
}

// ============================================================================
// Idle tick
// ============================================================================

void DisplaySleepController::tick() {
    BacklightBackend* backlight = m_host.backlight.get();
#if HELIX_ENABLE_SCREENSAVER
    ScreensaverManager::instance().on_idle_check_tick();
    // HELIX_SCREENSAVER_NOW: start a screensaver on the first tick. A registered saver name
    // picks that saver; any other value the configured one, or flying toasters.
    static bool screensaver_force_checked = false;
    if (!screensaver_force_checked) {
        screensaver_force_checked = true;
        const char* env = std::getenv("HELIX_SCREENSAVER_NOW");
        if (env) {
            const ScreensaverType force_type =
                helix::ui::resolve_screensaver_now(env, ScreensaverManager::configured_type());
            spdlog::info("[DisplaySleep] HELIX_SCREENSAVER_NOW={}, forcing screensaver type {}",
                         env, static_cast<int>(force_type));
            m_display_dimmed = true;
            ScreensaverManager::instance().start(force_type);
            m_screensaver_active = true;
            return;
        }
    }
#endif

    // If sleep-while-printing is disabled, inhibit *entering* sleep/dim during
    // active prints. We still need to honor wake-from-sleep touches: the
    // display may have entered sleep BEFORE the print started, and bailing out
    // here would strand the user on a blank screen for the duration of the
    // print.
    bool inhibit_sleep_entry = false;
    if (!DisplaySettingsManager::instance().get_sleep_while_printing()) {
        // Lifecycle: a user who turned off sleep-while-printing wants the
        // screen up through the pre-print homing too, which is when they are
        // most likely to be watching.
        const auto lifecycle = get_printer_state().print_state().get_print_lifecycle();
        if (job_holds_machine(lifecycle)) {
            // Reset LVGL activity timer so we don't immediately sleep when print ends
            lv_display_trigger_activity(nullptr);
            inhibit_sleep_entry = true;
        }
    }
    // A view waiting on the user mid-operation holds the screen up regardless.
    if (helix::display_awake_hold().is_held()) {
        lv_display_trigger_activity(nullptr);
        inhibit_sleep_entry = true;
    }

    // Get configured sleep timeout from settings (0 = disabled)
    int sleep_timeout_sec = DisplaySettingsManager::instance().get_display_sleep_sec();

    // Get LVGL inactivity time (milliseconds since last touch/input)
    uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);

    // Periodic debug logging (every 30 seconds when inactive > 10s)
    static uint32_t last_log_time = 0;
    uint32_t now = DisplayManager::get_ticks();
    if (inactive_ms > 10000 && (now - last_log_time) >= 30000) {
        spdlog::trace(
            "[DisplaySleep] Sleep check: inactive={}s, dim_timeout={}s, sleep_timeout={}s, "
            "dimmed={}, sleeping={}, backlight={}",
            inactive_ms / 1000, m_dim_timeout_sec, sleep_timeout_sec, m_display_dimmed,
            m_display_sleeping, backlight ? "yes" : "no");
        last_log_time = now;
    }

    // Android host sleep (#1245): Android pauses the app when it powers the panel
    // down and resumes it when the panel comes back, and neither transition is a
    // touch — so the activity check below never fires and the display would stay
    // logically asleep with keep-screen-on still cleared, re-sleeping forever and
    // never resuming the sleep callbacks. This function only runs while
    // foregrounded (the run loop short-circuits on m_backgrounded), so a bumped
    // resume counter means the panel is on again.
    //
    // The awake case is the matching invariant: the host must never be left free
    // to sleep while we consider the display awake, whatever path cleared
    // m_display_sleeping. set_keep_screen_on() is transition-guarded, so an awake
    // tick costs one member compare.
    bool resumed_from_host_sleep = false;
    if (!m_display_sleeping) {
        set_keep_screen_on(true);
    }
#ifdef __ANDROID__
    else if (m_last_sleep_mechanism == SleepMechanism::HostSleep) {
        resumed_from_host_sleep =
            host_sleep_needs_wake(true, m_resume_seq_at_sleep, android_get_resume_seq());
    }
#endif

    helix::IdleInputs in;
    in.state = m_display_sleeping ? helix::IdleState::Sleeping
               : m_display_dimmed ? helix::IdleState::Dimmed
                                  : helix::IdleState::Awake;
    in.inactive_ms = inactive_ms;
    in.dim_timeout_sec = m_dim_timeout_sec;
    in.sleep_timeout_sec = sleep_timeout_sec;
    in.inhibit_entry = inhibit_sleep_entry;
    in.can_dim = backlight && backlight->supports_dimming();
    in.wake_requested = m_wake_requested;
    in.host_resumed = resumed_from_host_sleep;
    in.remote_viewer = m_host.remote_viewer_active && m_host.remote_viewer_active();
    in.remote_viewer_arrived = in.remote_viewer && !m_remote_viewer_seen;
    m_remote_viewer_seen = in.remote_viewer;
#if HELIX_ENABLE_SCREENSAVER
    in.has_screensaver = ScreensaverManager::configured_type() != ScreensaverType::OFF;
    in.saver_running = m_screensaver_active;
    in.is_preview = m_screensaver_is_preview;
    in.preview_elapsed_ms = DisplayManager::get_ticks() - m_preview_start_tick_ms;
#endif

    // Wake on a request from sleep_aware_read_cb (embedded) or on LVGL activity (SDL, where
    // the wrapper is not installed because it breaks mouse device identification).
    // Two-stage idle (#1049): Dim lowers the backlight and/or starts the screensaver,
    // Sleep blanks or powers off. Both timeouts run from the same idle clock.
    const helix::IdleDecision decision = helix::decide_idle(in);
    if (decision.sleep_held != m_remote_sleep_held) {
        m_remote_sleep_held = decision.sleep_held;
        if (decision.sleep_held) {
            // Holding at dim needs something to dim with, or a display already dimmed.
            const bool can_hold_dim =
                in.state == helix::IdleState::Dimmed || in.can_dim || in.has_screensaver;
            spdlog::info("[DisplaySleep] Remote screen viewer present: holding {} instead of "
                         "sleeping after {}s",
                         can_hold_dim ? "at dim" : "awake", inactive_ms / 1000);
        } else {
            spdlog::info("[DisplaySleep] Remote screen sleep hold released ({})",
                         in.remote_viewer ? "display active again" : "viewer gone");
        }
    }

    switch (decision.action) {
    case helix::IdleAction::None:
        break;
    case helix::IdleAction::Wake:
        if (in.remote_viewer_arrived && in.state == helix::IdleState::Sleeping) {
            spdlog::info("[DisplaySleep] Remote screen viewer opened the screen; waking display");
        }
        m_wake_requested = false;
        m_wake_from_input = true;
        wake();
        m_wake_from_input = false;
        break;
    case helix::IdleAction::Sleep:
        enter_sleep(sleep_timeout_sec);
        break;
    case helix::IdleAction::StartSaver:
#if HELIX_ENABLE_SCREENSAVER
        m_display_dimmed = true;
        // Suspend the active panel lifecycle so widget timers (clock, etc.) do not
        // invalidate the UI underneath the screensaver.
        NavigationManager::instance().suspend_active();
        m_lifecycle_suspended = true;
        ScreensaverManager::instance().start(ScreensaverManager::configured_type());
        m_screensaver_active = true;
        if (backlight) {
            // The screensaver needs enough brightness to be seen, but a higher dim
            // setting wins.
            backlight->set_brightness(std::max(m_dim_brightness_percent, 50));
        }
        if (decision.sleep_held) {
            spdlog::info("[DisplaySleep] Screensaver started in place of sleep (remote viewer)");
        } else {
            spdlog::info("[DisplaySleep] Screensaver started after {}s inactivity",
                         m_dim_timeout_sec);
        }
#endif
        break;
    case helix::IdleAction::Dim:
        m_display_dimmed = true;
        if (backlight) {
            backlight->set_brightness(m_dim_brightness_percent);
        }
        if (decision.sleep_held) {
            spdlog::info("[DisplaySleep] Display dimmed to {}% in place of sleep (remote viewer)",
                         m_dim_brightness_percent);
        } else {
            spdlog::info("[DisplaySleep] Display dimmed to {}% after {}s inactivity",
                         m_dim_brightness_percent, m_dim_timeout_sec);
        }
        break;
    }
}

void DisplaySleepController::restore_display_output() {
    DisplayBackend* backend = m_host.backend.get();

    // Re-assert the host's keep-screen-on request first (#1245). Unconditional and
    // transition-guarded: if enter_sleep() handed the panel to Android we take it
    // back here, and on every other path (including all non-Android targets) this
    // is a no-op because the flag was never released.
    set_keep_screen_on(true);

    // Rendering comes back first, and unconditionally. enter_sleep() engages the
    // suppression on its own, so undoing it must not depend on which panel mechanism is
    // still available now: a backend that has gone away since would fall to the branch
    // below that only removes the overlay, leaving the no-op flush installed. LVGL then
    // keeps painting into it and the panel holds its last frame for good, while every
    // later wake reports success. No-op when suppression was never engaged.
    restore_flush_after_sleep();

    // Undo whatever enter_sleep() did to the panel output, mirroring its branches.
    // This must run BEFORE the post-wake lv_refr_now() (#303 wake-race).
    if (m_use_hardware_blank) {
        // Hardware path: unblank framebuffer (FBIOBLANK was used during sleep).
        if (backend) {
            backend->unblank_display();
        }
    } else if (m_use_power_off && backend) {
        // Power-off path: re-enable rendering, then power the panel back on. The
        // flush must be restored BEFORE the post-wake lv_refr_now() (in
        // wake()) so that synchronous render actually reaches the panel.
        // A software overlay may also exist if a prior power_off() failed and fell
        // back — remove it defensively. restore_flush_after_sleep() is a no-op if
        // suppression was never engaged (overlay fallback).
        restore_flush_after_sleep();
        backend->power_on();
        destroy_sleep_overlay();
    } else {
        // Software path: remove the black overlay (no FBIOBLANK to undo).
        destroy_sleep_overlay();
    }
    show_screen_after_dark_panel();
}

void DisplaySleepController::wake() {
    if (m_host.shutting_down) {
        return; // Shutdown in progress — avoid touching LVGL objects
    }

    if (!m_display_sleeping && !m_display_dimmed) {
        // An owed first frame restores the panel itself; restoring the flush here
        // would page-flip a stale frame onto a panel that is still powered off.
        if (m_wake_frame_owed) {
            return;
        }
        // Reaching here means the display is meant to be awake, so a flush still
        // suppressed is a frozen panel that no later wake would clear.
        restore_flush_after_sleep();
        return; // Already fully awake
    }

    bool was_sleeping = m_display_sleeping;
    bool was_dimmed = m_display_dimmed;
    m_display_sleeping = false;
    m_display_dimmed = false;

#if HELIX_ENABLE_SCREENSAVER
    bool was_preview = m_screensaver_is_preview;
    m_screensaver_is_preview = false;
    m_preview_start_tick_ms = 0;
    if (m_screensaver_active) {
        ScreensaverManager::instance().stop(m_wake_from_input ? helix::SaverRunEnd::TOUCH
                                                              : helix::SaverRunEnd::OTHER);
        m_screensaver_active = false;
    }
    // Resume only a suspend this manager requested. enter_sleep() stops the
    // screensaver without resuming, so gating on m_screensaver_active alone
    // would leave the view deactivated after a sleep that followed a dim — but
    // Application's background/foreground pair owns a second suspend of the
    // same latch, and a wake while backgrounded must not steal it.
    if (m_lifecycle_suspended) {
        m_lifecycle_suspended = false;
        NavigationManager::instance().resume_active();
    }
#else
    constexpr bool was_preview = false;
#endif

    // Ends any press LVGL has already started on every pointer, so the wake touch cannot
    // click, long-press or scroll whatever it landed on.
    m_host.cancel_pointer_presses();

    // A swallowed wake touch never reaches LVGL's activity clock, so without this the
    // display would wake and dim or sleep again on the next tick.
    lv_display_trigger_activity(nullptr);

    if (was_sleeping && helix::ui::UpdateQueue::instance().is_draining()) {
        // The rest of this batch and status are not applied in here, so the first frame
        // waits for the loop; the panel stays dark until then.
        m_wake_frame_owed = true;
        spdlog::info("[DisplaySleep] Display woken from sleep inside a batch; first frame owed");
    } else {
        if (was_sleeping) {
            render_first_wake_frame();
        }
        const int brightness = restore_user_brightness();
        spdlog::info("[DisplaySleep] Display woken from {}, brightness restored to {}%",
                     was_sleeping ? "sleep" : "dim", brightness);
    }

    // Auto-lock: show lock screen when waking from sleep or screensaver/dim.
    // Screensaver previews are user-initiated from settings — they didn't go
    // idle, so engaging auto-lock on preview dismiss would be surprising.
    if (helix::wake_should_auto_lock(was_sleeping, was_dimmed, was_preview,
                                     helix::LockManager::instance().auto_lock_enabled(),
                                     helix::LockManager::instance().has_pin())) {
        spdlog::info("[DisplaySleep] Auto-lock engaged on wake");
        helix::LockManager::instance().lock();
        helix::ui::LockScreenOverlay::instance().show();
    }

    // Notify subscribers (camera stream, etc.) to resume background work
    for (auto& cb : m_callbacks) {
        cb(false);
    }
}

void DisplaySleepController::render_first_wake_frame() {
    // Held status and queued observer work land before anything is painted, so the
    // first frame shows current values.
    if (m_host.settle_for_wake_frame) {
        m_host.settle_for_wake_frame();
    }

    // Restore the panel output (unblank / power-on / remove overlay) BEFORE
    // the synchronous render below so the framebuffer is ready when LVGL
    // paints — honoring the #303 black-screen-on-wake race.
    restore_display_output();

    // Force immediate full render after wake. lv_obj_invalidate() alone only
    // marks dirty regions — the actual render happens on the next timer tick,
    // which can race with framebuffer state changes and leave a black screen
    // on some hardware (#303). lv_refr_now() renders synchronously.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
}

int DisplaySleepController::restore_user_brightness() {
    const int brightness = DisplaySettingsManager::instance().user_brightness();
    if (BacklightBackend* backlight = m_host.backlight.get()) {
        backlight->set_brightness(brightness);
    }
    return brightness;
}

void DisplaySleepController::finish_owed_wake_frame() {
    if (!m_wake_frame_owed) {
        return;
    }
    m_wake_frame_owed = false;
    if (m_host.shutting_down || m_display_sleeping) {
        return;
    }
    render_first_wake_frame();
    const int brightness = restore_user_brightness();
    spdlog::info("[DisplaySleep] Display woken from sleep, brightness restored to {}%", brightness);
}

bool DisplaySleepController::note_press(int x, int y) {
    // The caller hides this press from LVGL, so it never reaches LVGL's activity clock;
    // m_wake_requested is how the next idle tick learns of it.
    if (!m_display_sleeping && !m_display_dimmed) {
        return false;
    }
    m_wake_requested = true;
    spdlog::info("[DisplaySleep] Wake touch at ({},{}) while {}: swallowed until release", x, y,
                 m_display_sleeping ? "sleeping" : "dimmed");
    return true;
}

#if HELIX_ENABLE_SCREENSAVER
void DisplaySleepController::preview_screensaver(int type) {
    if (m_host.shutting_down || m_screensaver_active) {
        return;
    }
    auto ss_type = static_cast<ScreensaverType>(type);
    if (ss_type == ScreensaverType::OFF) {
        return;
    }

    spdlog::info("[DisplaySleep] Previewing screensaver type {}", type);
    // Suspend active panel so widget timers stop updating the background
    NavigationManager::instance().suspend_active();
    m_lifecycle_suspended = true;
    ScreensaverManager::instance().start(ss_type, /*preview=*/true);
    // Mark display as dimmed so wake() runs on touch; is_preview
    // flag suppresses auto-lock on dismiss.
    m_display_dimmed = true;
    m_screensaver_active = true;
    m_screensaver_is_preview = true;
    m_preview_start_tick_ms = DisplayManager::get_ticks();
}
#endif

void DisplaySleepController::ensure_on() {
    // Force display awake at startup regardless of previous state
    restore_flush_after_sleep(); // defensive: never start up with flush suppressed
    set_keep_screen_on(true);    // #1245: never inherit a released host sleep lock
    m_display_sleeping = false;
    m_display_dimmed = false;

    const int brightness = DisplaySettingsManager::instance().user_brightness();

    // Apply to hardware - this ensures display is visible
    if (BacklightBackend* backlight = m_host.backlight.get()) {
        backlight->set_brightness(brightness);
    }
    spdlog::info("[DisplaySleep] Startup: forcing display ON at {}% brightness", brightness);
}

void DisplaySleepController::set_dim_timeout(int seconds) {
    m_dim_timeout_sec = seconds;
    spdlog::debug("[DisplaySleep] Dim timeout set to {}s", seconds);
}

void DisplaySleepController::restore_on_shutdown() {
    DisplayBackend* backend = m_host.backend.get();

    destroy_sleep_overlay();
    show_screen_after_dark_panel();

    // Re-enable rendering before the framebuffer clear / final brightness below,
    // otherwise the clear is committed through the no-op flush and never reaches
    // the panel (#1049). No-op if flush was never suppressed.
    restore_flush_after_sleep();

    // If we powered the panel down (#1049), bring it back on so the next app
    // doesn't inherit a powered-off panel.
    if (m_use_power_off && backend) {
        backend->power_on();
    }

    // Clear framebuffer to black so the last rendered frame doesn't persist
    // after the process exits (SIGTERM/SIGINT graceful shutdown). A power-off
    // hold has already painted opaque black, which this transparent clear
    // would undo on panels that read the X byte as alpha.
    if (backend && !m_flush_held_for_power_off) {
        backend->clear_framebuffer(0x00000000);
    }

    // Ensure display is awake before exiting so next app doesn't start with black screen
    const int brightness = DisplaySettingsManager::instance().user_brightness();

    if (BacklightBackend* backlight = m_host.backlight.get()) {
        backlight->set_brightness(brightness);
    }
    m_display_sleeping = false;
    m_wake_frame_owed = false;
    spdlog::debug("[DisplaySleep] Shutdown: restoring display to {}% brightness", brightness);
}

void DisplaySleepController::abandon_lvgl_state() {
    // The sleep overlay is an LVGL object freed by lv_deinit(), so only the pointer is
    // cleared: lv_obj_delete() ordering relative to the rest of LVGL teardown is fragile.
    // The screen holds the overlay and the power-off flush suppression took are released
    // here; a hold a running screensaver has taken is not.
    if (m_sleep_overlay) {
        helix::active_screen_hide_hold().release();
    }
    if (m_screen_hidden_for_dark_panel) {
        helix::active_screen_hide_hold().release();
        m_screen_hidden_for_dark_panel = false;
    }
    if (m_flush_suppressed_for_sleep) {
        helix::active_screen_hide_hold().release();
        m_flush_suppressed_for_sleep = false;
        m_saved_flush_cb_for_sleep = nullptr;
    }
    m_sleep_overlay = nullptr;
    m_wake_frame_owed = false;
    m_use_hardware_blank = false;
    m_use_power_off = false;
    // Back to the startup truth: SDL re-asserts FLAG_KEEP_SCREEN_ON the next time it
    // initializes video, so a re-init must not think we still owe Android a release.
    m_last_sleep_mechanism = SleepMechanism::SoftwareOverlay;
    m_keep_screen_on = true;
    m_resume_seq_at_sleep = 0;
}

} // namespace helix
