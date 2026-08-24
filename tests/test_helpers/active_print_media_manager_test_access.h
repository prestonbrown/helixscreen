// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "active_print_media_manager.h"

#include <string>

namespace helix {

// Grants tests visibility into ActivePrintMediaManager's thumbnail retry state
// (timer, attempt counter, loaded flag). Declared a friend of
// ActivePrintMediaManager (see active_print_media_manager.h). Follows the
// existing TestAccess pattern (tests/test_helpers/, [L088]) rather than adding
// production _for_testing() accessors.
//
// Timer note: unit tests don't advance LVGL timers reliably (the fixtures pause
// all timers — see tests/ui_test_utils.cpp lv_timer_handler_safe). Instead,
// fire_pending_retry() deletes the real one-shot timer and invokes the timer
// body directly, which is exactly what LVGL would do when the period elapses.
class ActivePrintMediaManagerTestAccess {
  public:
    static bool has_pending_retry(const ActivePrintMediaManager& m) {
        return m.retry_timer_.get() != nullptr;
    }

    static int retry_count(const ActivePrintMediaManager& m) {
        return m.thumbnail_retry_count_;
    }

    /// Provenance of the published thumbnail path (None / PreSet / Fetched).
    static ThumbnailOrigin thumbnail_origin(const ActivePrintMediaManager& m) {
        return m.thumbnail_origin_;
    }

    /// True only when a fetch this manager ran completed — the state that
    /// disarms the retry ladder. A PreSet (USB) path is deliberately NOT this.
    static bool thumbnail_loaded(const ActivePrintMediaManager& m) {
        return m.thumbnail_origin_ == ThumbnailOrigin::Fetched;
    }

    static std::string retry_filename(const ActivePrintMediaManager& m) {
        return m.retry_filename_;
    }

    static uint32_t retry_delay_ms(int retry_number) {
        return ActivePrintMediaManager::retry_delay_ms(retry_number);
    }

    static int max_attempts() {
        return ActivePrintMediaManager::MAX_THUMBNAIL_ATTEMPTS;
    }

    /// Fire the pending retry as if its lv_timer period had elapsed.
    /// Returns false (and does nothing) if no retry is pending.
    static bool fire_pending_retry(ActivePrintMediaManager& m) {
        lv_timer_t* t = m.retry_timer_.release();
        if (!t) {
            return false;
        }
        // The retry timer is one-shot (repeat_count 1): LVGL deletes it after
        // the callback. Mirror that by deleting before invoking the body.
        lv_timer_delete(t);
        m.on_retry_timer_fired();
        return true;
    }
};

} // namespace helix
