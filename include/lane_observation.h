// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
};

/// One reading from one source. Every field is optional so that "not observed"
/// is distinct from "observed as empty": a source that says nothing about a
/// field must not overwrite what another source knows.
struct Observation {
    explicit Observation(ObservationSource src) : source(src) {}

    ObservationSource source;

    std::optional<bool> present;

    std::optional<uint32_t> color_rgb;
    std::optional<std::string> color_name;
    std::optional<std::string> material;
    std::optional<std::string> brand;
    std::optional<std::string> spool_name;
    std::optional<std::string> catalog_id;
    std::optional<std::string> product_name;
    std::optional<int> spoolman_id;
    std::optional<int> spoolman_vendor_id;

    std::optional<float> remaining_weight_g;
    std::optional<float> total_weight_g;

    /// Every optional field above, as a tuple of references. Code that must
    /// touch all of them (amending one record onto another) folds over this
    /// instead of keeping its own field list, so a field added here reaches
    /// that code with no matching line to remember.
    auto fields() {
        return std::tie(present, color_rgb, color_name, material, brand, spool_name, catalog_id,
                        product_name, spoolman_id, spoolman_vendor_id, remaining_weight_g,
                        total_weight_g);
    }

    auto fields() const {
        return std::tie(present, color_rgb, color_name, material, brand, spool_name, catalog_id,
                        product_name, spoolman_id, spoolman_vendor_id, remaining_weight_g,
                        total_weight_g);
    }
};

} // namespace helix::ams
