// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file memory_monitor.h
 * @brief Background thread that samples memory usage and evaluates pressure thresholds
 *
 * Periodically reads /proc/self/status (Linux) and logs RSS, VmSize, etc.
 * Evaluates device-tier-aware thresholds and fires callbacks on breach.
 *
 * Usage:
 *   MemoryMonitor::instance().start();  // Start monitoring
 *   MemoryMonitor::instance().set_warning_callback(fn);  // Wire telemetry
 *   MemoryMonitor::instance().stop();   // Stop monitoring
 */

#pragma once

#include "main_loop_heartbeat.h"
#include "memory_utils.h"

#include <spdlog/common.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace helix {

/**
 * @brief Memory pressure level based on threshold evaluation
 */
enum class MemoryPressureLevel { none, elevated, warning, critical };

/**
 * @brief Device-tier-aware memory thresholds
 */
struct MemoryThresholds {
    size_t warn_rss_kb = 0;
    size_t critical_rss_kb = 0;
    size_t warn_available_kb = 0;
    size_t critical_available_kb = 0;
    size_t growth_5min_kb = 0; ///< Max acceptable RSS growth over 5 minutes

    size_t clear_warn_rss_kb = 0;
    size_t clear_critical_rss_kb = 0;
    size_t clear_warn_available_kb = 0;
    size_t clear_critical_available_kb = 0;

    /// Build thresholds appropriate for the device tier
    static MemoryThresholds for_device(const MemoryInfo& info);
};

/**
 * @brief Memory usage snapshot
 */
struct MemoryStats {
    size_t vm_size_kb = 0; ///< Virtual memory size (total mapped)
    size_t vm_rss_kb = 0;  ///< Resident set size (actual RAM)
    size_t vm_data_kb = 0; ///< Data + stack
    size_t vm_swap_kb = 0; ///< Swapped out memory
    size_t vm_peak_kb = 0; ///< Peak virtual memory
    size_t vm_hwm_kb = 0;  ///< Peak RSS (high water mark)
};

/**
 * @brief Data emitted when a memory pressure threshold is breached
 */
struct MemoryWarningEvent {
    MemoryPressureLevel level = MemoryPressureLevel::none;
    std::string reason;
    MemoryStats stats;
    MemoryInfo system_info;
    int64_t growth_5min_kb = 0;
    SmapsRollup smaps;
};

/**
 * @brief Background memory monitoring thread with threshold evaluation
 *
 * Singleton that periodically samples memory usage, evaluates pressure
 * thresholds, and fires callbacks when limits are breached.
 * Only active on Linux (reads /proc/self/status).
 */
class MemoryMonitor {
  public:
    using WarningCallback = std::function<void(const MemoryWarningEvent&)>;

    static MemoryMonitor& instance();

    /**
     * @brief Start the monitoring thread
     * @param interval_ms Sampling interval in milliseconds (default: 5000ms)
     */
    void start(int interval_ms = 5000);

    /**
     * @brief Stop the monitoring thread
     */
    void stop();

    /**
     * @brief Check if monitoring is active
     */
    bool is_running() const {
        return running_.load();
    }

    /**
     * @brief Get current memory stats (can be called from any thread)
     */
    static MemoryStats get_current_stats();

    /**
     * @brief Log current memory stats immediately (useful for specific events)
     * @param context Optional context string to include in log
     * @param level Log level (default: TRACE for backward compat with periodic loop callers)
     */
    static void log_now(const char* context = nullptr,
                        spdlog::level::level_enum level = spdlog::level::trace);

    /**
     * @brief Set callback for memory pressure warnings
     *
     * Called from the monitor thread when a threshold is breached.
     * Rate-limited to at most once per level per 5 minutes.
     */
    void set_warning_callback(WarningCallback cb);

    /// Unique ID for a registered pressure responder
    using PressureResponderId = uint32_t;

    /**
     * @brief Register a pressure responder callback
     *
     * Called from the monitor thread when a threshold is breached.
     * Responders receive only the pressure level (not full diagnostic data).
     * The implementation copies the responder list under callback_mutex_ and
     * invokes callbacks OUTSIDE the lock — so responders may themselves
     * call add/remove_pressure_responder without deadlocking. Responders
     * still need to be thread-safe against their own state.
     *
     * @return ID for later removal
     */
    PressureResponderId add_pressure_responder(std::function<void(MemoryPressureLevel)> cb);

    /**
     * @brief Remove a previously registered pressure responder
     *
     * Blocks if a callback is currently in flight (holds callback_mutex_).
     */
    void remove_pressure_responder(PressureResponderId id);

    /**
     * @brief Get current memory pressure level (lock-free)
     */
    MemoryPressureLevel pressure_level() const {
        return pressure_level_.load();
    }

    /// Fired from the monitor thread when the LVGL main loop has been stuck
    /// past the hang threshold. Argument is the stall duration in ms.
    using HangCallback = std::function<void(uint32_t stalled_ms)>;

    /**
     * @brief Watch the LVGL main loop for a hang, alongside memory sampling.
     *
     * This rides the existing monitor thread rather than spawning its own.
     * Thread creation is not free on the memory-constrained ARM targets — an
     * EAGAIN there terminates the process (#724, #837) — and this loop already
     * wakes on exactly the cadence a liveness check wants.
     *
     * Called from the monitor thread, at most once per stall; the loop
     * recovering re-arms it.
     */
    void set_hang_callback(HangCallback cb);

    /**
     * @brief Set the main-loop hang threshold. 0 disables detection.
     *
     * Default is MainLoopHangDetector::DEFAULT_THRESHOLD_MS. It has to clear the
     * longest legitimate main-thread block, and those are real: the startup XML
     * parse alone runs ~8s on AD5M.
     */
    void set_hang_threshold_ms(uint32_t ms);
    uint32_t hang_threshold_ms() const;

  private:
    MemoryMonitor() = default;
    ~MemoryMonitor();

    MemoryMonitor(const MemoryMonitor&) = delete;
    MemoryMonitor& operator=(const MemoryMonitor&) = delete;

    void monitor_loop();
    void check_main_loop_liveness();
    void evaluate_thresholds(const MemoryStats& stats);
    void fire_warning(MemoryPressureLevel level, const std::string& reason,
                      const MemoryStats& stats, const MemoryInfo& sys_info, int64_t growth_kb);

    std::atomic<bool> running_{false};
    std::atomic<int> interval_ms_{5000};
    std::thread monitor_thread_;

    // Threshold evaluation
    MemoryThresholds thresholds_{};
    std::atomic<MemoryPressureLevel> pressure_level_{MemoryPressureLevel::none};
    WarningCallback warning_callback_;
    std::mutex callback_mutex_;

    // Main-loop hang detection. hang_detector_ is touched only by the monitor
    // thread; the threshold is atomic because it is set from the UI thread at
    // startup, and the callback is guarded by callback_mutex_ like the others.
    MainLoopHangDetector hang_detector_{};
    std::atomic<uint32_t> hang_threshold_ms_{MainLoopHangDetector::DEFAULT_THRESHOLD_MS};
    HangCallback hang_callback_;
    std::vector<std::pair<PressureResponderId, std::function<void(MemoryPressureLevel)>>>
        pressure_responders_;
    std::atomic<uint32_t> next_responder_id_{1};

    // Growth tracking: circular buffer of RSS samples at 30s intervals (5-min window)
    static constexpr size_t RSS_HISTORY_SIZE = 10;
    std::array<size_t, RSS_HISTORY_SIZE> rss_history_{};
    size_t rss_history_index_ = 0;
    size_t rss_history_count_ = 0;
    int deep_sample_counter_ = 0; ///< Counts 5s ticks; deep sample every 6th (30s)

    // Rate limiting: last warning time per level
    static constexpr int NUM_LEVELS = 4;
    std::array<std::chrono::steady_clock::time_point, NUM_LEVELS> last_warning_time_{};
};

const char* pressure_level_to_string(MemoryPressureLevel level);

/// Compute pressure level from stats — extracted for testability
MemoryPressureLevel compute_pressure_level(const MemoryStats& stats,
                                           const MemoryThresholds& thresholds,
                                           MemoryPressureLevel current_level,
                                           const MemoryInfo& sys_info, int64_t growth_kb);

} // namespace helix
