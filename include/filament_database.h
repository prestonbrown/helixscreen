// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The user's sparse per-type fields, owned by MaterialSettingsManager
namespace filament {
struct MaterialOverride;
const MaterialOverride* get_material_override(std::string_view name);
} // namespace filament

/**
 * @file filament_database.h
 * @brief Filament material types with temperature recommendations
 *
 * The shipped rows are the `types` array in assets/filaments.json. The user
 * overlay's `types` array (user_filaments.json) patches a shipped row by name or
 * defines a new one. Everything here reads the merged snapshot from materials().
 *
 * Temperature sources:
 * - Manufacturer recommendations from major brands (Bambu, Polymaker, eSUN, etc.)
 * - Community consensus from r/3Dprinting and Voron Discord
 * - Tested ranges from the author's Voron 2.4
 */

namespace filament {

/**
 * @brief The fields of one type the user set in the overlay
 *
 * Only the fields the user set are present (sparse). They are already merged
 * into materials(); this view exists for callers that must tell a user value
 * from a shipped default.
 */
struct MaterialOverride {
    std::optional<int> nozzle_min;
    std::optional<int> nozzle_max;
    std::optional<int> bed_temp;
    std::optional<int> chamber_temp; ///< 0 = deliberate "no chamber heat" for this material
    std::optional<std::string> preheat_macro;  ///< Klipper macro name (uppercase canonical form)
    std::optional<bool> macro_handles_heating; ///< true = macro replaces SET_HEATER_TEMPERATURE
};

/**
 * @brief Material information with temperature recommendations
 */
struct MaterialInfo {
    const char* name;     ///< Material name (e.g., "PLA", "PETG")
    int nozzle_min;       ///< Minimum nozzle temperature (°C)
    int nozzle_max;       ///< Maximum nozzle temperature (°C)
    int bed_temp;         ///< Recommended bed temperature (°C)
    const char* category; ///< Category for grouping (e.g., "Standard", "Engineering")

    // Drying parameters
    int dry_temp_c;   ///< Drying temperature (0 = not hygroscopic)
    int dry_time_min; ///< Drying duration in minutes

    // Physical properties
    float density_g_cm3; ///< Material density (g/cm³)

    // Classification
    int chamber_temp_c;       ///< Recommended chamber temp (0 = none/open)
    const char* compat_group; ///< "PLA", "PETG", "ABS_ASA", "PA", "TPU", "PC", "HIGH_TEMP"

    bool user_defined = false; ///< Defined by the user overlay, no shipped row

    /**
     * @brief Get recommended nozzle temperature (midpoint of range)
     */
    [[nodiscard]] constexpr int nozzle_recommended() const {
        return (nozzle_min + nozzle_max) / 2;
    }

    /**
     * @brief Check if material requires an enclosure
     */
    [[nodiscard]] constexpr bool needs_enclosure() const {
        return chamber_temp_c > 0;
    }

    /**
     * @brief Check if material needs drying before use
     */
    [[nodiscard]] constexpr bool needs_drying() const {
        return dry_temp_c > 0;
    }
};

/**
 * @brief The effective material table: shipped types with the user overlay merged in
 *
 * Loaded on first use. The snapshot is immutable; a reload swaps in a new one,
 * and every `const char*` a MaterialInfo carries stays valid across reloads.
 * Empty when the asset is missing or unparseable (logged as an error).
 */
// NAMESPACE_OK: extends filament::, the material table's existing namespace
std::shared_ptr<const std::vector<MaterialInfo>> materials();

/// The shipped rows alone, before the user overlay: the defaults a user
/// override is measured against. Same lifetime rules as materials().
// NAMESPACE_OK: extends filament::, the material table's existing namespace
std::shared_ptr<const std::vector<MaterialInfo>> shipped_materials();

/// Re-read the shipped asset and the user overlay from their default paths.
// NAMESPACE_OK: extends filament::, the material table's existing namespace
void reload_materials();

/// Load from explicit paths; an empty @p overlay_path means no overlay.
// NAMESPACE_OK: extends filament::, the material table's existing namespace
void load_materials_from(const std::string& asset_path, const std::string& overlay_path);

/**
 * @brief Material name alias for common variations
 */
struct MaterialAlias {
    const char* alias;     ///< Alternative name
    const char* canonical; ///< Canonical MaterialInfo name
};

/**
 * @brief Common material name aliases
 */
// clang-format off
inline constexpr MaterialAlias kMaterialAliases[] = {
    {"Nylon",        "PA"},
    {"Nylon-CF",     "PA-CF"},
    {"Nylon-GF",     "PA-GF"},
    {"Polycarbonate","PC"},
    {"PLA Silk",     "Silk PLA"},
    {"Silk",         "Silk PLA"},
    {"Generic",      "PLA"},
    {"ULTEM",        "PEI"},
};
// clang-format on

/// Number of aliases in the database
inline constexpr size_t kAliasCount = sizeof(kMaterialAliases) / sizeof(kMaterialAliases[0]);

/**
 * @brief Resolve a material alias to its canonical name
 * @param name Material name or alias to resolve
 * @return Canonical name if alias found, original name otherwise
 */
inline std::string_view resolve_alias(std::string_view name) {
    std::string name_lower(name);
    name_lower = helix::text_io::to_lower(name_lower);

    for (const auto& alias : kMaterialAliases) {
        std::string alias_lower(alias.alias);
        alias_lower = helix::text_io::to_lower(alias_lower);

        if (alias_lower == name_lower) {
            return alias.canonical;
        }
    }
    return name;
}

/**
 * @brief Find material info by name (case-insensitive)
 * @param name Material name to look up (aliases are resolved automatically)
 * @return MaterialInfo if found, std::nullopt otherwise
 */
// NAMESPACE_OK: extends filament::, the material table's existing namespace
inline std::optional<MaterialInfo> find_material_in(const std::vector<MaterialInfo>& table,
                                                    std::string_view name) {
    std::string wanted(resolve_alias(name));
    wanted = helix::text_io::to_lower(wanted);

    for (const auto& mat : table) {
        std::string mat_lower(mat.name);
        mat_lower = helix::text_io::to_lower(mat_lower);
        if (mat_lower == wanted) {
            return mat;
        }
    }
    return std::nullopt;
}

inline std::optional<MaterialInfo> find_material(std::string_view name) {
    return find_material_in(*materials(), name);
}

/**
 * @brief Find the shipped row for a material, ignoring the user overlay
 * @return nullopt for an unknown name and for a type only the user defined
 */
// NAMESPACE_OK: extends filament::, the material table's existing namespace
inline std::optional<MaterialInfo> find_shipped_material(std::string_view name) {
    return find_material_in(*shipped_materials(), name);
}

/**
 * @brief Get all materials in a category
 * @param category Category name (e.g., "Standard", "Engineering")
 * @return Vector of matching materials
 */
inline std::vector<MaterialInfo> get_materials_by_category(std::string_view category) {
    std::vector<MaterialInfo> result;
    for (const auto& mat : *materials()) {
        if (category == mat.category) {
            result.push_back(mat);
        }
    }
    return result;
}

/**
 * @brief Get list of all unique category names
 * @return Vector of category names in order of appearance
 */
inline std::vector<const char*> get_categories() {
    std::vector<const char*> categories;
    for (const auto& mat : *materials()) {
        bool found = false;
        for (const auto* cat : categories) {
            if (std::string_view(cat) == mat.category) {
                found = true;
                break;
            }
        }
        if (!found) {
            categories.push_back(mat.category);
        }
    }
    return categories;
}

/**
 * @brief Get list of all material names (for dropdown population)
 * @return Vector of material name strings
 */
inline std::vector<const char*> get_all_material_names() {
    const auto table = materials();
    std::vector<const char*> names;
    names.reserve(table->size());
    for (const auto& mat : *table) {
        names.push_back(mat.name);
    }
    return names;
}

/**
 * @brief Get the compatibility group for a material
 * @param material Material name to look up
 * @return Compatibility group name, or nullptr if unknown
 */
inline const char* get_compatibility_group(std::string_view material) {
    auto mat = find_material(material);
    if (mat.has_value()) {
        return mat->compat_group;
    }
    return nullptr;
}

/**
 * @brief Check if two materials are compatible for endless spool
 * @param mat1 First material name
 * @param mat2 Second material name
 * @return true if materials are compatible (same group or either unknown)
 */
inline bool are_materials_compatible(std::string_view mat1, std::string_view mat2) {
    const char* group1 = get_compatibility_group(mat1);
    const char* group2 = get_compatibility_group(mat2);

    // Unknown materials are compatible with anything
    if (group1 == nullptr || group2 == nullptr) {
        return true;
    }

    // Same group = compatible
    return std::string_view(group1) == std::string_view(group2);
}

/**
 * @brief Drying preset by compatibility group
 */
struct DryingPreset {
    const char* name; ///< Group/preset name
    int temp_c;       ///< Drying temperature in °C
    int time_min;     ///< Drying time in minutes
};

/**
 * @brief Get drying presets grouped by compatibility group (for dropdown)
 *
 * The `types` table is the ONLY source of drying data in this codebase. This function
 * derives one preset per compatibility group by taking the group-wide MAXIMUM of
 * both dry_temp_c and dry_time_min across that group's hygroscopic members.
 *
 * Why max and not first-member (the old behaviour), and why one preset per group
 * rather than one per material:
 *
 *  - Max, because the two failure directions are not symmetric. Under-drying
 *    leaves moisture in the filament and ruins the print; over-drying *within a
 *    compat group* does not, because a compat group is by construction a set of
 *    chemically interchangeable materials whose glass-transition temperatures sit
 *    in the same band. Taking the first member silently under-dried 8 materials
 *    (PET/PET-CF/PET-GF at 65 °C got the PETG row's 55 °C; PA66/PA6-CF/PPA/
 *    PPA-CF/PPA-GF at 80 °C got the PA row's 70 °C).
 *  - Per group, because the consumer is a dryer preset dropdown
 *    (ams_types.h get_default_drying_presets() -> AMS environment overlay). One
 *    entry per material would turn a 12-row list into a 60+ row scroll for no
 *    added precision, and the preset `name` is displayed verbatim, so switching
 *    to material names would also change strings users already recognise.
 *
 * The safety of the max is not assumed — it is enforced by an invariant test that
 * checks each group's max drying temperature still clears the LOWEST nozzle_min
 * in that group by 100 °C. See tests/unit/test_filament_data_invariants.cpp.
 *
 * @return Vector of unique drying presets, one per hygroscopic compat group
 */
inline std::vector<DryingPreset> get_drying_presets_by_group() {
    std::vector<DryingPreset> presets;

    for (const auto& mat : *materials()) {
        if (mat.dry_temp_c == 0) {
            continue; // Skip non-hygroscopic materials
        }

        DryingPreset* existing = nullptr;
        for (auto& preset : presets) {
            if (std::string_view(preset.name) == mat.compat_group) {
                existing = &preset;
                break;
            }
        }

        if (existing == nullptr) {
            presets.push_back({mat.compat_group, mat.dry_temp_c, mat.dry_time_min});
        } else {
            // Widen to cover the most demanding member of the group.
            existing->temp_c = std::max(existing->temp_c, mat.dry_temp_c);
            existing->time_min = std::max(existing->time_min, mat.dry_time_min);
        }
    }

    return presets;
}

/**
 * @brief Get the drying preset that covers a specific material
 *
 * Resolves the material (aliases included), then returns its compat group's
 * preset from get_drying_presets_by_group(). Every consumer that needs "how do I
 * dry this material" must route through here rather than reading dry_temp_c off a
 * MaterialInfo directly, so that the answer a user sees in a per-material context
 * is the same answer the dryer preset dropdown offers.
 *
 * @param material Material name or alias
 * @return The covering preset, or nullopt if the material is unknown or its
 *         entire compat group is non-hygroscopic (e.g. PE, EVA)
 */
inline std::optional<DryingPreset> get_drying_preset_for_material(std::string_view material) {
    auto mat = find_material(material);
    if (!mat.has_value()) {
        return std::nullopt;
    }
    for (const auto& preset : get_drying_presets_by_group()) {
        if (std::string_view(preset.name) == std::string_view(mat->compat_group)) {
            return preset;
        }
    }
    return std::nullopt;
}

/// Filament diameter assumed when the printer has not reported its own
/// ([extruder] filament_diameter).
// NAMESPACE_OK: extends filament::, the material table's existing namespace
inline constexpr float kDefaultDiameterMm = 1.75f;

/// Filament cross-section in mm², pi * (d/2)^2
// NAMESPACE_OK: extends filament::, the material table's existing namespace
inline float cross_section_mm2(float diameter_mm) {
    const float radius_mm = diameter_mm / 2.0f;
    return static_cast<float>(M_PI) * radius_mm * radius_mm;
}

/**
 * @brief Calculate filament length from weight
 * @param weight_g Weight in grams
 * @param density Material density in g/cm³
 * @param diameter_mm Filament diameter in mm
 * @return Length in meters
 */
inline float weight_to_length_m(float weight_g, float density,
                                float diameter_mm = kDefaultDiameterMm) {
    // mass / density is cm³; x1000 is mm³, / area is mm of filament
    const float length_mm = (weight_g / density) * 1000.0f / cross_section_mm2(diameter_mm);
    return length_mm / 1000.0f;
}

/**
 * @brief Calculate filament weight in grams from length
 * @param length_mm Length in millimeters
 * @param density Material density in g/cm³
 * @param diameter_mm Filament diameter in mm
 * @return Mass in grams, or 0 if density or length is not positive
 */
inline float length_to_weight_g(float length_mm, float density,
                                float diameter_mm = kDefaultDiameterMm) {
    if (density <= 0.0f || length_mm <= 0.0f) {
        return 0.0f;
    }
    const float volume_mm3 = length_mm * cross_section_mm2(diameter_mm);
    return (volume_mm3 / 1000.0f) * density;
}

// ============================================================================
// Material Comfort Ranges (humidity thresholds for storage quality indicators)
// ============================================================================

/**
 * @brief Humidity thresholds for a given material type
 *
 * Used by AMS environment display to color-code humidity readings:
 *   - Below max_humidity_good: green (safe)
 *   - Between good and warn: yellow (caution)
 *   - Above max_humidity_warn: red (material degradation risk)
 */
struct MaterialComfortRange {
    const char* material;
    float max_humidity_good; ///< Below this = green (safe)
    float max_humidity_warn; ///< Below this = yellow, above = red
    int dry_temp_c;          ///< Recommended drying temperature (0 = no drying needed)
    int dry_time_hours;      ///< Recommended drying time in hours
};

/**
 * @brief Humidity thresholds per compatibility group
 *
 * This is the ONLY comfort data that is not derivable from the `types` table, because
 * there is no moisture-uptake field on MaterialInfo to derive it from. It is
 * keyed by compat_group rather than by material name on purpose: a group is a set
 * of chemically interchangeable materials, so one row covers every member and a
 * newly added type row inherits humidity coverage for free instead of
 * silently falling off the AMS humidity indicator.
 *
 * The drying temperature and time that get_comfort_range() reports are NOT listed
 * here — they come from get_drying_presets_by_group(), which derives them from
 * the `types` table. Adding dry_temp/dry_time columns to this table would recreate the
 * third drying source that this layout exists to eliminate.
 */
struct GroupHumidityRange {
    const char* group;
    float max_humidity_good; ///< Below this = green (safe)
    float max_humidity_warn; ///< Below this = yellow, above = red
};

// clang-format off
inline constexpr GroupHumidityRange kGroupHumidityRanges[] = {
    //  group          good   warn
    {"PLA",            50.0f, 65.0f},  // Tolerant; moisture shows as surface fuzz
    {"PETG",           40.0f, 55.0f},
    {"ABS_ASA",        35.0f, 50.0f},
    {"PA",             20.0f, 35.0f},  // Nylons absorb aggressively from ambient air
    {"PC",             30.0f, 45.0f},
    {"TPU",            40.0f, 55.0f},
    {"HIGH_TEMP",      20.0f, 35.0f},  // PEEK/PEI/PSU/PPSU/PPS - as hygroscopic as nylon
    {"PP",             50.0f, 65.0f},  // Polyolefin, very low uptake
    {"PE",             60.0f, 75.0f},  // Effectively non-hygroscopic
    {"CoPE",           40.0f, 55.0f},  // Copolyester, behaves like PETG
    {"EVA",            50.0f, 65.0f},
    {"SBS",            45.0f, 60.0f},  // Styrenic, low uptake
};
// clang-format on

/**
 * @brief Per-material humidity overrides
 *
 * Deliberately tiny. An entry here is only justified when a material's moisture
 * sensitivity is genuinely unlike the rest of its compat group — not merely a
 * different number someone once typed. Anything that can be expressed at group
 * level belongs in kGroupHumidityRanges above.
 */
// clang-format off
inline constexpr GroupHumidityRange kMaterialHumidityOverrides[] = {
    //  material       good   warn
    // Water-soluble supports dissolve in ambient humidity, so they need a far
    // tighter band than the PLA group they print alongside and are grouped with.
    {"PVA",            15.0f, 30.0f},
    {"BVOH",           15.0f, 30.0f},
    // HIPS is styrenic and only mildly hygroscopic; it groups with ABS/ASA for
    // endless-spool interchange (same chamber/bed regime), not for moisture.
    {"HIPS",           40.0f, 55.0f},
};
// clang-format on

/**
 * @brief Look up humidity comfort range and drying info for a material type
 *
 * Fully DERIVED: humidity thresholds come from the material's compat group (with
 * a small per-material override table), and drying temp/time come from
 * get_drying_presets_by_group(), which reads the `types` table. There is no independent
 * drying opinion in this function — that is the point.
 *
 * @param material Material name or alias (e.g., "PLA", "PETG", "Nylon")
 * @return Comfort range, or nullopt if the material does not resolve
 */
inline std::optional<MaterialComfortRange> get_comfort_range(const std::string& material) {
    auto mat = find_material(material);
    if (!mat.has_value()) {
        return std::nullopt;
    }

    MaterialComfortRange result{};
    result.material = mat->name;

    bool have_humidity = false;
    for (const auto& ovr : kMaterialHumidityOverrides) {
        if (std::string_view(ovr.group) == std::string_view(mat->name)) {
            result.max_humidity_good = ovr.max_humidity_good;
            result.max_humidity_warn = ovr.max_humidity_warn;
            have_humidity = true;
            break;
        }
    }
    if (!have_humidity) {
        for (const auto& gr : kGroupHumidityRanges) {
            if (std::string_view(gr.group) == std::string_view(mat->compat_group)) {
                result.max_humidity_good = gr.max_humidity_good;
                result.max_humidity_warn = gr.max_humidity_warn;
                have_humidity = true;
                break;
            }
        }
    }
    if (!have_humidity) {
        // A compat group with no humidity row would otherwise report 0/0 and
        // colour every reading red. Fail closed to "unknown" instead; the
        // invariant test asserts this branch is unreachable for shipped data.
        return std::nullopt;
    }

    // Drying: single source of truth, shared with the dryer preset dropdown.
    auto preset = get_drying_preset_for_material(mat->name);
    if (preset.has_value()) {
        result.dry_temp_c = preset->temp_c;
        result.dry_time_hours = preset->time_min / 60;
    } else {
        result.dry_temp_c = 0; // wholly non-hygroscopic group (PE, EVA)
        result.dry_time_hours = 0;
    }

    return result;
}

// ============================================================================
// Picker reachability
// ============================================================================

/**
 * @brief Material types that intentionally have NO catalog product
 *
 * The material picker builds its list from the PRODUCT catalog
 * (assets/filaments.json), not from the `types` table. A type with no product is
 * therefore invisible in the UI. For most rows that is a bug; for these it is the
 * design — they exist so that a material string arriving from Orca, a printer's
 * firmware, or Spoolman resolves to sane temperatures, and were never meant to be
 * user-selectable.
 *
 * This list is the machine-readable form of that intent. An invariant test
 * asserts the shipped catalog covers exactly the shipped types minus this list, in both
 * directions — so a new type with no product fails the build until someone
 * decides which bucket it belongs in, and an entry here that DOES gain a product
 * must be removed rather than rotting.
 *
 * Making one of these selectable is a one-line addition to
 * scripts/fixtures/filament_seed.json plus removing it here.
 */
// clang-format off
inline constexpr const char* kResolutionOnlyMaterials[] = {
    // No supported printer's stock hotend reaches these temperatures; the rows
    // exist so an externally-supplied string still resolves to sane data rather
    // than inheriting a 0 °C bed. (PPS/PPS-CF, the reachable end of HIGH_TEMP,
    // DO ship products.)
    "PEEK",     // 370-420 °C
    "PEI",      // 340-380 °C (ULTEM)
    "PSU",      // 340-380 °C
    "PPSU",     // 350-390 °C
    // Spec strings rather than shelf products. Users buy the named grade
    // (TPU-85A, PA6, PA12), not the descriptor, so offering both would show two
    // picker rows for one spool.
    "TPU-Soft", // descriptor; the shelf product is TPU-85A
    "PA66",     // unfilled PA66 is not sold as consumer filament; PA6-CF etc. are
    "PC-ABS",   // blend spec string carried by vendor/Orca metadata
};
// clang-format on

/// Number of resolution-only materials
inline constexpr size_t kResolutionOnlyCount =
    sizeof(kResolutionOnlyMaterials) / sizeof(kResolutionOnlyMaterials[0]);

/**
 * @brief Is this material deliberately absent from the product catalog?
 * @param name Material name (exact `types` spelling)
 */
inline bool is_resolution_only(std::string_view name) {
    for (const auto* m : kResolutionOnlyMaterials) {
        if (std::string_view(m) == name) {
            return true;
        }
    }
    return false;
}

} // namespace filament
