// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tile_layout.cpp
 * @brief Boundaries of the centred-tile sizing decision.
 *
 * The fixtures below are measured, not invented: they come from
 * theme_manager_get_font() and lv_text_get_width() at the medium tier, which is
 * what the suite's default display resolves to. Boundaries pinned against
 * measured widths are the ones the widget really meets.
 *
 * Run with: ./build/bin/helix-tests "[tile][layout]"
 */

#include "../test_helpers/sample_checks.h"
#include "src/ui/panel_widgets/tile_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Medium tier, measured.
///   icon_font_xs..xl  line height 18/24/33/48/66, glyph width 16/24/32/48/64;
///   xxl is #tile_icon_xxl_size, 128, drawn 132 tall, with the value in
///   font_xl and the label in font_body
///   paired value face font_xs/font_xs/font_small/font_body/font_heading
///   line height        18/18/22/24/34
///   "888 / 888°"       57/57/75/85/125      "888°"  26/26/34/38/56
///   label face is one rung below the value face; "Nozzle" 38/38/38/48/56
constexpr TileRungMetrics kMedium[kTileRungs] = {
    //  icon_w icon_h  full  cur  val_h  lbl_w lbl_h
    {16, 18, 57, 26, 18, 38, 18}, {24, 24, 57, 26, 18, 38, 18},  {32, 33, 75, 34, 22, 38, 18},
    {48, 48, 85, 38, 24, 48, 22}, {64, 66, 125, 56, 34, 56, 24}, {128, 132, 140, 63, 38, 56, 24},
};

constexpr int kGap = 4;

TileVerdict verdict_at(int w, int h, bool has_value = true, bool labels = true) {
    return decide_tile_layout(w, h, kGap, kMedium, has_value, labels);
}

} // namespace

TEST_CASE("a one-cell tile keeps a large glyph and all its content", "[tile][layout][1559]") {
    // 110x108 is one cell at medium (2 tracks of 55 minus the gutter).
    // Rung 4 needs 125px of width for "888 / 888°" and does not fit; rung 3
    // needs max(48, 85, 48) = 85 wide and 48+4+24+4+22 = 102 tall.
    TileVerdict v = verdict_at(110, 108);
    CHECK(v.fits);
    CHECK(v.icon_rung == 3);
    CHECK(v.direction == TileDirection::Column);
    CHECK(v.label == TileLabelRung::Label);
    CHECK(v.show_target);
}

TEST_CASE("a half-height strip turns into a row", "[tile][layout][1559]") {
    // No rung stacks inside 52px: the smallest is 18+4+18+4+18 = 62 tall.
    // Row at rung 1 needs 24+4+max(57,38) = 85 wide and max(24, 18+4+18) = 40
    // tall, so direction follows fit rather than aspect ratio.
    TileVerdict v = verdict_at(110, 52);
    CHECK(v.fits);
    CHECK(v.direction == TileDirection::Row);
    CHECK(v.label == TileLabelRung::Label);
}

TEST_CASE("the target half is surrendered before the label", "[tile][layout][1559]") {
    // 50px is under the 57px the narrowest full value needs, and under the
    // 77px the narrowest row needs, so the target goes. The label survives,
    // and dropping the target buys back enough width for rung 3:
    // max(48, 38, 48) = 48 wide, 48+4+24+4+22 = 102 tall.
    TileVerdict v = verdict_at(50, 108);
    CHECK(v.fits);
    CHECK_FALSE(v.show_target);
    CHECK(v.label == TileLabelRung::Label);
    CHECK(v.icon_rung == 3);
}

TEST_CASE("the label is surrendered only after the target", "[tile][layout][1559]") {
    // Too short for any stack that carries a label (smallest is 62), too
    // narrow for any row (narrowest is 77 with the target, 61 without).
    TileVerdict v = verdict_at(44, 44);
    CHECK(v.fits);
    CHECK(v.label == TileLabelRung::None);
    CHECK_FALSE(v.show_target);
}

TEST_CASE("a size that fits keeps fitting as it grows", "[tile][layout][1559]") {
    // The resize clamp walks outward assuming the first accepting size is the
    // nearest one; a non-monotonic answer makes that walk stop at the wrong
    // size or not terminate.
    int fitting = 0;
    helix::test::SampleChecks samples;
    for (int w = 8; w <= 400; w += 4) {
        for (int h = 8; h <= 400; h += 4) {
            if (!verdict_at(w, h).fits) {
                continue;
            }
            ++fitting;
            INFO("fits at " << w << "x" << h << " so it must still fit larger");
            SAMPLE_CHECK(samples, verdict_at(w + 4, h).fits, w * 1000 + h);
            SAMPLE_CHECK(samples, verdict_at(w, h + 4).fits, w * 1000 + h);
            SAMPLE_CHECK(samples, verdict_at(w + 4, h + 4).fits, w * 1000 + h);
        }
    }
    samples.done(3 * fitting);
    // A sweep where nothing ever fits would pass every CHECK above vacuously.
    CHECK(fitting > 100);
}

TEST_CASE("fits budgets the widest value, never a live reading", "[tile][layout][1559]") {
    // Whatever arrangement is chosen, the rung's own worst-case value has to
    // sit inside the box, or a tile sized while reading 95 clips at 888.
    for (int w = 8; w <= 240; w += 2) {
        TileVerdict v = verdict_at(w, 120);
        if (!v.fits) {
            continue;
        }
        const TileRungMetrics& m = kMedium[v.icon_rung];
        const int value_w = v.show_target ? m.value_full_w : m.value_current_w;
        INFO("width " << w << " chose rung " << v.icon_rung);
        if (v.direction == TileDirection::Column) {
            CHECK(std::max(m.icon_w, value_w) <= w);
        } else {
            CHECK(m.icon_w + 4 + value_w <= w);
        }
    }
}

TEST_CASE("a value-less tile is never refused a size a valued one accepts",
          "[tile][layout][1559]") {
    // The seven action tiles draw no value, so their floor must not be set by
    // one. Dropping content can only ever make a box easier to satisfy.
    int value_only_refusals = 0;
    for (int w = 12; w <= 200; w += 4) {
        for (int h = 12; h <= 200; h += 4) {
            const bool with = verdict_at(w, h, /*has_value=*/true).fits;
            const bool without = verdict_at(w, h, /*has_value=*/false).fits;
            if (with) {
                INFO("valued tile fits at " << w << "x" << h << " so a value-less one must");
                CHECK(without);
            }
            if (without && !with) {
                ++value_only_refusals;
            }
        }
    }
    // If the two never diverged the loop would prove nothing about the value
    // being what sets the floor.
    CHECK(value_only_refusals > 0);
}

TEST_CASE("size can hide a label but never show one the setting hides", "[tile][layout][1559]") {
    TileVerdict v = verdict_at(400, 400, /*has_value=*/true, /*labels=*/false);
    CHECK(v.fits);
    CHECK(v.label == TileLabelRung::None);
    // And with room to spare it still takes the largest glyph.
    CHECK(v.icon_rung == kTileRungs - 1);
}

TEST_CASE("a box too small for anything reports that it does not fit", "[tile][layout][1559]") {
    TileVerdict v = verdict_at(10, 10);
    CHECK_FALSE(v.fits);
}

TEST_CASE("a box nothing fits in still stacks along its long axis", "[tile][layout][1559]") {
    // A caller that renders anyway draws this arrangement, so a narrow tall box
    // must stay a column: a row spends width the box has least of.
    const TileVerdict tall = verdict_at(10, 200);
    CHECK_FALSE(tall.fits);
    CHECK(tall.direction == TileDirection::Column);

    const TileVerdict wide = verdict_at(200, 10);
    CHECK_FALSE(wide.fits);
    CHECK(wide.direction == TileDirection::Row);
}

TEST_CASE("a tile gives up text before shrinking its glyph below the authored rung",
          "[tile][layout][1559]") {
    // 80x108: rung 3 needs 85px of width for the target half, so keeping the
    // target means dropping to rung 2. Dropping the target keeps rung 3.
    const TileVerdict authored_lg = decide_tile_layout(80, 108, kGap, kMedium, true, true, 3);
    CHECK(authored_lg.fits);
    CHECK(authored_lg.icon_rung == 3);
    CHECK_FALSE(authored_lg.show_target);
    CHECK(authored_lg.label == TileLabelRung::Label);

    // Below the authored rung, content is what is kept: with no authored rung
    // to protect, the target survives at the smaller glyph.
    const TileVerdict no_floor = decide_tile_layout(80, 108, kGap, kMedium, true, true, 0);
    CHECK(no_floor.icon_rung == 2);
    CHECK(no_floor.show_target);

    // A box that cannot hold the authored glyph at all still shrinks it.
    const TileVerdict tight = decide_tile_layout(40, 40, kGap, kMedium, true, false, 3);
    CHECK(tight.fits);
    CHECK(tight.icon_rung < 3);
}

TEST_CASE("an identity label outlasts the glyph", "[tile][layout][identity]") {
    // 90x90 at authored lg: rung 3 holds the value but not the label under it
    // (48 + 4 + 24 + 4 + 22 = 102 tall), rung 2 holds all three.
    const TileVerdict plain = decide_tile_layout(90, 90, kGap, kMedium, true, true, 3, 0, false);
    CHECK(plain.icon_rung == 3);
    CHECK(plain.label == TileLabelRung::None);

    const TileVerdict named = decide_tile_layout(90, 90, kGap, kMedium, true, true, 3, 0, true);
    CHECK(named.fits);
    CHECK(named.icon_rung == 2);
    CHECK(named.label == TileLabelRung::Label);

    // It still goes once no rung can hold it: 50x22 fits rung 0 as a row with
    // the value beside it (16 + 4 + 26 = 46 wide), and the label beside
    // instead (16 + 4 + 38 = 58) or under it (58 tall) does not.
    const TileVerdict tight = decide_tile_layout(50, 22, kGap, kMedium, true, true, 3, 0, true);
    CHECK(tight.fits);
    CHECK(tight.label == TileLabelRung::None);

    // The setting still wins: an identity label the user hid stays hidden.
    const TileVerdict hidden = decide_tile_layout(90, 90, kGap, kMedium, true, false, 3, 0, true);
    CHECK(hidden.label == TileLabelRung::None);
    CHECK(hidden.icon_rung == 3);
}

TEST_CASE("a row gives up its side inset before it draws", "[tile][layout][1559]") {
    // A row tile insets its sides (styles.tile_row), so a row that fits the
    // bare box by less than the inset must not be chosen.
    // Rung 1 row with no label: 24 + 4 + 26 = 54 wide, 24 tall.
    const TileVerdict bare = decide_tile_layout(60, 30, kGap, kMedium, true, false, 0, 0);
    REQUIRE(bare.fits);
    CHECK(bare.direction == TileDirection::Row);
    const TileVerdict inset = decide_tile_layout(60, 30, kGap, kMedium, true, false, 0, 20);
    CHECK_FALSE((inset.fits && inset.direction == TileDirection::Row && inset.icon_rung >= 1));
}

TEST_CASE("a detail line under the label costs a column its height and a row nothing",
          "[tile][layout][1805]") {
    // Value-less, so rung 3 stacks 48+4+22 = 74 without the line and
    // 74+4+18 = 96 with it.
    TileRungMetrics with_detail[kTileRungs];
    for (int r = 0; r < kTileRungs; ++r) {
        with_detail[r] = kMedium[r];
        with_detail[r].detail_h = 18;
    }
    const TileVerdict plain = decide_tile_layout(110, 96, kGap, kMedium, false, true);
    const TileVerdict tight = decide_tile_layout(110, 95, kGap, with_detail, false, true);
    const TileVerdict exact = decide_tile_layout(110, 96, kGap, with_detail, false, true);
    REQUIRE(plain.direction == TileDirection::Column);
    REQUIRE(plain.label == TileLabelRung::Label);
    CHECK(exact.icon_rung == 3);
    CHECK(exact.direction == TileDirection::Column);
    CHECK(tight.icon_rung < 3);

    // A row never draws the line, so a box too short to stack decides as it would without it.
    const TileVerdict row_plain = decide_tile_layout(160, 38, kGap, kMedium, false, true);
    const TileVerdict row_detail = decide_tile_layout(160, 38, kGap, with_detail, false, true);
    REQUIRE(row_plain.direction == TileDirection::Row);
    CHECK(row_detail.direction == row_plain.direction);
    CHECK(row_detail.icon_rung == row_plain.icon_rung);
    CHECK(row_detail.label == row_plain.label);
}
