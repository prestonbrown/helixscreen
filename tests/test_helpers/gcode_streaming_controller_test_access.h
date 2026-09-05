// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gcode_streaming_controller.h"

#include <utility>

namespace helix {
namespace gcode {

/// Reaches the layer cache's raw hit/miss counters.
///
/// The controller publishes only get_cache_hit_rate(), a ratio over counters
/// that never reset. A test that wants to attribute hits to the accesses it
/// made has to zero them first and read both numbers afterwards, because a
/// ratio alone cannot separate "three hits" from "three hits and a miss the
/// warm-up left behind".
class GCodeStreamingControllerTestAccess {
  public:
    /// Zero the cache's hit and miss counters.
    ///
    /// Quiesce the prefetch worker (wait_for_prefetch_idle()) before calling
    /// this: the worker calls get_or_load() on every layer in its radius, so a
    /// batch still in flight keeps writing into the counters afterwards.
    static void reset_cache_stats(GCodeStreamingController& controller) {
        controller.cache_.reset_stats();
    }

    /// {hits, misses} since the last reset.
    static std::pair<size_t, size_t> cache_hit_stats(const GCodeStreamingController& controller) {
        return controller.cache_.hit_stats();
    }
};

} // namespace gcode
} // namespace helix
