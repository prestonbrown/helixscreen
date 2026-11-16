// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_stats_tile_layout.cpp
 * @brief print_stats_content() and decide_print_stats_tile_layout()'s parts per rung. Rung
 * choice itself is choose_size_rung's (test_size_rung.cpp).
 *
 * Run with: ./build/bin/helix-tests "[print_stats_tile][layout]"
 */

#include "src/ui/panel_widgets/print_stats_tile_layout.h"

#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

RungNeeds synthetic_needs() {
    RungNeeds n;
    n.strip = rung_box(100, 40);
    n.cell = rung_box(100, 100);
    n.wide = rung_box(220, 100);
    n.full = rung_box(220, 220);
    return n;
}

PrintStatsTileLayoutInput input_for(int w, int h,
                                    PrintStatsContent content = PrintStatsContent::Activity,
                                    bool lifetime_known = true) {
    PrintStatsTileLayoutInput in;
    in.content_w = w;
    in.content_h = h;
    in.content = content;
    in.lifetime_known = lifetime_known;
    in.needs = synthetic_needs();
    in.count_line_w = 60;
    in.full_line_w = 200;
    in.support_line_h = 20;
    return in;
}

PrintStatsTileVerdict verdict_for(int w, int h,
                                  PrintStatsContent content = PrintStatsContent::Activity,
                                  bool lifetime_known = true) {
    return decide_print_stats_tile_layout(input_for(w, h, content, lifetime_known));
}

} // namespace

TEST_CASE("print stats tile: content follows what is known", "[print_stats_tile][layout]") {
    CHECK(print_stats_content(false, 5, 10) == PrintStatsContent::Loading);
    CHECK(print_stats_content(true, 0, -1) == PrintStatsContent::NoHistory);
    CHECK(print_stats_content(true, 0, 0) == PrintStatsContent::NoHistory);
    CHECK(print_stats_content(true, 0, 12) == PrintStatsContent::Activity);
    CHECK(print_stats_content(true, 3, -1) == PrintStatsContent::Activity);
}

TEST_CASE("print stats tile: parts per rung with the lifetime totals known",
          "[print_stats_tile][layout]") {
    const auto strip = verdict_for(100, 40);
    CHECK_FALSE(strip.show_bars);
    CHECK_FALSE(strip.show_day_labels);
    CHECK_FALSE(strip.side_by_side);
    CHECK(strip.support == PrintStatsSupport::None);

    const auto cell = verdict_for(100, 120);
    CHECK_FALSE(cell.show_bars);
    CHECK_FALSE(cell.show_day_labels);
    CHECK_FALSE(cell.side_by_side);
    CHECK(cell.support == PrintStatsSupport::PrintsOnly);

    const auto wide = verdict_for(220, 100);
    CHECK(wide.show_bars);
    CHECK_FALSE(wide.show_day_labels);
    CHECK(wide.side_by_side);
    CHECK(wide.support == PrintStatsSupport::Full);

    const auto full = verdict_for(220, 220);
    CHECK(full.show_bars);
    CHECK(full.show_day_labels);
    CHECK_FALSE(full.side_by_side);
    CHECK(full.support == PrintStatsSupport::Full);
}

TEST_CASE("print stats tile: the support line steps down rather than cut",
          "[print_stats_tile][layout]") {
    // Cell: the count needs its own line's height under the stacked hero.
    CHECK(verdict_for(100, 119).support == PrintStatsSupport::None);
    CHECK(verdict_for(100, 120).support == PrintStatsSupport::PrintsOnly);

    auto in = input_for(220, 100);
    in.full_line_w = 220;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::Full);
    in.full_line_w = 221;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::PrintsOnly);
    in.count_line_w = 221;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::None);

    auto cell = input_for(100, 200);
    cell.count_line_w = 101;
    CHECK(decide_print_stats_tile_layout(cell).support == PrintStatsSupport::None);
}

TEST_CASE("print stats tile: no support line until the lifetime totals are known",
          "[print_stats_tile][layout]") {
    for (const auto& box : {std::pair{100, 40}, {100, 120}, {220, 100}, {220, 220}}) {
        INFO(box.first << "x" << box.second);
        CHECK(verdict_for(box.first, box.second, PrintStatsContent::Activity, false).support ==
              PrintStatsSupport::None);
    }
}

TEST_CASE("print stats tile: loading and empty history draw no bars or support",
          "[print_stats_tile][layout]") {
    for (const auto content : {PrintStatsContent::Loading, PrintStatsContent::NoHistory}) {
        const auto v = verdict_for(220, 220, content);
        CHECK_FALSE(v.show_bars);
        CHECK_FALSE(v.show_day_labels);
        CHECK(v.support == PrintStatsSupport::None);
        CHECK(v.rung == SizeRung::Full);
    }
}

TEST_CASE("print stats tile: the Strip takes the largest fonts that hold its line whole",
          "[print_stats_tile][layout]") {
    auto in = input_for(100, 40);
    // Widths per PrintStatsStripText, largest fonts first, the short label from 4.
    const int widths[kPrintStatsStripTexts] = {126, 109, 92, 80, 70, 60, 50, 40};
    for (int i = 0; i < kPrintStatsStripTexts; ++i)
        in.strip_line_w[i] = widths[i];
    CHECK(decide_print_stats_tile_layout(in).strip_text == PrintStatsStripText::BodyXsWeek);
    in.content_w = 109;
    CHECK(decide_print_stats_tile_layout(in).strip_text == PrintStatsStripText::BodyWeek);
    // Nothing with the full label fits: the short label in the largest fonts that do.
    in.content_w = 79;
    in.needs.strip = rung_box(70, 40);
    CHECK(decide_print_stats_tile_layout(in).strip_text == PrintStatsStripText::HeadingShort);
    // Nothing fits at all: the smallest form.
    for (int& w : in.strip_line_w)
        w = 500;
    CHECK(decide_print_stats_tile_layout(in).strip_text == PrintStatsStripText::SmallXsShort);
    // Other rungs always draw the largest form.
    CHECK(decide_print_stats_tile_layout(input_for(100, 120)).strip_text ==
          PrintStatsStripText::HeadingWeek);
}
