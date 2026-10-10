// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_trace.cpp
 * @brief BufferTrace: a 60 s step history of one buffer's bias.
 */

#include "buffer_reading.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("BufferTrace: empty draws nothing", "[buffer][trace]") {
    BufferTrace t;
    CHECK(t.window(100000).empty());
}

TEST_CASE("BufferTrace: a reading that never changes holds across the window", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.2f);
    const auto w = t.window(200000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].t_ms == 200000 - BufferTrace::kWindowMs);
    CHECK(w[0].bias == 0.2f);
    CHECK(w[0].valid);
}

TEST_CASE("BufferTrace: each reading holds until the next", "[buffer][trace]") {
    BufferTrace t;
    t.record(1000, true, 0.1f);
    t.record(31000, true, -0.4f);

    const auto w = t.window(61000);
    REQUIRE(w.size() == 2);
    CHECK(w[0].t_ms == 1000);
    CHECK(w[1].t_ms == 31000);

    // Once the first reading leaves the window, the second is what was held
    // at the window's start.
    const auto later = t.window(91000);
    REQUIRE(later.size() == 1);
    CHECK(later[0].t_ms == 31000);
    CHECK(later[0].bias == -0.4f);
}

TEST_CASE("BufferTrace: a repeat adds nothing", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.3f);
    t.record(500, true, 0.3f);
    CHECK(t.size() == 1);
}

TEST_CASE("BufferTrace: a reading that goes away is a gap", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.2f);
    t.record(10000, false, 0.9f);
    t.record(20000, false, 0.0f);
    REQUIRE(t.size() == 2);
    const auto w = t.window(20000);
    CHECK_FALSE(w.back().valid);
}

TEST_CASE("BufferTrace: old readings expire, keeping the one held into the window",
          "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.1f);
    t.record(10000, true, 0.2f);
    t.record(70001, true, 0.3f);
    CHECK(t.size() == 2); // 0 is gone; 10000 is held into [10001, 70001]
}

TEST_CASE("BufferTrace: a clock that goes back starts the trace again", "[buffer][trace]") {
    BufferTrace t;
    t.record(50000, true, 0.1f);
    t.record(40000, true, 0.5f);
    REQUIRE(t.size() == 1);
    const auto w = t.window(40000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].bias == 0.5f);
}

TEST_CASE("BufferTrace: points after now are not drawn", "[buffer][trace]") {
    BufferTrace t;
    t.record(1000, true, 0.1f);
    t.record(5000, true, 0.2f);
    const auto w = t.window(3000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].t_ms == 1000);
}

TEST_CASE("BufferTrace: memory is bounded", "[buffer][trace]") {
    BufferTrace t;
    for (int i = 0; i < 600; ++i) {
        t.record(i, true, static_cast<float>(i % 2));
    }
    CHECK(t.size() == BufferTrace::kMaxPoints);
}

namespace {
BufferReading fill(int pct, ui::ClogMeterStatus status = ui::ClogMeterStatus::Ok) {
    BufferReading r;
    r.source = BufferSource::Fps;
    r.gauge = BufferGauge::Fill;
    r.has_slider = true;
    r.value_pct = pct;
    r.target_pct = 50;
    r.status = status;
    return r;
}
} // namespace

TEST_CASE("BufferTrace: a fill reading records its pressure, not a bias", "[buffer][trace][fill]") {
    BufferTrace t;
    t.record(0, fill(32));
    t.record(10000, fill(97, ui::ClogMeterStatus::Fault));

    const auto w = t.window(20000);
    REQUIRE(w.size() == 2);
    CHECK(w[0].gauge == BufferGauge::Fill);
    CHECK(w[0].fill_pct == 32);
    CHECK(w[0].bias == 0.0f);
    CHECK(w[1].fill_pct == 97);
    CHECK(w[1].status == ui::ClogMeterStatus::Fault);
}

TEST_CASE("BufferTrace: a fill repeat adds nothing, a new severity does", "[buffer][trace][fill]") {
    BufferTrace t;
    t.record(0, fill(50));
    t.record(500, fill(50));
    CHECK(t.size() == 1);
    // The same pressure that turns into a warning (the lane finished loading) is a new point.
    t.record(900, fill(50, ui::ClogMeterStatus::Warning));
    CHECK(t.size() == 2);
}

TEST_CASE("BufferTrace: a bias point and a fill point at the same number differ",
          "[buffer][trace][fill]") {
    BufferTrace t;
    t.record(0, true, 0.0f);
    t.record(100, fill(0));
    CHECK(t.size() == 2);
}

TEST_CASE("BufferTrace: a reading with no gauge is a gap for a fill history too",
          "[buffer][trace][fill]") {
    BufferTrace t;
    t.record(0, fill(40));
    t.record(1000, BufferReading{});
    const auto w = t.window(2000);
    REQUIRE(w.size() == 2);
    CHECK_FALSE(w.back().valid);
}

TEST_CASE("BufferTrace: the value held into the window keeps its gauge", "[buffer][trace][fill]") {
    BufferTrace t;
    t.record(0, fill(77, ui::ClogMeterStatus::Warning));
    const auto w = t.window(200000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].t_ms == 200000 - BufferTrace::kWindowMs);
    CHECK(w[0].gauge == BufferGauge::Fill);
    CHECK(w[0].fill_pct == 77);
    CHECK(w[0].status == ui::ClogMeterStatus::Warning);
}
