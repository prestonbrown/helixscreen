// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/notification_api.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>

/**
 * @file error_reporting.h
 * @brief Convenience macros for error reporting with automatic UI notifications
 *
 * These macros combine spdlog logging with UI notifications for better user experience.
 *
 * Usage Examples:
 * ```cpp
 * // Internal error (logged but not shown to user)
 * LOG_ERROR_INTERNAL("Failed to create widget: {}", widget_name);
 *
 * // User-facing notifications (logged + toast)
 * NOTIFY_INFO("Configuration loaded");
 * NOTIFY_SUCCESS("File saved successfully");
 * NOTIFY_WARNING("Printer temperature approaching {}°C limit", temp);
 * NOTIFY_ERROR("Failed to save configuration");
 *
 * // Titled variants (display "Title: message" in toast)
 * NOTIFY_INFO_T("Startup", "Loading configuration...");
 * NOTIFY_SUCCESS_T("Save", "Configuration written to {}", filename);
 * NOTIFY_WARNING_T("Temperature", "Approaching {}°C limit", temp);
 * NOTIFY_ERROR_T("Save Failed", "Could not write to {}", filename);
 *
 * // Critical error (logged + modal dialog)
 * NOTIFY_ERROR_MODAL("Connection Failed", "Unable to reach printer at {}", ip_addr);
 * ```
 */

// ============================================================================
// Internal Errors (Log Only)
// ============================================================================

/**
 * @brief Log internal error (not shown to user)
 *
 * Use for widget creation failures, XML parsing errors, and other internal
 * issues that don't require user action.
 */
#define LOG_ERROR_INTERNAL(msg, ...) spdlog::error("[INTERNAL] " msg, ##__VA_ARGS__)

/**
 * @brief Log internal warning (not shown to user)
 */
#define LOG_WARN_INTERNAL(msg, ...) spdlog::warn("[INTERNAL] " msg, ##__VA_ARGS__)

// ============================================================================
// User-Facing Errors (Log + Toast Notification)
// ============================================================================

/**
 * @brief Report error with toast notification
 *
 * Logs error and shows non-blocking toast. Use for recoverable errors
 * that don't require immediate user action.
 */
#define NOTIFY_ERROR(msg, ...)                                                                     \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::error("[USER] {}", formatted_msg);                                                 \
        ui_notification_error(nullptr, formatted_msg.c_str(), false);                              \
    } while (0)

/**
 * @brief Report error with title and toast notification
 *
 * Like NOTIFY_ERROR but includes a title for context (e.g., "Save Failed").
 * Use when the error needs additional context beyond the message.
 */
#define NOTIFY_ERROR_T(title, msg, ...)                                                            \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::error("[USER] {}: {}", title, formatted_msg);                                      \
        ui_notification_error(title, formatted_msg.c_str(), false);                                \
    } while (0)

/**
 * @brief Report warning with toast notification
 *
 * Logs warning and shows non-blocking toast. Use for potential issues
 * that user should be aware of.
 */
#define NOTIFY_WARNING(msg, ...)                                                                   \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::warn("[USER] {}", formatted_msg);                                                  \
        ui_notification_warning(formatted_msg.c_str());                                            \
    } while (0)

/**
 * @brief Report warning with title and toast notification
 */
#define NOTIFY_WARNING_T(title, msg, ...)                                                          \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::warn("[USER] {}: {}", title, formatted_msg);                                       \
        ui_notification_warning(title, formatted_msg.c_str());                                     \
    } while (0)

/**
 * @brief Report info with toast notification
 */
#define NOTIFY_INFO(msg, ...)                                                                      \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::info("[USER] {}", formatted_msg);                                                  \
        ui_notification_info(formatted_msg.c_str());                                               \
    } while (0)

/**
 * @brief Report info with title and toast notification
 */
#define NOTIFY_INFO_T(title, msg, ...)                                                             \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::info("[USER] {}: {}", title, formatted_msg);                                       \
        ui_notification_info(title, formatted_msg.c_str());                                        \
    } while (0)

/**
 * @brief Report success with toast notification
 */
#define NOTIFY_SUCCESS(msg, ...)                                                                   \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::info("[USER] {}", formatted_msg);                                                  \
        ui_notification_success(formatted_msg.c_str());                                            \
    } while (0)

/**
 * @brief Report success with title and toast notification
 */
#define NOTIFY_SUCCESS_T(title, msg, ...)                                                          \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::info("[USER] {}: {}", title, formatted_msg);                                       \
        ui_notification_success(title, formatted_msg.c_str());                                     \
    } while (0)

// ============================================================================
// Critical Errors (Log + Modal Dialog)
// ============================================================================

/**
 * @brief Report critical error with modal dialog
 *
 * Logs error and shows blocking modal dialog. Use for critical errors
 * that require user acknowledgment (connection failures, hardware errors).
 */
#define NOTIFY_ERROR_MODAL(title, msg, ...)                                                        \
    do {                                                                                           \
        std::string formatted_msg = ::fmt::format(msg, ##__VA_ARGS__);                             \
        spdlog::error("[CRITICAL] {}: {}", title, formatted_msg);                                  \
        ui_notification_error(title, formatted_msg.c_str(), true);                                 \
    } while (0)

// ============================================================================
// Context-Aware Error Reporting
// ============================================================================

/**
 * @brief RAII error context for operations that might fail
 *
 * Usage:
 * ```cpp
 * ErrorContext ctx("Save Configuration");
 * if (!save_to_disk()) {
 *     ctx.error("Disk write failed");  // Shows toast
 * }
 * if (hardware_fault) {
 *     ctx.critical("Hardware disconnected");  // Shows modal
 * }
 * ```
 */
class ErrorContext {
  public:
    explicit ErrorContext(const char* operation) : operation_(operation) {}

    /**
     * @brief Report non-critical error in this context
     *
     * @param details Error details
     */
    void error(const char* details) {
        spdlog::error("[{}] {}", operation_, details);
        ui_notification_error(operation_, details, false);
    }

    /**
     * @brief Report critical error in this context
     *
     * @param details Error details
     */
    void critical(const char* details) {
        spdlog::error("[{}] CRITICAL: {}", operation_, details);
        ui_notification_error(operation_, details, true);
    }

    /**
     * @brief Report warning in this context
     *
     * @param details Warning details
     */
    void warning(const char* details) {
        spdlog::warn("[{}] {}", operation_, details);
        std::string msg = ::fmt::format("{}: {}", operation_, details);
        ui_notification_warning(msg.c_str());
    }

  private:
    const char* operation_;
};
