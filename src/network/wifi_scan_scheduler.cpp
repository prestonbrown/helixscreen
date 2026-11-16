// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_scan_scheduler.h"

#include <algorithm>

namespace helix::wifi {

void ScanScheduler::on_scan_started() {
    scan_outstanding_ = true;
}

void ScanScheduler::on_scan_complete(size_t result_count, bool connected) {
    scan_outstanding_ = false;

    const bool unchanged = has_last_count_ && (result_count == last_count_);
    last_count_ = result_count;
    has_last_count_ = true;

    if (unchanged) {
        unchanged_streak_++;
        interval_ms_ = std::min(interval_ms_ + kBaseIntervalMs, kMaxIntervalMs);
        if (connected && unchanged_streak_ >= 2) {
            suppressed_ = true;
        }
    } else {
        unchanged_streak_ = 0;
        interval_ms_ = kBaseIntervalMs;
        // Deliberately does NOT clear suppressed_ — only on_user_refresh()
        // and on_disconnected() do that. In practice this branch can only
        // run while suppressed if a caller forced a scan through despite
        // should_trigger() being false (e.g. a direct on_scan_complete()
        // call in a test); the normal timer path never gets here already
        // suppressed, since should_trigger() would have blocked the scan
        // that produced this result.
    }
}

void ScanScheduler::on_scan_failed() {
    // Deliberately touches nothing but the outstanding flag. In particular
    // interval_ms_ is left exactly as it was — not grown (a failure isn't
    // evidence of a stable environment) and not reset to kBaseIntervalMs
    // either (a failure isn't evidence the environment changed). Whatever
    // cadence was in effect before the failed attempt is still the best
    // guess for the next one.
    scan_outstanding_ = false;
}

void ScanScheduler::on_user_refresh() {
    suppressed_ = false;
    unchanged_streak_ = 0;
    interval_ms_ = kBaseIntervalMs;
}

void ScanScheduler::on_disconnected() {
    suppressed_ = false;
    unchanged_streak_ = 0;
    interval_ms_ = kBaseIntervalMs;
}

bool ScanScheduler::should_trigger() const {
    return !scan_outstanding_ && !suppressed_;
}

uint32_t ScanScheduler::next_interval_ms() const {
    return interval_ms_;
}

bool ScanScheduler::suppressed() const {
    return suppressed_;
}

} // namespace helix::wifi
