// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_eta_estimator.h"

#include <algorithm>
#include <cmath>

namespace helix {

void PrintEtaEstimator::reset() {
    smoothed_remaining_ = 0.0;
    has_smoothed_ = false;
    last_duration_s_ = 0;
    anchor_duration_s_ = 0;
    anchor_fraction_ = 0.0;
}

int PrintEtaEstimator::remaining_seconds(const PrintEtaInputs& in) {
    const double fraction =
        in.progress_fraction >= 0.0 ? std::min(in.progress_fraction, 1.0) : in.progress_pct / 100.0;
    const double pct = fraction * 100.0;

    // Still preparing: what is left is the rest of the prep plus the whole job.
    if (pct <= 0.0 && in.preprint_remaining_s > 0 && in.slicer_estimate_s > 0) {
        return in.preprint_remaining_s + in.slicer_estimate_s;
    }

    if (pct >= 100.0) {
        return 0;
    }

    if (pct < 1.0) {
        return kNoEstimate;
    }

    double raw_remaining = 0.0;
    if (pct < 5.0 && in.slicer_estimate_s > 0) {
        // Too early to extrapolate: a handful of seconds against a whole job
        // turns any rate estimate into noise, so the slicer's number stands alone.
        raw_remaining = in.slicer_estimate_s * (1.0 - fraction);
    } else if (in.print_duration_s > 0) {
        raw_remaining = in.print_duration_s * (1.0 - fraction) / fraction;

        // Early on, damp the extrapolation toward the slicer's estimate. The
        // weight falls to zero at kBlendBelowPct, so the two agree at the
        // handover and the number does not jump as the blend switches off.
        if (pct < kBlendBelowPct && in.slicer_estimate_s > 0) {
            const double slicer_weight = (kBlendBelowPct - pct) / kBlendBelowPct;
            const double slicer_remaining = in.slicer_estimate_s * (1.0 - fraction);
            raw_remaining =
                slicer_weight * slicer_remaining + (1.0 - slicer_weight) * raw_remaining;
        }
    } else if (in.slicer_estimate_s > 0) {
        // Printing has not started clocking yet, so there is no rate to observe.
        return whole_seconds(in.slicer_estimate_s * (1.0 - fraction));
    } else {
        return kNoEstimate;
    }

    if (in.print_duration_s <= 0) {
        return whole_seconds(raw_remaining);
    }
    return whole_seconds(smooth(raw_remaining, in.print_duration_s, fraction));
}

int PrintEtaEstimator::whole_seconds(double seconds) {
    // Whole seconds, rounded down. The tolerance keeps an exact quotient that
    // binary fractions miss by a hair (600 * 0.2 / 0.8) from losing a second.
    return static_cast<int>(seconds + 1e-6);
}

double PrintEtaEstimator::smooth(double raw_remaining, int duration_s, double fraction) {
    if (!has_smoothed_ || duration_s < last_duration_s_) {
        smoothed_remaining_ = raw_remaining;
        has_smoothed_ = true;
        last_duration_s_ = duration_s;
        anchor_duration_s_ = duration_s;
        anchor_fraction_ = fraction;
        return smoothed_remaining_;
    }

    // Between progress reports nothing new is known about the rate, so the
    // estimate counts down with the print clock. print_duration stands still
    // while paused, so a pause holds the number without a special case.
    smoothed_remaining_ -= duration_s - last_duration_s_;
    last_duration_s_ = duration_s;

    // Progress moved: the extrapolation is fresh evidence. Pull toward it by
    // how much print time it covers, so the answer follows a real change of
    // pace within a few kSmoothingSeconds whether progress arrives four times a
    // second or once a minute.
    if (fraction != anchor_fraction_ && duration_s > anchor_duration_s_) {
        const double weight =
            1.0 - std::exp(-(duration_s - anchor_duration_s_) / kSmoothingSeconds);
        smoothed_remaining_ += weight * (raw_remaining - smoothed_remaining_);
        anchor_duration_s_ = duration_s;
        anchor_fraction_ = fraction;
    }

    // A stall that outlasts the estimate (a long tool change or reheat near
    // the end) keeps the print clock running while progress stands still, and
    // the countdown alone would read 0:00 with work left. The extrapolation
    // grows during a stall, so half of it keeps a number on the screen until
    // progress moves and the correction above takes over. In steady printing
    // this floor sits far below the countdown and never binds.
    smoothed_remaining_ = std::max(smoothed_remaining_, 0.5 * raw_remaining);
    return smoothed_remaining_;
}

} // namespace helix
