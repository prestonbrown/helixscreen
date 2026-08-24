// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

/**
 * @file ui_notification_threshold.h
 * @brief Pure mapping + process-wide cache for the minimum-toast-severity
 *        user setting (#1213).
 *
 * Keeps no LVGL dep, so the rule is unit-testable in isolation and the cache
 * can live header-only (an inline atomic, deduped across TUs by C++17 - no
 * dedicated .cpp, so the test build links without pulling ui_notification.cpp).
 * Severity ints mirror the ToastSeverity enum ordering
 * (INFO=0, SUCCESS=1, WARNING=2, ERROR=3) - see ui_toast_manager.h.
 *
 * SafetySettingsManager owns the persisted value and pushes it here at init
 * and on change; the toast sites in ui_notification.cpp read the cache.
 */

namespace helix::ui::notifications {

/**
 * @brief Map the user's setting index to the minimum severity int that toasts.
 *
 *   0 "All"               -> INFO    (0)  everything toasts
 *   1 "Warnings & errors" -> WARNING (2)  INFO and SUCCESS suppressed
 *   2 "Errors only"       -> ERROR   (3)  only ERROR toasts
 *
 * Any out-of-range value falls back to "All" (INFO): a corrupt or unknown
 * setting must never silently swallow an error toast.
 */
constexpr int min_toast_severity_for_index(int threshold_index) noexcept {
    switch (threshold_index) {
    case 1:
        return 2; // WARNING
    case 2:
        return 3; // ERROR
    default:
        return 0; // INFO ("All", and any out-of-range)
    }
}

/**
 * @brief True if a notification of @p severity should produce a toast.
 *
 * @param severity        the notification's severity (0=INFO ... 3=ERROR)
 * @param threshold_index the user's setting index (0/1/2, see above)
 */
constexpr bool severity_meets_threshold(int severity, int threshold_index) noexcept {
    return severity >= min_toast_severity_for_index(threshold_index);
}

/**
 * @brief Process-wide cache of the user's setting index (0/1/2).
 *
 * `inline` (C++17) so every TU shares one instance with no dedicated .cpp.
 * Defaults to 0 ("All") so nothing is suppressed until SafetySettingsManager
 * pushes the persisted value at init.
 */
inline std::atomic<int> min_toast_severity_cache{0};

/// Set the cached threshold index (clamped to 0/1/2). Thread-safe.
inline void set_min_toast_severity_cache(int index) noexcept {
    int clamped = (index == 1 || index == 2) ? index : 0;
    min_toast_severity_cache.store(clamped, std::memory_order_relaxed);
}

/// Read the cached threshold index. Thread-safe; safe on background threads.
inline int get_min_toast_severity_cache() noexcept {
    return min_toast_severity_cache.load(std::memory_order_relaxed);
}

} // namespace helix::ui::notifications
