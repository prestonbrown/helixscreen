// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main_loop_handler.h
 * @brief Main loop timing coordination
 *
 * Handles timing-related concerns in the main loop:
 * - Auto-screenshot after delay
 * - Auto-quit timeout
 * - Benchmark mode FPS tracking
 */

#pragma once

#include <cstdint>

namespace helix::application {

/**
 * @brief Manages main loop timing and benchmarking
 *
 * Encapsulates timing logic that would otherwise clutter main_loop():
 * - Screenshot timing (trigger after configurable delay)
 * - Auto-quit timeout (exit after N seconds)
 * - Benchmark mode (FPS calculation and reporting)
 */
class MainLoopHandler {
  public:
    struct Config {
        // Screenshot settings
        bool screenshot_enabled{false};
        uint32_t screenshot_delay_ms{0};

        // Auto-quit timeout (0 = disabled)
        int timeout_sec{0};

        bool benchmark_mode{false};
        uint32_t benchmark_report_interval_ms{5000};
    };

    struct BenchmarkReport {
        float fps{0.0f};
        uint32_t frame_count{0};
        float elapsed_sec{0.0f};
    };

    struct FinalBenchmarkReport {
        float total_runtime_sec{0.0f};
    };

    /**
     * @brief Initialize with configuration and start tick
     *
     * @param config Configuration settings
     * @param start_tick_ms Current tick when main loop starts
     */
    void init(const Config& config, uint32_t start_tick_ms);

    /**
     * @brief Process a frame tick
     *
     * Call once per frame with current tick value.
     * Updates internal timing state.
     *
     * @param current_tick_ms Current tick in milliseconds
     */
    void on_frame(uint32_t current_tick_ms);

    /**
     * @brief Check if screenshot should be taken
     */
    bool should_take_screenshot() const;

    /**
     * @brief Mark screenshot as taken (prevents re-trigger)
     */
    void mark_screenshot_taken();

    /**
     * @brief Check if auto-quit timeout has elapsed
     */
    bool should_quit() const;

    /**
     * @brief Get elapsed time since init
     */
    uint32_t elapsed_ms() const;

    // Benchmark mode

    /**
     * @brief Get current benchmark frame count
     */
    uint32_t benchmark_frame_count() const {
        return m_benchmark_frame_count;
    }

    /**
     * @brief Check if benchmark report is due
     */
    bool benchmark_should_report() const;

    /**
     * @brief Get and consume benchmark report (resets counters)
     */
    BenchmarkReport benchmark_get_report();

    /**
     * @brief Get final benchmark summary
     */
    FinalBenchmarkReport benchmark_get_final_report() const;

  private:
    Config m_config;
    uint32_t m_start_tick{0};
    uint32_t m_current_tick{0};

    // Screenshot state
    uint32_t m_screenshot_time{0};
    bool m_screenshot_taken{false};

    // Benchmark state
    uint32_t m_benchmark_frame_count{0};
    uint32_t m_benchmark_last_report{0};
};

} // namespace helix::application
