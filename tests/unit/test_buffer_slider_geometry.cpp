// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_slider_geometry.cpp
 * @brief Where the buffer slider's block, target window and end stops land,
 *        and how its trace is laid out. Pure, no LVGL.
 */

#include "buffer_slider_geometry.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// 24 x 114: the strand overhangs 7 px at each end, the block travels a band
// from y = 9 to 105, 16 px tall with 80 px of travel, centre y = 17 + (1 - bias) * 40.
constexpr int kW = 24;
constexpr int kH = 114;

TEST_CASE("buffer_slider_geometry: loose up, tight down", "[buffer][slider][geometry]") {
    CHECK(buffer_slider_geometry(0.0f, kW, kH).block.y == 49);
    CHECK(buffer_slider_geometry(1.0f, kW, kH).block.y == 9);
    CHECK(buffer_slider_geometry(-1.0f, kW, kH).block.y == 89);
    CHECK(buffer_slider_geometry(0.0f, kW, kH).block.h == 16);
    CHECK(buffer_slider_y(0.0f, kH) == 57);
}

TEST_CASE("buffer_slider_geometry: the strand runs through and past the housing",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, kW, kH);
    CHECK(g.strand_x == 12);
    CHECK(g.strand_w == 3);
    CHECK(g.housing.x == 0);
    CHECK(g.housing.w == kW);
    CHECK(g.housing.y == 7);
    CHECK(g.housing.y + g.housing.h == kH - 7);
}

TEST_CASE("buffer_slider_geometry: the block nearly fills the housing, inside it",
          "[buffer][slider][geometry]") {
    for (float bias : {-1.0f, 0.0f, 1.0f}) {
        const auto g = buffer_slider_geometry(bias, kW, kH);
        CHECK(g.block.x == 3);
        CHECK(g.block.w == 18);
        CHECK(g.block.y > g.housing.y);
        CHECK(g.block.y + g.block.h < g.housing.y + g.housing.h);
    }
}

TEST_CASE("buffer_slider_geometry: the target window holds the block while on target",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, kW, kH);
    CHECK(g.target.x == 2);
    CHECK(g.target.w == 20);
    CHECK(g.target.y == 36); // block top at +0.3, less a pixel
    CHECK(g.target.h == 42); // down to the block bottom at -0.3, plus a pixel
    for (float bias : {-0.3f, 0.3f}) {
        const auto b = buffer_slider_geometry(bias, kW, kH).block;
        CHECK(b.y > g.target.y);
        CHECK(b.y + b.h < g.target.y + g.target.h);
    }
    const auto off = buffer_slider_geometry(-0.5f, kW, kH).block;
    CHECK(off.y + off.h > g.target.y + g.target.h);
}

TEST_CASE("buffer_slider_geometry: the end stops cover block centres past the fault band",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, kW, kH);
    CHECK(g.danger_top.y == 9);
    CHECK(g.danger_top.h == 20); // down to the centre at +0.7
    CHECK(g.danger_bottom.y == 85);
    CHECK(g.danger_bottom.h == 20);
    CHECK(g.danger_top.x == 2);
    CHECK(g.danger_top.w == 20);
}

TEST_CASE("buffer_slider_geometry: grip lines across a chunky block, none on a thin one",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, kW, kH);
    REQUIRE(g.grip_count == 3);
    CHECK(g.grip_y[0] == g.block.y + 4);
    CHECK(g.grip_y[1] == g.block.y + 8);
    CHECK(g.grip_y[2] == g.block.y + 12);
    CHECK(g.grip_x1 == 6);
    CHECK(g.grip_x2 == 17);
    CHECK(buffer_slider_geometry(0.0f, 16, 44).grip_count == 0);
}

TEST_CASE("buffer_slider_geometry: out-of-range bias stays in the housing",
          "[buffer][slider][geometry]") {
    CHECK(buffer_slider_geometry(3.0f, kW, kH).block.y == 9);
    CHECK(buffer_slider_geometry(-3.0f, kW, kH).block.y == 89);
    CHECK(buffer_slider_geometry(std::nanf(""), kW, kH).block.y == 49);
}

TEST_CASE("buffer_slider_geometry: small and empty boxes", "[buffer][slider][geometry]") {
    const auto small = buffer_slider_geometry(0.0f, 16, 32);
    CHECK(small.block.h == 6);
    CHECK(small.block.y > small.housing.y);
    CHECK(small.block.y + small.block.h < small.housing.y + small.housing.h);
    CHECK(buffer_slider_geometry(0.5f, 0, 100).block.h == 0);
    CHECK(buffer_slider_geometry(0.5f, 24, 0).target.h == 0);
}

TEST_CASE("buffer_trace_y: the full height, loose at the top", "[buffer][trace][geometry]") {
    CHECK(buffer_trace_y(1.0f, 101) == 0);
    CHECK(buffer_trace_y(0.0f, 101) == 50);
    CHECK(buffer_trace_y(-1.0f, 101) == 100);
    CHECK(buffer_trace_y(-3.0f, 101) == 100);
    CHECK(buffer_trace_y(0.5f, 0) == 0);
}

TEST_CASE("buffer_trace_polylines: newest at x = 0, each reading a step",
          "[buffer][trace][geometry]") {
    // now = 100 s, 120 px wide: 60 s of history is 120 px, 2 px per second.
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.5f, true}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    const auto& l = lines[0];
    REQUIRE(l.size() == 4);
    CHECK(l[0].x == 0); // +0.5 from now ...
    CHECK(l[0].y == 25);
    CHECK(l[1].x == 60); // ... back to 70 s
    CHECK(l[1].y == 25);
    CHECK(l[2].x == 60); // step to 0.0 ...
    CHECK(l[2].y == 50);
    CHECK(l[3].x == 120); // ... held back to 40 s, the window's start
    CHECK(l[3].y == 50);
    CHECK(l[0].y == buffer_trace_y(0.5f, 100));
}

TEST_CASE("buffer_trace_polylines: each reading carries its own band",
          "[buffer][trace][geometry]") {
    // Oldest to newest: danger, warning, ok. Each step is judged by its own
    // reading, not by the newest one.
    const std::vector<BufferTracePoint> w = {
        {10000, -0.9f, true}, {40000, 0.5f, true}, {70000, 0.1f, true}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    const auto& l = lines[0];
    REQUIRE(l.size() == 6);
    CHECK(l[0].status == ClogMeterStatus::Ok);
    CHECK(l[1].status == ClogMeterStatus::Ok);
    CHECK(l[2].status == ClogMeterStatus::Warning);
    CHECK(l[3].status == ClogMeterStatus::Warning);
    CHECK(l[4].status == ClogMeterStatus::Fault);
    CHECK(l[5].status == ClogMeterStatus::Fault);
    // The hold at each reading takes that reading's band; the step between two
    // takes the worse.
    CHECK(buffer_trace_segment_status(l[0], l[1]) == ClogMeterStatus::Ok);
    CHECK(buffer_trace_segment_status(l[1], l[2]) == ClogMeterStatus::Warning);
    CHECK(buffer_trace_segment_status(l[2], l[3]) == ClogMeterStatus::Warning);
    CHECK(buffer_trace_segment_status(l[3], l[4]) == ClogMeterStatus::Fault);
    CHECK(buffer_trace_segment_status(l[4], l[5]) == ClogMeterStatus::Fault);
}

TEST_CASE("buffer_trace_polylines: a gap breaks the line", "[buffer][trace][geometry]") {
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.0f, false}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].front().x == 60);
    CHECK(lines[0].back().x == 120);
}

TEST_CASE("buffer_trace_polylines: nothing to draw", "[buffer][trace][geometry]") {
    CHECK(buffer_trace_polylines({}, 100000, 120, 100).empty());
    CHECK(buffer_trace_polylines({{0, 0.0f, true}}, 100000, 0, 100).empty());
}

TEST_CASE("buffer_trace_unrecorded_x: the minute not yet recorded", "[buffer][trace][geometry]") {
    constexpr int64_t now = 100000;
    CHECK(buffer_trace_unrecorded_x({}, now, 120) == 0);
    CHECK(buffer_trace_unrecorded_x({{now, 0.1f, true}}, now, 120) == 0);
    CHECK(buffer_trace_unrecorded_x({{now - 10000, 0.1f, true}}, now, 120) == 20);
    CHECK(buffer_trace_unrecorded_x({{now - 60000, 0.1f, true}}, now, 120) == 120);
}

// Fill gauge in the same 24 x 114 box: housing y 7..107, the track it fills is
// y 9..105 (96 px), 0% at y = 105 and 100% at y = 9.
TEST_CASE("buffer_fill_geometry: the fill rises from the bottom in proportion",
          "[buffer][slider][geometry][fill]") {
    const auto g0 = buffer_fill_geometry(0, -1, kW, kH);
    const auto g50 = buffer_fill_geometry(50, -1, kW, kH);
    const auto g100 = buffer_fill_geometry(100, -1, kW, kH);
    const int bottom = g0.track.y + g0.track.h;

    CHECK(g0.fill.h == 0);
    CHECK(g50.fill.h == g50.track.h / 2);
    CHECK(g100.fill.h == g100.track.h);
    for (const auto& g : {g0, g50, g100}) {
        CHECK(g.fill.y + g.fill.h == bottom); // anchored at the bottom
        CHECK(g.fill.x == g.track.x);
        CHECK(g.fill.w == g.track.w);
    }
    CHECK(g100.fill.y == g100.track.y);
    CHECK(g50.fill.h > buffer_fill_geometry(25, -1, kW, kH).fill.h);
}

TEST_CASE("buffer_fill_geometry: the fill stays in the housing and clamps",
          "[buffer][slider][geometry][fill]") {
    const auto over = buffer_fill_geometry(250, -1, kW, kH);
    CHECK(over.fill.h == over.track.h);
    CHECK(buffer_fill_geometry(-40, -1, kW, kH).fill.h == 0);
    const auto g = buffer_fill_geometry(100, -1, kW, kH);
    CHECK(g.track.y >= g.housing.y);
    CHECK(g.track.y + g.track.h <= g.housing.y + g.housing.h);
    CHECK(g.housing.w == kW);
    // The same footprint the slider's housing has.
    const auto slider = buffer_slider_geometry(0.0f, kW, kH);
    CHECK(g.housing.y == slider.housing.y);
    CHECK(g.housing.h == slider.housing.h);
}

TEST_CASE("buffer_fill_geometry: a tick at the set point, none without one",
          "[buffer][slider][geometry][fill]") {
    const auto g = buffer_fill_geometry(30, 50, kW, kH);
    REQUIRE(g.has_target);
    const int center = g.target.y + g.target.h / 2;
    CHECK(center == g.track.y + g.track.h - g.track.h / 2);
    CHECK(g.target.w > g.track.w - 1); // crosses the housing
    // Moves with the set point, and is independent of the reading.
    CHECK(buffer_fill_geometry(90, 50, kW, kH).target.y == g.target.y);
    CHECK(buffer_fill_geometry(30, 80, kW, kH).target.y < g.target.y);

    CHECK_FALSE(buffer_fill_geometry(30, -1, kW, kH).has_target);
    CHECK(buffer_fill_geometry(30, 0, kW, kH).has_target);
    const auto top = buffer_fill_geometry(30, 400, kW, kH);
    CHECK(top.target.y + top.target.h / 2 == top.track.y);
}

TEST_CASE("buffer_fill_geometry: empty boxes lay out nothing", "[buffer][slider][geometry][fill]") {
    CHECK(buffer_fill_geometry(50, 50, 0, 100).fill.h == 0);
    CHECK_FALSE(buffer_fill_geometry(50, 50, 24, 0).has_target);
}

TEST_CASE("buffer_fill_trace_y: 100 at the top row, 0 at the bottom",
          "[buffer][trace][geometry][fill]") {
    CHECK(buffer_fill_trace_y(100, 101) == 0);
    CHECK(buffer_fill_trace_y(50, 101) == 50);
    CHECK(buffer_fill_trace_y(0, 101) == 100);
    CHECK(buffer_fill_trace_y(-5, 101) == 100);
    CHECK(buffer_fill_trace_y(180, 101) == 0);
    CHECK(buffer_fill_trace_y(50, 0) == 0);
}

TEST_CASE("buffer_trace_polylines: a fill point plots pressure with its own severity",
          "[buffer][trace][geometry][fill]") {
    BufferTracePoint older;
    older.t_ms = 40000;
    older.valid = true;
    older.gauge = BufferGauge::Fill;
    older.fill_pct = 20;
    BufferTracePoint newer = older;
    newer.t_ms = 70000;
    newer.fill_pct = 97;
    newer.status = ClogMeterStatus::Fault;

    const auto lines = buffer_trace_polylines({older, newer}, 100000, 120, 101);
    REQUIRE(lines.size() == 1);
    const auto& l = lines[0];
    REQUIRE(l.size() == 4);
    CHECK(l[0].y == buffer_fill_trace_y(97, 101));
    CHECK(l[0].status == ClogMeterStatus::Fault);
    CHECK(l[2].y == buffer_fill_trace_y(20, 101));
    // Pressure 20 would be a Fault on the bias scale; here it is Ok.
    CHECK(l[2].status == ClogMeterStatus::Ok);
}
