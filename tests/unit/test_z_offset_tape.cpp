// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_tape.cpp
 * @brief The Z offset step's tape: zoom per step, ticks and their labels, where
 * the estimated bed draws, the drag-point estimate and the guide line.
 *
 * Rows use the 800x480 diagram's geometry: tip at 107, ruler rows 32..146,
 * the bed's paper at 178 when drawn at the bottom.
 *
 * Run with: ./build/bin/helix-tests "[zoffset][tape]"
 */

#include "src/ui/calibration/z_offset_tape.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ztape;

namespace {
constexpr int kTip = 107, kTop = 32, kBottom = 146, kStaticPaper = 178;
}

TEST_CASE("z offset tape: the zoom follows the step", "[zoffset][tape]") {
    CHECK(zoom_for_step(1000).span_microns == 4000);
    CHECK(zoom_for_step(100).span_microns == 1000);
    CHECK(zoom_for_step(50).span_microns == 400);
    CHECK(zoom_for_step(10).span_microns == 200);
    CHECK(zoom_for_step(100).every_microns == 250);
    CHECK(range_text(zoom_for_step(1000)) == "\xC2\xB1"
                                             "2 mm");
    CHECK(range_text(zoom_for_step(100)) == "\xC2\xB1"
                                            "0.5 mm");
    CHECK(range_text(zoom_for_step(10)) == "\xC2\xB1"
                                           "0.1 mm");
}

TEST_CASE("z offset tape: step indices map to the four sizes", "[zoffset][tape]") {
    CHECK(step_microns(0) == 10);
    CHECK(step_microns(3) == 1000);
    CHECK(step_microns(kDefaultStepIndex) == 100);
    CHECK(step_microns(7) == 100);
    CHECK(step_microns(-1) == 100);
}

TEST_CASE("z offset tape: ticks scroll past the fixed tip", "[zoffset][tape]") {
    const Zoom z = zoom_for_step(100);
    const auto t = ticks(1000, z, kTip, kTop, kBottom);
    // 114 px per mm: 1.5 at 50, 1.25 at 79, 1.0 at the tip, 0.75 at 136.
    REQUIRE(t.size() == 4);
    CHECK(t[0].microns == 750);
    CHECK(t[0].y == 136);
    CHECK(t[2].microns == 1250);
    CHECK(t[2].y == 79);
    CHECK(t[1].y == kTip);
    CHECK(t[3].microns == 1500);
    CHECK(t[3].y == 50);

    // Moving Z down by a tick moves every tick down the screen with it.
    const auto lower = ticks(750, z, kTip, kTop, kBottom);
    REQUIRE_FALSE(lower.empty());
    bool tip_tick = false;
    for (const auto& k : lower) {
        CHECK(k.y >= kTop);
        CHECK(k.y <= kBottom);
        tip_tick = tip_tick || (k.microns == 750 && k.y == kTip);
    }
    CHECK(tip_tick);
}

TEST_CASE("z offset tape: ticks below zero keep their spacing", "[zoffset][tape]") {
    const auto t = ticks(-120, zoom_for_step(10), kTip, kTop, kBottom);
    REQUIRE(t.size() >= 2);
    for (size_t i = 1; i < t.size(); ++i) {
        CHECK(t[i].microns - t[i - 1].microns == 50);
    }
}

TEST_CASE("z offset tape: labels never round a quarter", "[zoffset][tape]") {
    const Zoom z = zoom_for_step(100);
    CHECK(tick_label(1500, z) == "1.5");
    CHECK(tick_label(1250, z) == "1.25");
    CHECK(tick_label(750, z) == "0.75");
    CHECK(tick_label(0, z) == "0.0");
    CHECK(tick_label(-250, z) == "-0.25");
    CHECK(tick_label(2000, zoom_for_step(1000)) == "2");
    CHECK(tick_label(-1000, zoom_for_step(1000)) == "-1");
    CHECK(tick_label(50, zoom_for_step(10)) == "0.05");
}

TEST_CASE("z offset tape: the estimated bed rises to the nozzle", "[zoffset][tape]") {
    const Zoom z = zoom_for_step(100);
    SECTION("no estimate: bed unknown, at the bottom") {
        const auto b = place_bed(1000, kNoEstimate, z, kTip, kTop, kBottom, kStaticPaper);
        CHECK(b.view == BedView::Unknown);
    }
    SECTION("far above the estimate: drawn at the bottom, further below") {
        const auto b = place_bed(1000, 235, z, kTip, kTop, kBottom, kStaticPaper);
        CHECK(b.view == BedView::FarBelow);
    }
    SECTION("near the estimate: the paper sits where the estimate is") {
        const auto b = place_bed(400, 235, z, kTip, kTop, kBottom, kStaticPaper);
        CHECK(b.view == BedView::Estimated);
        CHECK(b.paper_top == kTip + 19); // 0.165 mm at 114 px/mm
    }
    SECTION("at the estimate: the paper is at the tip, not yet past") {
        const auto b = place_bed(235, 235, z, kTip, kTop, kBottom, kStaticPaper);
        CHECK(b.view == BedView::Estimated);
        CHECK(b.paper_top == kTip);
    }
    SECTION("below the estimate: the nozzle rests on it, past last time") {
        const auto b = place_bed(200, 235, z, kTip, kTop, kBottom, kStaticPaper);
        CHECK(b.view == BedView::PastLastTime);
        CHECK(b.paper_top == kTip);
    }
}

TEST_CASE("z offset tape: the drag point comes from the start and the saved offset",
          "[zoffset][tape]") {
    // PROBE_CALIBRATE starts 5 mm over the trigger point.
    CHECK(expected_drag_microns(EstimateSource::ProbeCalibrate, 6200, 1200) == 0);
    CHECK(expected_drag_microns(EstimateSource::ProbeCalibrate, 5000, -1850) == 1850);
    CHECK(expected_drag_microns(EstimateSource::ProbeCalibrate, std::nullopt, 1200) == kNoEstimate);
    CHECK(expected_drag_microns(EstimateSource::Endstop, 3000, 1200) == 0);
    CHECK(expected_drag_microns(EstimateSource::None, 5000, 1200) == kNoEstimate);

    CHECK(estimate_source_for("PROBE_CALIBRATE") == EstimateSource::ProbeCalibrate);
    CHECK(estimate_source_for("Z_ENDSTOP_CALIBRATE") == EstimateSource::Endstop);
    CHECK(estimate_source_for("BEACON_CALIBRATE") == EstimateSource::None);
}

TEST_CASE("z offset tape: the guide reads against the estimate, or from the start",
          "[zoffset][tape]") {
    auto g = guide(1000, 235, 5000, false);
    CHECK(g.text == "about 0.765 mm above where the paper dragged last time");
    CHECK(g.tone == GuideTone::Above);
    CHECK(guide(1000, 235, 5000, true).text == "0.765 mm above last time");

    g = guide(200, 235, 5000, false);
    CHECK(g.text == "about 0.035 mm below where the paper dragged last time");
    CHECK(g.tone == GuideTone::Past);

    g = guide(235, 235, 5000, false);
    CHECK(g.text == "right where the paper dragged last time");
    CHECK(g.tone == GuideTone::Above);

    g = guide(4700, kNoEstimate, 5000, false);
    CHECK(g.text == "down 0.300 mm from start");
    CHECK(g.tone == GuideTone::Neutral);
    CHECK(guide(5100, kNoEstimate, 5000, true).text == "up 0.100 from start");
    CHECK(guide(5000, kNoEstimate, 5000, false).text == "start position");
}
