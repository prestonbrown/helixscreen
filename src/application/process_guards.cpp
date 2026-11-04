// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file process_guards.cpp
 * @brief Startup guards that survive a process death: crash-restart marker, GPU
 *        crash-loop guard files, and the watchdog's Safe Mode marker.
 */

#include "process_guards.h"

#include "config.h"
#include "core/data_root_resolver.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace helix {

namespace {

// Async-signal-safe copy of crash_marker_path(). The SIGTERM handler clears the
// marker with unlink(2) and may not build the path itself: std::string, the
// function-local static's guard, and std::filesystem::remove are all unsafe in
// a handler. The path is snapshotted here once on the main thread at startup;
// s_crash_marker_path_ready gates the handler until it is.
char s_crash_marker_path[PATH_MAX] = {0};
volatile sig_atomic_t s_crash_marker_path_ready = 0;

// GPU 3D crash-loop guard file (issues #966 / #1084 / #1085). The 3D GLES
// renderer writes this immediately before its first GPU draw and removes it
// after the first successful frame. If the driver hard-faults inside the draw
// the process dies with the file still present; finding it here at startup
// means the last session crashed in the GPU path, so we promote it to a
// persistent block. Routed through the writable config dir like the other
// markers so it survives RO-rootfs platforms.
const std::string& gpu_3d_guard_path() {
    static const std::string p = helix::writable_path("gpu_3d_guard");
    return p;
}

// GPU 2D blur crash-loop guard file. The DRM+EGL backdrop-blur path writes this
// immediately before its first Mali/EGL init and removes it once the pipeline is
// up. If the driver hard-faults inside that init (an in-driver SIGSEGV the
// reactive check_gl() guard cannot catch), the process dies with the file still
// present; finding it here at startup means the last session crashed initializing
// GPU blur, so we promote it to a persistent block. Routed through the writable
// config dir like the other markers so it survives RO-rootfs platforms.
const std::string& gpu_blur_guard_path() {
    static const std::string p = helix::writable_path("gpu_blur_guard");
    return p;
}

// Safe-mode marker written by the watchdog when it detects a deterministic
// crash loop (kCrashLoopMaxCrashes same-signature crashes within the
// kCrashLoopWindowSec window). When present at startup, the application
// defers Moonraker connection so the user can reach Settings and clear the
// underlying state (a stuck subscription field, bad printer URL, etc.)
// without re-crashing on the same code path.
//
// One-shot: deleted once the main loop is running and the user can dismiss
// the banner. A clean reboot exits Safe Mode automatically.
const std::string& safe_mode_marker_path() {
    static const std::string p = helix::writable_path("safe_mode.flag");
    return p;
}

} // namespace

// Crash loop detection marker file. Routed through the writable config
// dir so it survives RO-rootfs platforms (Yocto squashfs etc.).
const std::string& crash_marker_path() {
    static const std::string p = helix::writable_path(".crash_restart_count");
    return p;
}

bool cache_crash_marker_path_for_signal(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(s_crash_marker_path)) {
        spdlog::warn("[Application] Crash marker path not signal-cacheable ({} bytes): '{}'",
                     path.size(), path);
        s_crash_marker_path_ready = 0;
        return false;
    }
    std::memcpy(s_crash_marker_path, path.c_str(), path.size() + 1);
    s_crash_marker_path_ready = 1;
    return true;
}

void clear_crash_marker_signal_safe() {
    if (s_crash_marker_path_ready == 0) {
        return;
    }
    // ENOENT is the common case (no marker written, e.g. --test mode).
    (void)::unlink(s_crash_marker_path);
}

void clear_crash_marker(const std::string& path) noexcept {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        spdlog::warn("[Application] Could not remove crash marker {}: {}", path, ec.message());
    }
}

bool record_start_and_check_crash_loop(const std::string& marker_path, long long now_epoch) {
    constexpr size_t kMaxCrashRestarts = 3;
    constexpr long long kCrashWindowSec = 120;

    // Read existing timestamps and filter to recent window
    std::vector<long long> recent_timestamps;
    {
        std::ifstream in(marker_path);
        long long ts;
        while (in >> ts) {
            if (now_epoch - ts < kCrashWindowSec) {
                recent_timestamps.push_back(ts);
            }
        }
    }

    if (recent_timestamps.size() >= kMaxCrashRestarts) {
        spdlog::error("[Application] Crash loop detected: {} restarts within {}s - "
                      "booting in safe mode (no plugins, default home layout)",
                      recent_timestamps.size(), kCrashWindowSec);
        clear_crash_marker(marker_path);
        get_runtime_config()->crash_loop_safe_mode = true;
        return true;
    }
    // Write filtered timestamps plus current restart
    std::ofstream out(marker_path, std::ios::trunc);
    for (auto ts : recent_timestamps) {
        out << ts << "\n";
    }
    out << now_epoch << "\n";
    return false;
}

void crash_loop_detected_and_record() {
    if (get_runtime_config()->is_test_mode()) {
        return;
    }
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    record_start_and_check_crash_loop(crash_marker_path(), now_epoch);
}

void promote_surviving_gpu_guards() {
    // Promote a surviving GPU 3D crash-loop guard to a persistent block. The
    // guard file only survives if the last session died inside the GPU driver
    // mid-draw (the renderer clears it after the first successful frame). Set
    // /display/gpu_3d_blocked so the gcode viewer uses the pure-CPU 2D path,
    // then remove the guard so a subsequent clean run can re-arm it.
    {
        std::error_code ec;
        if (std::filesystem::exists(gpu_3d_guard_path(), ec)) {
            spdlog::warn("[Application] GPU 3D crash-loop guard survived — last session likely "
                         "faulted inside the GPU driver; blocking 3D gcode preview");
            Config::get_instance()->set<bool>("/display/gpu_3d_blocked", true);
            Config::get_instance()->save();
            std::filesystem::remove(gpu_3d_guard_path(), ec);
        }
    }

    // Promote a surviving GPU blur crash-loop guard to a persistent block. The
    // guard file only survives if the last session died inside the Mali/EGL blur
    // init mid-setup (backdrop_blur.cpp clears it once the pipeline is up). Set
    // /display/gpu_blur_blocked so the backdrop uses the pure-CPU blur path, then
    // remove the guard so a subsequent clean run can re-arm it.
    {
        std::error_code ec;
        if (std::filesystem::exists(gpu_blur_guard_path(), ec)) {
            spdlog::warn("[Application] GPU blur crash-loop guard survived — last session likely "
                         "faulted inside the GPU driver; blocking GPU backdrop blur");
            Config::get_instance()->set<bool>("/display/gpu_blur_blocked", true);
            Config::get_instance()->save();
            std::filesystem::remove(gpu_blur_guard_path(), ec);
        }
    }
}

bool consume_safe_mode_marker() {
    // Watchdog writes one of these two paths — primary (writable_path) first,
    // then /tmp fallback if the primary path is unwritable (read-only fs,
    // stuck systemd namespace, etc.). Check both so any successful write
    // by the watchdog reaches us.
    std::vector<std::string> paths{
        safe_mode_marker_path(),
        "/tmp/helix-screen-safe-mode.flag",
    };
    bool found = false;
    for (const auto& path : paths) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        spdlog::warn("[Application] Safe Mode marker present at {} — booting without "
                     "Moonraker connection",
                     path);
        std::filesystem::remove(path, ec);
        if (ec) {
            spdlog::warn("[Application] Failed to remove Safe Mode marker {}: {}", path,
                         ec.message());
        }
        found = true;
    }
    return found;
}

} // namespace helix
