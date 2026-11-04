// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "splash_screen_manager.h"

#include "ui_utils.h"

// Private LVGL header needed to read display->flush_cb for the no-op swap
#include "display/lv_display_private.h"
#include "display_manager.h"

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace helix::application {

namespace {

// Verify a PID belongs to our early splash binary by reading /proc/<pid>/comm.
// Mirrors helix_watchdog.cpp's pid_is_our_splash() PID-recycling guard: if the
// original splash was reaped and the kernel handed the PID to an unrelated
// process, signalling it would be a bug. Returns false on any uncertainty.
bool pid_is_helix_splash(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef __linux__
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
    FILE* f = fopen(comm_path, "r");
    if (!f) {
        return false;
    }
    char comm[64] = {0};
    bool got_line = (fgets(comm, sizeof(comm), f) != nullptr);
    fclose(f);
    if (!got_line) {
        return false;
    }
    size_t n = strlen(comm);
    if (n > 0 && comm[n - 1] == '\n') {
        comm[n - 1] = '\0';
    }
    // Linux truncates /proc/<pid>/comm to 15 chars; "helix-splash" (12) fits.
    return strcmp(comm, "helix-splash") == 0;
#else
    (void)pid;
    return false;
#endif
}

// Parse HELIX_SPLASH_PID from the environment. Returns <= 0 if unset/invalid.
pid_t read_env_splash_pid() {
    const char* p = getenv("HELIX_SPLASH_PID");
    if (p == nullptr || *p == '\0') {
        return 0;
    }
    char* end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p || v <= 0) {
        return 0;
    }
    return static_cast<pid_t>(v);
}

void repaint_full_screen() {
    if (lv_obj_t* screen = lv_screen_active()) {
        lv_obj_update_layout(screen);
        helix::ui::invalidate_all_recursive(screen);
        lv_refr_now(nullptr);
    }
}

} // namespace

void SplashScreenManager::start(pid_t splash_pid) {
    m_splash_pid = splash_pid;
    m_start_time = std::chrono::steady_clock::now();
    m_signaled = false;
    m_discovery_complete = false;
    m_post_refresh_frames = 0;

    // DRM path: the watchdog launches helix-screen WITHOUT --splash-pid, so the
    // early fb0 splash (helix-splash) is an orphan we never signal. It keeps
    // repainting /dev/fb0 (the remote screen) until its own 180s backstop, then
    // clears it — the remote goes black. When we were given no PID, adopt the one
    // the watchdog exported in HELIX_SPLASH_PID (verified via /proc/<pid>/comm) so
    // the normal handoff path (check_and_signal -> SIGUSR1) retires it, no longer
    // dependent solely on the watchdog's single reap. Only when the arg PID was
    // absent — a launcher-provided PID always wins.
    if (m_splash_pid <= 0) {
        pid_t env_pid = read_env_splash_pid();
        if (pid_is_helix_splash(env_pid)) {
            m_splash_pid = env_pid;
            spdlog::info("[SplashManager] Adopted early splash PID {} from HELIX_SPLASH_PID "
                         "(DRM handoff retirement)",
                         env_pid);
        }
    }
}

void SplashScreenManager::on_discovery_complete() {
    m_discovery_complete = true;
}

void SplashScreenManager::check_and_signal() {
    if (m_signaled) {
        return; // Already signaled
    }

    // No splash process
    if (m_splash_pid <= 0) {
        m_signaled = true;
        m_post_refresh_frames = 1;
        return;
    }

    // Wait for discovery completion OR timeout before dismissing splash
    auto elapsed = elapsed_ms();

    if (!m_discovery_complete && elapsed < kDiscoveryTimeoutMs) {
        return; // Keep splash showing, will retry on next frame
    }

    m_signaled = true;

    if (!m_discovery_complete) {
        spdlog::warn("[SplashManager] Discovery timeout ({}ms elapsed), exiting splash anyway",
                     elapsed);
    } else {
        spdlog::debug("[SplashManager] Discovery complete after {}ms, dismissing splash", elapsed);
    }

    signal_and_wait();

    // Schedule post-splash refresh
    spdlog::info("[SplashManager] Splash exited, scheduling post-splash refresh");
    m_post_refresh_frames = 1;
}

int64_t SplashScreenManager::elapsed_ms() const {
    auto elapsed = std::chrono::steady_clock::now() - m_start_time;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

void SplashScreenManager::mark_refresh_done() {
    if (m_post_refresh_frames > 0) {
        m_post_refresh_frames--;
    }
}

void SplashScreenManager::begin_render_suppression(pid_t launcher_splash_pid) {
    // Prevents framebuffer flicker from both processes writing to the same framebuffer
    // simultaneously. Validating the PID avoids an unnecessary wait until the failsafe.
    if (launcher_splash_pid > 0 && kill(launcher_splash_pid, 0) == 0 && !has_exited()) {
        m_suppression.begin();

        // LVGL's invalidation system sends LV_EVENT_REFR_REQUEST which resumes the refresh
        // timer (undoing lv_timer_pause), so pausing the timer alone is insufficient:
        // rendering still happens. With the flush callback replaced, nothing reaches the
        // framebuffer even if LVGL renders.
        lv_display_t* disp = lv_display_get_default();
        if (disp) {
            m_original_flush_cb = disp->flush_cb;
            lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(d); // Must signal ready to avoid hang
            });
            spdlog::debug("[SplashManager] Flush callback replaced with no-op (splash PID {})",
                          launcher_splash_pid);
        }
        spdlog::debug("[SplashManager] Display invalidation suppressed while splash is active");
    }
}

void SplashScreenManager::arm_invalidation_failsafe(uint32_t now_tick) {
    m_suppression_start_tick = now_tick;
}

void SplashScreenManager::dismiss_for_rotation_probe(DisplayManager* display) {
    if (!has_exited()) {
        spdlog::info("[SplashManager] Dismissing splash for rotation probe");
        on_discovery_complete();
        check_and_signal();
        restore_flush_callback(display);
        m_suppression.end();
    }
}

void SplashScreenManager::run_handoff_frame(DisplayManager* display, uint32_t now_tick) {
    // ready_to_signal() makes this fire exactly once, on the frame the splash is about to be
    // retired. Gated on a remote sink so devices without one skip the forced redraw.
    if (display && display->remote_screen_active() && ready_to_signal()) {
        // If a suppressed-flush splash path was active (launcher passed --splash-pid), lift
        // suppression first so the repaint is not a no-op; on the DRM watchdog path
        // invalidation was never suppressed. Ending it here also stops the post-signal
        // block below from repainting a second time.
        m_suppression.end();
        restore_flush_callback(display); // no-op on the DRM path (flush never swapped)
        repaint_full_screen();
    }

    check_and_signal();

    if (m_suppression.active() && needs_post_splash_refresh()) {
        m_suppression.end();
        restore_flush_callback(display);
        spdlog::info("[SplashManager] Post-splash handoff: flush callback restored, painting UI");
        repaint_full_screen();
        mark_refresh_done();
    }

    if (m_suppression.active() &&
        (now_tick - m_suppression_start_tick) >= kInvalidationFailsafeMs) {
        m_suppression.end();
        restore_flush_callback(display);
        spdlog::warn("[SplashManager] Invalidation failsafe triggered after {}ms",
                     kInvalidationFailsafeMs);
        lv_obj_invalidate(lv_screen_active());
    }
}

void SplashScreenManager::restore_flush_callback(DisplayManager* display) {
    if (m_original_flush_cb) {
        if (display) {
            display->restore_flush_cb(m_original_flush_cb);
        }
        m_original_flush_cb = nullptr;
    }
}

void SplashScreenManager::signal_and_wait() {
    spdlog::info("[SplashManager] Signaling splash process (PID {}) to exit...", m_splash_pid);

    if (kill(m_splash_pid, SIGUSR1) != 0) {
        spdlog::warn("[SplashManager] Failed to signal splash process: {}", strerror(errno));
        m_splash_pid = 0;
        return;
    }

    // Wait for splash to exit
    // Check /proc/<pid>/status for zombie state because kill(pid, 0) returns 0 for zombies
    int wait_attempts = 50;

#ifdef __linux__
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/status", m_splash_pid);

    while (wait_attempts-- > 0) {
        // First check if process exists at all
        if (kill(m_splash_pid, 0) != 0) {
            break; // Process gone
        }

        // Check if it's a zombie (exited but not reaped)
        FILE* f = fopen(proc_path, "r");
        if (f) {
            char line[256];
            bool is_zombie = false;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "State:", 6) == 0) {
                    is_zombie = (strchr(line, 'Z') != nullptr);
                    break;
                }
            }
            fclose(f);
            if (is_zombie) {
                spdlog::debug("[SplashManager] Splash process exited (zombie, waiting for reap)");
                break;
            }
        }

        usleep(20000); // 20ms
    }
#else
    // macOS/other: just poll with kill()
    while (wait_attempts-- > 0) {
        if (kill(m_splash_pid, 0) != 0) {
            break;
        }
        usleep(20000);
    }
#endif

    if (wait_attempts <= 0) {
        // Escalate: SIGTERM then SIGKILL as defense-in-depth
        spdlog::warn("[SplashManager] Splash did not exit after SIGUSR1, sending SIGTERM");
        kill(m_splash_pid, SIGTERM);
        usleep(200000); // 200ms

        if (kill(m_splash_pid, 0) == 0) {
            spdlog::warn("[SplashManager] Splash still alive, sending SIGKILL");
            kill(m_splash_pid, SIGKILL);
            usleep(50000); // 50ms
        }
    } else {
        spdlog::info("[SplashManager] Splash process exited");
    }

    m_splash_pid = 0;
}

} // namespace helix::application
