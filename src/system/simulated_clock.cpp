// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "simulated_clock.h"

#include "runtime_config.h"

#include <mutex>

namespace helix::sim {

namespace {

/// The process-wide simulated timeline, behind one lock.
///
/// `elapsed` is simulated time already banked. The real clock adds the segment
/// since `real_anchor` on top; a manual scope adds to `elapsed` directly. Both
/// paths therefore leave the timeline where the other picks it up, and a factor
/// change mid-run banks the segment it applied to rather than rescaling it.
struct ClockState {
    std::mutex mutex;
    bool manual = false;
    bool anchored = false;
    std::chrono::steady_clock::time_point real_anchor{};
    SimulatedClock::duration elapsed{};
    double factor = MIN_SPEED;
};

ClockState& clock_state() {
    static ClockState state;
    return state;
}

/// Caller must hold `state.mutex`.
SimulatedClock::duration locked_now(ClockState& state) {
    if (state.manual) {
        return state.elapsed;
    }

    const auto real_now = std::chrono::steady_clock::now();
    const double current = SimSpeed::global().factor();

    if (!state.anchored) {
        state.anchored = true;
        state.real_anchor = real_now;
        state.factor = current;
    } else if (current != state.factor) {
        state.elapsed +=
            SimSpeed::of(state.factor).accelerate_progress(real_now - state.real_anchor);
        state.real_anchor = real_now;
        state.factor = current;
    }

    return state.elapsed +
           SimSpeed::of(state.factor).accelerate_progress(real_now - state.real_anchor);
}

} // namespace

SimSpeed SimSpeed::global() {
    return SimSpeed::of(get_runtime_config()->sim_speedup);
}

SimulatedClock::time_point SimulatedClock::now() {
    ClockState& state = clock_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return time_point(locked_now(state));
}

SimulatedClock::ManualScope::ManualScope(SimSpeed speed) : speed_(speed) {
    ClockState& state = clock_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    previous_active_ = state.manual;
    // Take over from wherever the timeline stands so the handover does not
    // move it, then stop the real clock contributing to it.
    state.elapsed = locked_now(state);
    state.manual = true;
}

SimulatedClock::ManualScope::~ManualScope() {
    ClockState& state = clock_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.manual = previous_active_;
    // Real time kept running while the manual clock was in charge. Re-anchoring
    // resumes from what the scope advanced to instead of leaping by however
    // long the scope lived.
    state.anchored = false;
}

void SimulatedClock::ManualScope::advance(duration sim_delta) {
    if (sim_delta <= duration::zero()) {
        return;
    }
    ClockState& state = clock_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.elapsed += sim_delta;
}

void SimulatedClock::ManualScope::advance_real(std::chrono::steady_clock::duration real_delta) {
    advance(speed_.accelerate_progress(real_delta));
}

SimulatedClock::time_point SimulatedClock::ManualScope::now() const {
    ClockState& state = clock_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return time_point(state.elapsed);
}

} // namespace helix::sim
