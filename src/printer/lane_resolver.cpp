// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_resolver.h"

namespace helix::ams {

ResolvedLane resolve(const LaneSources& sources) {
    ResolvedLane out;

    // Presence is sensed. Nothing in the fleet reports identity from hardware,
    // so identity metadata is never evidence that a spool is present: a vendor
    // cache that still remembers the last spool would otherwise resurrect an
    // emptied lane on every poll.
    if (sources.sensed.has_value() && sources.sensed->present.has_value()) {
        out.present = *sources.sensed->present;
    }

    return out;
}

} // namespace helix::ams
