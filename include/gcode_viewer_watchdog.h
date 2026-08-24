// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::gcode_viewer {

/// One tick's observation of the 2D renderer's progressive-cache state, fed to
/// the stall watchdog. `prev_cached == -2` is the "never sampled" sentinel.
struct WatchdogObservation {
    int cached;       ///< renderer cached_up_to_layer_ this tick
    int target;       ///< renderer current/target layer this tick
    int prev_cached;  ///< cached observed last tick (-2 = never sampled)
    int prev_target;  ///< target observed last tick
    int stall_streak; ///< consecutive confirmed-stall ticks accumulated so far
};

/// Decision returned by the watchdog for this tick.
struct WatchdogDecision {
    bool kick;        ///< force lv_obj_invalidate() to retry the stalled render
    bool give_up;     ///< stop self-healing; caller should surface an error state
    int stall_streak; ///< streak value to persist for the next tick
};

/// Pure decision logic for the renderer-stall watchdog.
///
/// @param obs             this tick's observation
/// @param max_stall_kicks consecutive confirmed-stall ticks tolerated before
///                        giving up (stop kicking, signal error)
inline WatchdogDecision watchdog_evaluate(const WatchdogObservation& obs, int max_stall_kicks) {
    constexpr int NEVER_SAMPLED = -2;

    const bool first_sample = (obs.prev_cached == NEVER_SAMPLED);
    const bool behind_target = (obs.cached < obs.target);
    const bool same_state = (obs.cached == obs.prev_cached) && (obs.target == obs.prev_target);

    // Healthy or inconclusive this tick: caught up, first sample, or the
    // (cached,target) pair changed since last tick (progress or a retarget).
    // Reset the streak — only a stable, behind-target observation counts as a
    // confirmed stall.
    if (first_sample || !behind_target || !same_state) {
        return WatchdogDecision{false, false, 0};
    }

    // Confirmed stall this tick.
    const int streak = obs.stall_streak + 1;
    if (streak >= max_stall_kicks) {
        // Persistent external failure (e.g. disk full): the self-heal can't
        // recover by re-invalidating. Stop kicking and signal give-up so the
        // caller surfaces an error instead of thrashing.
        return WatchdogDecision{false, true, streak};
    }
    return WatchdogDecision{true, false, streak};
}

} // namespace helix::gcode_viewer
