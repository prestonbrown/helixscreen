// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

#include <optional>

namespace helix::ams {

struct LaneSources;

/// The record slot @p source occupies on @p lane. The one mapping from
/// ObservationSource to its LaneSources member; apply() and drop() both route
/// through it, and the definition below gives every ObservationSource a case,
/// so a missing one is a -Wswitch warning rather than a silent gap.
[[nodiscard]] const std::optional<Observation>& record_for(const LaneSources& lane,
                                                           ObservationSource source);
[[nodiscard]] std::optional<Observation>& record_for(LaneSources& lane, ObservationSource source);

/// The readings a lane currently holds, one slot per source. Sources are
/// separate destinations, so a write to one cannot disturb another. What the
/// UI shows is computed from these by resolve(), never stored back into them.
struct LaneSources {
    std::optional<Observation> sensed;
    std::optional<Observation> firmware;
    std::optional<Observation> record;
    std::optional<Observation> spoolman;
    std::optional<Observation> metered;
    std::optional<Observation> pending;

    /// Replace this source's record with @p obs. Whole-record replacement, so a
    /// field a source stops reporting stops contributing.
    void apply(const Observation& obs) {
        record_for(*this, obs.source) = obs;
    }

    /// Drop one source's record entirely.
    void drop(ObservationSource s) {
        record_for(*this, s).reset();
    }
};

inline const std::optional<Observation>& record_for(const LaneSources& lane,
                                                    ObservationSource source) {
    switch (source) {
    case ObservationSource::Sensed:
        return lane.sensed;
    case ObservationSource::Firmware:
        return lane.firmware;
    case ObservationSource::Record:
        return lane.record;
    case ObservationSource::Spoolman:
        return lane.spoolman;
    case ObservationSource::Metered:
        return lane.metered;
    case ObservationSource::Pending:
        return lane.pending;
    }
    return lane.sensed;
}

inline std::optional<Observation>& record_for(LaneSources& lane, ObservationSource source) {
    return const_cast<std::optional<Observation>&>(
        record_for(static_cast<const LaneSources&>(lane), source));
}

} // namespace helix::ams
