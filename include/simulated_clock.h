// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file simulated_clock.h
 * @brief The one simulated-time source: `--sim-speed` as a clock and as a scale factor.
 *
 * Mock subsystems fast-forward by applying `RuntimeConfig::sim_speedup` in one
 * of two opposite directions, and code that MEASURES a mock printer's progress
 * has to read the same fast-forwarded timeline or it will sit through the real
 * seconds the mock just skipped.
 *
 * Three operations cover every use, and they are named so a caller states the
 * effect it wants rather than the arithmetic:
 *
 * - `SimSpeed::shorten_wait_ms()` / `shorten_wait_seconds()` — DIVIDES. Ask it
 *   at a sleep or delay site: "I want `n` simulated units of waiting; how long
 *   do I really sleep?" Faster speed means a shorter answer.
 * - `SimSpeed::accelerate_progress()` — MULTIPLIES. Ask it wherever a rate or an
 *   elapsed span is converted into simulated progress: "one real tick just
 *   passed; how much simulated time is that?" Faster speed means a larger
 *   answer.
 * - `SimulatedClock::now()` — the timeline itself. Subtract two of its
 *   time_points to measure a duration in simulated time.
 *
 * `SimulatedClock::time_point` is a distinct type from
 * `std::chrono::steady_clock::time_point`, so mixing the two timelines in one
 * subtraction does not compile.
 *
 * @threading `SimulatedClock::now()` is safe from any thread. A `SimSpeed` is
 *            an immutable value.
 */

#pragma once

#include <algorithm>
#include <chrono>

namespace helix::sim {

/// Slowest factor any simulated-time consumer accepts; matches `--sim-speed`.
inline constexpr double MIN_SPEED = 1.0;
/// Fastest factor any simulated-time consumer accepts; matches `--sim-speed`.
inline constexpr double MAX_SPEED = 1000.0;

/// Bring any factor — a flag value, an env override, a composed product — into
/// the one range every simulated-time consumer shares.
[[nodiscard]] constexpr double clamp_speed(double factor) {
    // NaN compares false against both bounds, so test for the valid interval
    // rather than clamping, and fall back to real time when it fails.
    if (!(factor >= MIN_SPEED)) {
        return MIN_SPEED;
    }
    return std::min(factor, MAX_SPEED);
}

/**
 * @brief How much faster than real time a simulation runs.
 *
 * Always in [MIN_SPEED, MAX_SPEED]. A subsystem that has its own multiplier
 * composes it over the global flag with `composed_with()`, so one `--sim-speed`
 * carries every subsystem's own pace with it.
 */
class SimSpeed {
  public:
    /// Real time: no fast-forward at all.
    constexpr SimSpeed() = default;

    /// @param factor Speedup factor, clamped into [MIN_SPEED, MAX_SPEED].
    [[nodiscard]] static constexpr SimSpeed of(double factor) {
        return SimSpeed(clamp_speed(factor));
    }

    /// The process-wide `--sim-speed` factor (`RuntimeConfig::sim_speedup`).
    [[nodiscard]] static SimSpeed global();

    /**
     * @brief This speed with a subsystem's own multiplier folded in.
     *
     * The mock dryer runs at 60x with no flag so a 4-hour cycle is watchable;
     * `SimSpeed::global().composed_with(60)` makes `--sim-speed 10` mean 600x
     * for the dryer while the rest of the app stays at 10x.
     *
     * @param subsystem_factor The subsystem's own multiplier at 1x global
     * @return The product, clamped back into [MIN_SPEED, MAX_SPEED]
     */
    [[nodiscard]] constexpr SimSpeed composed_with(double subsystem_factor) const {
        return SimSpeed(clamp_speed(factor_ * clamp_speed(subsystem_factor)));
    }

    /// The raw multiplier. Prefer the named operations below.
    [[nodiscard]] constexpr double factor() const {
        return factor_;
    }

    /// True when nothing is being fast-forwarded, so simulated time is real time.
    [[nodiscard]] constexpr bool is_real_time() const {
        return factor_ == MIN_SPEED;
    }

    // -- DIVIDES: less real waiting -----------------------------------------

    /**
     * @brief Real milliseconds to sleep so `sim_ms` of simulated time passes.
     *
     * At 10x, a 2000ms simulated delay is 200ms of real sleeping. Never returns
     * less than 1ms for a positive request, so a sleep site cannot busy-spin.
     */
    [[nodiscard]] int shorten_wait_ms(int sim_ms) const {
        if (sim_ms <= 0) {
            return 0;
        }
        return std::max(1, static_cast<int>(static_cast<double>(sim_ms) / factor_));
    }

    /// Real seconds to wait so `sim_seconds` of simulated time passes.
    [[nodiscard]] double shorten_wait_seconds(double sim_seconds) const {
        return sim_seconds / factor_;
    }

    // -- MULTIPLIES: more simulated progress --------------------------------

    /**
     * @brief Simulated progress made per unit of real progress.
     *
     * At 10x, a simulator stepping 0.1 real seconds per tick advances its
     * simulated world by 1.0 second, and a dryer at 60 simulated seconds per
     * real second runs at 600.
     */
    [[nodiscard]] constexpr double accelerate_progress(double per_real_unit) const {
        return per_real_unit * factor_;
    }

    /// Simulated time that elapsed while `real_elapsed` of real time passed.
    [[nodiscard]] std::chrono::nanoseconds
    accelerate_progress(std::chrono::steady_clock::duration real_elapsed) const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double, std::nano>(static_cast<double>(real_elapsed.count()) *
                                                     factor_));
    }

  private:
    constexpr explicit SimSpeed(double clamped_factor) : factor_(clamped_factor) {}

    double factor_ = MIN_SPEED;
};

/**
 * @brief The fast-forwarded timeline mock subsystems make progress on.
 *
 * A steady clock advancing `SimSpeed::global()` simulated seconds per real
 * second. Code that measures how long a mock printer took — pre-print phase
 * durations, probe gaps, heating rates — reads this instead of
 * `std::chrono::steady_clock` so its numbers stay the same at every
 * `--sim-speed`, and only the real seconds spent waiting shrink.
 *
 * Without `--sim-speed` (every non-`--test` run) the factor is 1.0 and this is
 * `std::chrono::steady_clock` with a different origin.
 */
class SimulatedClock {
  public:
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<SimulatedClock, duration>;
    static constexpr bool is_steady = true;

    /// Current point on the simulated timeline. Monotonic across speed changes.
    [[nodiscard]] static time_point now();

    /**
     * @brief Test seam: replaces the timeline with one the test advances by hand.
     *
     * While a scope is alive, `SimulatedClock::now()` returns only what the test
     * has advanced to, so a test can cover an hour of simulated pre-print
     * without sleeping.
     *
     * Nested scopes share one timeline, and on destruction the real clock
     * resumes from where the scope left it, so `SimulatedClock::now()` never
     * goes backwards across a scope boundary.
     */
    class ManualScope {
      public:
        /// @param speed Factor `advance_real()` applies; defaults to the global flag.
        explicit ManualScope(SimSpeed speed = SimSpeed::global());
        ~ManualScope();

        ManualScope(const ManualScope&) = delete;
        ManualScope& operator=(const ManualScope&) = delete;

        /// Move the simulated clock forward by a span of SIMULATED time.
        void advance(duration sim_delta);

        /// Move it forward by what `real_delta` of real time would produce at `speed`.
        void advance_real(std::chrono::steady_clock::duration real_delta);

        /// Where the manual clock currently sits.
        [[nodiscard]] time_point now() const;

      private:
        SimSpeed speed_;
        bool previous_active_ = false;
    };
};

} // namespace helix::sim
