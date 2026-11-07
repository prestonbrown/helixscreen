// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_resolver.h"

namespace helix::ams {

namespace {

const Observation* held(const std::optional<Observation>& obs) {
    return obs.has_value() ? &*obs : nullptr;
}

/// Lay every identity field @p obs observed onto @p out.
void lay_identity(const Observation& obs, ResolvedLane& out) {
    if (obs.color_rgb.has_value())
        out.color_rgb = *obs.color_rgb;
    if (obs.color_name.has_value())
        out.color_name = *obs.color_name;
    if (obs.material.has_value())
        out.material = *obs.material;
    if (obs.brand.has_value())
        out.brand = *obs.brand;
    if (obs.spool_name.has_value())
        out.spool_name = *obs.spool_name;
    if (obs.catalog_id.has_value())
        out.catalog_id = *obs.catalog_id;
    if (obs.product_name.has_value())
        out.product_name = *obs.product_name;
    if (obs.spoolman_id.has_value())
        out.spoolman_id = *obs.spoolman_id;
    if (obs.spoolman_filament_id.has_value())
        out.spoolman_filament_id = *obs.spoolman_filament_id;
    if (obs.spoolman_vendor_id.has_value())
        out.spoolman_vendor_id = *obs.spoolman_vendor_id;
    if (obs.spoolman_orca_filament_id.has_value())
        out.spoolman_orca_filament_id = *obs.spoolman_orca_filament_id;
}

void lay_weights(const Observation& obs, ResolvedLane& out) {
    if (obs.remaining_weight_g.has_value())
        out.remaining_weight_g = *obs.remaining_weight_g;
    if (obs.total_weight_g.has_value())
        out.total_weight_g = *obs.total_weight_g;
}

} // namespace

int linked_spool_id(const LaneSources& sources) {
    for (const Observation* obs :
         {held(sources.pending), held(sources.firmware), held(sources.record)}) {
        if (obs != nullptr && obs->spoolman_id.has_value()) {
            return *obs->spoolman_id > 0 ? *obs->spoolman_id : 0;
        }
    }
    return 0;
}

ResolvedLane resolve(const LaneSources& sources) {
    ResolvedLane out;

    // Presence and docking are sensed. Identity is never evidence that a spool
    // is present: a firmware field that still names the last spool would
    // otherwise resurrect an emptied lane on every poll.
    if (sources.sensed.has_value() && sources.sensed->present.has_value()) {
        out.present = *sources.sensed->present;
    }
    if (sources.sensed.has_value() && sources.sensed->tool_docked.has_value()) {
        out.tool_docked = *sources.sensed->tool_docked;
    }

    // Spoolman's details describe one spool, so they count only while that
    // spool is the one the lane links.
    const int link = linked_spool_id(sources);
    const Observation* spoolman = held(sources.spoolman);
    if (spoolman != nullptr && (link <= 0 || spoolman->spoolman_id.value_or(0) != link)) {
        spoolman = nullptr;
    }

    // Weakest first, so each pass overwrites what a weaker source said. A
    // source silent on a field leaves the weaker value standing.
    for (const Observation* obs :
         {spoolman, held(sources.record), held(sources.firmware), held(sources.pending)}) {
        if (obs != nullptr) {
            lay_identity(*obs, out);
        }
    }
    for (const Observation* obs : {held(sources.metered), held(sources.record),
                                   held(sources.firmware), spoolman, held(sources.pending)}) {
        if (obs != nullptr) {
            lay_weights(*obs, out);
        }
    }
    return out;
}

} // namespace helix::ams
