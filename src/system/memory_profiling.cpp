// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file memory_profiling.cpp
 * @brief Development-time memory profiling implementation
 */

#include "memory_profiling.h"

#include "lvgl/lvgl.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <unistd.h>

namespace helix {

// Internal State

namespace {

/// Enable periodic memory reporting (30-second intervals)
std::atomic<bool> g_periodic_enabled{false};

/// Flag set by signal handler, checked by timer callback
std::atomic<bool> g_snapshot_requested{false};

/// Baseline RSS captured at init time, for delta calculations
int64_t g_baseline_rss_kb = 0;

/// Track if already initialized
bool g_initialized = false;

/// Timer for periodic memory reporting
lv_timer_t* g_report_timer = nullptr;

/**
 * @brief Log current memory usage
 * @param label Label for the log entry
 */
void log_memory_snapshot_impl(const char* label) {
    int64_t rss_kb = 0, hwm_kb = 0;

    if (read_memory_stats(rss_kb, hwm_kb)) {
        SmapsRollup smaps;
        bool have_smaps = read_smaps_rollup(smaps);

        int64_t delta = (g_baseline_rss_kb > 0) ? (rss_kb - g_baseline_rss_kb) : 0;

        if (have_smaps) {
            spdlog::info("[Memory Profiling] {} RSS={}KB HWM={}KB Private={}KB "
                         "PSS={}KB Shared={}KB PrivClean={}KB Swap={}KB Delta={:+}KB",
                         label, rss_kb, hwm_kb, smaps.private_dirty_kb, smaps.pss_kb,
                         smaps.shared_clean_kb + smaps.shared_dirty_kb, smaps.private_clean_kb,
                         smaps.swap_kb, delta);
        } else {
            spdlog::info("[Memory Profiling] {} RSS={}KB HWM={}KB Delta={:+}KB", label, rss_kb,
                         hwm_kb, delta);
        }
    } else {
        spdlog::debug("[Memory Profiling] stats not available (non-Linux platform)");
    }
}

/**
 * @brief SIGUSR1 signal handler for on-demand memory snapshots
 *
 * Signal-safe: only sets an atomic flag, no logging from handler.
 */
void sigusr1_handler(int /*signum*/) {
    g_snapshot_requested.store(true, std::memory_order_release);
}

/**
 * @brief LVGL timer callback for periodic memory reporting
 *
 * Checks for signal-requested snapshots and periodic reporting.
 */
void memory_report_timer_cb(lv_timer_t* /*timer*/) {
    if (g_snapshot_requested.exchange(false, std::memory_order_acquire)) {
        log_memory_snapshot_impl("signal");
    }

    // Periodic report (if enabled)
    if (g_periodic_enabled.load(std::memory_order_acquire)) {
        log_memory_snapshot_impl("periodic");
    }
}

} // anonymous namespace

// Public API

void MemoryProfiler::init(bool enable_periodic) {
    if (g_initialized) {
        spdlog::warn("[Memory Profiling] MemoryProfiler::init() called multiple times");
        return;
    }
    g_initialized = true;

    // Capture baseline RSS
    int64_t rss_kb = 0, hwm_kb = 0;
    if (read_memory_stats(rss_kb, hwm_kb)) {
        g_baseline_rss_kb = rss_kb;
        spdlog::debug("[Memory Profiling] baseline RSS={}KB", rss_kb);
    }

    // Install SIGUSR1 handler for on-demand snapshots
    struct sigaction sa {};
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, nullptr) == 0) {
        spdlog::debug("[Memory Profiling] SIGUSR1 handler installed (kill -USR1 {} for snapshot)",
                      getpid());
    }

    g_periodic_enabled.store(enable_periodic, std::memory_order_release);

    // Create LVGL timer for periodic reporting (30 seconds)
    g_report_timer = lv_timer_create(memory_report_timer_cb, 30000, nullptr);
}

void MemoryProfiler::request_snapshot() {
    g_snapshot_requested.store(true, std::memory_order_release);
}

void MemoryProfiler::log_snapshot(const char* label) {
    log_memory_snapshot_impl(label);
}

void MemoryProfiler::set_periodic_enabled(bool enabled) {
    g_periodic_enabled.store(enabled, std::memory_order_release);
}

bool MemoryProfiler::is_periodic_enabled() {
    return g_periodic_enabled.load(std::memory_order_acquire);
}

void MemoryProfiler::shutdown() {
    // Clean up timer - must be deleted explicitly before LVGL shutdown
    // Check lv_is_initialized() to avoid crash during static destruction
    if (lv_is_initialized()) {
        if (g_report_timer) {
            lv_timer_delete(g_report_timer);
            g_report_timer = nullptr;
        }
    }
    g_initialized = false;
}

} // namespace helix
