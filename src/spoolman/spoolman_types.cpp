// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_types.h"

#include "ams_types.h"
#include "lane_observation.h"

#include <string>

// NAMESPACE_OK: defines the file-scope declaration in spoolman_types.h
void apply_spool_to_slot(helix::SlotInfo& info, const SpoolInfo& spool) {
    // A link replaces the slot's identity whole, so a field the spool does not
    // state lands blank rather than leaving the previous spool's value behind.
    const helix::ams::Observation stated = helix::ams::spool_identity_observation(spool);
    info.spoolman_id = stated.spoolman_id.value_or(0);
    info.spoolman_filament_id = spool.filament_id;
    info.spoolman_vendor_id = stated.spoolman_vendor_id.value_or(0);
    info.spoolman_orca_filament_id = stated.spoolman_orca_filament_id.value_or(std::string{});
    info.material = stated.material.value_or(std::string{});
    info.brand = stated.brand.value_or(std::string{});
    info.spool_name = stated.spool_name.value_or(std::string{});
    info.multi_color_hexes = spool.multi_color_hexes;
    info.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
    info.total_weight_g = static_cast<float>(spool.initial_weight_g);
    info.nozzle_temp_min = spool.nozzle_temp_min;
    info.nozzle_temp_max = spool.nozzle_temp_max;
    info.bed_temp = spool.bed_temp_recommended;

    // A Spoolman spool record has no colour-name field, so there is nothing
    // truthful to put in SlotInfo::color_name — the field docs/specs/filament_slots.md
    // defines as "human-readable COLOUR label, distinct from the `color` hex" and
    // publishes to the lane_data namespace that OrcaSlicer and Happy Hare read.
    // Deriving one from the hex would freeze a value that is recomputable for
    // free from the `color` we already publish, and that goes stale the moment
    // an override changes the hex; the display path already calls
    // helix::get_color_name_from_hex() as its own fallback.
    //
    // Blank is therefore the answer, but only when Spoolman actually replaces
    // the colour: an existing user-entered colour label describes the colour it
    // was entered against, so it survives a link that carries no colour and is
    // dropped by one that does.
    if (stated.color_rgb.has_value()) {
        info.color_rgb = *stated.color_rgb;
        info.color_name.clear();
    }
}
