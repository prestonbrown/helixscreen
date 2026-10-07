// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_state.h"

#include <chrono>

namespace helix {

// Friend access to AmsState internals (the header already declares this class
// as a friend; AmsState itself lives in the global namespace). Definition lives
// in ONE place so two test translation units cannot each define their own and
// violate the ODR.
//
// The post-unload runout grace is time-bounded, and a test that waits out a
// 30-second window is a 30-second test. Backdating the arm stamp is the only
// way to exercise expiry without sleeping.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
class AmsStateTestAccess {
  public:
    /// Arm the grace directly, as if an unload had just completed. For tests
    /// about what CONSUMES the grace; the arming logic itself is driven through
    /// sync_from_backend() so it stays covered by real code.
    static void arm_post_unload_runout_grace(AmsState& ams) {
        auto& g = ams.runout_grace_;
        std::lock_guard<std::mutex> lock(g.mutex_);
        g.armed_ = true;
        g.armed_at_ = RunoutGrace::Clock::now();
    }

    /// Move the arm stamp back in time so the grace reads as older than it is.
    static void age_post_unload_runout_grace(AmsState& ams, std::chrono::seconds by) {
        auto& g = ams.runout_grace_;
        std::lock_guard<std::mutex> lock(g.mutex_);
        g.armed_at_ -= by;
    }

    /// Move the optimistic-action deadline back in time, so a hold the
    /// sidebar armed with its real budget reads as that much older.
    static void age_optimistic_action(AmsState& ams, std::chrono::milliseconds by) {
        if (ams.optimistic_action_until_) {
            *ams.optimistic_action_until_ -= by;
        }
    }

    /// Peek at the flag without consuming it.
    [[nodiscard]] static bool post_unload_runout_grace_armed(AmsState& ams) {
        auto& g = ams.runout_grace_;
        std::lock_guard<std::mutex> lock(g.mutex_);
        return g.armed_;
    }

    /// The production window, so tests express "just inside" / "just outside"
    /// against the real constant instead of a hardcoded copy of it.
    static constexpr std::chrono::seconds grace_window() {
        return RunoutGrace::WINDOW;
    }

    /// Drive the clog-meter subject sync with a hand-built AmsSystemInfo, so
    /// tests about what lands in the meter's text buffers need no live backend.
    static void sync_clog_meter(AmsState& ams, const AmsSystemInfo& info) {
        ams.sync_clog_meter_from_info(info);
    }

    /// Drive the buffer reading sync with a hand-built AmsSystemInfo at a
    /// chosen time, so trace tests need neither a backend nor a clock.
    static void sync_buffer(AmsState& ams, const AmsSystemInfo& info, int64_t now_ms) {
        ams.sync_buffer_from_info(info, now_ms);
    }

    /// Drop every buffer trace, which outlive a test on the singleton.
    static void clear_buffer_traces(AmsState& ams) {
        ams.buffer_traces_.clear();
    }

    /// Return the action subject to IDLE. A test that ends mid-operation leaves
    /// LOADING/UNLOADING/SELECTING standing, which reads as "filament is moving"
    /// to every later test that asks is_filament_operation_active().
    static void reset_action(AmsState& ams) {
        if (ams.initialized_) {
            ams.set_action(AmsAction::IDLE);
        }
    }

    /// Drop a toolchange narration left latched by an earlier test. It clears
    /// only on an action edge to IDLE, and a test that ends already IDLE never
    /// produces one, so the stale label would outrank every later detail.
    static void clear_narration(AmsState& ams) {
        if (ams.initialized_ && !ams.last_narration_label_.empty()) {
            ams.set_narration_phase(-1, "");
        }
    }
};
} // namespace helix
