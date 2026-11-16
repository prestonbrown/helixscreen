// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jog_coalescer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace helix {

/// A nozzle wiper behind the chute park, as the printer's own config places it:
/// a strip from X x_min to x_max whose edge nearest the bed lies at Y y.
struct ChuteWiperLine {
    double x_min;
    double x_max;
    double y;
};

/// Decides how far each chute calibration jog may move.
///
/// The park position, where the calibration's prepare step leaves the toolhead,
/// is the reference for every limit. At or before park Y, X stays within kXReachMm
/// of park: whatever X is saved is where the firmware sends the head for every
/// later purge. That reach is per session because no factory chute X is recorded
/// to bound it absolutely: each save moves the next session's park. The rear zone
/// past park Y holds the chute and the cutter, so Y enters it only with X within
/// kFineReachMm of park, and X moves there only by single steps of kFineStepMm
/// inside that same reach.
///
/// Limits apply to the commanded position as well as the live one. A jog is sent
/// without waiting for the previous one and the live position lags it, so a live
/// reading alone lets fast taps overshoot; nothing else moves the toolhead during
/// the jog step, which keeps the commanded position accurate.
///
/// A known wiper (ChuteWiperLine) behind park adds one more limit: with X at or
/// past the wiper's near end, on the side of park the wiper lies, Y stops
/// kWiperMarginMm short of the wiper line, and with Y beyond that line X does
/// not move onto the wiper. With no wiper known, only the limits above apply.
class ChuteJogGuard {
  public:
    static constexpr double kXReachMm = 15.0;
    /// How far X may be from park with the head past park Y, in single steps of
    /// at most kFineStepMm. Both are cautious guesses: the clearances around the
    /// chute and cutter are unmeasured on hardware.
    static constexpr double kFineReachMm = 2.0;
    static constexpr double kFineStepMm = 0.1;
    /// Time for the position feed to report the park after prepare acks.
    static constexpr uint32_t kSettleMs = 1000;
    /// Stored positions truncate to 0.01mm, so a head at park can read a hair off.
    static constexpr double kParkToleranceMm = 0.05;
    /// The pad's distances arrive as float, so a 0.1 step can read a hair over.
    static constexpr double kStepToleranceMm = 0.001;
    /// How far short of a known wiper line Y stops.
    static constexpr double kWiperMarginMm = 0.3;

    enum class Outcome {
        Move,            ///< Send `delta`.
        PositionUnknown, ///< Not armed, or the park position has not settled.
        AxisLimit,       ///< At a limit; `warn` is set on the first blocked attempt.
        CoarsePastPark,  ///< X step larger than kFineStepMm refused past park Y.
        RepeatPastPark,  ///< X hold repeat refused past park Y; a tap needs no message.
        XDisplaced,      ///< Y refused past park because X is over kFineReachMm off park.
    };

    struct Decision {
        double delta;
        Outcome outcome;
        bool warn;
    };

    /// Start a jog session once prepare has acked. Call once per session: a
    /// later call while the head is past park would take that Y as the park.
    /// @p wiper is the printer's wiper, when its config names one.
    void arm(uint32_t now_ms, std::optional<ChuteWiperLine> wiper = std::nullopt) {
        *this = ChuteJogGuard{};
        armed_ = true;
        armed_at_ms_ = now_ms;
        wiper_ = wiper;
    }

    void disarm() {
        *this = ChuteJogGuard{};
    }

    Decision jog_x(double delta, bool is_repeat, uint32_t now_ms, double live_x, double live_y,
                   double axis_min, double axis_max) {
        if (!latch_park(now_ms, live_x, live_y)) {
            return {0.0, Outcome::PositionUnknown, false};
        }
        // Past park, a step and a hold can only run on into what cannot be seen,
        // so X takes single fine steps there.
        double reach = kXReachMm;
        const double far_y = std::max(commanded_y_, live_y);
        if (far_y > park_y_ + kParkToleranceMm) {
            if (is_repeat) {
                return {0.0, Outcome::RepeatPastPark, false};
            }
            if (std::abs(delta) > kFineStepMm + kStepToleranceMm) {
                return {0.0, Outcome::CoarsePastPark, true};
            }
            reach = kFineReachMm;
        }
        double min = std::max(axis_min, park_x_ - reach);
        double max = std::min(axis_max, park_x_ + reach);
        if (const auto cap = wiper_cap_y(); cap && far_y > *cap + kParkToleranceMm) {
            if (wiper_->x_min > park_x_) {
                max = std::min(max, wiper_->x_min - kParkToleranceMm);
            } else if (wiper_->x_max < park_x_) {
                min = std::max(min, wiper_->x_max + kParkToleranceMm);
            }
        }
        return step(delta, live_x, commanded_x_, min, max, warned_x_);
    }

    Decision jog_y(double delta, uint32_t now_ms, double live_x, double live_y, double axis_min,
                   double axis_max) {
        if (!latch_park(now_ms, live_x, live_y)) {
            return {0.0, Outcome::PositionUnknown, false};
        }
        const double x_off_park =
            std::max(std::abs(commanded_x_ - park_x_), std::abs(live_x - park_x_));
        if (x_off_park > kFineReachMm + kParkToleranceMm) {
            if (delta > 0.0 && std::max(commanded_y_, live_y) >= park_y_ - kParkToleranceMm) {
                return {0.0, Outcome::XDisplaced, true};
            }
            axis_max = std::min(axis_max, park_y_);
        }
        if (const auto cap = wiper_cap_y(); cap && on_wiper_side(commanded_x_, live_x)) {
            axis_max = std::min(axis_max, *cap);
        }
        return step(delta, live_y, commanded_y_, axis_min, axis_max, warned_y_);
    }

    /// Park Y of this session; meaningful once a jog has returned CoarsePastPark.
    [[nodiscard]] double park_y() const {
        return park_y_;
    }

    /// Park X of this session; meaningful once a jog has returned XDisplaced.
    [[nodiscard]] double park_x() const {
        return park_x_;
    }

  private:
    /// The Y a known wiper stops the head at; none without a wiper, or when its
    /// line does not lie past park.
    [[nodiscard]] std::optional<double> wiper_cap_y() const {
        if (!wiper_) {
            return std::nullopt;
        }
        const double cap = wiper_->y - kWiperMarginMm;
        if (cap <= park_y_ + kParkToleranceMm) {
            return std::nullopt;
        }
        return cap;
    }

    /// Whether either X reading is at or past the wiper's near end, counted
    /// from park toward the wiper.
    [[nodiscard]] bool on_wiper_side(double commanded_x, double live_x) const {
        if (wiper_->x_min > park_x_) {
            return std::max(commanded_x, live_x) > wiper_->x_min - kParkToleranceMm;
        }
        if (wiper_->x_max < park_x_) {
            return std::min(commanded_x, live_x) < wiper_->x_max + kParkToleranceMm;
        }
        return true;
    }

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
    std::optional<ChuteWiperLine> wiper_;
};

} // namespace helix
