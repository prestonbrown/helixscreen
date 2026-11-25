// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file print_start_purge_shape.h
 * @brief Recognises a stationary purge (blob/poop or waste chute) from motion
 *
 * A stationary purge extrudes continuously with the head held at one XY: Z
 * climbs for a blob purge and stays put over a chute. The shapes around it
 * differ: a purge line moves XY, probing moves Z without extruding, and a
 * filament load or tool change pulses and retracts the extruder. Every test
 * here is relative to the motion's own earlier samples, never to a location,
 * height or speed, because purge spots and travel heights differ per printer.
 *
 * Pure: no LVGL, no threads.
 */

#include "ams_types.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace helix {

/// How long forward flow must run unbroken before it reads as a purge. Longer
/// rejects more load and tool-change pushes; shorter shows the purge sooner.
/// Three seconds outlasts the stop-start pushes of a load while a blob or chute
/// purge, which runs for many seconds, still shows near its start.
inline constexpr uint64_t kPurgeFlowSustainMs = 3000;

/// XY drift, from the flow's first sample, that still counts as the head
/// holding still. Absorbs position rounding only; a purge line leaves it at once.
inline constexpr double kPurgeXyStillMm = 1.0;

/**
 * @brief Tracks one stretch of forward extrusion with the head held still
 *
 * Feed main-thread samples in time order. A sample that is not extruding
 * forward (stopped or retracting), moves XY off the stretch's first sample, or
 * lowers Z from the previous sample ends the stretch.
 */
class PurgeFlowTracker {
  public:
    /// @return true while the current stretch has lasted kPurgeFlowSustainMs
    bool note_sample(uint64_t ms, bool extruding_forward, double x, double y, double z) {
        const bool continues = start_ms_ && extruding_forward &&
                               std::fabs(x - x0_) <= kPurgeXyStillMm &&
                               std::fabs(y - y0_) <= kPurgeXyStillMm && z >= last_z_;
        if (!continues) {
            start_ms_.reset();
            if (extruding_forward) {
                start_ms_ = ms;
                x0_ = x;
                y0_ = y;
            }
        }
        last_z_ = z;
        return start_ms_ && ms - *start_ms_ >= kPurgeFlowSustainMs;
    }

    void reset() {
        start_ms_.reset();
    }

  private:
    std::optional<uint64_t> start_ms_;
    double x0_ = 0.0;
    double y0_ = 0.0;
    double last_z_ = 0.0;
};

/// What decides whether the motion seen is a purge.
struct PurgeEvidence {
    bool ams_present = false;           ///< a filament system backend is active
    AmsAction action = AmsAction::IDLE; ///< its current action (ignored without one)
    bool filament_loaded = false;       ///< its filament-loaded state
    bool heaters_at_target = false;     ///< nozzle (with a target set) and bed at target
    bool stationary_flow = false;       ///< PurgeFlowTracker reports a sustained stretch
};

/**
 * @brief Is the printer purging?
 *
 * A filament system that says it is purging is believed outright. Otherwise a
 * sustained stationary flow is a purge once the heaters are at target, unless
 * the filament system is mid unload/select/cut/tip (its own pushes), or reports
 * no filament loaded. A load counts once its filament has reached the toolhead:
 * AFC runs its poop inside the load, so its action stays LOADING through the
 * purge, while the push to the nozzle before it happens with nothing loaded.
 * Without a filament system the motion stands alone.
 */
inline bool is_purge(const PurgeEvidence& e) {
    if (e.ams_present && e.action == AmsAction::PURGING) {
        return true;
    }
    if (!e.stationary_flow || !e.heaters_at_target) {
        return false;
    }
    if (!e.ams_present) {
        return true;
    }
    switch (e.action) {
    case AmsAction::UNLOADING:
    case AmsAction::SELECTING:
    case AmsAction::CUTTING:
    case AmsAction::FORMING_TIP:
        return false;
    default:
        return e.filament_loaded;
    }
}

} // namespace helix
