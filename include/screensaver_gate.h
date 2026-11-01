// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_cpu_clock.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

/**
 * @file screensaver_gate.h
 * @brief How much CPU a running screensaver may use, and what happens when it uses more
 *
 * Pure: the manager feeds it samples from the process CPU clock and acts on its answers.
 */

namespace helix::ui {

/// Share of one core a running saver may use. Idle: 4 or more cores 1.00 (the
/// saver is the foreground activity, and one core of a 4+ core board leaves the
/// machine responsive), or 0.50 when every core is an in-order efficiency design
/// (PlatformCapabilities::efficiency_cores_only), where a whole core of drawing still
/// leaves the UI behind; 3 cores 0.75, 2 cores 0.50, otherwise 0.10 (a core count
/// that failed to parse reads as one core). While printing: 0.25, 0.185, 0.125 and
/// 0.05, since klippy shares the SoC and a starved host faults the print with
/// "Timer too close".
double saver_budget_share(int cores, bool efficiency_cores_only, bool printing);

enum class GateDecision {
    KEEP,      ///< within budget: the level stays
    STEP_DOWN, ///< over budget: run one level lower
    TOO_HEAVY, ///< over budget at the lowest level: the board cannot afford this saver
    STEP_UP,   ///< comfortably under budget for long enough: run one level higher
};

/// What a window measuring `share` of one core against `budget` means for a saver running at
/// `level` of `level_count` levels.
GateDecision decide_saver_level(double share, double budget, size_t level, size_t level_count);

/// Consecutive windows a raise asks for, comfortably under budget, so a stored
/// low level climbs back only after a sustained quiet run.
inline constexpr int kStepUpWindows = 3;

/// A window counts toward a raise at or below this fraction of the budget:
/// marginal comfort must not buy a more expensive rung.
inline constexpr double kStepUpComfort = 0.6;

/// Consecutive-window comfort streak toward a step-up. A window above
/// kStepUpComfort of the budget resets it, even one still within budget.
class SaverComfortStreak {
  public:
    void add(double share, double budget) {
        streak_ = share <= budget * kStepUpComfort ? streak_ + 1 : 0;
    }
    void reset() {
        streak_ = 0;
    }
    int streak() const {
        return streak_;
    }

  private:
    int streak_ = 0;
};

/// A raise for a saver at `level` whose comfort streak reached kStepUpWindows.
/// It never raises above `ceiling`, the highest rung (lowest level number)
/// still open: 0 is the top rung, and a rung that stepped down this run is
/// closed, since rungs cost about twice each other and a raise into it would
/// only step down again.
GateDecision decide_saver_level_up(size_t level, int comfortable_windows, size_t ceiling);

/// The app's CPU rate while no saver runs, over the last idle stretch of up to kSpanNs.
class IdleBaseline {
  public:
    static constexpr uint64_t kSpanNs = 10000000000ULL;
    /// With fewer seconds of samples than this the rate reads as 0, which can only make the
    /// gate step down early, never late.
    static constexpr uint64_t kMinSpanNs = 3000000000ULL;

    void add(CpuSample sample);
    void reset();
    /// Cores of CPU per core of wall time between the oldest and newest sample held.
    double rate() const;
    size_t size() const {
        return samples_.size();
    }

  private:
    std::deque<CpuSample> samples_;
};

/// Measures one run of a saver: a warm-up, then consecutive windows for as long as it runs.
class SaverGateSession {
  public:
    static constexpr uint64_t kWarmupNs = 1000000000ULL;
    static constexpr uint64_t kWindowNs = 5000000000ULL;

    /// Starts a run at `start`, with `baseline_rate` cores of idle work to subtract.
    void begin(CpuSample start, double baseline_rate);
    /// Starts a new window at `now` without a warm-up, for a level that just changed.
    void restart_window(CpuSample now);
    /// Feeds a sample; returns `(cpu delta - baseline * wall) / wall` of the window that closed
    /// at it, as a share of one core, or nullopt while none has.
    std::optional<double> add(CpuSample sample);

  private:
    CpuSample start_{};
    CpuSample window_start_{};
    bool warming_up_ = true;
    double baseline_rate_ = 0.0;
};

/// HELIX_SCREENSAVER_BUDGET_PCT (a whole percent of one core, 1 to 400) and
/// HELIX_SCREENSAVER_LEVEL (a whole level, 0 to 99); a malformed value warns and reads as unset.
struct SaverEnvOverrides {
    std::optional<uint32_t> budget_pct;
    std::optional<uint32_t> level;
};

SaverEnvOverrides saver_env_overrides();

} // namespace helix::ui
