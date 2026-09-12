// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration for override merge in find_material()
namespace filament {
struct MaterialOverride;
const MaterialOverride* get_material_override(std::string_view name);
} // namespace filament

/**
 * @file filament_database.h
 * @brief Static database of filament materials with temperature recommendations
 *
 * Provides a comprehensive list of common 3D printing materials with their
 * recommended temperature ranges. Used by the Edit Filament modal to auto-derive
 * temperatures when a material is selected.
 *
 * Temperature sources:
 * - Manufacturer recommendations from major brands (Bambu, Polymaker, eSUN, etc.)
 * - Community consensus from r/3Dprinting and Voron Discord
 * - Tested ranges from the author's Voron 2.4
 */

namespace filament {

/**
 * @brief User override for material temperature settings
 *
 * Only overridden fields are present (sparse storage).
 * Applied transparently in find_material() so all callers
 * automatically get user-customized values.
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
 * @brief Static database of common filament materials
 *
 * Materials are grouped by category:
 * - Standard: PLA, PETG - most common, beginner-friendly
 * - Engineering: ABS, ASA, PC, PA - require enclosure/higher temps
 * - Flexible: TPU, TPE - rubber-like materials
 * - Support: PVA, HIPS - dissolvable/breakaway supports
 * - Specialty: Wood-fill, Marble, Metal-fill - decorative
 * - High-Temp: PEEK, PEI - industrial applications
 */
// clang-format off
inline constexpr MaterialInfo MATERIALS[] = {
    // name           nozzle   bed   category        dry_temp dry_min density chamber compat_group
    //                min max                        °C       min     g/cm³   °C

    // === Standard Materials (No enclosure required) ===
    {"PLA",         190, 220, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},
    {"PLA+",        200, 230, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},
    {"PLA-CF",      200, 230, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},       // Carbon fiber PLA
    {"PLA-GF",      200, 230, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},       // Glass fiber PLA
    {"Silk PLA",    200, 230, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},       // Shiny finish PLA
    {"Matte PLA",   200, 230, 60,  "Standard",      45, 240,  1.24f,  0,  "PLA"},
    {"PETG",        230, 260, 80,  "Standard",      55, 360,  1.27f,  0,  "PETG"},
    {"PETG-CF",     240, 270, 80,  "Standard",      55, 360,  1.27f,  0,  "PETG"},      // Carbon fiber PETG
    {"PETG-GF",     240, 270, 80,  "Standard",      55, 360,  1.27f,  0,  "PETG"},      // Glass fiber PETG
    {"PCTG",        240, 270, 80,  "Standard",      55, 360,  1.23f,  0,  "PETG"},      // PETG variant, clearer
    {"PET-CF",      270, 300, 80,  "Standard",      65, 480,  1.30f,  0,  "PETG"},      // Carbon fiber PET (Polymaker Fiberon)
    {"PET-GF",      270, 300, 80,  "Standard",      65, 480,  1.40f,  0,  "PETG"},      // Glass fiber PET (Polymaker Fiberon)
    {"PET",         250, 270, 80,  "Standard",      65, 480,  1.38f,  0,  "PETG"},      // Unmodified PET - runs hotter than PETG
    {"PHA",         190, 220, 60,  "Standard",      45, 240,  1.25f,  0,  "PLA"},       // Polyhydroxyalkanoate, usually sold as a PLA/PHA blend

    // === Engineering Materials (Enclosure recommended) ===
    {"ABS",         240, 270, 100, "Engineering",   60, 240,  1.04f,  50, "ABS_ASA"},
    {"ABS+",        240, 270, 100, "Engineering",   60, 240,  1.04f,  50, "ABS_ASA"},
    {"ABS-CF",      240, 270, 100, "Engineering",   60, 240,  1.10f,  50, "ABS_ASA"},   // Carbon fiber ABS
    {"ABS-GF",      240, 270, 100, "Engineering",   60, 240,  1.15f,  50, "ABS_ASA"},   // Glass fiber ABS
    {"ASA",         240, 270, 100, "Engineering",   60, 240,  1.07f,  50, "ABS_ASA"},   // UV-resistant ABS alternative
    {"ASA+",        240, 270, 100, "Engineering",   60, 240,  1.07f,  50, "ABS_ASA"},   // Enhanced ASA
    {"ASA-CF",      250, 280, 100, "Engineering",   60, 240,  1.12f,  50, "ABS_ASA"},   // Carbon fiber ASA
    {"ASA-GF",      250, 280, 100, "Engineering",   60, 240,  1.18f,  50, "ABS_ASA"},   // Glass fiber ASA
    {"PC",          260, 300, 110, "Engineering",   80, 480,  1.20f,  55, "PC"},        // Polycarbonate
    {"PC-CF",       270, 300, 110, "Engineering",   80, 480,  1.20f,  55, "PC"},        // Carbon fiber PC
    {"PC-GF",       270, 300, 110, "Engineering",   80, 480,  1.35f,  55, "PC"},        // Glass fiber PC
    {"PC-ABS",      250, 280, 100, "Engineering",   60, 240,  1.12f,  50, "ABS_ASA"},   // PC/ABS blend

    // === Nylon/Polyamide (Enclosure required, dry storage) ===
    {"PA",          250, 280, 80,  "Engineering",   70, 480,  1.14f,  50, "PA"},        // Generic nylon
    {"PA6",         250, 280, 80,  "Engineering",   70, 480,  1.14f,  50, "PA"},
    {"PA12",        250, 280, 80,  "Engineering",   70, 480,  1.14f,  50, "PA"},
    {"PA66",        260, 290, 90,  "Engineering",   80, 480,  1.14f,  55, "PA"},        // Nylon 66
    {"PA-CF",       260, 290, 80,  "Engineering",   70, 480,  1.14f,  50, "PA"},        // Carbon fiber nylon
    {"PA-GF",       260, 290, 80,  "Engineering",   70, 480,  1.14f,  50, "PA"},        // Glass fiber nylon
    {"PA6-CF",      270, 300, 90,  "Engineering",   80, 480,  1.15f,  50, "PA"},        // Carbon fiber nylon 6
    {"PPA",         280, 320, 100, "Engineering",   80, 480,  1.18f,  60, "PA"},        // Polyphthalamide
    {"PPA-CF",      290, 320, 100, "Engineering",   80, 480,  1.17f,  60, "PA"},        // Carbon fiber polyphthalamide
    {"PPA-GF",      290, 320, 100, "Engineering",   80, 480,  1.25f,  60, "PA"},        // Glass fiber polyphthalamide

    // === Polyolefins (poor bed adhesion; each keeps its OWN compat group so
    //     endless spool never cross-swaps them with a chemically unrelated material) ===
    {"PP",          220, 250, 60,  "Engineering",   60, 240,  0.90f,  0,  "PP"},        // Polypropylene
    {"PP-CF",       225, 255, 60,  "Engineering",   60, 240,  0.98f,  0,  "PP"},        // Carbon fiber PP
    {"PP-GF",       225, 255, 60,  "Engineering",   60, 240,  1.05f,  0,  "PP"},        // Glass fiber PP
    {"PE",          220, 250, 60,  "Engineering",   0,  0,    0.95f,  0,  "PE"},        // Polyethylene - negligible moisture uptake, no drying

    // === Flexible Materials ===
    {"TPU",         210, 240, 50,  "Flexible",      55, 240,  1.21f,  0,  "TPU"},       // Shore 95A typical
    {"TPU-Soft",    200, 230, 50,  "Flexible",      55, 240,  1.21f,  0,  "TPU"},       // Shore 85A or softer
    {"TPE",         200, 230, 50,  "Flexible",      55, 240,  1.21f,  0,  "TPU"},
    {"TPU-95A",     210, 240, 50,  "Flexible",      55, 240,  1.21f,  0,  "TPU"},       // Shore 95A hardness
    {"TPU-85A",     200, 230, 50,  "Flexible",      55, 240,  1.19f,  0,  "TPU"},       // Shore 85A hardness (softer)
    // CoPE/EVA/SBS get their own compat groups: they are semi-flexible but do NOT
    // interchange with TPU, so endless spool must not treat them as swappable.
    {"CoPE",        190, 240, 55,  "Flexible",      55, 240,  1.29f,  0,  "CoPE"},      // Copolyester elastomer - range taken from the only shipped product, uncertain
    {"EVA",         190, 220, 50,  "Flexible",      0,  0,    0.95f,  0,  "EVA"},       // Ethylene-vinyl acetate - conservative range, uncertain; low Vicat so no drying
    {"SBS",         215, 250, 70,  "Flexible",      50, 240,  1.02f,  0,  "SBS"},       // Styrene-butadiene-styrene

    // === Support Materials ===
    {"PVA",         180, 210, 60,  "Support",       45, 240,  1.23f,  0,  "PLA"},       // Water-soluble
    {"HIPS",        230, 250, 100, "Support",       60, 240,  1.05f,  50, "ABS_ASA"},   // Limonene-soluble
    {"BVOH",        190, 220, 60,  "Support",       45, 240,  1.10f,  0,  "PLA"},       // Water-soluble (better than PVA)

    // === Specialty/Decorative ===
    {"Wood PLA",    190, 220, 60,  "Specialty",     45, 240,  1.24f,  0,  "PLA"},       // Wood fiber fill
    {"Marble PLA",  200, 220, 60,  "Specialty",     45, 240,  1.24f,  0,  "PLA"},       // Marble effect
    {"Metal PLA",   200, 230, 60,  "Specialty",     45, 240,  1.24f,  0,  "PLA"},       // Metal powder fill
    {"Glow PLA",    200, 230, 60,  "Specialty",     45, 240,  1.24f,  0,  "PLA"},       // Glow-in-the-dark
    {"Color-Change",200, 230, 60,  "Specialty",     45, 240,  1.24f,  0,  "PLA"},       // Temperature reactive
    // Foaming ("Aero"/LW) grades: the wide nozzle range IS the control knob - hotter
    // means more foaming. Density below is the UNFOAMED value; effective density
    // drops with foaming ratio, so length-from-weight math is approximate.
    {"PLA-AERO",    200, 260, 55,  "Specialty",     45, 240,  1.21f,  0,  "PLA"},       // Foaming/lightweight PLA (LW-PLA)
    {"ASA-AERO",    240, 280, 100, "Specialty",     60, 240,  0.99f,  50, "ABS_ASA"},   // Foaming/lightweight ASA

    // === Recycled Materials ===
    {"rPLA",        190, 220, 60,  "Recycled",      45, 240,  1.24f,  0,  "PLA"},       // Recycled PLA
    {"rPETG",       230, 260, 80,  "Recycled",      55, 360,  1.27f,  0,  "PETG"},      // Recycled PETG

    // === High-Temperature Industrial ===
    {"PEEK",        370, 420, 120, "High-Temp",     100, 720, 1.30f,  80, "HIGH_TEMP"}, // Requires all-metal hotend
    {"PEI",         340, 380, 120, "High-Temp",     100, 720, 1.27f,  80, "HIGH_TEMP"}, // ULTEM
    {"PSU",         340, 380, 120, "High-Temp",     100, 720, 1.24f,  80, "HIGH_TEMP"}, // Polysulfone
    {"PPSU",        350, 390, 140, "High-Temp",     100, 720, 1.29f,  80, "HIGH_TEMP"}, // Medical grade
    {"PPS",         320, 350, 120, "High-Temp",     100, 720, 1.35f,  80, "HIGH_TEMP"}, // Polyphenylene sulfide
    {"PPS-CF",      320, 350, 120, "High-Temp",     100, 720, 1.42f,  80, "HIGH_TEMP"}, // Carbon fiber PPS
};
// clang-format on

/// Number of materials in the database
inline constexpr size_t MATERIAL_COUNT = sizeof(MATERIALS) / sizeof(MATERIALS[0]);

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
inline constexpr MaterialAlias MATERIAL_ALIASES[] = {
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
inline constexpr size_t ALIAS_COUNT = sizeof(MATERIAL_ALIASES) / sizeof(MATERIAL_ALIASES[0]);

/**
 * @brief Resolve a material alias to its canonical name
 * @param name Material name or alias to resolve
 * @return Canonical name if alias found, original name otherwise
 */
inline std::string_view resolve_alias(std::string_view name) {
    std::string name_lower(name);
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

    for (const auto& alias : MATERIAL_ALIASES) {
        std::string alias_lower(alias.alias);
        std::transform(alias_lower.begin(), alias_lower.end(), alias_lower.begin(), ::tolower);

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
inline std::optional<MaterialInfo> find_material(std::string_view name) {
    // First resolve any alias
    std::string_view resolved = resolve_alias(name);

    for (const auto& mat : MATERIALS) {
        // Case-insensitive comparison
        std::string mat_lower(mat.name);
        std::string name_lower(resolved);
        std::transform(mat_lower.begin(), mat_lower.end(), mat_lower.begin(), ::tolower);
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        if (mat_lower == name_lower) {
            MaterialInfo result = mat;
            const auto* ovr = get_material_override(mat.name);
            if (ovr) {
                if (ovr->nozzle_min)
                    result.nozzle_min = *ovr->nozzle_min;
                if (ovr->nozzle_max)
                    result.nozzle_max = *ovr->nozzle_max;
                if (ovr->bed_temp)
                    result.bed_temp = *ovr->bed_temp;
                if (ovr->chamber_temp)
                    result.chamber_temp_c = *ovr->chamber_temp;
            }
            return result;
        }
    }
    return std::nullopt;
}

/**
 * @brief Get all materials in a category
 * @param category Category name (e.g., "Standard", "Engineering")
 * @return Vector of matching materials
 */
inline std::vector<MaterialInfo> get_materials_by_category(std::string_view category) {
    std::vector<MaterialInfo> result;
    for (const auto& mat : MATERIALS) {
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
    for (const auto& mat : MATERIALS) {
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
    std::vector<const char*> names;
    names.reserve(MATERIAL_COUNT);
    for (const auto& mat : MATERIALS) {
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
 * MATERIALS[] is the ONLY source of drying data in this codebase. This function
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

    for (const auto& mat : MATERIALS) {
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

/**
 * @brief Calculate filament length from weight
 * @param weight_g Weight in grams
 * @param density Material density in g/cm³
 * @param diameter_mm Filament diameter in mm (default 1.75)
 * @return Length in meters
 */
inline float weight_to_length_m(float weight_g, float density, float diameter_mm = 1.75f) {
    // Volume = mass / density (in cm³)
    float volume_cm3 = weight_g / density;

    // Cross-sectional area in cm² (diameter in mm -> radius in cm)
    float radius_cm = (diameter_mm / 2.0f) / 10.0f;
    float area_cm2 = static_cast<float>(M_PI) * radius_cm * radius_cm;

    // Length = volume / area (in cm, then convert to m)
    float length_cm = volume_cm3 / area_cm2;
    return length_cm / 100.0f;
}

/**
 * @brief Calculate filament weight in grams from length
 * @param length_mm Length in millimeters
 * @param density Material density in g/cm³
 * @param diameter_mm Filament diameter in mm (default 1.75)
 * @return Mass in grams, or 0 if density or length is not positive
 */
inline float length_to_weight_g(float length_mm, float density, float diameter_mm = 1.75f) {
    if (density <= 0.0f || length_mm <= 0.0f) {
        return 0.0f;
    }
    float radius_mm = diameter_mm / 2.0f;
    float area_mm2 = static_cast<float>(M_PI) * radius_mm * radius_mm;
    float volume_mm3 = length_mm * area_mm2;
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
 * This is the ONLY comfort data that is not derivable from MATERIALS[], because
 * there is no moisture-uptake field on MaterialInfo to derive it from. It is
 * keyed by compat_group rather than by material name on purpose: a group is a set
 * of chemically interchangeable materials, so one row covers every member and a
 * newly added MATERIALS row inherits humidity coverage for free instead of
 * silently falling off the AMS humidity indicator.
 *
 * The drying temperature and time that get_comfort_range() reports are NOT listed
 * here — they come from get_drying_presets_by_group(), which derives them from
 * MATERIALS[]. Adding dry_temp/dry_time columns to this table would recreate the
 * third drying source that this layout exists to eliminate.
 */
struct GroupHumidityRange {
    const char* group;
    float max_humidity_good; ///< Below this = green (safe)
    float max_humidity_warn; ///< Below this = yellow, above = red
};

// clang-format off
inline constexpr GroupHumidityRange GROUP_HUMIDITY_RANGES[] = {
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
 * level belongs in GROUP_HUMIDITY_RANGES above.
 */
// clang-format off
inline constexpr GroupHumidityRange MATERIAL_HUMIDITY_OVERRIDES[] = {
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
 * get_drying_presets_by_group(), which reads MATERIALS[]. There is no independent
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
    for (const auto& ovr : MATERIAL_HUMIDITY_OVERRIDES) {
        if (std::string_view(ovr.group) == std::string_view(mat->name)) {
            result.max_humidity_good = ovr.max_humidity_good;
            result.max_humidity_warn = ovr.max_humidity_warn;
            have_humidity = true;
            break;
        }
    }
    if (!have_humidity) {
        for (const auto& gr : GROUP_HUMIDITY_RANGES) {
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
 * (assets/filaments.json), not from MATERIALS[]. A type with no product is
 * therefore invisible in the UI. For most rows that is a bug; for these it is the
 * design — they exist so that a material string arriving from Orca, a printer's
 * firmware, or Spoolman resolves to sane temperatures, and were never meant to be
 * user-selectable.
 *
 * This list is the machine-readable form of that intent. An invariant test
 * asserts the shipped catalog covers exactly MATERIALS minus this list, in both
 * directions — so a new type with no product fails the build until someone
 * decides which bucket it belongs in, and an entry here that DOES gain a product
 * must be removed rather than rotting.
 *
 * Making one of these selectable is a one-line addition to
 * scripts/fixtures/cfs_seed.json plus removing it here.
 */
// clang-format off
inline constexpr const char* RESOLUTION_ONLY_MATERIALS[] = {
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
inline constexpr size_t RESOLUTION_ONLY_COUNT =
    sizeof(RESOLUTION_ONLY_MATERIALS) / sizeof(RESOLUTION_ONLY_MATERIALS[0]);

/**
 * @brief Is this material deliberately absent from the product catalog?
 * @param name Material name (exact MATERIALS[] spelling)
 */
inline bool is_resolution_only(std::string_view name) {
    for (const auto* m : RESOLUTION_ONLY_MATERIALS) {
        if (std::string_view(m) == name) {
            return true;
        }
    }
    return false;
}

} // namespace filament
