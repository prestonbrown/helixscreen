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

#include "try_reserve.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

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

// First accumulation buffer for a response. A known Content-Length (for a
// Range request, the length of the range) sizes it exactly; an unknown one
// (<= 0) starts small and grows. Allocating the whole cap up front holds
// 512 KB of PSRAM for a 5 KB thumbnail and fragments the heap.
inline constexpr size_t UNKNOWN_LENGTH_START_BYTES = 16 * 1024;

inline constexpr size_t initial_buffer_bytes(size_t cap, long long content_length) {
    if (content_length > 0) {
        return static_cast<unsigned long long>(content_length) < cap
                   ? static_cast<size_t>(content_length)
                   : cap;
    }
    return UNKNOWN_LENGTH_START_BYTES < cap ? UNKNOWN_LENGTH_START_BYTES : cap;
}

// The next buffer size once @p current is full: doubled, never past @p cap.
inline constexpr size_t next_buffer_bytes(size_t current, size_t cap) {
    return current >= cap / 2 ? cap : current * 2;
}

// The lane's buffers reserve through these (try_reserve.h).
using helix::reserve_allocation_bytes;
using helix::try_reserve;

// A transport read that returned for lack of data within its own timeout, as
// distinct from the end of the body (0) and a failure (any other negative).
inline constexpr int TRANSPORT_AGAIN = -0x7FFF;

// How long one fetch may take, and how long its body may go without a byte.
// A link that stops delivering mid-body must not hold the lane forever.
struct LaneDeadlines {
    int64_t total_ms;
    int64_t stall_ms;
};

// Most one transport read asks for. A read keeps going until it fills what it
// was asked for, so on a trickling link an unbounded one would hold off the
// deadlines and the cancel check for the whole remaining body.
inline constexpr int BODY_READ_CHUNK = 1024;

enum class BodyRead { Ok, AllocFailed, ReadFailed, OverCap, Stalled, TimedOut, Cancelled };

// Reads a response body into @p body, at most @p cap bytes: a known
// Content-Length sizes the first buffer, growing up to the cap otherwise. A
// full cap with more still coming is OverCap, never a truncated success.
// @p t provides `int read(char*, int)` (bytes, 0 at the end of the body,
// TRANSPORT_AGAIN for no data yet, other negatives for failure) and `bool
// complete()`; @p now returns milliseconds. A set @p cancelled stops the read
// before the next byte.
template <class Transport, class Now>
BodyRead read_capped_body(Transport& t, size_t cap, long long content_length, std::string& body,
                          Now now, LaneDeadlines deadlines, const std::atomic<bool>* cancelled) {
    body.clear();
    if (!try_reserve(body, initial_buffer_bytes(cap, content_length))) {
        return BodyRead::AllocFailed;
    }
    const int64_t start = now();
    int64_t last_progress = start;
    size_t total = 0;
    // reserve() can hand back more than asked for, so the cap bounds the bytes
    // read, never the capacity.
    auto room = [&body, cap]() { return std::min(body.capacity(), cap); };
    BodyRead result = BodyRead::Ok;
    for (;;) {
        if (cancelled && cancelled->load()) {
            result = BodyRead::Cancelled;
            break;
        }
        if (total >= cap) {
            result = t.complete() ? BodyRead::Ok : BodyRead::OverCap;
            break;
        }
        if (total == room()) {
            if (t.complete()) {
                break;
            }
            if (!try_reserve(body, next_buffer_bytes(body.capacity(), cap))) {
                result = BodyRead::AllocFailed;
                break;
            }
        }
        body.resize(room()); // within capacity: no allocation
        const int want =
            static_cast<int>(std::min(body.size() - total, static_cast<size_t>(BODY_READ_CHUNK)));
        const int n = t.read(&body[total], want);
        const int64_t at = now();
        if (n > 0) {
            total += static_cast<size_t>(n);
            last_progress = at;
        } else if (n == 0) {
            break; // the body is complete
        } else if (n != TRANSPORT_AGAIN) {
            result = BodyRead::ReadFailed;
            break;
        }
        if (at - start >= deadlines.total_ms) {
            result = BodyRead::TimedOut;
            break;
        }
        if (at - last_progress >= deadlines.stall_ms) {
            result = BodyRead::Stalled;
            break;
        }
    }
    body.resize(total);
    return result;
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
