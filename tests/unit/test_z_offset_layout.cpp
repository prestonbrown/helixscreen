// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_layout.cpp
 * @brief decide_z_offset_layout()'s flags per rung, and the value text it feeds. Rung
 * choice itself is choose_size_rung's (test_size_rung.cpp).
 *
 * The needs below are the boxes ZOffsetWidget measured at the medium tier, in the faces
 * the labels draw in.
 *
 * Run with: ./build/bin/helix-tests "[z_offset][layout]"
 */

#include "src/ui/panel_widgets/z_offset_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr RungNeeds kNeeds{rung_box(30, 37), rung_box(58, 19),  rung_box(37, 66),
                           rung_box(89, 51), rung_box(199, 51), rung_box(166, 142)};
/// A tool changer's Full box carries the tool's name on a line of its own.
constexpr RungNeeds kToolNeeds{rung_box(30, 37), rung_box(58, 19),  rung_box(37, 66),
                               rung_box(89, 51), rung_box(199, 51), rung_box(166, 167)};

ZOffsetVerdict verdict_for(int w, int h, bool tool_changer = false) {
    return decide_z_offset_layout({w, h, tool_changer, tool_changer ? kToolNeeds : kNeeds});
}

} // namespace

TEST_CASE("z_offset layout: flags follow the rung", "[z_offset][layout]") {
    struct Row {
        int w, h;
        bool tool;
        SizeRung rung;
        int decimals;
        bool save_row, steppers, tool_label;
    };
    const Row rows[] = {
        {30, 37, false, SizeRung::Glance, 2, false, false, false},
        {58, 19, false, SizeRung::Strip, 3, false, false, false},
        {37, 66, false, SizeRung::Column, 3, false, false, false},
        {89, 51, false, SizeRung::Cell, 3, false, false, false},
        {199, 51, false, SizeRung::Wide, 3, true, false, false},
        {166, 142, false, SizeRung::Full, 3, true, true, false},
        {400, 400, false, SizeRung::Full, 3, true, true, false},
        // a tool changer names its tool from Full only
        {199, 51, true, SizeRung::Wide, 3, true, false, false},
        {166, 167, true, SizeRung::Full, 3, true, true, true},
    };
    for (const auto& r : rows) {
        INFO(r.w << "x" << r.h << " tool=" << r.tool);
        const auto v = verdict_for(r.w, r.h, r.tool);
        CHECK(v.rung == r.rung);
        CHECK(v.decimals == r.decimals);
        CHECK(v.show_save_row == r.save_row);
        CHECK(v.show_steppers == r.steppers);
        CHECK(v.show_tool_label == r.tool_label);
    }
}

TEST_CASE("z_offset value text: decimals, sign and the unknown value", "[z_offset][layout]") {
    struct Row {
        int microns;
        int decimals;
        const char* text;
    };
    const Row rows[] = {
        {0, 2, "0.00"},     {0, 3, "0.000"},      {50, 3, "0.050"}, {-125, 3, "-0.125"},
        {-126, 2, "-0.13"}, {-4, 2, "0.00"},      {4, 2, "0.00"},   {-6, 2, "-0.01"},
        {1250, 3, "1.250"}, {-2000, 3, "-2.000"},
    };
    for (const auto& r : rows) {
        INFO(r.microns << "um at " << r.decimals);
        CHECK(format_z_offset_value(r.microns, r.decimals) == r.text);
    }
    CHECK(format_z_offset_value(std::nullopt, 3) == "--");
}
