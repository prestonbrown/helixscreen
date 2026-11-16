// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file lvgl_thread_check.h
 * @brief Catch LVGL calls made off the LVGL thread
 *
 * lvgl_thread_check_hook.patch calls helix_lvgl_thread_check() at the LVGL
 * entry points that must only run on the LVGL thread: subject set/notify,
 * lv_timer_create / lv_timer_delete and lv_obj_delete / lv_obj_clean. The
 * hook compares the caller's thread with UpdateQueue's main thread and acts
 * on the mode set here.
 *
 * Report mode is on under --test and in helix-tests; HELIX_LVGL_THREAD_CHECK
 * overrides it (docs/devel/ENVIRONMENT_VARIABLES.md). Release builds compile
 * the LVGL-side calls out (lv_conf.h LV_HELIX_THREAD_CHECK), so this module
 * is never reached there.
 */

#include <cstddef>

namespace helix::lvgl_thread_check {

enum class Mode {
    Off,    ///< No check
    Report, ///< Log each distinct off-thread call site once, with a backtrace
    Abort,  ///< Log the first off-thread call, then abort
};

enum class Action { None, Report, Abort };

/// What the hook does for one call. Pure: the hook supplies the thread and
/// whether this call site has been reported already.
[[nodiscard]] Action decide(Mode mode, bool on_lvgl_thread, bool site_reported) noexcept;

/// "off" / "report" / "abort" (as HELIX_LVGL_THREAD_CHECK spells them); null,
/// empty or anything else yields @p fallback.
[[nodiscard]] Mode parse_mode(const char* value, Mode fallback) noexcept;

/// The mode HELIX_LVGL_THREAD_CHECK names, else @p fallback.
[[nodiscard]] Mode mode_from_env(Mode fallback) noexcept;

void set_mode(Mode mode) noexcept;
[[nodiscard]] Mode current_mode() noexcept;

/// Off-thread calls reported since start (or the last reset): one per
/// distinct call site in report mode.
[[nodiscard]] std::size_t reported_site_count() noexcept;

/// Forget the reported sites, so a test sees its own site as new.
void reset_reported_sites() noexcept;

} // namespace helix::lvgl_thread_check

/// Called by the patched LVGL entry points. @p entry is the LVGL function's
/// name, @p caller the return address into its caller.
// NAMESPACE_OK: C linkage; the patched LVGL entry points call this symbol by name
extern "C" void helix_lvgl_thread_check(const char* entry, const void* caller);
