// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure, platform-independent logic used by EspHttpLane (Task 10: R2 bounded
// queue, R3 size cap). Deliberately free of ESP-IDF/pthread/esp_http_client
// includes so it can be compiled by both the ESP32 IDF build
// (esp_http_lane.cpp) and the desktop host test suite
// (tests/unit/test_esp32_http_lane_queue.cpp) via the repo-root include path
// both builds already have. Mirrors reconnect_backoff.h's extraction pattern
// from Task 9.

#pragma once

#include <cstddef>

namespace helix::http {

// Hard ceiling on any single in-memory fetch the lane will perform, chosen
// from the largest thumbnail size measured on the Voron's .thumbs/ directory
// during Task 10 (see esp32p4-task-10-report.md) plus margin. Enforced
// regardless of what a caller asks for — protects the PSRAM accumulation
// buffer even if a future caller passes an unbounded request.
inline constexpr size_t HARD_CAP_BYTES = 512 * 1024;

// Clamps a caller's requested max_bytes to the lane's hard ceiling. A
// requested size of 0 means "no explicit cap" (the caller wants the whole
// response, up to whatever the lane allows), which also resolves to the hard
// ceiling rather than an unbounded fetch.
inline constexpr size_t clamp_fetch_cap(size_t requested_max_bytes) {
    if (requested_max_bytes == 0 || requested_max_bytes > HARD_CAP_BYTES) {
        return HARD_CAP_BYTES;
    }
    return requested_max_bytes;
}

// Bounded-queue depth accounting. The lane owns one instance guarded by its
// own mutex; submit_get() calls try_acquire() before queuing a job and
// worker_loop() calls release() once that job (success or error) completes.
// Extracted as a standalone, mutex-free counter so the accept/reject decision
// is unit-testable without pthread/esp_http_client — the real class supplies
// the thread safety.
class BoundedSlotCounter {
  public:
    explicit constexpr BoundedSlotCounter(size_t max_depth) : max_depth_(max_depth) {}

    // Returns false (does not acquire a slot) when already at max_depth —
    // callers must treat this as "reject the submission", never block or grow
    // the queue past max_depth.
    constexpr bool try_acquire() {
        if (in_flight_ >= max_depth_) {
            return false;
        }
        ++in_flight_;
        return true;
    }

    constexpr void release() {
        if (in_flight_ > 0) {
            --in_flight_;
        }
    }

    [[nodiscard]] constexpr size_t in_flight() const {
        return in_flight_;
    }

    [[nodiscard]] constexpr size_t max_depth() const {
        return max_depth_;
    }

  private:
    size_t max_depth_;
    size_t in_flight_ = 0;
};

} // namespace helix::http
