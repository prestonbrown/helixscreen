// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <string>

namespace helix::ui {

/// Raw filament-product form field values, as typed by the user. An empty
/// string means the field was left blank. Kept free of LVGL types so the
/// sparse-JSON construction and validation below are unit-testable without a
/// display: the modal reads its widgets into one of these, then calls the
/// helpers.
struct FilamentFormValues {
    std::string id;         ///< explicit id; blank -> derived from brand+name
    std::string brand;      ///< optional
    std::string name;       ///< optional
    std::string type;       ///< required (material dropdown selection)
    std::string nozzle_min; ///< numeric text; blank -> omit
    std::string nozzle_max; ///< numeric text; blank -> omit
    std::string nozzle;     ///< recommended nozzle temp; blank -> omit
    std::string bed;        ///< bed temp; blank -> omit
    std::string density;    ///< g/cm^3; blank -> omit
};

/// The id to persist for @p v: the explicit id if the user typed one, else a
/// slug of "brand name" (lowercased, whitespace runs collapsed to single
/// hyphens, trimmed). Returns "" only when id, brand and name are all blank.
std::string derive_product_id(const FilamentFormValues& v);

/// Validate before save. Returns true when the form may be persisted; on false
/// @p error_out carries a user-facing message. Checks: a non-empty derived id,
/// a non-empty type, and nozzle_min <= nozzle_max when BOTH are provided.
bool validate_product_form(const FilamentFormValues& v, std::string& error_out);

/// Build the SPARSE product JSON written to config/user_filaments.json. Always
/// emits `id`, `type`, and `"source":"user"`. Emits `brand`/`name` only when
/// non-empty; `nozzle_min`/`nozzle_max`/`nozzle`/`bed` as integers and
/// `density` as a float ONLY for fields the user actually filled in — a blank
/// numeric field is omitted so it never clobbers an inherited/catalog value on
/// merge. NOTE the JSON keys deliberately differ from the EffectiveFilament
/// struct field names: recommended nozzle -> `nozzle`, bed_temp -> `bed`,
/// density_g_cm3 -> `density`.
nlohmann::json build_product_json(const FilamentFormValues& v);

} // namespace helix::ui
