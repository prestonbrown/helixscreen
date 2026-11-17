// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_eta_estimator.h"

#include <algorithm>

namespace helix {

void PrintEtaEstimator::reset() {
    smoothed_remaining_ = 0.0;
    has_smoothed_ = false;
    excluded_share_ = 0.0;
    prior_excluded_share_ = 0.0;
    anchor_progress_pct_ = 0;
    anchor_duration_s_ = 0;
}

void PrintEtaEstimator::note_excluded_share(const PrintEtaInputs& in) {
    const double share = std::clamp(in.excluded_share, 0.0, 1.0);
    if (share == excluded_share_) {
        return;
    }
    prior_excluded_share_ = excluded_share_;
    excluded_share_ = share;
    anchor_progress_pct_ = in.progress_pct;
    anchor_duration_s_ = in.print_duration_s;
    // The work left just changed size. Smoothing toward the new answer would
    // leave the old one on screen for minutes after the user excluded a part.
    has_smoothed_ = false;
}

double PrintEtaEstimator::rate_remaining(const PrintEtaInputs& in) const {
    const int progress = in.progress_pct;
    const double whole_rate_remaining =
        static_cast<double>(in.print_duration_s) * (100 - progress) / progress;

    // Nothing excluded, or excluded before any progress was made: every second
    // observed was spent on the work that is still left, so the rate stands.
    if (excluded_share_ <= 0.0 || anchor_progress_pct_ < 1) {
        return whole_rate_remaining;
    }

    // The rate before the exclusion was set by more work than is left now.
    // Excluding two of four equal parts halves the work per percent of file.
    const double prior_kept = 1.0 - prior_excluded_share_;
    const double kept_ratio = prior_kept > 0.0 ? (1.0 - excluded_share_) / prior_kept : 0.0;
    const double before = static_cast<double>(anchor_duration_s_) / anchor_progress_pct_ *
                          (100 - progress) * kept_ratio;

    const int since_pct = progress - anchor_progress_pct_;
    const int since_s = in.print_duration_s - anchor_duration_s_;
    if (since_pct <= 0 || since_s <= 0) {
        return before;
    }

    // Since the exclusion the printer has shown its real rate for the work that
    // is left. It takes over as that sample grows, which also corrects a share
    // that over- or under-states the excluded objects' real cost.
    const double after = static_cast<double>(since_s) / since_pct * (100 - progress);
    const double after_weight =
        std::min(1.0, static_cast<double>(since_pct) / kSettleAfterExclusionPct);
    return after_weight * after + (1.0 - after_weight) * before;
}

int PrintEtaEstimator::remaining_seconds(const PrintEtaInputs& in) {
    const int progress = in.progress_pct;
    note_excluded_share(in);
    const double kept = 1.0 - excluded_share_;
    const double slicer_job_s = in.slicer_estimate_s * kept;

    // Still preparing: what is left is the rest of the prep plus the whole job.
    if (progress == 0 && in.preprint_remaining_s > 0 && in.slicer_estimate_s > 0) {
        return in.preprint_remaining_s + static_cast<int>(slicer_job_s);
    }

    if (progress >= 100) {
        return 0;
    }

    if (progress < 1) {
        return kNoEstimate;
    }

    // Too early to extrapolate: a handful of seconds against a whole job turns
    // any rate estimate into noise, so the slicer's number stands alone.
    if (progress < 5 && in.slicer_estimate_s > 0) {
        return static_cast<int>(slicer_job_s * (100 - progress) / 100);
    }

    if (in.print_duration_s > 0) {
        double raw_remaining = rate_remaining(in);

        // Early on, damp the extrapolation toward the slicer's estimate. The
        // weight falls to zero at kBlendBelowPct, so the two agree at the
        // handover and the number does not jump as the blend switches off.
        if (progress < kBlendBelowPct && in.slicer_estimate_s > 0) {
            const double slicer_weight =
                static_cast<double>(kBlendBelowPct - progress) / kBlendBelowPct;
            const double slicer_remaining = slicer_job_s * (100.0 - progress) / 100.0;
            raw_remaining =
                slicer_weight * slicer_remaining + (1.0 - slicer_weight) * raw_remaining;
        }

        // Alpha rises with progress, so the estimate is very stable while the
        // sample is small and converges once the rate is worth believing:
        // 0.06 at 1%, 0.20 at 15%, capped at 0.30 from 25% on.
        const double alpha = std::min(0.3, 0.05 + progress * 0.01);
        if (!has_smoothed_) {
            smoothed_remaining_ = raw_remaining;
            has_smoothed_ = true;
        } else {
            smoothed_remaining_ = alpha * raw_remaining + (1.0 - alpha) * smoothed_remaining_;
        }
        return static_cast<int>(smoothed_remaining_);
    }

    // Printing has not started clocking yet, so there is no rate to observe.
    if (in.slicer_estimate_s > 0) {
        return static_cast<int>(slicer_job_s * (100 - progress) / 100);
    }

    return kNoEstimate;
}

} // namespace helix
