// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_control_breaker.h"

#include <algorithm>

namespace helix::wifi {

ControlBreaker::Change ControlBreaker::record(uint32_t elapsed_ms, uint64_t now_ms) {
    probe_inflight_ = false;
    if (elapsed_ms < config_.slow_call_ms) {
        strikes_ = 0;
        if (open_) {
            open_ = false;
            return Change::Recovered;
        }
        return Change::None;
    }

    ++strikes_;
    if (open_) {
        backoff_ms_ = std::min(backoff_ms_ * 2, config_.max_probe_ms);
        next_probe_ms_ = now_ms + backoff_ms_;
        return Change::None;
    }
    if (strikes_ >= config_.trip_after) {
        open_ = true;
        backoff_ms_ = config_.first_probe_ms;
        next_probe_ms_ = now_ms + backoff_ms_;
        return Change::Tripped;
    }
    return Change::None;
}

bool ControlBreaker::allow_call(uint64_t now_ms) {
    if (!open_) {
        return true;
    }
    if (probe_inflight_ || now_ms < next_probe_ms_) {
        return false;
    }
    probe_inflight_ = true;
    return true;
}

} // namespace helix::wifi
