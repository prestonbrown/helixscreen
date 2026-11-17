// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_eta_estimator.cpp
 * @brief The remaining-time ladder, rung by rung.
 *
 * Run with: ./build/bin/helix-tests "[print][eta]"
 *
 * Five rungs decide the number a user watches for hours, and the interesting
 * one blends two disagreeing sources and then smooths the result. Every case
 * here names its own arithmetic rather than restating the implementation's
 * formula, because a test that recomputes the thing under test cannot fail.
 *
 * kNoEstimate is a rung too, and the one most easily broken by accident:
 * returning 0 where there is no answer reads as "finished" on the panel.
 */

#include "print_eta_estimator.h"

#include "../catch_amalgamated.hpp"

using helix::PrintEtaEstimator;
using helix::PrintEtaInputs;

namespace {

/// A job with a slicer estimate and nothing else established yet.
PrintEtaInputs job(int progress_pct, int print_duration_s, int slicer_estimate_s) {
    PrintEtaInputs in;
    in.progress_pct = progress_pct;
    in.print_duration_s = print_duration_s;
    in.slicer_estimate_s = slicer_estimate_s;
    return in;
}

} // namespace

TEST_CASE("ETA: while preparing, what is left is the prep plus the whole job", "[print][eta]") {
    PrintEtaEstimator est;
    PrintEtaInputs in;
    in.progress_pct = 0;
    in.preprint_remaining_s = 120;
    in.slicer_estimate_s = 600;

    // 120s of prep still to run, then a 600s job nobody has started.
    CHECK(est.remaining_seconds(in) == 720);
}

TEST_CASE("ETA: a finished print has nothing left", "[print][eta]") {
    PrintEtaEstimator est;
    CHECK(est.remaining_seconds(job(100, 5000, 6000)) == 0);
}

TEST_CASE("ETA: under 5 percent the slicer estimate stands alone", "[print][eta]") {
    PrintEtaEstimator est;

    // 4% done of a 1000s job leaves 96% of it, and the four seconds of evidence
    // the printer has produced so far are not worth extrapolating from - note
    // the wildly wrong print_duration is ignored entirely.
    CHECK(est.remaining_seconds(job(4, 1, 1000)) == 960);
}

TEST_CASE("ETA: past the blend window it extrapolates the observed rate", "[print][eta]") {
    PrintEtaEstimator est;

    // Half done in 600s, so the other half should also take about 600s. Above
    // the blend window, so the slicer's opinion does not enter.
    CHECK(est.remaining_seconds(job(50, 600, 99999)) == 600);
}

TEST_CASE("ETA: with no elapsed print time there is no rate, so the slicer answers",
          "[print][eta]") {
    PrintEtaEstimator est;

    // Progress without a clock: half of a 1000s job remains.
    CHECK(est.remaining_seconds(job(50, 0, 1000)) == 500);
}

TEST_CASE("ETA: inside the blend window the answer sits between the two sources", "[print][eta]") {
    // 5% done after 100s extrapolates to 1900s remaining, which is what a tiny
    // sample does to you. The slicer says 950s. The blend has to land strictly
    // between them, or it is not blending.
    PrintEtaEstimator blended;
    const int with_slicer = blended.remaining_seconds(job(5, 100, 1000));

    PrintEtaEstimator bare;
    const int without_slicer = bare.remaining_seconds(job(5, 100, 0));

    CHECK(without_slicer == 1900);
    CHECK(with_slicer > 950);
    CHECK(with_slicer < 1900);
}

TEST_CASE("ETA: the blend hands over cleanly at the top of the window", "[print][eta]") {
    // At 15% the slicer's weight has fallen to zero, so blended and bare must
    // agree. A weight that does not reach zero makes the number jump the moment
    // the blend switches off.
    PrintEtaEstimator blended;
    PrintEtaEstimator bare;
    CHECK(blended.remaining_seconds(job(15, 600, 9000)) == bare.remaining_seconds(job(15, 600, 0)));
}

TEST_CASE("ETA: the estimate is smoothed, not snapped, toward a new rate", "[print][eta]") {
    PrintEtaEstimator est;

    // First sample seeds: 600s elapsed at 50% extrapolates to 600s left.
    REQUIRE(est.remaining_seconds(job(50, 600, 0)) == 600);

    // The printer then slows: 800s elapsed at the same 50% says 800s left. At
    // this progress alpha is capped at 0.3, so the published number moves three
    // tenths of the way: 0.3*800 + 0.7*600.
    CHECK(est.remaining_seconds(job(50, 800, 0)) == 660);
}

TEST_CASE("ETA: reset forgets the previous print", "[print][eta]") {
    PrintEtaEstimator est;
    REQUIRE(est.remaining_seconds(job(50, 600, 0)) == 600);

    // Without the reset the next print's first sample would be averaged against
    // the last print's final answer, which is how a short job inherits a long
    // job's ETA.
    est.reset();
    CHECK(est.remaining_seconds(job(50, 800, 0)) == 800);
}

TEST_CASE("ETA: inputs that support no answer say so rather than saying zero", "[print][eta]") {
    PrintEtaEstimator est;

    // Nothing started and no slicer estimate: zero here would render as a
    // finished print.
    CHECK(est.remaining_seconds(job(0, 0, 0)) == PrintEtaEstimator::kNoEstimate);

    // Preparing, but the file carried no estimate to add the prep to.
    PrintEtaInputs prep_only;
    prep_only.progress_pct = 0;
    prep_only.preprint_remaining_s = 120;
    CHECK(est.remaining_seconds(prep_only) == PrintEtaEstimator::kNoEstimate);

    // Underway, but with neither a clock nor an estimate to go on.
    CHECK(est.remaining_seconds(job(40, 0, 0)) == PrintEtaEstimator::kNoEstimate);
}

namespace {

PrintEtaInputs excluded(PrintEtaInputs in, double share) {
    in.excluded_share = share;
    return in;
}

} // namespace

TEST_CASE("ETA: excluded work comes out of the slicer's estimate", "[print][eta][exclude_object]") {
    SECTION("under 5 percent") {
        // Half the job excluded leaves 500s of a 1000s job, and 96% of that is
        // still to print.
        PrintEtaEstimator est;
        CHECK(est.remaining_seconds(excluded(job(4, 1, 1000), 0.5)) == 480);
    }
    SECTION("while preparing") {
        // Parts excluded before the start shrink the job, not the prep.
        PrintEtaEstimator est;
        PrintEtaInputs in;
        in.preprint_remaining_s = 120;
        in.slicer_estimate_s = 600;
        in.excluded_share = 0.5;
        CHECK(est.remaining_seconds(in) == 420);
    }
}

TEST_CASE("ETA: an exclusion mid-print scales the rate seen before it",
          "[print][eta][exclude_object]") {
    SECTION("half the work excluded halves what is left, at once") {
        PrintEtaEstimator est;
        REQUIRE(est.remaining_seconds(job(50, 600, 0)) == 600);
        // 12s per percent with every part, 6s per percent with half of them,
        // over the 50% still to go. No smoothing toward the old 600s.
        CHECK(est.remaining_seconds(excluded(job(50, 600, 0), 0.5)) == 300);
    }
    SECTION("every part but the current one excluded") {
        PrintEtaEstimator est;
        REQUIRE(est.remaining_seconds(job(50, 600, 0)) == 600);
        // Three of four equal parts gone: a quarter of the work per percent.
        CHECK(est.remaining_seconds(excluded(job(50, 600, 0), 0.75)) == 150);
    }
}

TEST_CASE("ETA: after an exclusion the rate observed since then takes over",
          "[print][eta][exclude_object]") {
    PrintEtaEstimator est;
    REQUIRE(est.remaining_seconds(job(50, 600, 0)) == 600);
    REQUIRE(est.remaining_seconds(excluded(job(50, 600, 0), 0.5)) == 300);

    // The ten points since the exclusion took 90s, 9s per percent rather than
    // the 6s the share predicted, so 40% left is 360s. Smoothed from 300s at
    // alpha 0.3: 0.3*360 + 0.7*300.
    CHECK(est.remaining_seconds(excluded(job(60, 690, 0), 0.5)) == 318);
}

TEST_CASE("ETA: parts excluded before printing leave the observed rate alone",
          "[print][eta][exclude_object]") {
    PrintEtaEstimator est;
    // Excluded at 0%: no second of the print was spent on the excluded parts.
    REQUIRE(est.remaining_seconds(excluded(job(0, 0, 1000), 0.5)) ==
            PrintEtaEstimator::kNoEstimate);
    CHECK(est.remaining_seconds(excluded(job(50, 600, 1000), 0.5)) == 600);
}

TEST_CASE(
    "ETA: parts already excluded when the estimator first sees the print leave the rate alone",
    "[print][eta][exclude_object]") {
    // Started or reconnected mid-print: when the parts were excluded is
    // unknown, so the rate observed so far stands for the work still left.
    PrintEtaEstimator est;
    CHECK(est.remaining_seconds(excluded(job(50, 600, 0), 0.5)) == 600);
    // Still 12s per percent ten points on, so 40% left is 480s. Smoothed from
    // 600s at alpha 0.3: 0.3*480 + 0.7*600.
    CHECK(est.remaining_seconds(excluded(job(60, 720, 0), 0.5)) == 564);
}
