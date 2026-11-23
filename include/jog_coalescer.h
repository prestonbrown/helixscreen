// SPDX-License-Identifier: GPL-3.0-or-later
// include/jog_coalescer.h
#pragma once

#include "axis_move.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>

namespace helix {

/**
 * Serializes jog moves: one RPC in flight, further taps accumulate
 * algebraically into pending deltas and flush as ONE combined move when the
 * in-flight move acks. An absolute target pending behind a move REPLACES
 * whatever was pending (latest target wins; a pending delta is discarded),
 * and a delta arriving while a target is pending discards that target.
 * Main-thread only; callers marshal acks/errors onto
 * the UI thread before touching this.
 */
class JogCoalescer {
  public:
    /** Either a relative delta or an absolute target. */
    using CoalescedMove = std::variant<AxisMove, AxisTarget>;

    /** Tap arrived. Returns the move to send NOW if idle; nullopt if it was
     *  accumulated behind the in-flight move. */
    std::optional<AxisMove> on_tap(const AxisMove& delta) {
        if (in_flight_) {
            if (auto* pending = std::get_if<AxisMove>(&pending_)) {
                pending->dx += delta.dx;
                pending->dy += delta.dy;
                pending->dz += delta.dz;
            } else {
                pending_ = delta; // a delta discards a pending target
            }
            return std::nullopt;
        }
        in_flight_ = true;
        inflight_ = delta;
        return delta;
    }

    /** Absolute target arrived. Returns the target to send NOW if idle;
     *  nullopt if it replaced something behind the in-flight move. A pending
     *  target is overwritten wholesale, never merged per-axis. */
    std::optional<AxisTarget> on_target(const AxisTarget& target) {
        if (in_flight_) {
            pending_ = target;
            return std::nullopt;
        }
        in_flight_ = true;
        inflight_ = target;
        return target;
    }

    /** In-flight move acked. Returns the pending flush to send (stays in
     *  flight) or nullopt (now idle). */
    std::optional<CoalescedMove> on_ack() {
        if (pending_empty()) {
            in_flight_ = false;
            inflight_ = AxisMove{};
            pending_ = AxisMove{};
            return std::nullopt;
        }
        inflight_ = pending_;
        pending_ = AxisMove{};
        return inflight_;
    }

    /** In-flight move failed: drop pending, go idle. */
    void on_error() {
        reset();
    }

    /** Drop all state (panel deactivate, print start, UI teardown). */
    void reset() {
        in_flight_ = false;
        inflight_ = AxisMove{};
        pending_ = AxisMove{};
    }

    bool in_flight() const {
        return in_flight_;
    }

    /** Predicted axis position once in-flight and pending travel land: a
     *  target with the axis set sets it, a delta adds, anything else passes
     *  `current` through. Used for envelope clamping. */
    double predicted_x(double current) const {
        return predict(current, &AxisTarget::x, &AxisMove::dx);
    }
    double predicted_y(double current) const {
        return predict(current, &AxisTarget::y, &AxisMove::dy);
    }
    double predicted_z(double current) const {
        return predict(current, &AxisTarget::z, &AxisMove::dz);
    }

    /** Z where a target enqueued now would start: after the in-flight move,
     *  ignoring pending travel, which on_target() discards. */
    double target_start_z(double current) const {
        return predict(current, &AxisTarget::z, &AxisMove::dz, false);
    }

  private:
    double predict(double current, std::optional<double> AxisTarget::*axis, double AxisMove::*delta,
                   bool include_pending = true) const {
        double v = current;
        const auto apply = [&v, axis, delta](const CoalescedMove& m) {
            if (const auto* t = std::get_if<AxisTarget>(&m)) {
                if (t->*axis) {
                    v = *(t->*axis);
                }
            } else {
                v += std::get<AxisMove>(m).*delta;
            }
        };
        apply(inflight_);
        if (include_pending) {
            apply(pending_);
        }
        return v;
    }

    bool pending_empty() const {
        if (const auto* pending = std::get_if<AxisMove>(&pending_)) {
            return !pending->any();
        }
        return !std::get<AxisTarget>(pending_).any();
    }

    bool in_flight_ = false;
    CoalescedMove inflight_{};
    CoalescedMove pending_{};
};

/// Limits for a move that starts at `position`: [min, max], stretched to reach
/// the position when the head sits outside it (parked in a service zone past
/// the print area). A move from there toward the envelope keeps its length
/// instead of being pulled to the edge, and a move further out stays refused.
struct AxisRange {
    double min;
    double max;
};
inline AxisRange range_reaching(double position, double min, double max) {
    return {std::min(min, position), std::max(max, position)};
}

/**
 * Clamp a jog delta so predicted-position + delta stays inside [min, max]
 * (range_reaching the predicted position). Returns 0 rather than a
 * direction-reversing correction when the predicted position is already
 * at/past the edge in the tap direction.
 */
inline double clamp_jog_delta(double current, double uncommitted, double delta, double min,
                              double max) {
    const double predicted = current + uncommitted;
    const AxisRange range = range_reaching(predicted, min, max);
    const double clamped = std::clamp(predicted + delta, range.min, range.max) - predicted;
    if (clamped * delta <= 0.0) {
        return 0.0;
    }
    return clamped;
}

/// Outcome of clamping one jog delta against an axis's limits.
struct JogClampResult {
    double allowed; ///< Permitted delta; 0.0 when the axis cannot move at all.
    bool warn;      ///< Raise the "blocked" message now.
    bool latch;     ///< The caller's new per-axis latch value; store it unconditionally.
};

/// Clamp a jog delta against an axis's limits and decide whether this attempt
/// is the first blocked one of an approach.
///
/// `already_warned` is the caller's latch for this axis. `latch` in the result
/// is its new value and is always meaningful, so a caller assigns it without
/// branching: a jog that moves clears the latch, so retreating from a limit and
/// returning to it warns again.
///
/// Partial travel is not a block. A request for 10mm that yields 2mm moves 2mm
/// and says nothing; only a request that yields nothing is worth a message.
///
/// "Yields nothing" is an epsilon test, not `== 0.0`: a predicted position a
/// hair inside the envelope leaves a sub-micron residual, which an exact
/// compare reads as travel and the warning never fires.
inline JogClampResult clamp_jog_with_warn(double current, double uncommitted, double delta,
                                          double min, double max, bool already_warned) {
    const double allowed = clamp_jog_delta(current, uncommitted, delta, min, max);
    const bool blocked = std::abs(allowed) <= AxisMove::kEpsilonMm;
    if (!blocked) {
        return {allowed, false, false};
    }
    return {0.0, !already_warned, true};
}

/// Whether a jog must be refused because it cannot be clamped: with no known
/// envelope or no known live position there is nothing to clamp against, and
/// sending the jog unclamped would trust exactly the values that are missing.
inline bool jog_refused_for_unknown_position(bool bounds_known, bool position_known) {
    return !bounds_known || !position_known;
}

/// The jog feedrate actually used, given what the user stored and what the
/// printer permits. Storage keeps the user's choice so a machine with a higher
/// ceiling gets it back; emission can never exceed the limit. Bounds are plain
/// doubles so this header stays dependency-free — pass a SafetyLimits' fields.
inline int effective_jog_speed_mm_min(int stored_mm_min, double min_mm_min, double max_mm_min) {
    // min-then-max rather than std::clamp: the bounds are caller-supplied, and
    // std::clamp is undefined when min > max while this form resolves any
    // ordering to the maximum.
    const double v = static_cast<double>(stored_mm_min);
    const double bounded = std::min(std::max(v, min_mm_min), max_mm_min);
    // Only the raised case rounds up: truncating a fractional min_mm_min lands
    // one below the floor is_safe_feedrate() enforces, while a value pulled
    // down to a fractional max_mm_min has to stay under it.
    return static_cast<int>(bounded > v ? std::ceil(bounded) : bounded);
}

} // namespace helix
