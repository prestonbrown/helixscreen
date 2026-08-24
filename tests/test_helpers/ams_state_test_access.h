// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_state.h"

#include <chrono>

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
        std::lock_guard<std::recursive_mutex> lock(ams.mutex_);
        ams.post_unload_runout_grace_ = true;
        ams.post_unload_runout_grace_at_ = std::chrono::steady_clock::now();
    }

    /// Move the arm stamp back in time so the grace reads as older than it is.
    static void age_post_unload_runout_grace(AmsState& ams, std::chrono::seconds by) {
        std::lock_guard<std::recursive_mutex> lock(ams.mutex_);
        ams.post_unload_runout_grace_at_ -= by;
    }

    /// Peek at the flag without consuming it.
    [[nodiscard]] static bool post_unload_runout_grace_armed(AmsState& ams) {
        std::lock_guard<std::recursive_mutex> lock(ams.mutex_);
        return ams.post_unload_runout_grace_;
    }

    /// The production window, so tests express "just inside" / "just outside"
    /// against the real constant instead of a hardcoded copy of it.
    static constexpr std::chrono::seconds grace_window() {
        return AmsState::POST_UNLOAD_RUNOUT_GRACE;
    }
};
