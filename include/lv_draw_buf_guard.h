// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file lv_draw_buf_guard.h
 * @brief The one correct way to free an lv_draw_buf_t a draw task may still hold.
 *
 * THE HAZARD (#929). A cached frame buffer is handed to LVGL as `dsc.src` and,
 * when LV_USE_OS is on, a parallel draw unit may still be reading it after the
 * owning renderer decides to free it. v0.99.54 production telemetry pinned the
 * faulting source pointer to a freed mmap'd cache_buf_. lv_draw_wait_for_finish()
 * blocks until every draw unit's pending tasks complete, after which no in-flight
 * task references the buffer and destroying it is safe. It is a no-op when
 * LV_USE_OS == 0, so single-threaded builds pay nothing.
 *
 * WHY THIS IS A FUNCTION AND NOT A COMMENT. The sequence was maintained by
 * copy-paste across six sites in the two G-code renderers, and by the time it was
 * folded up here they had already drifted apart in two different directions:
 *
 *   - lv_draw_wait_for_finish() was missing entirely from
 *     GCodeGLESRenderer::clear_cached_frame(), which frees the very same
 *     draw_buf_ that the destructor twelve hundred lines earlier guards while
 *     citing this exact bug number.
 *   - The lv_is_initialized() guard was on 2 of the 6, and crash breadcrumbs on 1.
 *
 * Each of those reads as correct in isolation. That is what makes a hand-copied
 * invariant a latent bug rather than mere redundancy.
 *
 * WHY THE lv_is_initialized() CHECK. During shutdown a renderer may be destroyed
 * after lv_deinit(). The buffer's memory went with LVGL's heap, so there is
 * nothing to free and calling into the draw layer would touch torn-down state -
 * the pointer is simply dropped.
 */

#pragma once

#include "system/crash_handler.h"

#include <lvgl/lvgl.h>

namespace helix {

/**
 * @brief Free a draw buffer that a parallel draw unit may still be reading.
 *
 * Nulls @p buf whether or not a free actually happened, so the caller cannot be
 * left holding a dangling pointer on the post-lv_deinit() path.
 *
 * MAIN THREAD ONLY - lv_draw_wait_for_finish() drives LVGL's draw units.
 *
 * @param buf Buffer to free, by reference; set to nullptr on return.
 * @param tag Short name for the crash breadcrumbs, e.g. "cache_buf". Breadcrumbs
 *            are what turned #929 from an unattributable SIGSEGV into a fix, so
 *            every site emits them now rather than just the one that happened to.
 */
inline void safe_draw_buf_destroy(lv_draw_buf_t*& buf, const char* tag) {
    if (buf == nullptr) {
        return;
    }
    if (lv_is_initialized()) {
        crash_handler::breadcrumb::note(tag, "destroy_pre");
        lv_draw_wait_for_finish();
        lv_draw_buf_destroy(buf);
        crash_handler::breadcrumb::note(tag, "destroy_post");
    }
    buf = nullptr;
}

} // namespace helix
