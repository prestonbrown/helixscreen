// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file splash_screen_manager.h
 * @brief Splash screen lifecycle management
 *
 * Coordinates splash screen dismissal with printer discovery timing, and owns the
 * framebuffer handoff: rendering stays suppressed while the splash process paints,
 * and comes back with one full repaint when it is retired.
 */

#pragma once

#include "invalidation_suppression.h"

#include <chrono>
#include <cstdint>
#include <sys/types.h>

class DisplayManager; // NAMESPACE_OK: declared at global scope

namespace helix::application {

/**
 * @brief Manages splash screen lifecycle and discovery coordination
 *
 * The splash screen should stay visible until either:
 * 1. Printer discovery completes (preferred)
 * 2. Timeout expires (fallback)
 *
 * After splash exits, schedules display refresh to repaint the UI.
 */
class SplashScreenManager {
  public:
    /// Discovery timeout in milliseconds (how long to wait before giving up)
    static constexpr int64_t kDiscoveryTimeoutMs = 8000;

    /// Hard deadline for suppressed rendering. Must exceed kDiscoveryTimeoutMs.
    static constexpr uint32_t kInvalidationFailsafeMs = 11000;

    /**
     * @brief Initialize with splash process PID
     *
     * @param splash_pid Process ID of splash screen (0 or negative = no splash)
     */
    void start(pid_t splash_pid);

    /**
     * @brief Mark printer discovery as complete
     *
     * Call this when Moonraker discovery finishes. This allows the splash
     * to exit before the timeout if discovery is fast.
     */
    void on_discovery_complete();

    /**
     * @brief Check conditions and signal splash to exit if ready
     *
     * Call once per frame. Signals the splash process when:
     * - Discovery is complete, OR
     * - Timeout has elapsed
     *
     * After signaling, waits briefly for the process to exit.
     */
    void check_and_signal();

    /**
     * @brief Check if splash has been signaled to exit
     */
    bool has_exited() const {
        return m_signaled;
    }

    /**
     * @brief True on the frame the next check_and_signal() would actually signal.
     *
     * Side-effect-free mirror of check_and_signal()'s decision: not yet signaled
     * AND (no splash PID, discovery complete, or the discovery timeout elapsed).
     * Used to force a full fb0-mirror repaint at handoff BEFORE the early splash
     * is signaled to exit, so the remote screen shows the real UI, not black.
     * Returns false once signaled, so a handoff repaint fires exactly once.
     */
    bool ready_to_signal() const {
        if (m_signaled) {
            return false;
        }
        if (m_splash_pid <= 0 || m_discovery_complete) {
            return true;
        }
        return elapsed_ms() >= kDiscoveryTimeoutMs;
    }

    /**
     * @brief Check if discovery has completed
     */
    bool is_discovery_complete() const {
        return m_discovery_complete;
    }

    /**
     * @brief Get elapsed time since start() was called
     */
    int64_t elapsed_ms() const;

    /**
     * @brief Check if post-splash screen refresh is needed
     *
     * After splash exits, the framebuffer needs to be repainted.
     */
    bool needs_post_splash_refresh() const {
        return m_post_refresh_frames > 0;
    }

    /**
     * @brief Mark one refresh frame as done
     *
     * Call after performing a full screen refresh.
     */
    void mark_refresh_done();

    /**
     * @brief Stop rendering from reaching the framebuffer while the launcher's splash lives
     *
     * Disables invalidation and swaps the flush callback for a no-op on the default
     * display. Takes the PID the launcher passed, not one start() adopted: the splash the
     * DRM watchdog path adopts paints only fb0, the remote screen, so rendering stays on
     * there. A PID that no longer exists (a crashed launcher) suppresses nothing.
     */
    void begin_render_suppression(pid_t launcher_splash_pid);

    /// Starts the failsafe deadline for suppressed rendering. Call once on main loop entry.
    void arm_invalidation_failsafe(uint32_t now_tick);

    /**
     * @brief Retire the splash now and hand the screen back, for a probe that must be seen
     *
     * Does nothing once the splash has exited.
     */
    void dismiss_for_rotation_probe(DisplayManager* display);

    /**
     * @brief Run one frame of the handoff. Call once per main loop frame, after LVGL's timers.
     *
     * With a remote-screen sink active, the frame that retires the splash runs: restore the
     * flush callback, full repaint (which mirrors a whole frame into fb0), THEN signal. The
     * splash exits without clearing fb0, so a signal sent first leaves the remote on the
     * splash's frozen frame or on black. Without a sink it signals first and repaints when
     * the splash is gone. Past kInvalidationFailsafeMs it forces rendering back on, so a
     * failed handoff cannot leave a black screen.
     */
    void run_handoff_frame(DisplayManager* display, uint32_t now_tick);

    /// Puts the saved flush callback back through @p display. Does nothing with none saved.
    void restore_flush_callback(DisplayManager* display);

  private:
    /// Signal the splash process and wait for it to exit
    void signal_and_wait();

    pid_t m_splash_pid{0};
    bool m_signaled{false};
    bool m_discovery_complete{false};
    int m_post_refresh_frames{0};
    std::chrono::steady_clock::time_point m_start_time{std::chrono::steady_clock::now()};

    /// LVGL flush callback saved while the no-op is installed
    lv_display_flush_cb_t m_original_flush_cb{nullptr};
    /// Display invalidation suppressed while the launcher's splash owns the framebuffer
    helix::InvalidationSuppression m_suppression;
    uint32_t m_suppression_start_tick{0};
};

} // namespace helix::application
