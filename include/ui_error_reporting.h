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
 * @brief Translated toasts from callbacks on any thread. core/error_reporting.h
 *        holds the NOTIFY_* macros every layer uses, and ams_error.h the AmsError
 *        renderers.
 */

// ============================================================================
// Translated errors from callbacks on any thread
// ============================================================================

// The splash binary links no UpdateQueue, so it gets only core/error_reporting.h's macros.
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
