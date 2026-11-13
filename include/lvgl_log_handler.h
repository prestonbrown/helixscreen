// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file lvgl_log_handler.h
 * @brief Custom LVGL log handler that routes through spdlog
 *
 * Routes all LVGL log messages through spdlog for consistent logging.
 * Provides enhanced debugging for subject type mismatch warnings by
 * looking up subject metadata in the SubjectDebugRegistry.
 */

#pragma once

#include <string_view>

namespace helix {
namespace logging {

/**
 * @brief Register the custom LVGL log handler
 *
 * Call this after spdlog initialization to route LVGL logs through spdlog.
 * Replaces printf-based LVGL logging with spdlog-based logging.
 *
 * Features:
 * - Routes LVGL log levels to corresponding spdlog levels
 * - Detects subject type mismatch warnings and enriches with debug info
 * - Looks up subjects in SubjectDebugRegistry for enhanced debugging
 */
void register_lvgl_log_handler();

/**
 * @brief Retire the sink: subsequent LVGL logs are dropped without touching spdlog
 *
 * The sink's lifetime is anchored to the first registration, so its static
 * destruction retires it while spdlog is still alive. Late LVGL log traffic
 * during static teardown (lv_subject_deinit -> lv_observer_delete ->
 * LV_LOG_WARN) is a no-op instead of a read into a freed logger. Callable
 * explicitly by tests; register_lvgl_log_handler() re-arms it.
 */
void retire_lvgl_log_handler();

/**
 * @brief Suppress LVGL translation warnings (downgrade to trace)
 *
 * When true, translation-related LVGL warnings (missing language, missing tag)
 * are logged at trace level instead of debug. Used during init_translations()
 * to avoid noisy startup output from expected incomplete translations.
 */
void set_suppress_translation_warnings(bool suppress);

/**
 * @brief Forget every message the repeat dedupe has seen.
 *
 * The cache holds at most 256 bodies and wipes itself when full, so a process
 * that has already logged many distinct warnings can lose "first occurrence"
 * state at an arbitrary later point. A test that asserts on first-occurrence
 * level starts from an empty cache instead.
 */
void reset_lvgl_log_dedupe();

/**
 * @brief The telemetry anomaly code a WARN-level LVGL message maps to, or nullptr.
 *
 * Matches fixed message text only, never the addresses some messages carry, so
 * repeats of one failure share one code.
 */
const char* anomaly_code_for_warning(std::string_view msg);

} // namespace logging
} // namespace helix
