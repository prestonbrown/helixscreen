// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace helix::ams {

/// Where a reading came from. Presence is sensed; identity is declared.
/// No AMS backend in the fleet reports a spool identity from hardware, so an
/// identity value only ever originates from a human or a database.
enum class ObservationSource {
    Sensed,      ///< Real hardware. Presence and motion only, never identity.
    Spoolman,    ///< The Spoolman server.
    LocalUser,   ///< A human editing in HelixScreen.
    VendorCache, ///< Firmware-persisted metadata. A cache of a past declaration.
    Metered,     ///< The consumption meter.
    /// What our own stored record remembered from before this session.
    ///
    /// Distinct from VendorCache, which is what the machine states on THIS
    /// frame. The two had one destination and two producers: a lane's stored
    /// record was filed as VendorCache at load, and the backend's own parse
    /// then replaced that record whole, erasing every field the frame did not
    /// restate. A brand nobody had typed since last boot vanished on the first
    /// poll.
    Remembered,
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

    std::optional<float> remaining_weight_g;
    std::optional<float> total_weight_g;

    /// When this statement was made, from the record it was filed from.
    ///
    /// Bookkeeping for the newest-edit-wins comparison between a lane's own
    /// statement and a record another tool wrote, never part of what the
    /// statement says: it deliberately stays out of fields(), so no amend or
    /// merge carries it and resolve() never sees it.
    std::optional<std::chrono::system_clock::time_point> edited_at;

    /// Every optional field above, as a tuple of references. Code that must
    /// touch all of them (amending one record onto another) folds over this
    /// instead of keeping its own field list, so a field added here reaches
    /// that code with no matching line to remember.
    auto fields() {
        return std::tie(present, tool_docked, color_rgb, color_name, material, brand, spool_name,
                        catalog_id, product_name, spoolman_id, spoolman_filament_id,
                        spoolman_vendor_id, remaining_weight_g, total_weight_g);
    }

    auto fields() const {
        return std::tie(present, tool_docked, color_rgb, color_name, material, brand, spool_name,
                        catalog_id, product_name, spoolman_id, spoolman_filament_id,
                        spoolman_vendor_id, remaining_weight_g, total_weight_g);
    }
};

} // namespace helix::ams
