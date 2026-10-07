// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: exercises the shipped ESP32 header unmodified --
//                 firmware/helixscreen-esp32/components/helixnet/http_lane_queue.h. That
//                 is production firmware code; this gate only scans include/ and src/, so
//                 a firmware/ include reads to it as no include at all.

/**
 * @file test_esp32_http_lane_queue.cpp
 * @brief Host-side tests for the pure bounded-queue + cap logic EspHttpLane
 * uses (Plan 4 Task 10: R2 bounded queue, R3 size cap).
 *
 * esp_http_lane.cpp/.h are IDF-coupled (pthread, esp_http_client) and cannot
 * be compiled or unit-tested on the desktop build. The two pieces of pure
 * logic that drive its accept/reject and cap-clamping decisions —
 * clamp_fetch_cap() and BoundedSlotCounter — were extracted into
 * firmware/helixscreen-esp32/components/helixnet/http_lane_queue.h (no
 * ESP-IDF/pthread/esp_http_client includes), mirroring Task 9's
 * reconnect_backoff.h extraction, and are exercised here, unmodified, via the
 * repo-root include path the desktop test build already has (`-I.` in
 * mk/tests.mk's $(INCLUDES)).
 */

#include "../test_helpers/scoped_try_reserve_failure.h"
#include "firmware/helixscreen-esp32/components/helixnet/http_lane_queue.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::http::BoundedSlotCounter;
using helix::http::clamp_fetch_cap;
using helix::http::HARD_CAP_BYTES;

TEST_CASE("clamp_fetch_cap enforces the hard ceiling regardless of the request", "[esp32][http]") {
    // Well under the cap — passed through unchanged.
    REQUIRE(clamp_fetch_cap(1024) == 1024);
    REQUIRE(clamp_fetch_cap(100 * 1024) == 100 * 1024);

    // Exactly at the cap — unchanged.
    REQUIRE(clamp_fetch_cap(HARD_CAP_BYTES) == HARD_CAP_BYTES);

    // Over the cap — clamped down, never grows past the ceiling.
    REQUIRE(clamp_fetch_cap(HARD_CAP_BYTES + 1) == HARD_CAP_BYTES);
    REQUIRE(clamp_fetch_cap(10 * 1024 * 1024) == HARD_CAP_BYTES);
}

TEST_CASE("clamp_fetch_cap(0, ...) means \"no explicit cap\", not \"zero bytes\"",
          "[esp32][http]") {
    // A caller-requested 0 resolves to the hard ceiling — it must NOT mean
    // "fetch nothing" (that would silently break any caller that forgets to
    // set an explicit max_bytes).
    REQUIRE(clamp_fetch_cap(0) == HARD_CAP_BYTES);
}

TEST_CASE("BoundedSlotCounter accepts up to max_depth then rejects", "[esp32][http]") {
    BoundedSlotCounter slots(3);
    REQUIRE(slots.in_flight() == 0);

    REQUIRE(slots.try_acquire());
    REQUIRE(slots.try_acquire());
    REQUIRE(slots.try_acquire());
    REQUIRE(slots.in_flight() == 3);

    // Queue full — the 4th submission must be rejected, not queued.
    REQUIRE_FALSE(slots.try_acquire());
    REQUIRE(slots.in_flight() == 3); // rejection doesn't mutate state

    // Completing one job frees a slot for the next submission.
    slots.release();
    REQUIRE(slots.in_flight() == 2);
    REQUIRE(slots.try_acquire());
    REQUIRE(slots.in_flight() == 3);
}

TEST_CASE("BoundedSlotCounter::release() never underflows below zero", "[esp32][http]") {
    // Defensive: release() with nothing in flight must not wrap a size_t
    // counter to SIZE_MAX (which would then never report "full" again).
    BoundedSlotCounter slots(2);
    slots.release();
    slots.release();
    REQUIRE(slots.in_flight() == 0);

    // Still behaves correctly afterward — no corrupted state from the
    // defensive releases above.
    REQUIRE(slots.try_acquire());
    REQUIRE(slots.try_acquire());
    REQUIRE_FALSE(slots.try_acquire());
}

TEST_CASE("A submission abandoned after try_acquire() must give its slot back", "[esp32][http]") {
    // submit_get() acquires a slot before it knows whether the worker pthread
    // exists. When the spawn fails it abandons the submission, and the worker
    // that would normally release() the slot never runs — so submit_get() has
    // to release it itself. Without that, max_depth consecutive spawn failures
    // wedge the lane at "queue full" for the rest of the session even after
    // the condition that blocked the spawn has cleared.
    constexpr size_t DEPTH = 8; // EspHttpLane::QUEUE_DEPTH, as a literal — see below
    BoundedSlotCounter slots(DEPTH);

    for (size_t attempt = 0; attempt < DEPTH * 3; ++attempt) {
        REQUIRE(slots.try_acquire()); // never rejects: every prior attempt gave its slot back
        slots.release();              // the abandon path
        REQUIRE(slots.in_flight() == 0);
    }

    // The lane is still fully available afterward — nothing leaked.
    for (size_t i = 0; i < DEPTH; ++i) {
        REQUIRE(slots.try_acquire());
    }
    REQUIRE_FALSE(slots.try_acquire());
}

TEST_CASE("BoundedSlotCounter::max_depth() reports the configured depth", "[esp32][http]") {
    // 8 matches EspHttpLane::QUEUE_DEPTH (esp_http_lane.h) — kept as a literal
    // here since that header pulls no ESP-IDF includes but is still the
    // IDF-coupled class's home, not this pure-logic test's.
    BoundedSlotCounter slots(8);
    REQUIRE(slots.max_depth() == 8);
    REQUIRE(slots.in_flight() == 0);
}

TEST_CASE("initial_buffer_bytes sizes from Content-Length, never past the cap",
          "[esp32][http][lane_buffer]") {
    using helix::http::initial_buffer_bytes;
    using helix::http::UNKNOWN_LENGTH_START_BYTES;
    // A 9 KB thumbnail under a 512 KB cap takes 9 KB, not 512 KB.
    CHECK(initial_buffer_bytes(HARD_CAP_BYTES, 9274) == 9274);
    // A 200 KB range answered with its own length.
    CHECK(initial_buffer_bytes(200 * 1024, 200 * 1024) == 200 * 1024);
    // A server ignoring Range reports the whole file: the cap bounds it.
    CHECK(initial_buffer_bytes(200 * 1024, 53472182LL) == 200 * 1024);
    // Unknown length (chunked, or no header) starts small.
    CHECK(initial_buffer_bytes(HARD_CAP_BYTES, -1) == UNKNOWN_LENGTH_START_BYTES);
    CHECK(initial_buffer_bytes(HARD_CAP_BYTES, 0) == UNKNOWN_LENGTH_START_BYTES);
    CHECK(initial_buffer_bytes(4096, -1) == 4096);
}

TEST_CASE("next_buffer_bytes doubles and stops at the cap", "[esp32][http][lane_buffer]") {
    using helix::http::next_buffer_bytes;
    CHECK(next_buffer_bytes(16 * 1024, HARD_CAP_BYTES) == 32 * 1024);
    CHECK(next_buffer_bytes(300 * 1024, HARD_CAP_BYTES) == HARD_CAP_BYTES);
    CHECK(next_buffer_bytes(HARD_CAP_BYTES, HARD_CAP_BYTES) == HARD_CAP_BYTES);
}

TEST_CASE("try_reserve reports an impossible allocation instead of aborting",
          "[esp32][http][lane_buffer]") {
    using helix::http::try_reserve;
    std::string s = "kept";
    // Past what a string can hold: reserve() would throw, an abort on the firmware.
    CHECK_FALSE(try_reserve(s, s.max_size() + 1));
    CHECK(s == "kept");

    REQUIRE(try_reserve(s, 64 * 1024));
    CHECK(s.capacity() >= 64 * 1024);
    CHECK(s == "kept");
}

TEST_CASE("reserve_allocation_bytes predicts what reserve() really allocates",
          "[esp32][http][lane_buffer]") {
    using helix::http::reserve_allocation_bytes;
    std::string s;
    s.reserve(128 * 1024);
    const size_t before = s.capacity();
    REQUIRE(before >= 128 * 1024);
    REQUIRE(before < 200 * 1024);

    // libstdc++ grows to at least twice the old capacity, so asking for 200 KB
    // from 128 KB allocates 256 KB: the probe has to cover that, not 200 KB.
    const size_t predicted = reserve_allocation_bytes(before, 200 * 1024, s.max_size());
    s.reserve(200 * 1024);
    CHECK(predicted == s.capacity());
    CHECK(predicted > 200 * 1024);
}

TEST_CASE("try_reserve on a vector fails cleanly and leaves it untouched",
          "[esp32][http][lane_buffer][oom]") {
    std::vector<int> v{1, 2, 3};
    const size_t cap = v.capacity();
    {
        helix::ScopedTryReserveFailure no_memory;
        CHECK_FALSE(helix::try_reserve(v, 1000));
    }
    CHECK(v.capacity() == cap);
    CHECK(v.size() == 3);

    REQUIRE(helix::try_reserve(v, 1000));
    CHECK(v.capacity() >= 1000);
}

// ============================================================================
// read_capped_body: the lane's body read, against a scripted transport
// ============================================================================

namespace {

using helix::http::BodyRead;
using helix::http::LaneDeadlines;
using helix::http::read_capped_body;

/// What the network does next: a segment of bytes arriving, a quiet gap, the
/// end of the body, or a failure, each taking @p ms.
struct Step {
    int result;     ///< > 0 bytes arrive, AGAIN a gap with nothing, 0 the end, < 0 failure
    int64_t ms = 1; ///< how long it takes
};

/// esp_http_client_read() as it behaves: it keeps reading until @p len is
/// filled, and returns early only on the end of the body, a failure, or a
/// socket read that waited SOCKET_TIMEOUT_MS with nothing to show.
struct FakeTransport {
    static constexpr int AGAIN = helix::http::TRANSPORT_AGAIN;
    static constexpr int64_t SOCKET_TIMEOUT_MS = 5000;
    std::vector<Step> steps;
    size_t at = 0;
    int64_t* clock;
    bool complete_at_end = true;
    int reads = 0;

    int read(char* buf, int len) {
        ++reads;
        int got = 0;
        int64_t quiet = 0;
        for (;;) {
            if (at >= steps.size()) {
                // Past the script the server has gone silent: the socket read
                // waits out its timeout.
                *clock += SOCKET_TIMEOUT_MS - quiet;
                return got ? got : AGAIN;
            }
            Step& s = steps[at];
            if (s.result > 0) {
                *clock += s.ms;
                s.ms = 0;
                quiet = 0;
                const int n = std::min(s.result, len - got);
                std::memset(buf + got, 'x', static_cast<size_t>(n));
                got += n;
                s.result -= n;
                if (s.result == 0) {
                    ++at;
                }
                if (got == len) {
                    return got;
                }
            } else if (s.result == AGAIN) {
                ++at;
                if (quiet + s.ms >= SOCKET_TIMEOUT_MS) {
                    *clock += SOCKET_TIMEOUT_MS - quiet;
                    return got ? got : AGAIN;
                }
                *clock += s.ms; // a gap the socket read waits through
                quiet += s.ms;
            } else {
                if (got) {
                    return got; // the end or the failure is seen on the next read
                }
                ++at;
                *clock += s.ms;
                return s.result;
            }
        }
    }
    bool complete() const {
        return complete_at_end && at >= steps.size();
    }
};

constexpr LaneDeadlines DEADLINES{30000, 10000};

} // namespace

TEST_CASE("a body read ends when the body does", "[esp32][http][lane_timeout]") {
    int64_t now = 0;
    FakeTransport t{{{4000}, {4000}, {2000}, {0}}, 0, &now};
    std::string body;
    CHECK(read_capped_body(
              t, 64 * 1024, 10000, body, [&] { return now; }, DEADLINES, nullptr) == BodyRead::Ok);
    CHECK(body.size() == 10000);
}

TEST_CASE("a body that stops arriving mid-transfer fails after the no-progress limit",
          "[esp32][http][lane_timeout]") {
    int64_t now = 0;
    // 3 KB, then silence.
    FakeTransport t{{{3000}}, 0, &now};
    std::string body;
    const BodyRead r =
        read_capped_body(t, 64 * 1024, 9000, body, [&] { return now; }, DEADLINES, nullptr);
    CHECK(r == BodyRead::Stalled);
    // Ten seconds of silence, seen at the end of a socket timeout.
    CHECK(now >= 10000);
    CHECK(now <= 10000 + 2 * FakeTransport::SOCKET_TIMEOUT_MS);
}

TEST_CASE("a body that trickles in past the overall deadline fails near it",
          "[esp32][http][lane_timeout]") {
    int64_t now = 0;
    // A 500-byte segment every 4 s: never quiet long enough for a socket
    // timeout, and the 50 KB body would take over six minutes.
    std::vector<Step> trickle;
    for (int i = 0; i < 100; ++i) {
        trickle.push_back({FakeTransport::AGAIN, 4000});
        trickle.push_back({500, 1});
    }
    FakeTransport t{trickle, 0, &now};
    std::string body;
    CHECK(read_capped_body(
              t, 64 * 1024, 50000, body, [&] { return now; }, DEADLINES, nullptr) ==
          BodyRead::TimedOut);
    // One bounded read past the deadline at most, not the rest of the body.
    CHECK(now <= 40000);
}

TEST_CASE("a cancel lands within one bounded read of a trickling body",
          "[esp32][http][lane_timeout]") {
    int64_t now = 0;
    std::vector<Step> trickle;
    for (int i = 0; i < 100; ++i) {
        trickle.push_back({FakeTransport::AGAIN, 4000});
        trickle.push_back({500, 1});
    }
    FakeTransport t{trickle, 0, &now};
    std::atomic<bool> cancelled{false};
    std::string body;
    // The panel cancels 6 s in.
    const auto clock = [&] {
        if (now >= 6000) {
            cancelled = true;
        }
        return now;
    };
    CHECK(read_capped_body(t, 64 * 1024, 50000, body, clock, DEADLINES, &cancelled) ==
          BodyRead::Cancelled);
    CHECK(now <= 6000 + 10000);
}

TEST_CASE("a transport failure, an over-cap body and a cancel each end the read",
          "[esp32][http][lane_timeout]") {
    int64_t now = 0;
    std::string body;

    FakeTransport failing{{{1000}, {-1}}, 0, &now};
    CHECK(read_capped_body(
              failing, 64 * 1024, 9000, body, [&] { return now; }, DEADLINES, nullptr) ==
          BodyRead::ReadFailed);

    // A server ignoring Range: the cap fills and more is coming.
    FakeTransport big{std::vector<Step>(10, Step{4096}), 0, &now};
    big.complete_at_end = false;
    CHECK(read_capped_body(
              big, 8192, -1, body, [&] { return now; }, DEADLINES, nullptr) == BodyRead::OverCap);

    std::atomic<bool> cancelled{true};
    FakeTransport cancel{{{1000}, {1000}, {0}}, 0, &now};
    CHECK(read_capped_body(
              cancel, 64 * 1024, 2000, body, [&] { return now; }, DEADLINES, &cancelled) ==
          BodyRead::Cancelled);
    CHECK(cancel.reads == 0); // not one byte more once it is unwanted
}
