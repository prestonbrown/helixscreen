// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

#include <optional>

namespace helix::ams {

/// The readings a lane currently holds, one slot per source. Sources are
/// separate destinations, so a write to one cannot disturb another. What the
/// UI shows is computed from these by resolve(), never stored back into them.
struct LaneSources {
    std::optional<Observation> sensed;
    std::optional<Observation> spoolman;
    std::optional<Observation> local_user;
    std::optional<Observation> vendor_cache;
    std::optional<Observation> metered;

    /// Replace this source's record with @p obs. Whole-record replacement, so a
    /// field a source stops reporting stops contributing.
    void apply(const Observation& obs) {
        switch (obs.source) {
        case ObservationSource::Sensed:
            sensed = obs;
            break;
        case ObservationSource::Spoolman:
            spoolman = obs;
            break;
        case ObservationSource::LocalUser:
            local_user = obs;
            break;
        case ObservationSource::VendorCache:
            vendor_cache = obs;
            break;
        case ObservationSource::Metered:
            metered = obs;
            break;
        }
    }

    /// Drop one source's record entirely. This covers a clear that discards
    /// what that source knew; it does not cover `src/ui/ui_ams_edit_overlay.cpp`'s
    /// unlink, which keeps identity across the clear and needs a record demoted
    /// to a weaker source rather than dropped. No promotion/demotion operation
    /// exists for that case.
    void drop(ObservationSource s) {
        switch (s) {
        case ObservationSource::Sensed:
            sensed.reset();
            break;
        case ObservationSource::Spoolman:
            spoolman.reset();
            break;
        case ObservationSource::LocalUser:
            local_user.reset();
            break;
        case ObservationSource::VendorCache:
            vendor_cache.reset();
            break;
        case ObservationSource::Metered:
            metered.reset();
            break;
        }
    }
};

} // namespace helix::ams
