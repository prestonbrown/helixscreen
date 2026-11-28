// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bed_occupancy_state.h"

namespace helix {

/// Reaches the state's history retry timer, a repeating timer the test
/// harness's timer pump skips.
class BedOccupancyStateTestAccess {
  public:
    /// Runs the history retry timer's callback once, as its period elapsing would.
    /// False when no retry is armed.
    static bool fire_history_retry(BedOccupancyState& state) {
        lv_timer_t* timer = state.history_retry_timer_.get();
        if (!timer) {
            return false;
        }
        BedOccupancyState::history_retry_cb(timer);
        return true;
    }

    [[nodiscard]] static bool history_retry_armed(const BedOccupancyState& state) {
        return static_cast<bool>(state.history_retry_timer_);
    }
};

} // namespace helix
