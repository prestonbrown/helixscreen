// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification.h"
#ifndef HELIX_SPLASH_ONLY
#include "ui_update_queue.h"
#endif

#include "ams_error.h"
#include "moonraker_error.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>

/**
 * @file ui_error_reporting.h
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
// Translated errors from callbacks on any thread
// ============================================================================

// The splash binary links no UpdateQueue, so it gets the macros above only.
#ifndef HELIX_SPLASH_ONLY
namespace helix::ui {

namespace detail {
inline std::string localize_arg(const MoonrakerError& err) {
    return err.localized_message();
}
template <typename T> const T& localize_arg(const T& value) {
    return value;
}
} // namespace detail

/**
 * @brief A toast with a translated format string, callable from any thread
 *
 * A toast is main-thread only, while a Moonraker callback runs on whichever
 * thread answered: the caller's for a local refusal, the WebSocket thread for a
 * printer reply. The translation and the toast run on the main thread, inline
 * when already there.
 * A MoonrakerError argument renders as its localized_message().
 *
 * Arguments are copied into a callback that may run later, so a char pointer
 * (a c_str() of a temporary) would dangle: pass std::string.
 *
 * @param fmt_tag Untranslated format with static lifetime; wrap the literal in TR_NOOP
 */
template <typename... Args>
void notify_tr(ToastSeverity severity, const char* fmt_tag, Args... args) {
    static_assert(!(std::is_pointer_v<std::decay_t<Args>> || ...),
                  "notify_tr copies its arguments for later; pass std::string, not a char*");
    run_on_main("notify_tr", [severity, fmt_tag, args...]() {
        const auto format = ::fmt::runtime(lv_tr(fmt_tag));
        switch (severity) {
        case ToastSeverity::INFO:
            NOTIFY_INFO(format, detail::localize_arg(args)...);
            break;
        case ToastSeverity::SUCCESS:
            NOTIFY_SUCCESS(format, detail::localize_arg(args)...);
            break;
        case ToastSeverity::WARNING:
            NOTIFY_WARNING(format, detail::localize_arg(args)...);
            break;
        case ToastSeverity::ERROR:
            NOTIFY_ERROR(format, detail::localize_arg(args)...);
            break;
        }
    });
}

/// notify_tr() at ERROR severity, the shape every Moonraker error callback uses.
template <typename... Args> void notify_error_tr(const char* fmt_tag, Args... args) {
    notify_tr(ToastSeverity::ERROR, fmt_tag, std::move(args)...);
}

} // namespace helix::ui
#endif // HELIX_SPLASH_ONLY

// ============================================================================
// AMS errors — the one place an AmsError becomes user-visible
// ============================================================================

namespace helix::ui {

/**
 * @brief Compose the visible body of an AmsError toast.
 *
 * @p context is the operation name, and it is only worth passing when the
 * backend's own @c user_msg would not already say which operation failed.
 * Every load/unload/eject/tool-change factory in AmsErrorHelper writes a
 * complete sentence ("Failed to unload filament", "Slot 2 is empty"), so
 * prefixing those produced "Unload failed: Failed to unload filament" — pure
 * duplication that also ate a line of a 480x272 toast. The generic results
 * (COMMAND_FAILED -> "Command failed", WRONG_STATE -> "Cannot perform this
 * action now") name nothing, which is where a context word earns its place.
 */
[[nodiscard]] inline std::string ams_error_body(const AmsError& err, const char* context) {
    std::string body =
        err.user_msg.empty() ? std::string(ams_result_to_string(err.result)) : err.user_msg;
    if (context && *context) {
        body = std::string(context) + ": " + body;
    }
    return body;
}

/**
 * @brief Report an AmsError to the user, suggestion included.
 *
 * AmsError::suggestion was populated by every backend and rendered by nothing:
 * 30-odd call sites each open-coded `NOTIFY_ERROR(..., err.user_msg)` with
 * their own verb prefix, so users read "Cannot run filament operation while
 * printing" and never "Pause the print first, then load, unload, or change
 * filament" — the half that gets them unstuck. This is the one renderer.
 *
 * The technical message is logged, not shown; it was previously dropped
 * entirely at most of those sites.
 *
 * @param err     The failure. A successful AmsError is ignored.
 * @param context Optional operation name — see ams_error_body().
 */
inline void notify_ams_error(const AmsError& err, const char* context = nullptr) {
    if (err.success()) {
        return;
    }
    const std::string body = ams_error_body(err, context);
    spdlog::error("[USER] {} [{}] {}", body, ams_result_to_string(err.result), err.technical_msg);
    ui_notification_error_with_detail(body.c_str(), err.suggestion.c_str());
}

/**
 * @brief Same, at warning severity — for a refusal the UI predicted itself.
 *
 * A pre-guard that declines before dispatching (the filament surfaces' print
 * gate) is not a failure the printer reported, so it does not deserve the error
 * tone or the error chime.
 */
inline void notify_ams_warning(const AmsError& err, const char* context = nullptr) {
    if (err.success()) {
        return;
    }
    const std::string body = ams_error_body(err, context);
    spdlog::warn("[USER] {} [{}] {}", body, ams_result_to_string(err.result), err.technical_msg);
    ui_notification_warning_with_detail(body.c_str(), err.suggestion.c_str());
}

} // namespace helix::ui

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
