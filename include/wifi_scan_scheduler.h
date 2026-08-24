// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace helix::wifi {

/**
 * @brief Pure state machine deciding when a periodic WiFi scan should fire.
 *
 * Owns no timer, does no I/O, and never touches LVGL — it only answers
 * "should the next tick actually trigger a scan?" and "how long should the
 * next tick wait?". The caller (WiFiManager) owns the lv_timer_t and is the
 * only thing that calls trigger_scan() / lv_timer_set_period().
 *
 * Policy:
 * - No overlap: a scan already in flight suppresses should_trigger() until
 *   on_scan_complete() reports it done.
 * - Backoff: each time a scan reports the same result count as the previous
 *   scan, the interval grows by BASE_INTERVAL_MS (capped at MAX_INTERVAL_MS).
 *   A changed count snaps the interval back to BASE_INTERVAL_MS.
 * - Suppression: once the result count has been unchanged for two
 *   consecutive scans while connected, scanning is suppressed entirely
 *   (should_trigger() stays false) until a manual refresh or a disconnect.
 *   This is the case that matters most: a single-radio station sitting on
 *   the network settings page, already connected, whose scan results have
 *   stopped changing — there is nothing left to learn by continuing to
 *   knock the radio off-channel every few seconds.
 */
class ScanScheduler {
  public:
    static constexpr uint32_t BASE_INTERVAL_MS = 10000;
    static constexpr uint32_t MAX_INTERVAL_MS = 30000;

    /// Call immediately before triggering a scan. Marks a scan outstanding.
    void on_scan_started();

    /// Call once a scan's results are known. Updates backoff/suppression
    /// state and clears the outstanding flag.
    void on_scan_complete(size_t result_count, bool connected);

    /// Call when a scan attempt could not be resolved into a result at all
    /// (trigger_scan() failed synchronously, or results couldn't be
    /// fetched after a successful trigger). Clears ONLY the outstanding
    /// flag — a failed attempt carries no information about whether the
    /// network environment is stable, so it must not feed the "results are
    /// unchanged" inference: it does not touch last_count_/has_last_count_,
    /// does not advance or reset unchanged_streak_, does not grow or reset
    /// the interval, and never sets or clears suppressed_. See
    /// on_scan_complete()'s doc for why folding a failure into it as a
    /// zero-result scan is wrong: three failures while connected would
    /// look identical to three genuinely-unchanged scans and suppress
    /// scanning permanently — exactly backwards for a broken control
    /// socket, the scenario this feature exists to help diagnose.
    void on_scan_failed();

    /// Call when the user explicitly asks for a fresh scan (e.g. opening the
    /// network settings page). Clears suppression and resets the interval.
    void on_user_refresh();

    /// Call on a genuine disconnect. Clears suppression and resets the
    /// interval — a new network is a new environment worth re-learning.
    void on_disconnected();

    /// False while a scan is outstanding or scanning is suppressed.
    bool should_trigger() const;

    /// Interval the caller should use for its next timer tick.
    uint32_t next_interval_ms() const;

    /// True once scan results have gone stable while connected.
    bool suppressed() const;

  private:
    bool scan_outstanding_ = false;
    uint32_t interval_ms_ = BASE_INTERVAL_MS;
    size_t last_count_ = 0;
    bool has_last_count_ = false;
    int unchanged_streak_ = 0;
    bool suppressed_ = false;
};

} // namespace helix::wifi
