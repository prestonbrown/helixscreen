// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_loop_handler.h"

namespace helix::application {

void MainLoopHandler::init(const Config& config, uint32_t start_tick_ms) {
    m_config = config;
    m_start_tick = start_tick_ms;
    m_current_tick = start_tick_ms;

    if (config.screenshot_enabled) {
        m_screenshot_time = start_tick_ms + config.screenshot_delay_ms;
    }
    m_screenshot_taken = false;

    m_benchmark_frame_count = 0;
    m_benchmark_last_report = start_tick_ms;
}

void MainLoopHandler::on_frame(uint32_t current_tick_ms) {
    m_current_tick = current_tick_ms;

    if (m_config.benchmark_mode) {
        m_benchmark_frame_count++;
    }
}

bool MainLoopHandler::should_take_screenshot() const {
    if (!m_config.screenshot_enabled || m_screenshot_taken) {
        return false;
    }
    return m_current_tick >= m_screenshot_time;
}

void MainLoopHandler::mark_screenshot_taken() {
    m_screenshot_taken = true;
}

bool MainLoopHandler::should_quit() const {
    if (m_config.timeout_sec <= 0) {
        return false;
    }
    uint32_t timeout_ms = static_cast<uint32_t>(m_config.timeout_sec) * 1000U;
    return elapsed_ms() >= timeout_ms;
}

uint32_t MainLoopHandler::elapsed_ms() const {
    return m_current_tick - m_start_tick;
}

bool MainLoopHandler::benchmark_should_report() const {
    if (!m_config.benchmark_mode) {
        return false;
    }
    return (m_current_tick - m_benchmark_last_report) >= m_config.benchmark_report_interval_ms;
}

MainLoopHandler::BenchmarkReport MainLoopHandler::benchmark_get_report() {
    BenchmarkReport report;
    report.frame_count = m_benchmark_frame_count;

    uint32_t elapsed = m_current_tick - m_benchmark_last_report;
    report.elapsed_sec = elapsed / 1000.0f;

    if (report.elapsed_sec > 0) {
        report.fps = m_benchmark_frame_count / report.elapsed_sec;
    }

    // Reset counters for next interval
    m_benchmark_frame_count = 0;
    m_benchmark_last_report = m_current_tick;

    return report;
}

MainLoopHandler::FinalBenchmarkReport MainLoopHandler::benchmark_get_final_report() const {
    FinalBenchmarkReport report;
    report.total_runtime_sec = elapsed_ms() / 1000.0f;
    return report;
}

} // namespace helix::application
