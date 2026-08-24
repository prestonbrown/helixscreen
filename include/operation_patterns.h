// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/**
 * @file operation_patterns.h
 * @brief Shared pattern definitions for detecting pre-print operations
 *
 * This file consolidates operation detection patterns used by both:
 * - PrintStartAnalyzer (scans PRINT_START macro in printer.cfg)
 * - GCodeOpsDetector (scans G-code file content)
 *
 * Having a single source of truth ensures consistency and makes it easy
 * to add new patterns that work across both analyzers.
 */

/**
 * @brief Categories of pre-print operations
 *
 * These represent the semantic meaning of operations, not the specific
 * command names (which vary by printer/config).
 */
enum class OperationCategory {
    BED_MESH,     ///< Bed mesh calibration (BED_MESH_CALIBRATE, G29)
    QGL,          ///< Quad gantry leveling (QUAD_GANTRY_LEVEL)
    Z_TILT,       ///< Z-tilt adjustment (Z_TILT_ADJUST)
    BED_LEVEL,    ///< Physical bed/gantry leveling (parent of QGL and Z_TILT)
    NOZZLE_CLEAN, ///< Nozzle cleaning/wiping (CLEAN_NOZZLE, BRUSH_NOZZLE)
    PURGE_LINE,   ///< Purge/prime line (PURGE_LINE, PRIME_LINE)
    HOMING,       ///< Homing axes (G28)
    CHAMBER_SOAK, ///< Chamber heat soak (HEAT_SOAK)
    SKEW_CORRECT, ///< Skew correction (SKEW_PROFILE, SET_SKEW)
    START_PRINT,  ///< The print start macro itself (PRINT_START, START_PRINT)
    UNKNOWN,      ///< Unrecognized operation
};

/**
 * @brief A single operation keyword pattern
 */
struct OperationKeyword {
    const char* keyword;        ///< Command/macro name to match (e.g., "BED_MESH_CALIBRATE")
    OperationCategory category; ///< Semantic category
    const char* skip_param;     ///< Suggested skip parameter name (e.g., "SKIP_BED_MESH")
    bool exact_match;           ///< True for G-codes (exact), false for macros (substring)
};

/**
 * @brief Master list of operation keywords
 *
 * This is the single source of truth for all operation detection.
 * Both PrintStartAnalyzer and GCodeOpsDetector use this list.
 */
// clang-format off
inline const OperationKeyword OPERATION_KEYWORDS[] = {
    // === Bed Mesh ===
    // Matches BED_MESH_CALIBRATE, BED_MESH_PROFILE, AUTO_BED_LEVEL, etc.
    {"BED_MESH",             OperationCategory::BED_MESH, "SKIP_BED_MESH",     false},
    {"AUTO_BED_LEVEL",       OperationCategory::BED_MESH, "SKIP_BED_MESH",     false},
    {"G29",                  OperationCategory::BED_MESH, "SKIP_BED_MESH",     true},

    // === Quad Gantry Level ===
    {"QUAD_GANTRY_LEVEL",    OperationCategory::QGL,          "SKIP_QGL",          false},
    {"QGL",                  OperationCategory::QGL,          "SKIP_QGL",          false},

    // === Z Tilt ===
    {"Z_TILT_ADJUST",        OperationCategory::Z_TILT,       "SKIP_Z_TILT",       false},
    {"Z_TILT",               OperationCategory::Z_TILT,       "SKIP_Z_TILT",       false},

    // === Nozzle Cleaning ===
    // Substring matching: _CLEAN_NOZZLE matches CLEAN_NOZZLE, etc.
    {"CLEAN_NOZZLE",         OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},
    {"NOZZLE_CLEAN",         OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},
    {"NOZZLE_WIPE",          OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},
    {"WIPE_NOZZLE",          OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},
    {"BRUSH_NOZZLE",         OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},
    {"NOZZLE_BRUSH",         OperationCategory::NOZZLE_CLEAN, "SKIP_NOZZLE_CLEAN", false},

    // === Purge/Prime Line ===
    // Substring matching: _PRIME_NOZZLE matches PRIME_NOZZLE, etc.
    {"PURGE",                OperationCategory::PURGE_LINE,   "SKIP_PURGE",        false},
    {"PRIME",                OperationCategory::PURGE_LINE,   "SKIP_PURGE",        false},
    {"INTRO_LINE",           OperationCategory::PURGE_LINE,   "SKIP_PURGE",        false},

    // === Homing ===
    {"G28",                  OperationCategory::HOMING,       "SKIP_HOMING",       true},
    {"SAFE_HOME",            OperationCategory::HOMING,       "SKIP_HOMING",       false},

    // === Chamber Soak ===
    {"HEAT_SOAK",            OperationCategory::CHAMBER_SOAK, "SKIP_SOAK",         false},
    {"CHAMBER_SOAK",         OperationCategory::CHAMBER_SOAK, "SKIP_SOAK",         false},
    {"SET_HEATER_TEMPERATURE HEATER=chamber", OperationCategory::CHAMBER_SOAK, "SKIP_SOAK", false},

    // === Skew Correction ===
    {"SKEW_PROFILE",         OperationCategory::SKEW_CORRECT, "SKIP_SKEW",         false},
    {"SET_SKEW",             OperationCategory::SKEW_CORRECT, "SKIP_SKEW",         false},
    {"SKEW",                 OperationCategory::SKEW_CORRECT, "SKIP_SKEW",         false},
};
// clang-format on

inline constexpr size_t OPERATION_KEYWORDS_COUNT =
    sizeof(OPERATION_KEYWORDS) / sizeof(OPERATION_KEYWORDS[0]);

/**
 * @brief Skip parameter variations for detecting controllability
 *
 * When scanning a macro, we look for these parameter names in {% if %} blocks
 * to determine if an operation can be skipped.
 */
// clang-format off
inline const std::vector<std::string> SKIP_PARAM_VARIATIONS[] = {
    // Index 0: BED_MESH
    {"SKIP_BED_MESH", "SKIP_MESH", "SKIP_BED_LEVELING", "NO_BED_MESH", "SKIP_LEVEL"},
    // Index 1: QGL
    {"SKIP_QGL", "SKIP_GANTRY", "NO_QGL", "SKIP_QUAD_GANTRY_LEVEL"},
    // Index 2: Z_TILT
    {"SKIP_Z_TILT", "SKIP_TILT", "NO_Z_TILT", "SKIP_Z_TILT_ADJUST"},
    // Index 3: BED_LEVEL (parent of QGL and Z_TILT)
    {"SKIP_BED_LEVEL", "SKIP_LEVELING", "SKIP_LEVEL", "NO_BED_LEVEL"},
    // Index 4: NOZZLE_CLEAN
    {"SKIP_NOZZLE_CLEAN", "SKIP_CLEAN", "NO_CLEAN"},
    // Index 5: PURGE_LINE
    {"SKIP_PURGE", "SKIP_PRIME", "NO_PURGE", "NO_PRIME", "DISABLE_PRIMING"},
    // Index 6: HOMING
    {"SKIP_HOMING", "SKIP_HOME", "NO_HOME"},
    // Index 7: CHAMBER_SOAK
    {"SKIP_SOAK", "SKIP_HEAT_SOAK", "NO_SOAK", "SKIP_CHAMBER"},
    // Index 8: SKEW_CORRECT
    {"SKIP_SKEW", "NO_SKEW", "DISABLE_SKEW", "DISABLE_SKEW_CORRECT"},
};
// clang-format on

/**
 * @brief Perform (opt-in) parameter variations for detecting controllability
 *
 * When scanning a macro, we look for these parameter names in {% if %} blocks
 * to determine if an operation can be explicitly enabled.
 *
 * These use opt-in semantics: param=1 means "do it", param=0 or omitted means "skip it".
 */
// clang-format off
inline const std::vector<std::string> PERFORM_PARAM_VARIATIONS[] = {
    // Index 0: BED_MESH
    {"PERFORM_BED_MESH", "DO_BED_MESH", "ENABLE_BED_MESH", "FORCE_BED_MESH", "FORCE_LEVELING"},
    // Index 1: QGL
    {"PERFORM_QGL", "DO_QGL", "ENABLE_QGL", "FORCE_QGL"},
    // Index 2: Z_TILT
    {"PERFORM_Z_TILT", "DO_Z_TILT", "ENABLE_Z_TILT", "FORCE_Z_TILT"},
    // Index 3: BED_LEVEL (parent of QGL and Z_TILT)
    {"PERFORM_BED_LEVEL", "DO_BED_LEVEL", "ENABLE_BED_LEVEL", "FORCE_BED_LEVEL"},
    // Index 4: NOZZLE_CLEAN
    {"PERFORM_NOZZLE_CLEAN", "DO_NOZZLE_CLEAN", "ENABLE_NOZZLE_CLEAN", "FORCE_NOZZLE_CLEAN"},
    // Index 5: PURGE_LINE
    {"PERFORM_PURGE", "DO_PURGE", "ENABLE_PURGE", "FORCE_PURGE",
     "PERFORM_PRIME", "DO_PRIME", "ENABLE_PRIME", "FORCE_PRIME"},
    // Index 6: HOMING
    {"PERFORM_HOMING", "DO_HOMING", "ENABLE_HOMING", "FORCE_HOMING"},
    // Index 7: CHAMBER_SOAK
    {"PERFORM_SOAK", "DO_SOAK", "ENABLE_SOAK", "FORCE_SOAK",
     "PERFORM_HEAT_SOAK", "DO_HEAT_SOAK", "ENABLE_HEAT_SOAK", "FORCE_HEAT_SOAK"},
    // Index 8: SKEW_CORRECT
    {"PERFORM_SKEW", "DO_SKEW", "ENABLE_SKEW", "FORCE_SKEW"},
};
// clang-format on

/**
 * @brief Slicer-style short parameter names for G-code detection
 *
 * These are short parameter names that slicers may pass to START_PRINT macros.
 * They are separate from SKIP_X and PERFORM_X variations because they may collide
 * with Jinja2 printer object names (e.g., "bed_mesh" in printer['bed_mesh']).
 *
 * Used by GCodeOpsDetector for parameter parsing, NOT by PrintStartAnalyzer.
 */
// clang-format off
inline const std::vector<std::string> SLICER_PARAM_VARIATIONS[] = {
    // Index 0: BED_MESH
    {"MESH", "BED_MESH", "DO_BED_MESH"},
    // Index 1: QGL
    {"QGL", "GANTRY_LEVEL", "DO_QGL"},
    // Index 2: Z_TILT
    {"Z_TILT", "TILT_ADJUST"},
    // Index 3: BED_LEVEL (parent of QGL and Z_TILT)
    {},
    // Index 4: NOZZLE_CLEAN
    {"NOZZLE_CLEAN", "CLEAN_NOZZLE", "WIPE"},
    // Index 5: PURGE_LINE
    {"PURGE", "PRIME"},
    // Index 6: HOMING
    {},
    // Index 7: CHAMBER_SOAK
    {"CHAMBER_SOAK", "SOAK_TIME"},
    // Index 8: SKEW_CORRECT
    {},
};
// clang-format on

/**
 * @brief Get slicer-style short parameter variations for a category
 *
 * @param cat The operation category
 * @return Vector of short parameter name variations, or empty if none
 */
inline const std::vector<std::string>& get_slicer_param_variations(OperationCategory cat) {
    static const std::vector<std::string> empty;
    size_t idx = static_cast<size_t>(cat);
    constexpr size_t count = sizeof(SLICER_PARAM_VARIATIONS) / sizeof(SLICER_PARAM_VARIATIONS[0]);
    if (idx < count) {
        return SLICER_PARAM_VARIATIONS[idx];
    }
    return empty;
}

/**
 * @brief Get human-readable name for a category
 */
inline const char* category_name(OperationCategory cat) {
    switch (cat) {
    case OperationCategory::BED_MESH:
        return "Bed mesh";
    case OperationCategory::QGL:
        return "Quad gantry leveling";
    case OperationCategory::Z_TILT:
        return "Z-tilt adjustment";
    case OperationCategory::BED_LEVEL:
        return "Bed leveling";
    case OperationCategory::NOZZLE_CLEAN:
        return "Nozzle cleaning";
    case OperationCategory::PURGE_LINE:
        return "Purge line";
    case OperationCategory::HOMING:
        return "Homing";
    case OperationCategory::CHAMBER_SOAK:
        return "Chamber heat soak";
    case OperationCategory::SKEW_CORRECT:
        return "Skew correction";
    case OperationCategory::START_PRINT:
        return "Start print";
    case OperationCategory::UNKNOWN:
    default:
        return "Unknown";
    }
}

/**
 * @brief Get machine-readable key for a category (for deduplication)
 */
inline const char* category_key(OperationCategory cat) {
    switch (cat) {
    case OperationCategory::BED_MESH:
        return "bed_mesh";
    case OperationCategory::QGL:
        return "qgl";
    case OperationCategory::Z_TILT:
        return "z_tilt";
    case OperationCategory::BED_LEVEL:
        return "bed_level";
    case OperationCategory::NOZZLE_CLEAN:
        return "nozzle_clean";
    case OperationCategory::PURGE_LINE:
        return "purge_line";
    case OperationCategory::HOMING:
        return "homing";
    case OperationCategory::CHAMBER_SOAK:
        return "chamber_soak";
    case OperationCategory::SKEW_CORRECT:
        return "skew_correct";
    case OperationCategory::START_PRINT:
        return "start_print";
    case OperationCategory::UNKNOWN:
    default:
        return "unknown";
    }
}

/**
 * @brief Get skip parameter variations for a category
 *
 * @param cat The operation category
 * @return Vector of skip parameter name variations, or empty if none
 */
inline const std::vector<std::string>& get_skip_variations(OperationCategory cat) {
    static const std::vector<std::string> empty;
    size_t idx = static_cast<size_t>(cat);
    constexpr size_t count = sizeof(SKIP_PARAM_VARIATIONS) / sizeof(SKIP_PARAM_VARIATIONS[0]);
    if (idx < count) {
        return SKIP_PARAM_VARIATIONS[idx];
    }
    return empty;
}

/**
 * @brief Check if a category is a physical bed leveling operation
 *
 * Returns true for BED_LEVEL, QGL, and Z_TILT categories.
 * Useful for unified handling where SKIP_BED_LEVEL should affect all physical leveling.
 */
inline bool is_bed_level_category(OperationCategory cat) {
    return cat == OperationCategory::BED_LEVEL || cat == OperationCategory::QGL ||
           cat == OperationCategory::Z_TILT;
}

/**
 * @brief Get all skip parameter variations that could disable this category
 *
 * For QGL and Z_TILT, includes both specific variations (SKIP_QGL, SKIP_Z_TILT)
 * AND the unified BED_LEVEL variations. This allows SKIP_BED_LEVEL to work
 * as a catch-all for physical bed leveling operations.
 */
inline std::vector<std::string> get_all_skip_variations(OperationCategory cat) {
    std::vector<std::string> result;
    const auto& own_vars = get_skip_variations(cat);
    result.insert(result.end(), own_vars.begin(), own_vars.end());

    // For QGL and Z_TILT, also accept BED_LEVEL variations as unified skip
    if (cat == OperationCategory::QGL || cat == OperationCategory::Z_TILT) {
        const auto& bed_level_vars = get_skip_variations(OperationCategory::BED_LEVEL);
        result.insert(result.end(), bed_level_vars.begin(), bed_level_vars.end());
    }
    return result;
}

/**
 * @brief Get perform (opt-in) parameter variations for a category
 *
 * @param cat The operation category
 * @return Vector of perform parameter name variations, or empty if none
 */
inline const std::vector<std::string>& get_perform_variations(OperationCategory cat) {
    static const std::vector<std::string> empty;
    size_t idx = static_cast<size_t>(cat);
    constexpr size_t count = sizeof(PERFORM_PARAM_VARIATIONS) / sizeof(PERFORM_PARAM_VARIATIONS[0]);
    if (idx < count) {
        return PERFORM_PARAM_VARIATIONS[idx];
    }
    return empty;
}

/**
 * @brief Get all perform (opt-in) parameter variations that could enable this category
 *
 * Returns variations like PERFORM_BED_MESH, DO_BED_MESH, FORCE_BED_MESH, etc.
 * These use opt-in semantics: param=1 means "do it", param=0 means "skip it".
 *
 * For QGL and Z_TILT, includes both specific variations (PERFORM_QGL, PERFORM_Z_TILT)
 * AND the unified BED_LEVEL variations. This allows PERFORM_BED_LEVEL to work
 * as a catch-all for physical bed leveling operations.
 *
 * @param cat The operation category
 * @return Vector of perform parameter name variations
 */
inline std::vector<std::string> get_all_perform_variations(OperationCategory cat) {
    std::vector<std::string> result;
    const auto& own_vars = get_perform_variations(cat);
    result.insert(result.end(), own_vars.begin(), own_vars.end());

    // For QGL and Z_TILT, also accept BED_LEVEL variations as unified perform
    if (cat == OperationCategory::QGL || cat == OperationCategory::Z_TILT) {
        const auto& bed_level_vars = get_perform_variations(OperationCategory::BED_LEVEL);
        result.insert(result.end(), bed_level_vars.begin(), bed_level_vars.end());
    }
    return result;
}

// ============================================================================
// String Utilities (LT3)
// ============================================================================

/**
 * @brief Convert string to uppercase
 */
inline std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

/**
 * @brief Convert string to lowercase
 */
inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

/**
 * @brief Case-insensitive substring search (pure ASCII).
 *
 * Folds only A-Z/a-z, so it is locale-independent and leaves every other byte
 * (including high-bit-set and UTF-8 continuation bytes) untouched. That matches
 * the earlier ::toupper-based fold under the "C" locale this process keeps
 * (only LC_TIME is ever setlocale'd, so LC_CTYPE stays "C"), without the two
 * per-call std::string allocations or the ::toupper-on-negative-char UB.
 *
 * Empty needle returns false rather than the std::string::find "found at 0"
 * convention: every call site searches for a concrete token, and an empty
 * needle matching everything is a foot-gun the former local copies guarded
 * against explicitly. Verified equivalent over all real needles across every
 * byte 0x01-0xFF plus UTF-8 in de/fr/ru/ja.
 *
 * Accepts std::string_view, so std::string and string literals convert without
 * allocating.
 */
inline bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char hc = haystack[i + j];
            char nc = needle[j];
            if (hc >= 'A' && hc <= 'Z') {
                hc = static_cast<char>(hc + 32);
            }
            if (nc >= 'A' && nc <= 'Z') {
                nc = static_cast<char>(nc + 32);
            }
            if (hc != nc) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Case-insensitive string equality
 */
inline bool equals_ci(const std::string& a, const std::string& b) {
    return to_upper(a) == to_upper(b);
}

/**
 * @brief Find keyword entry by pattern string (substring match, case-insensitive)
 *
 * Uses substring matching so `_PRIME_NOZZLE` matches `PRIME_NOZZLE`,
 * `AUTO_BED_LEVEL` matches `BED_LEVEL`, etc. This catches custom macro
 * prefixes/suffixes automatically.
 *
 * G-codes use exact matching to avoid false positives (G28 inside FOO_G28_BAR).
 * All matching is case-insensitive.
 *
 * @param pattern Command to search for
 * @return Pointer to keyword entry, or nullptr if not found
 */
inline const OperationKeyword* find_keyword(const std::string& pattern) {
    // Always uppercase for case-insensitive comparison
    std::string pat = to_upper(pattern);

    for (size_t i = 0; i < OPERATION_KEYWORDS_COUNT; ++i) {
        std::string keyword = to_upper(OPERATION_KEYWORDS[i].keyword);

        if (OPERATION_KEYWORDS[i].exact_match) {
            // G-codes: exact match only (avoid G28 matching inside FOO_G28_BAR)
            if (pat == keyword) {
                return &OPERATION_KEYWORDS[i];
            }
        } else {
            // Macros: substring match (catches _PRIME_NOZZLE, AUTO_BED_LEVEL, etc.)
            if (pat.find(keyword) != std::string::npos) {
                return &OPERATION_KEYWORDS[i];
            }
        }
    }
    return nullptr;
}

// ============================================================================
// Parameter Matching Infrastructure (LT3)
// ============================================================================

/**
 * @brief Semantic type for parameter interpretation
 */
enum class ParameterSemantic {
    OPT_OUT, ///< SKIP_*: param=1 means skip, param=0 means do
    OPT_IN   ///< PERFORM_*/DO_*/FORCE_*: param=1 means do, param=0 means skip
};

/**
 * @brief Result of matching a parameter name to an operation category
 */
struct ParamMatchResult {
    OperationCategory category = OperationCategory::UNKNOWN;
    ParameterSemantic semantic = ParameterSemantic::OPT_OUT;
    std::string matched_param;
};

/**
 * @brief Match a parameter name to its operation category
 *
 * Searches through all skip and perform variations for all categories
 * to find a match. Case-insensitive matching.
 *
 * @param param_name The parameter name to match (e.g., "SKIP_BED_MESH", "FORCE_LEVELING")
 * @param include_slicer_params If true, also checks slicer-style short params (MESH, QGL, etc.)
 * @return ParamMatchResult if found, nullopt otherwise
 */
inline std::optional<ParamMatchResult>
match_parameter_to_category(const std::string& param_name, bool include_slicer_params = false) {
    // Check all categories
    for (size_t i = 0; i < static_cast<size_t>(OperationCategory::UNKNOWN); ++i) {
        auto cat = static_cast<OperationCategory>(i);

        // Check perform variations (OPT_IN) first - they're more specific
        for (const auto& var : get_all_perform_variations(cat)) {
            if (equals_ci(param_name, var)) {
                return ParamMatchResult{cat, ParameterSemantic::OPT_IN, var};
            }
        }

        // Check skip variations (OPT_OUT)
        for (const auto& var : get_all_skip_variations(cat)) {
            if (equals_ci(param_name, var)) {
                return ParamMatchResult{cat, ParameterSemantic::OPT_OUT, var};
            }
        }

        // Check slicer-style short params (OPT_IN semantics: value=1 means do it)
        if (include_slicer_params) {
            for (const auto& var : get_slicer_param_variations(cat)) {
                if (equals_ci(param_name, var)) {
                    return ParamMatchResult{cat, ParameterSemantic::OPT_IN, var};
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace helix
