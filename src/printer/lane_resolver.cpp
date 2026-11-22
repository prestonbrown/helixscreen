// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_resolver.h"

#include <array>

namespace helix::ams {

namespace {

const Observation* held(const std::optional<Observation>& obs) {
    return obs.has_value() ? &*obs : nullptr;
}

/// Lay every identity field @p obs observed onto @p out.
void lay_identity(const Observation& obs, ResolvedLane& out) {
    if (obs.color_rgb.has_value())
        out.color_rgb = *obs.color_rgb;
    if (obs.multi_color_hexes.has_value())
        out.multi_color_hexes = *obs.multi_color_hexes;
    if (obs.color_mode.has_value())
        out.color_mode = *obs.color_mode;
    if (obs.translucent.has_value())
        out.translucent = *obs.translucent;
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

/// Whether a source stronger than the strongest one stating @p field names a
/// different color than that source did: the filament it described is gone.
template <typename Field>
bool color_replaced(const std::array<const Observation*, 4>& by_strength, Field field) {
    size_t source = by_strength.size();
    for (size_t i = 0; i < by_strength.size(); ++i) {
        if (by_strength[i] != nullptr && (by_strength[i]->*field).has_value()) {
            source = i;
        }
    }
    if (source == by_strength.size()) {
        return false;
    }
    const std::optional<uint32_t> color = by_strength[source]->color_rgb;
    for (size_t i = source + 1; color && i < by_strength.size(); ++i) {
        const Observation* stronger = by_strength[i];
        if (stronger != nullptr && stronger->color_rgb &&
            (*stronger->color_rgb & 0xFFFFFF) != (*color & 0xFFFFFF)) {
            return true;
        }
    }
    return false;
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
    const std::array<const Observation*, 4> by_strength = {
        spoolman, held(sources.record), held(sources.firmware), held(sources.pending)};
    for (const Observation* obs : by_strength) {
        if (obs != nullptr) {
            lay_identity(*obs, out);
        }
    }
    // A list belongs to the color its own source stated beside it. A stronger
    // source naming a different color has replaced the filament, so the list
    // goes too; one naming the same color (a backend that snaps a pick to its
    // palette states that row in both places) keeps it. Translucency belongs
    // to its color the same way.
    if (out.multi_color_hexes && !out.multi_color_hexes->empty() &&
        color_replaced(by_strength, &Observation::multi_color_hexes)) {
        out.multi_color_hexes = std::string();
        out.color_mode = std::string();
    }
    if (out.translucent.value_or(false) && color_replaced(by_strength, &Observation::translucent)) {
        out.translucent = false;
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
