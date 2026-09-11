// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
    ObservationSource source = ObservationSource::Sensed;

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

    /// Set when this reading is the echo of a write we issued. Lets a consumer
    /// tell its own value coming back from a third party's edit without the
    /// per-backend suppressors that answer the same question today.
    std::optional<uint64_t> echo_token;
};

} // namespace helix::ams
