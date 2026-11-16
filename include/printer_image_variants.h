// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "json_fwd.h"

#include <string>
#include <string_view>
#include <vector>

/**
 * @file printer_image_variants.h
 * @brief Pick a printer database entry's image from the hardware the app detected
 *
 * An entry may carry an ordered `image_variants` list next to its `image`:
 *
 *     "image_variants": [{"when": {"filament_system": "cfs"}, "image": "creality-k2-cfs.png"}]
 *
 * Conditions:
 *  - `filament_system`: an AmsType spelling; holds when a backend of that kind
 *    has a unit present.
 *  - `enclosure_top`: true or false; holds when the firmware's report of a
 *    removable enclosure top (include/enclosure_top.h) equals it. A printer that
 *    reports none, or has not reported yet, reads false.
 *  - `chamber_heater`: true or false; holds when the printer having a chamber
 *    heater (detected or manually assigned) equals it.
 *
 * The first variant whose every condition matches wins; none matching means the
 * plain `image`. Vendor names live in the database only: a condition names a
 * detected capability, and this module answers it from the facts it is handed.
 */

namespace helix::printer_images {

/// What the app has detected about the connected printer that can choose an image.
struct HardwareFacts {
    /// Every registered filament-system backend's kind; empty when none is detected.
    std::vector<AmsType> filament_systems;
    /// The firmware reports a removable enclosure top fitted.
    bool enclosure_top = false;
    /// The printer has a chamber heater, detected or manually assigned.
    bool has_chamber_heater = false;
};

/// Whether @p key is a condition `image_variants` understands.
[[nodiscard]] bool is_known_variant_condition(std::string_view key);

/// Whether @p value is a value the condition @p key can ever match.
/// `filament_system` takes an AmsType spelling ams_type_from_string() parses
/// ("cfs", "afc", "happy_hare", ...); "none" is not one. `enclosure_top` and
/// `chamber_heater` take a JSON boolean.
[[nodiscard]] bool is_valid_variant_condition_value(std::string_view key,
                                                    const nlohmann::json& value);

/// A variant's `when` object against @p facts: every condition must hold.
/// A condition this build does not know, a value it cannot parse, or a `when`
/// that is not an object makes the variant NOT match, so an entry naming a
/// newer condition degrades to its plain image. An empty `when` matches.
[[nodiscard]] bool variant_matches(const nlohmann::json& when, const HardwareFacts& facts);

/// The image filename @p entry shows for @p facts: the first matching
/// `image_variants` entry's `image`, else the entry's own `image` ("" if absent).
[[nodiscard]] std::string select_image(const nlohmann::json& entry, const HardwareFacts& facts);

} // namespace helix::printer_images
