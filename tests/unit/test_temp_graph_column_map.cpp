// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_temp_graph_column_map.cpp
 * @brief The per-column mapping the gradient renderer and its cache both use.
 *
 * Run with: ./build/bin/helix-tests "[temp_graph][gradient]"
 *
 * column_series_y() decides two things at once, which is why it is worth
 * testing on its own:
 *
 *   - WHERE the gradient fill starts in a column (the renderer), and
 *   - WHETHER a redraw can be skipped (the cache signature hashes it).
 *
 * The second is the interesting one. A pushed sample always shifts the curve,
 * so the cache is dirtied every time; hashing raw sample values is exact and
 * useless, because real temperatures jitter a tenth of a degree per sample and
 * the hash then always changes. Measured on a Creality K2 Plus that produced 40
 * renders, 0 skips, median 167ms each. Quantising to the pixel row instead made
 * roughly half the frames skippable - so the "a sub-pixel change maps to the
 * same row" case below is the whole optimisation, and the "a real move does
 * NOT" case is what stops it freezing the graph.
 */

#include "temp_graph_column_map.h"

#include "../catch_amalgamated.hpp"

using helix::temp_graph::COLUMN_NOT_DRAWN;
using helix::temp_graph::column_series_y;
using helix::temp_graph::column_to_point;

namespace {
// Deci-degrees, matching the chart's storage: 0-250C over a 100px content box.
constexpr int32_t Y_MIN = 0;
constexpr int32_t Y_MAX = 2500;
constexpr int32_t CH = 100;

int32_t at(int32_t deci) {
    return column_series_y(deci, deci, 0, Y_MIN, Y_MAX, CH);
}
} // namespace

TEST_CASE("Column mapping places a value at the expected pixel row",
          "[temp_graph][gradient][column_map]") {
    // Top of range sits at the top row, bottom of range falls to the floor and
    // so is not drawn at all.
    CHECK(at(Y_MAX) == 0);
    CHECK(at(Y_MIN) == COLUMN_NOT_DRAWN);

    // Mid-range lands mid-box. Derived from the range, not copied from the
    // implementation: half the span should be half the height above the floor.
    CHECK(at(Y_MAX / 2) == CH / 2);
}

TEST_CASE("A sub-pixel change within a row maps to the same row",
          "[temp_graph][gradient][column_map]") {
    // One pixel spans (Y_MAX - Y_MIN) / CH = 25 deci-degrees, i.e. 2.5 C, and
    // the per-sample jitter on a real printer is far smaller. Sit mid-bucket:
    // the contract is that jitter INSIDE a row is invisible, not that every
    // small change is - a value sitting on a boundary still flips, which is the
    // next test and is why the measured skip rate was ~47% rather than 100%.
    const int32_t one_px = (Y_MAX - Y_MIN) / CH; // 25
    const int32_t base = 1000 + one_px / 2;      // mid-row, not on the edge
    const int32_t row = at(base);
    REQUIRE(row != COLUMN_NOT_DRAWN);

    CHECK(at(base + 1) == row); // +0.1 C
    CHECK(at(base + 5) == row); // +0.5 C
    CHECK(at(base - 5) == row); // -0.5 C

    // This is exactly the case the old value-hash could not see, and why it
    // never skipped a frame.
    CHECK(at(base + 1) == at(base - 1));
}

TEST_CASE("A change across a row boundary is still reported",
          "[temp_graph][gradient][column_map]") {
    // Quantising is not free: a value sitting on a boundary flips row for a
    // 0.1 C move, so those frames still render. That is correct - the skip must
    // never hide a change that reaches the screen - and it is why the optimisation
    // removes about half the renders rather than all of them.
    const int32_t one_px = (Y_MAX - Y_MIN) / CH;
    const int32_t on_edge = 1000; // exactly 40 * one_px
    REQUIRE(on_edge % one_px == 0);
    CHECK(at(on_edge + 1) != at(on_edge - 1));
}

TEST_CASE("A change big enough to move a row is reported", "[temp_graph][gradient][column_map]") {
    const int32_t base = 1000;
    const int32_t row = at(base);
    REQUIRE(row != COLUMN_NOT_DRAWN);

    // A full pixel's worth must move, or the skip would freeze a graph that is
    // genuinely changing.
    const int32_t one_px = (Y_MAX - Y_MIN) / CH;
    CHECK(at(base + 2 * one_px) != row);
    CHECK(at(base - 2 * one_px) != row);
}

TEST_CASE("Missing points are reported as not drawn", "[temp_graph][gradient][column_map]") {
    SECTION("both ends missing") {
        CHECK(column_series_y(LV_CHART_POINT_NONE, LV_CHART_POINT_NONE, 128, Y_MIN, Y_MAX, CH) ==
              COLUMN_NOT_DRAWN);
    }
    SECTION("one end missing falls back to the other, and still draws") {
        const int32_t solo = column_series_y(LV_CHART_POINT_NONE, 1000, 128, Y_MIN, Y_MAX, CH);
        CHECK(solo != COLUMN_NOT_DRAWN);
        CHECK(solo == at(1000));
        CHECK(column_series_y(1000, LV_CHART_POINT_NONE, 128, Y_MIN, Y_MAX, CH) == solo);
    }
}

TEST_CASE("Interpolation honours the fractional weight", "[temp_graph][gradient][column_map]") {
    const int32_t lo = 500;
    const int32_t hi = 2000;

    // frac 0 is entirely the first point; 255 is (all but a step of) the second.
    CHECK(column_series_y(lo, hi, 0, Y_MIN, Y_MAX, CH) == at(lo));

    const int32_t mid = column_series_y(lo, hi, 128, Y_MIN, Y_MAX, CH);
    // Rows grow downward, so the midpoint sits between the two endpoint rows.
    CHECK(mid < at(lo));
    CHECK(mid > at(hi));
}

TEST_CASE("Column-to-point mapping spans the data and clamps at the end",
          "[temp_graph][gradient][column_map]") {
    constexpr int32_t CW = 200;
    constexpr int32_t PC = 50;

    int32_t idx = -1;
    int32_t frac = -1;

    column_to_point(0, CW, PC, idx, frac);
    CHECK(idx == 0);
    CHECK(frac == 0);

    // The last column must never index past the final pair, or the walk would
    // read off the end of the series.
    column_to_point(CW - 1, CW, PC, idx, frac);
    CHECK(idx == PC - 2);
    CHECK(frac == 255);

    // Monotonic across the sweep.
    int32_t prev = -1;
    for (int32_t x = 0; x < CW; ++x) {
        column_to_point(x, CW, PC, idx, frac);
        CHECK(idx >= prev);
        CHECK(idx <= PC - 2);
        CHECK(frac >= 0);
        CHECK(frac <= 255);
        prev = idx;
    }
}
