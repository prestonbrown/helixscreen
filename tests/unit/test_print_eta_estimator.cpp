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

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "print_eta_estimator.h"
#include "printer_cache_registry.h"
#include "printer_state.h"

#include <algorithm>
#include <cstdlib>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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

    // The file's header already puts byte progress a sliver above zero.
    in.progress_fraction = 0.003;
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

TEST_CASE("ETA: a steady print counts down without jumping", "[print][eta]") {
    // A 3600s job at its slicer's pace, sampled four times a second as
    // Moonraker delivers print_stats. Between progress ticks the number falls
    // by the time that passed and nothing more; extrapolating from a whole
    // percent (M73) alone swings it by elapsed/percent every tick, 360s at 10%.
    // Byte progress keeps it on the truth once the slicer blend ends at 15%.
    for (const bool whole_percent : {true, false}) {
        CAPTURE(whole_percent);
        const auto frame = [whole_percent](int elapsed) {
            return whole_percent ? job(elapsed / 36, elapsed, 3600)
                                 : fine_job(elapsed / 3600.0, elapsed, 3600);
        };
        PrintEtaEstimator est;
        int previous = est.remaining_seconds(frame(36));
        REQUIRE(previous != PrintEtaEstimator::kNoEstimate);
        int worst_jump = 0;
        int worst_error = 0;
        for (int quarter = 37 * 4; quarter <= 3564 * 4; ++quarter) {
            const int elapsed = quarter / 4;
            const int now = est.remaining_seconds(frame(elapsed));
            worst_jump = std::max(worst_jump, std::abs(previous - now));
            if (!whole_percent && elapsed >= 540) {
                worst_error = std::max(worst_error, std::abs(now - (3600 - elapsed)));
            }
            previous = now;
        }
        CHECK(worst_jump <= 2);
        CHECK(worst_error <= 5);
        // 36s of the job remain.
        CHECK(std::abs(previous - 36) <= 5);
    }
}

TEST_CASE("ETA: the countdown follows the print clock", "[print][eta]") {
    PrintEtaEstimator est;
    SECTION("a pause holds the estimate, and a clock that runs backwards starts over") {
        // Half done in 3000s leaves 3000s; past the blend window the slicer's
        // opinion does not enter. print_duration stops while paused, and
        // frames keep arriving.
        REQUIRE(est.remaining_seconds(fine_job(0.5, 3000, 99999)) == 3000);
        for (int frame = 0; frame < 100; ++frame) {
            CHECK(est.remaining_seconds(fine_job(0.5, 3000, 99999)) == 3000);
        }
        // A new print the reset never reached restarts its clock below the last
        // sample. Counting down by a negative interval would add the old
        // print's time to the new one's estimate.
        CHECK(est.remaining_seconds(fine_job(0.25, 100)) == 300);
    }
    SECTION("a change of pace is smoothed, not snapped") {
        REQUIRE(est.remaining_seconds(fine_job(0.5, 600)) == 600);
        // 0.505 at 610s extrapolates to about 598s left; counting down says
        // 590. Ten seconds of evidence against a minute-long smoothing window
        // moves the answer only part of the way.
        const int next = est.remaining_seconds(fine_job(0.505, 610));
        CHECK(next > 590);
        CHECK(next < 597);
    }
    SECTION("a print that slows down is followed within a few minutes") {
        // Half of a 3600s job in 1800s, then the rest at three quarters speed.
        for (int elapsed = 1700; elapsed <= 1800; ++elapsed) {
            (void)est.remaining_seconds(fine_job(elapsed / 3600.0, elapsed));
        }
        int now = 0;
        for (int t = 1; t <= 300; ++t) {
            now = est.remaining_seconds(fine_job(0.5 + 0.5 * t / 2400.0, 1800 + t));
        }
        // The extrapolation itself has only partly caught up after 300s (it
        // divides all 2100s by all progress so far), so compare against it,
        // and against the old pace, which would say 1500s.
        const double fraction = 0.5 + 0.5 * 300 / 2400.0;
        const double extrapolated = 2100.0 * (1.0 - fraction) / fraction;
        CHECK(std::abs(now - extrapolated) <= 0.03 * extrapolated);
        CHECK(now > 1560);
    }
    SECTION("a stall near the end never reads as finished") {
        // 95% of a 3600s job, 180s left, then a 300s tool change: print time
        // runs, progress does not. Counting down alone reaches 0:00 two minutes
        // into it with the job unfinished.
        REQUIRE(est.remaining_seconds(fine_job(0.95, 3420)) == 180);
        int lowest = 180;
        for (int t = 1; t <= 300; ++t) {
            lowest = std::min(lowest, est.remaining_seconds(fine_job(0.95, 3420 + t)));
        }
        CHECK(lowest > 0);
    }
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
    SECTION("before the print clock starts") {
        // 500s of a 1000s job kept, 60% of it still to print.
        PrintEtaEstimator est;
        CHECK(est.remaining_seconds(excluded(job(40, 0, 1000), 0.5)) == 300);
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
    SECTION("with no clock yet") {
        // Half of a 1000s job excluded leaves 500s, half of which remains.
        PrintEtaEstimator est;
        CHECK(est.remaining_seconds(excluded(job(50, 0, 1000), 0.5)) == 250);
    }
    SECTION("inside the blend window") {
        // 10% in 100s extrapolates to 900s. The slicer's half job has 450s
        // left, and it weighs a third at 10%: 450/3 + 900*2/3 = 750, give or
        // take the rounding of the thirds.
        PrintEtaEstimator est;
        const int blended = est.remaining_seconds(excluded(job(10, 100, 1000), 0.5));
        CHECK(blended >= 749);
        CHECK(blended <= 750);
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
    // the 6s the share predicted, so 40% left is 360s. The countdown from the
    // exclusion says 300 - 90 = 210s, and 90s of evidence pulls it
    // 1 - exp(-90/60) of the way toward 360: 210 + 0.777 * 150 = 326.
    CHECK(est.remaining_seconds(excluded(job(60, 690, 0), 0.5)) == 326);
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
    // Still 12s per percent ten points on, so 40% left is 480s, which is also
    // where counting 120s down from 600s lands. Halving the earlier rate for
    // the exclusion would have said 240s.
    CHECK(est.remaining_seconds(excluded(job(60, 720, 0), 0.5)) == 480);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "ETA: a first status frame mid-print with parts already excluded leaves the rate "
                 "alone",
                 "[print][eta][exclude_object][printer_state]") {
    lv_init_safe();
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    state.update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});

    // The snapshot at an app start mid-print: half way through, one of two
    // parts excluded, all in one frame.
    nlohmann::json snapshot = {
        {"print_stats",
         {{"state", "printing"}, {"print_duration", 600.0}, {"total_duration", 620.0}}},
        {"virtual_sdcard", {{"progress", 0.5}}},
        {"exclude_object",
         {{"objects", {{{"name", "a"}}, {{"name", "b"}}}},
          {"excluded_objects", {"a"}},
          {"current_object", "b"}}}};
    state.update_from_status(snapshot);
    auto* time_left = state.print_state().get_print_time_left_subject();

    // The next print_stats tick is the first sample with progress. 12s per
    // percent over the 50% left is 600s; anchoring the exclusion at 50% would
    // halve the earlier rate and read 300s.
    state.update_from_status(
        nlohmann::json{{"print_stats", {{"print_duration", 600.0}, {"total_duration", 620.0}}}});
    CHECK(lv_subject_get_int(time_left) == 600);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "ETA: an exclusion in a status frame shrinks the time left until the printer "
                 "changes",
                 "[print][eta][exclude_object][printer_state]") {
    lv_init_safe();
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    state.update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
    state.update_from_status(nlohmann::json{
        {"print_stats",
         {{"state", "printing"}, {"print_duration", 600.0}, {"total_duration", 620.0}}},
        {"virtual_sdcard", {{"progress", 0.5}}},
        {"exclude_object",
         {{"objects", {{{"name", "a"}}, {{"name", "b"}}}},
          {"excluded_objects", nlohmann::json::array()},
          {"current_object", "b"}}}});
    auto* time_left = state.print_state().get_print_time_left_subject();
    const nlohmann::json tick = {
        {"print_stats", {{"print_duration", 600.0}, {"total_duration", 620.0}}}};

    state.update_from_status(tick);
    REQUIRE(lv_subject_get_int(time_left) == 600);

    // One of the two parts excluded halves the work per percent: 300s left.
    state.update_from_status(nlohmann::json{{"exclude_object", {{"excluded_objects", {"a"}}}}});
    state.update_from_status(tick);
    CHECK(lv_subject_get_int(time_left) == 300);

    // The next printer's first frame owes nothing to the last one's exclusions.
    helix::PrinterCacheRegistry::instance().invalidate_all();
    state.update_from_status(tick);
    CHECK(lv_subject_get_int(time_left) == 600);
}
