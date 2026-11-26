// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mini_bars.cpp
 * @brief The bar view's layout: plan_ams_mini_bars() shows every lane when the
 * box has room for readable bars, keeps each unit whole and in order, boxes the
 * units, and drops whole units (never the active one) only when it must.
 */

#include "ui_ams_mini_status.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsMiniBarBoxes;
using helix::ui::AmsMiniBarMetrics;
using helix::ui::AmsMiniBarPlan;
using helix::ui::plan_ams_mini_bars;

namespace {

/// The home tile's bar metrics, with the bar width capped at @p max_bar_w.
AmsMiniBarMetrics metrics(int max_bar_w = 14) {
    AmsMiniBarMetrics m;
    m.gap = 2;
    m.unit_gap = 4;
    m.line_gap = 2;
    m.box_pad = 2;
    m.box_border = 1;
    m.status_h = 5;
    m.min_bar_w = 4;
    m.max_bar_w = max_bar_w;
    m.min_bar_h = 8;
    m.max_bar_h = 4 * max_bar_w;
    m.overflow_w = 16;
    return m;
}

/// The torture mock: five units, 16 lanes.
const std::vector<int> kTorture = {4, 2, 4, 2, 4};

struct Extent {
    int w = 0;
    int h = 0;
};

/// What each unit's box adds on each axis, both sides.
int box_extent(const AmsMiniBarPlan& p, const AmsMiniBarMetrics& m) {
    switch (p.boxes) {
    case AmsMiniBarBoxes::Every:
        return 2 * (m.box_pad + m.box_border);
    case AmsMiniBarBoxes::ActiveOnly:
        return 2 * m.box_border;
    case AmsMiniBarBoxes::None:
        return 0;
    }
    return 0;
}

/// Lanes the plan shows of @p unit.
int shown_of(const AmsMiniBarPlan& p, const std::vector<int>& lanes, int unit) {
    return unit == p.cut_unit ? p.cut_lanes : lanes[static_cast<size_t>(unit)];
}

/// What the plan takes, laid out the way the widget lays it out.
Extent extent(const AmsMiniBarPlan& p, const std::vector<int>& lanes, const AmsMiniBarMetrics& m) {
    Extent e;
    for (const auto& line : p.lines) {
        int w = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            const int n = shown_of(p, lanes, line[i]);
            w += n * p.bar_w + (n - 1) * m.gap + box_extent(p, m);
            if (i > 0)
                w += m.unit_gap;
        }
        e.w = std::max(e.w, w);
    }
    const int lines = static_cast<int>(p.lines.size());
    e.h = lines * (p.bar_h + m.status_h + box_extent(p, m)) + std::max(0, lines - 1) * m.line_gap;
    return e;
}

std::vector<int> placed_units(const AmsMiniBarPlan& p) {
    std::vector<int> out;
    for (const auto& line : p.lines)
        out.insert(out.end(), line.begin(), line.end());
    return out;
}

int placed_lanes(const AmsMiniBarPlan& p, const std::vector<int>& lanes) {
    int n = 0;
    for (int u : placed_units(p))
        n += shown_of(p, lanes, u);
    return n;
}

} // namespace

TEST_CASE("ams_mini bars: a five-unit rig shows every lane where a 3x2 tile has room",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(21); // 168px tile: width_px / 8
    const AmsMiniBarPlan p = plan_ams_mini_bars(kTorture, 2, 168, 79, m);

    CHECK(p.hidden_lanes == 0);
    CHECK(placed_lanes(p, kTorture) == 16);
    CHECK(p.boxes == AmsMiniBarBoxes::Every);
    CHECK(p.bar_h >= m.min_bar_h);
    const Extent e = extent(p, kTorture, m);
    CHECK(e.w <= 168);
    CHECK(e.h <= 79);
}

TEST_CASE("ams_mini bars: each unit stays whole and in order", "[ams_mini][ams_mini_bars]") {
    const auto m = metrics();
    const AmsMiniBarPlan p = plan_ams_mini_bars(kTorture, 2, 108, 79, m);

    const std::vector<int> units = placed_units(p);
    CHECK(units == std::vector<int>{0, 1, 2, 3, 4});
    CHECK(p.lines.size() >= 2); // 16 lanes in 108px take more than one line
}

TEST_CASE("ams_mini bars: a single unit is never boxed", "[ams_mini][ams_mini_bars]") {
    const auto m = metrics();
    const AmsMiniBarPlan p = plan_ams_mini_bars({4}, 0, 108, 79, m);

    CHECK(p.boxes == AmsMiniBarBoxes::None);
    REQUIRE(p.lines.size() == 1);
    CHECK(p.bar_w == m.max_bar_w); // four lanes sit under the tile's width cap
    CHECK(p.hidden_lanes == 0);
}

TEST_CASE("ams_mini bars: eight lanes of one unit all show in a 90px tile",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(11);
    const AmsMiniBarPlan p = plan_ams_mini_bars({8}, -1, 81, 60, m);

    CHECK(p.hidden_lanes == 0);
    CHECK(placed_lanes(p, {8}) == 8);
    CHECK(extent(p, {8}, m).w <= 81);
}

TEST_CASE("ams_mini bars: a tight tile keeps only the active unit's box",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(13);
    // The 800x480 2x1 tile (97x32 for the bars): boxing every unit leaves the
    // bars too short, boxing only the active one keeps all 16 lanes readable.
    const AmsMiniBarPlan p = plan_ams_mini_bars(kTorture, 2, 97, 32, m);

    CHECK(p.boxes == AmsMiniBarBoxes::ActiveOnly);
    CHECK(p.hidden_lanes == 0);
    CHECK(p.bar_h >= m.min_bar_h);
    const Extent e = extent(p, kTorture, m);
    CHECK(e.w <= 97);
    CHECK(e.h <= 32);
}

TEST_CASE("ams_mini bars: units drop whole, never the active one, and +N counts their lanes",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(10);
    const std::vector<int> big = {4, 4, 4, 4, 4, 4, 4, 4};
    const int active = 6;
    const AmsMiniBarPlan p = plan_ams_mini_bars(big, active, 80, 22, m);

    REQUIRE(p.hidden_lanes > 0);
    const std::vector<int> units = placed_units(p);
    CHECK(std::find(units.begin(), units.end(), active) != units.end());
    CHECK(p.hidden_lanes + placed_lanes(p, big) == 32);
    CHECK(std::is_sorted(units.begin(), units.end()));
    CHECK(p.bar_h >= m.min_bar_h);
    CHECK(p.bar_w >= m.min_bar_w);
    CHECK(extent(p, big, m).w <= 80 - m.overflow_w);
}

TEST_CASE("ams_mini bars: one unit too wide for a micro tile shows what fits and +N the rest",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(8);
    // A 12-gate ERCF on a 64px micro tile: 57px for the bars.
    const AmsMiniBarPlan p = plan_ams_mini_bars({12}, -1, 57, 40, m);

    REQUIRE(p.hidden_lanes > 0);
    CHECK(placed_lanes(p, {12}) + p.hidden_lanes == 12);
    CHECK(p.bar_w >= m.min_bar_w);
    CHECK(extent(p, {12}, m).w <= 57 - m.overflow_w);
}

TEST_CASE("ams_mini bars: a cut unit keeps its active lane in view", "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(8);
    // The same 12-gate ERCF, loaded on lane 10, which lies past the lanes that fit.
    const AmsMiniBarPlan p = plan_ams_mini_bars({12}, 0, 57, 40, m, 10);

    REQUIRE(p.cut_unit == 0);
    REQUIRE(p.cut_lanes < 10);
    CHECK(p.cut_first <= 10);
    CHECK(p.cut_first + p.cut_lanes > 10);
    CHECK(p.cut_first + p.cut_lanes <= 12);
    CHECK(p.hidden_lanes == 12 - p.cut_lanes);

    // An active lane among the first lanes keeps the window at the start.
    CHECK(plan_ams_mini_bars({12}, 0, 57, 40, m, 2).cut_first == 0);
}

TEST_CASE("ams_mini bars: an absent unit is neither placed nor counted",
          "[ams_mini][ams_mini_bars]") {
    const auto m = metrics();
    const std::vector<int> lanes = {4, 0, 4};
    const AmsMiniBarPlan p = plan_ams_mini_bars(lanes, 0, 108, 79, m);

    const std::vector<int> units = placed_units(p);
    CHECK(units == std::vector<int>{0, 2});
    CHECK(p.hidden_lanes == 0);
}

TEST_CASE("ams_mini bars: no active unit boxes every unit or none", "[ams_mini][ams_mini_bars]") {
    const auto m = metrics(13);
    const AmsMiniBarPlan p = plan_ams_mini_bars(kTorture, -1, 97, 32, m);

    CHECK(p.boxes != AmsMiniBarBoxes::ActiveOnly);
    CHECK(p.hidden_lanes == 0);
}

TEST_CASE("ams_mini bars: no plan leaves its box", "[ams_mini][ams_mini_bars]") {
    std::mt19937 rng(20261126);
    std::uniform_int_distribution<int> unit_count(1, 8);
    std::uniform_int_distribution<int> lane_count(0, 6);
    std::uniform_int_distribution<int> width(40, 260);
    std::uniform_int_distribution<int> height(16, 140);

    for (int trial = 0; trial < 500; ++trial) {
        std::vector<int> lanes(static_cast<size_t>(unit_count(rng)));
        for (int& n : lanes)
            n = lane_count(rng);
        if (std::accumulate(lanes.begin(), lanes.end(), 0) == 0)
            lanes[0] = 1;
        std::vector<int> present;
        for (size_t i = 0; i < lanes.size(); ++i)
            if (lanes[i] > 0)
                present.push_back(static_cast<int>(i));
        const int active =
            (trial % 3 == 0) ? -1 : present[static_cast<size_t>(trial) % present.size()];
        const int w = width(rng);
        const int h = height(rng);
        const auto m = metrics(std::clamp(w / 8, 4, 24));

        const AmsMiniBarPlan p = plan_ams_mini_bars(lanes, active, w, h, m);
        INFO("trial " << trial << " " << w << "x" << h);

        const int total = std::accumulate(lanes.begin(), lanes.end(), 0);
        CHECK(placed_lanes(p, lanes) + p.hidden_lanes == total);
        const std::vector<int> units = placed_units(p);
        CHECK(std::is_sorted(units.begin(), units.end()));
        if (active >= 0)
            CHECK(std::find(units.begin(), units.end(), active) != units.end());
        CHECK(p.bar_w <= m.max_bar_w);
        CHECK(p.bar_h <= m.max_bar_h);

        const Extent e = extent(p, lanes, m);
        CHECK(e.w <= w - (p.hidden_lanes > 0 ? m.overflow_w : 0));
        CHECK(e.h <= h);
        CHECK(p.bar_w >= m.min_bar_w);
        CHECK(p.bar_h >= m.min_bar_h);
    }
}
