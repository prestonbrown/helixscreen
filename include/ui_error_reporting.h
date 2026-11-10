// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification.h"

#include "core/error_reporting.h"
#ifndef HELIX_SPLASH_ONLY
#include "core/update_queue.h"
#endif

#include "ams_error.h"
#include "moonraker_error.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>

/**
 * @file ui_error_reporting.h
 * @brief The error reports that name UI and backend types: core/error_reporting.h
 *        holds the NOTIFY_* macros and ErrorContext every layer uses.
 */

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
 * Every backend fills AmsError::suggestion, and it is the half that gets the
 * user unstuck: "Pause the print first, then load, unload, or change filament"
 * beside "Cannot run filament operation while printing". This is the one
 * renderer, so no call site open-codes `NOTIFY_ERROR(..., err.user_msg)` and
 * drops it.
 *
 * The technical message is logged, not shown.
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
