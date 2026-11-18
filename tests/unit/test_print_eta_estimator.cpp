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

#include <algorithm>
#include <cstdlib>

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

/// A job whose progress is known as a fraction (virtual_sdcard bytes).
PrintEtaInputs fine_job(double fraction, int print_duration_s, int slicer_estimate_s = 0) {
    PrintEtaInputs in =
        job(static_cast<int>(fraction * 100.0), print_duration_s, slicer_estimate_s);
    in.progress_fraction = fraction;
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
    REQUIRE(est.remaining_seconds(fine_job(0.5, 600)) == 600);

    // The next ten seconds ran slower than that pace: 0.505 at 610s
    // extrapolates to about 598s left, while counting down from 600 says 590.
    // Ten seconds of evidence
    // against a minute-long smoothing window moves the answer only part of the
    // way from the countdown toward the fresh extrapolation.
    const int next = est.remaining_seconds(fine_job(0.505, 610));
    CHECK(next > 590);
    CHECK(next < 597);
}

TEST_CASE("ETA: whole-percent progress never makes the estimate jump", "[print][eta]") {
    // A 3600s job reporting progress only in whole percents (M73), sampled four
    // times a second the way Moonraker delivers print_stats. Between percent
    // ticks nothing new is known, so the published number must fall by the
    // time that passed and nothing more; a tick must not throw it around.
    // Extrapolating from the whole percent alone swings it by T/p every tick:
    // 360s at 10%.
    PrintEtaEstimator est;
    int previous = est.remaining_seconds(job(1, 36, 3600));
    REQUIRE(previous != PrintEtaEstimator::kNoEstimate);
    int worst_jump = 0;
    for (int quarter = 37 * 4; quarter <= 3564 * 4; ++quarter) {
        const int elapsed = quarter / 4;
        const int now = est.remaining_seconds(job(elapsed / 36, elapsed, 3600));
        worst_jump = std::max(worst_jump, std::abs(previous - now));
        previous = now;
    }
    // A frame advances at most one second of print time.
    CHECK(worst_jump <= 2);
    // And the countdown still lands on the truth: 36s of a 3600s job remain.
    CHECK(std::abs(previous - 36) <= 5);
}

TEST_CASE("ETA: fine-grained progress tracks the real remaining time", "[print][eta]") {
    // Byte progress arrives as a fraction. On a print running exactly at its
    // pace, the estimate should sit on the true remaining time from the moment
    // the blend with the slicer stops, and never step by more than a frame.
    PrintEtaEstimator est;
    int previous = est.remaining_seconds(fine_job(0.01, 36, 3600));
    int worst_jump = 0;
    int worst_error = 0;
    for (int elapsed = 37; elapsed <= 3564; ++elapsed) {
        const int now = est.remaining_seconds(fine_job(elapsed / 3600.0, elapsed, 3600));
        worst_jump = std::max(worst_jump, std::abs(previous - now));
        if (elapsed >= 540) { // past the 15% blend window
            worst_error = std::max(worst_error, std::abs(now - (3600 - elapsed)));
        }
        previous = now;
    }
    CHECK(worst_jump <= 2);
    CHECK(worst_error <= 5);
}

TEST_CASE("ETA: a pause holds the estimate where it stood", "[print][eta]") {
    // print_duration stops while paused, and frames keep arriving.
    PrintEtaEstimator est;
    REQUIRE(est.remaining_seconds(fine_job(0.5, 1800)) == 1800);
    for (int frame = 0; frame < 100; ++frame) {
        CHECK(est.remaining_seconds(fine_job(0.5, 1800)) == 1800);
    }
}

TEST_CASE("ETA: a print that slows down is followed within a few minutes", "[print][eta]") {
    // Half of a 3600s job in 1800s, then the rest at three quarters speed: the
    // second half takes 2400s. Smoothing must not hold on to the old pace.
    PrintEtaEstimator est;
    for (int elapsed = 1700; elapsed <= 1800; ++elapsed) {
        (void)est.remaining_seconds(fine_job(elapsed / 3600.0, elapsed));
    }
    int now = 0;
    const int slow_for = 300;
    for (int t = 1; t <= slow_for; ++t) {
        const double fraction = 0.5 + 0.5 * t / 2400.0;
        now = est.remaining_seconds(fine_job(fraction, 1800 + t));
    }
    // The extrapolation itself has only partly caught up after 300s (it divides
    // all 2100s by all progress so far), so compare against it, not the truth.
    const double fraction = 0.5 + 0.5 * slow_for / 2400.0;
    const double extrapolated = (1800.0 + slow_for) * (1.0 - fraction) / fraction;
    CHECK(std::abs(now - extrapolated) <= 0.03 * extrapolated);
    // And it has moved well off the old pace, which would say 1500s.
    CHECK(now > 1560);
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
