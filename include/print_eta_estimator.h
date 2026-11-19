// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file print_eta_estimator.h
 * @brief How long the running print has left.
 *
 * Pure logic: no LVGL, no Config, no threads, same shape as PreprintPredictor.
 * The rule is worth having on its own because it is the only place that blends
 * the slicer's estimate with the rate actually observed, and because a number
 * the user watches for hours deserves to be assertable without standing up a
 * panel to read it off.
 */

namespace helix {

/// One status payload's worth of input to the ladder.
struct PrintEtaInputs {
    /// 0-100. Below 1 the extrapolation has no sample to work from.
    int progress_pct = 0;
    /// The same progress at full resolution, 0.0-1.0, or negative when only the
    /// whole percent is known. Moonraker's fractions move every frame; a whole
    /// percent stands still for minutes on a long print.
    double progress_fraction = -1.0;
    /// Seconds actually spent printing. Deliberately NOT total_duration, which
    /// counts preheat and pauses and so inflates every extrapolation from it.
    int print_duration_s = 0;
    /// The slicer's estimate for the whole job, from metadata or the header.
    int slicer_estimate_s = 0;
    /// Predicted pre-print time still to run, while the job is still preparing.
    int preprint_remaining_s = 0;
    /// Share of the job's work that excluded objects account for, 0 to 1.
    /// The slicer's estimate and the file still include that work, but the
    /// printer skips it.
    double excluded_share = 0.0;
};

/**
 * @brief Remaining print time from progress, elapsed time and the slicer estimate.
 *
 * A class rather than a free function because the answer is smoothed, and
 * smoothing has to remember the previous answer. The published number counts
 * down with print time and is corrected toward the extrapolation only when
 * progress moves, weighted by the print time that correction covers. Coarse
 * progress (whole-percent M73) would otherwise throw the extrapolation up and
 * down by elapsed/percent at every tick.
 */
class PrintEtaEstimator {
  public:
    /// These inputs support no answer. The caller leaves its subject alone
    /// rather than publishing a zero, which would read as "finished".
    static constexpr int kNoEstimate = -1;

    /// Seconds remaining, or kNoEstimate. Advances the smoothing state.
    [[nodiscard]] int remaining_seconds(const PrintEtaInputs& in);

    /// Forget the smoothed value. A new print's first sample must not be
    /// averaged against the previous print's last one.
    void reset();

  private:
    /// Below this progress the observed rate is blended with the slicer's
    /// estimate, weighted toward the slicer as progress approaches zero.
    static constexpr double kBlendBelowPct = 15.0;
    /// Print time over which a disagreement between the published number and
    /// the fresh extrapolation is mostly resolved (1 - 1/e of it).
    static constexpr double kSmoothingSeconds = 60.0;

    /// Progress points after an exclusion over which the rate observed since
    /// then takes over from the earlier rate scaled down by the excluded share.
    static constexpr double kSettleAfterExclusionPct = 10.0;

    /// Remaining seconds extrapolated from the observed print rate, with the
    /// work of excluded objects taken out.
    [[nodiscard]] double rate_remaining(const PrintEtaInputs& in, double pct) const;

    /// Records where the excluded share last changed, so the rate before the
    /// change and the rate after it can each be judged against the right work.
    void note_excluded_share(const PrintEtaInputs& in, double pct);

    /// Counts the published number down by the print time since the last
    /// sample, then corrects it toward raw_remaining when progress has moved.
    double smooth(double raw_remaining, int duration_s, double fraction);
    static int whole_seconds(double seconds);

    double smoothed_remaining_ = 0.0;
    bool has_smoothed_ = false;
    int last_duration_s_ = 0; ///< print_duration of the previous sample
    /// print_duration and progress when the countdown last corrected toward
    /// the extrapolation.
    int countdown_anchor_duration_s_ = 0;
    double countdown_anchor_fraction_ = 0.0;

    double excluded_share_ = 0.0;
    /// The share in force before the most recent change.
    double prior_excluded_share_ = 0.0;
    /// Progress (percent) and print_duration when the excluded share changed.
    double anchor_progress_pct_ = 0.0;
    int anchor_duration_s_ = 0;
    /// A sample with progress has been seen, so a later change in the
    /// excluded share happened while this estimator was watching.
    bool seen_progress_ = false;
};

} // namespace helix
