// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jog_coalescer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace helix {

/// Jog distances the chute calibration offers, indexed by the overlay's step
/// subject and by the step buttons' user_data.
inline constexpr double kChuteJogStepsMm[] = {0.1, 0.5, 1.0, 10.0};
inline constexpr int kChuteJogDefaultStepIndex = 1;

/// The distance for a step index; an unknown index gets the default step, so a
/// bad value can never select the largest one.
inline double chute_jog_step_mm(int index) {
    const int count = static_cast<int>(sizeof(kChuteJogStepsMm) / sizeof(kChuteJogStepsMm[0]));
    return kChuteJogStepsMm[index >= 0 && index < count ? index : kChuteJogDefaultStepIndex];
}

/// Decides how far each chute calibration jog may move.
///
/// The park position, where the calibration's prepare step leaves the toolhead,
/// is the reference for two limits. The rear zone past park Y holds the chute and
/// the cutter, and the only recorded X there is the stored chute X, so X is
/// refused while Y is past park. Whatever X is saved is where the firmware sends
/// the head for every later purge, so X stays within kXReachMm of park.
///
/// Limits apply to the commanded position as well as the live one. A jog is sent
/// without waiting for the previous one and the live position lags it, so a live
/// reading alone lets fast taps overshoot; nothing else moves the toolhead during
/// the jog step, which keeps the commanded position accurate.
class ChuteJogGuard {
  public:
    static constexpr double kXReachMm = 15.0;
    /// Time for the position feed to report the park after prepare acks.
    static constexpr uint32_t kSettleMs = 1000;
    /// Stored positions truncate to 0.01mm, so a head at park can read a hair off.
    static constexpr double kParkToleranceMm = 0.05;

    enum class Outcome {
        Move,            ///< Send `delta`.
        PositionUnknown, ///< Not armed, or the park position has not settled.
        AxisLimit,       ///< At a limit; `warn` is set on the first blocked attempt.
        PastPark,        ///< X refused because Y is past park.
    };

    struct Decision {
        double delta;
        Outcome outcome;
        bool warn;
    };

    /// Start a jog session once prepare has acked. Call once per session: a
    /// later call while the head is past park would take that Y as the park.
    void arm(uint32_t now_ms) {
        *this = ChuteJogGuard{};
        armed_ = true;
        armed_at_ms_ = now_ms;
    }

    void disarm() {
        *this = ChuteJogGuard{};
    }

    Decision jog_x(double delta, uint32_t now_ms, double live_x, double live_y, double axis_min,
                   double axis_max) {
        if (!latch_park(now_ms, live_x, live_y)) {
            return {0.0, Outcome::PositionUnknown, false};
        }
        if (std::max(commanded_y_, live_y) > park_y_ + kParkToleranceMm) {
            return {0.0, Outcome::PastPark, true};
        }
        return step(delta, live_x, commanded_x_, std::max(axis_min, park_x_ - kXReachMm),
                    std::min(axis_max, park_x_ + kXReachMm), warned_x_);
    }

    Decision jog_y(double delta, uint32_t now_ms, double live_x, double live_y, double axis_min,
                   double axis_max) {
        if (!latch_park(now_ms, live_x, live_y)) {
            return {0.0, Outcome::PositionUnknown, false};
        }
        return step(delta, live_y, commanded_y_, axis_min, axis_max, warned_y_);
    }

    /// Park Y of this session; meaningful once a jog has returned PastPark.
    [[nodiscard]] double park_y() const {
        return park_y_;
    }

  private:
    bool latch_park(uint32_t now_ms, double live_x, double live_y) {
        if (!armed_) {
            return false;
        }
        if (!latched_) {
            if (now_ms - armed_at_ms_ < kSettleMs) {
                return false;
            }
            park_x_ = commanded_x_ = live_x;
            park_y_ = commanded_y_ = live_y;
            latched_ = true;
        }
        return true;
    }

    static Decision step(double delta, double live, double& commanded, double min, double max,
                         bool& warned) {
        // The reading farther along the direction of travel: the commanded one
        // when live lags, the live one if the firmware refused an earlier jog.
        const double basis = delta > 0.0 ? std::max(commanded, live) : std::min(commanded, live);
        const JogClampResult result = clamp_jog_with_warn(basis, 0.0, delta, min, max, warned);
        warned = result.latch;
        if (std::abs(result.allowed) <= AxisMove::kEpsilonMm) {
            return {0.0, Outcome::AxisLimit, result.warn};
        }
        // A head outside the envelope retreats by the step asked for, not all
        // the way to the envelope's edge at once.
        const double allowed = std::abs(result.allowed) > std::abs(delta) ? delta : result.allowed;
        commanded += allowed;
        return {allowed, Outcome::Move, false};
    }

    bool armed_ = false;
    bool latched_ = false;
    uint32_t armed_at_ms_ = 0;
    double park_x_ = 0.0;
    double park_y_ = 0.0;
    double commanded_x_ = 0.0;
    double commanded_y_ = 0.0;
    bool warned_x_ = false;
    bool warned_y_ = false;
};

} // namespace helix
