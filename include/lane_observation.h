// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace helix::ams {

/// Where a reading came from. A lane shows what Moonraker holds: presence from
/// the hardware, identity from the firmware's own fields or the lane record in
/// Moonraker's database, and an edit of ours only while its write is in flight.
enum class ObservationSource {
    Sensed,   ///< Real hardware. Presence and docking only, never identity.
    Firmware, ///< What the firmware frame, or a record the firmware wrote, states.
    Record,   ///< The lane record HelixScreen reads from Moonraker's database.
    Spoolman, ///< The Spoolman server's details for the lane's linked spool. Fills only.
    Metered,  ///< The consumption meter's estimate.
    Pending,  ///< A HelixScreen edit whose write has not echoed back yet.
};

/// One reading from one source. Every field is optional so that "not observed"
/// is distinct from "observed as empty": a source that says nothing about a
/// field must not overwrite what another source knows.
struct Observation {
    explicit Observation(ObservationSource src) : source(src) {}

    ObservationSource source;

    std::optional<bool> present;

    /// A toolhead occupying the slot, docked or on the carriage. Sensed by a
    /// physical tool changer, which cannot see the filament inside it: this is
    /// a different question from `present`, and a backend that answers one
    /// must leave the other unset rather than let them stand in for each other.
    std::optional<bool> tool_docked;

    std::optional<uint32_t> color_rgb;
    std::optional<std::string> color_name;
    std::optional<std::string> material;
    std::optional<std::string> brand;
    std::optional<std::string> spool_name;
    std::optional<std::string> catalog_id;
    std::optional<std::string> product_name;
    std::optional<int> spoolman_id;
    std::optional<int> spoolman_filament_id;
    std::optional<int> spoolman_vendor_id;
    /// The OrcaSlicer preset id the linked spool's filament names
    /// (kSpoolmanOrcaIdExtraKey). Only the Spoolman source states it.
    std::optional<std::string> spoolman_orca_filament_id;

    std::optional<float> remaining_weight_g;
    std::optional<float> total_weight_g;

    /// Every optional field above, as a tuple of references. Code that must
    /// touch all of them (amending one record onto another) folds over this
    /// instead of keeping its own field list, so a field added here reaches
    /// that code with no matching line to remember.
    auto fields() {
        return std::tie(present, tool_docked, color_rgb, color_name, material, brand, spool_name,
                        catalog_id, product_name, spoolman_id, spoolman_filament_id,
                        spoolman_vendor_id, spoolman_orca_filament_id, remaining_weight_g,
                        total_weight_g);
    }

    auto fields() const {
        return std::tie(present, tool_docked, color_rgb, color_name, material, brand, spool_name,
                        catalog_id, product_name, spoolman_id, spoolman_filament_id,
                        spoolman_vendor_id, spoolman_orca_filament_id, remaining_weight_g,
                        total_weight_g);
    }
};

} // namespace helix::ams
