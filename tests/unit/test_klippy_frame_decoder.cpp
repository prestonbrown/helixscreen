// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "klippy_frame_decoder.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

std::vector<std::string> collect(helix::KlippyFrameDecoder& d, const std::string& chunk) {
    std::vector<std::string> out;
    d.feed(chunk.data(), chunk.size(), [&out](std::string_view f) { out.emplace_back(f); });
    return out;
}

} // namespace

TEST_CASE("decoder emits a whole frame from one chunk", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    auto got = collect(d, std::string("{\"a\":1}") + '\x03');
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "{\"a\":1}");
    CHECK(d.pending_bytes() == 0);
}

TEST_CASE("decoder reassembles a frame split across chunks", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    CHECK(collect(d, "{\"abc\"").empty());
    CHECK(collect(d, ":123").empty());
    auto got = collect(d, std::string("}") + '\x03');
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "{\"abc\":123}");
}

TEST_CASE("decoder emits several frames from one chunk", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    auto got = collect(d, std::string("one") + '\x03' + "two" + '\x03' + "three" + '\x03');
    REQUIRE(got.size() == 3);
    CHECK(got[0] == "one");
    CHECK(got[1] == "two");
    CHECK(got[2] == "three");
    CHECK(d.pending_bytes() == 0);
}

TEST_CASE("decoder handles a chunk boundary landing exactly on the terminator", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    CHECK(collect(d, "payload").empty());
    auto got = collect(d, std::string(1, '\x03'));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "payload");
}

TEST_CASE("decoder emits a complete frame and retains the partial that follows",
          "[belt][decoder]") {
    // The real first read carries the 88-byte ack plus the head of the first
    // 13 KB batch. Emitting the ack must not discard the batch prefix.
    helix::KlippyFrameDecoder d;
    auto got = collect(d, std::string("ack") + '\x03' + "partial-batch");
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "ack");
    CHECK(d.pending_bytes() == 13);

    auto rest = collect(d, std::string("-tail") + '\x03');
    REQUIRE(rest.size() == 1);
    CHECK(rest[0] == "partial-batch-tail");
}

TEST_CASE("decoder drops empty frames", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    auto got = collect(d, std::string(1, '\x03') + '\x03' + "real" + '\x03');
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "real");
}

TEST_CASE("decoder refuses to buffer without bound", "[belt][decoder]") {
    // A peer that never sends a terminator must not be able to exhaust memory
    // on a 512 MB printer board.
    helix::KlippyFrameDecoder d;
    const std::string junk(1024 * 1024, 'x');
    for (int i = 0; i < 8; ++i) {
        collect(d, junk);
    }
    CHECK(d.overflowed());
    CHECK(d.pending_bytes() <= helix::KlippyFrameDecoder::MAX_PENDING_BYTES);
}

TEST_CASE("reset clears pending bytes and the overflow latch", "[belt][decoder]") {
    helix::KlippyFrameDecoder d;
    collect(d, std::string(2 * 1024 * 1024, 'x'));
    collect(d, std::string(4 * 1024 * 1024, 'x'));
    REQUIRE(d.overflowed());
    d.reset();
    CHECK(d.pending_bytes() == 0);
    CHECK_FALSE(d.overflowed());

    auto got = collect(d, std::string("fresh") + '\x03');
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "fresh");
}
