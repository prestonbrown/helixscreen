// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file memory_profiling.h
 * @brief Development-time memory profiling utilities
 *
 * Provides periodic memory logging and on-demand snapshots via SIGUSR1.
 * This is a development feature to help track memory usage and leaks.
 *
 * Usage:
 *   - Call init() after LVGL is initialized
 *   - Send SIGUSR1 to get an on-demand snapshot: kill -USR1 $(pidof helix-screen)
 *   - Enable --memory-report CLI flag for periodic (30s) logging
 */

namespace helix {

/**
 * @brief Memory profiling subsystem
 *
 * Integrates with LVGL timers and Unix signals for reporting.
 */
class MemoryProfiler {
  public:
    /**
     * @brief Initialize memory profiling
     *
     * Sets up baseline RSS measurement, installs SIGUSR1 handler,
     * and creates an LVGL timer for periodic reporting.
     *
     * @param enable_periodic If true, log memory stats every 30 seconds
     */
    static void init(bool enable_periodic);

    /**
     * @brief Request a memory snapshot
     *
     * Thread-safe. Typically called from signal handler.
     * The actual logging happens on the next LVGL timer tick.
     */
    static void request_snapshot();

    /**
     * @brief Log current memory usage immediately
     *
     * @param label Label for the log entry (e.g., "startup", "periodic", "signal")
     */
    static void log_snapshot(const char* label = "manual");

    /**
     * @brief Enable or disable periodic reporting
     *
     * @param enabled If true, log memory stats every 30 seconds
     */
    static void set_periodic_enabled(bool enabled);

    /**
     * @brief Check if periodic reporting is enabled
     */
    static bool is_periodic_enabled();

    /**
     * @brief Shutdown memory profiling and clean up resources
     *
     * Deletes the LVGL timer. Safe to call during shutdown.
     * Checks lv_is_initialized() to avoid crash during static destruction.
     */
    static void shutdown();
};

} // namespace helix
