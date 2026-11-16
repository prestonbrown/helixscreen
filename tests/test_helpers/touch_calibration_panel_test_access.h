// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "touch_calibration_panel.h"

#include <cstdint>
#include <functional>

namespace helix {

/**
 * @brief Test-only access to TouchCalibrationPanel debounce internals.
 *
 * The press-debounce gate (`debounce_enabled_`) is normally seeded once from
 * the process-global `RuntimeConfig::touch_cal_debounce()` static cache. That
 * cache is order-dependent across a test binary, so tests must NOT rely on the
 * env var to toggle it. This access class provides deterministic seams:
 *
 *  - `set_debounce_enabled()` overrides the gate per-panel-instance.
 *  - `set_now_fn()` injects a monotonic-ms clock so the refractory + stall
 *    windows can be advanced synchronously without spinning a real timer.
 */
class TouchCalibrationPanelTestAccess {
  public:
    static void set_debounce_enabled(TouchCalibrationPanel& p, bool enabled) {
        p.debounce_enabled_ = enabled;
    }

    static bool debounce_enabled(const TouchCalibrationPanel& p) {
        return p.debounce_enabled_;
    }

    static bool press_pending(const TouchCalibrationPanel& p) {
        return p.press_pending_;
    }

    /// Inject a deterministic monotonic-ms clock used for refractory + stall logic.
    static void set_now_fn(TouchCalibrationPanel& p, std::function<uint32_t()> fn) {
        p.now_fn_ = std::move(fn);
    }

    /// Drive the stall-timeout fallback directly (the LVGL timer would otherwise
    /// fire this). Commits a pending press outstanding >= kStallCommitMs.
    static void commit_pending_if_stale(TouchCalibrationPanel& p, uint32_t now) {
        p.commit_pending_if_stale(now);
    }

    static uint32_t refractory_ms() {
        return TouchCalibrationPanel::kRefractoryMs;
    }

    static uint32_t stall_commit_ms() {
        return TouchCalibrationPanel::kStallCommitMs;
    }

    /// Fire the VERIFY countdown-expiry callback directly. The production path
    /// runs it off a 1s-period lv_timer, which a test would have to pump
    /// `verify_timeout_seconds_` worth of virtual ticks to reach — once per
    /// round. Driving the callback keeps the multi-round tests deterministic.
    static void fire_verify_timeout(TouchCalibrationPanel& p) {
        p.stop_countdown_timer();
        if (p.timeout_callback_) {
            p.timeout_callback_();
        }
    }

    /// Fire the fast-revert callback directly (the 3s one-shot timer's payload).
    static void fire_fast_revert(TouchCalibrationPanel& p) {
        if (p.fast_revert_callback_) {
            p.fast_revert_callback_();
        }
    }

    /// Run the real fast-revert decision (the timer callback's body) against the
    /// counters accumulated so far, so a test can assert whether the broken-matrix
    /// heuristic would have fired.
    static bool fast_revert_would_fire(const TouchCalibrationPanel& p) {
        return p.state_ == TouchCalibrationPanel::State::VERIFY && p.verify_raw_touch_count_ > 0 &&
               p.verify_onscreen_touch_count_ == 0;
    }

    static int verify_raw_touch_count(const TouchCalibrationPanel& p) {
        return p.verify_raw_touch_count_;
    }

    static int verify_onscreen_touch_count(const TouchCalibrationPanel& p) {
        return p.verify_onscreen_touch_count_;
    }

    /// Timer starters + handles, for the leak regression: a second start must
    /// cancel the first rather than strand it in LVGL's list still pointing at
    /// this panel.
    static void start_fast_revert_timer(TouchCalibrationPanel& p) {
        p.start_fast_revert_timer();
    }
    static void start_countdown_timer(TouchCalibrationPanel& p) {
        p.start_countdown_timer();
    }
    static lv_timer_t* fast_revert_timer(const TouchCalibrationPanel& p) {
        return p.fast_revert_timer_;
    }
    static lv_timer_t* countdown_timer(const TouchCalibrationPanel& p) {
        return p.countdown_timer_;
    }
};

} // namespace helix
