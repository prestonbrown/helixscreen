// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "operation_patterns.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// String Utility Tests (Phase 1)
// ============================================================================

TEST_CASE("operation_patterns - to_upper utility", "[operation_patterns][string]") {
    SECTION("Converts lowercase to uppercase") {
        REQUIRE(to_upper("hello") == "HELLO");
        REQUIRE(to_upper("world") == "WORLD");
    }

    SECTION("Preserves uppercase") {
        REQUIRE(to_upper("HELLO") == "HELLO");
    }

    SECTION("Handles mixed case") {
        REQUIRE(to_upper("HeLLo WoRLd") == "HELLO WORLD");
    }

    SECTION("Handles empty string") {
        REQUIRE(to_upper("") == "");
    }

    SECTION("Preserves non-alphabetic characters") {
        REQUIRE(to_upper("123_test!@#") == "123_TEST!@#");
    }
}

TEST_CASE("operation_patterns - to_lower utility", "[operation_patterns][string]") {
    SECTION("Converts uppercase to lowercase") {
        REQUIRE(to_lower("HELLO") == "hello");
        REQUIRE(to_lower("WORLD") == "world");
    }

    SECTION("Preserves lowercase") {
        REQUIRE(to_lower("hello") == "hello");
    }

    SECTION("Handles mixed case") {
        REQUIRE(to_lower("HeLLo WoRLd") == "hello world");
    }

    SECTION("Handles empty string") {
        REQUIRE(to_lower("") == "");
    }
}

TEST_CASE("operation_patterns - contains_ci utility", "[operation_patterns][string]") {
    SECTION("Finds exact match") {
        REQUIRE(contains_ci("BED_MESH_CALIBRATE", "BED_MESH"));
    }

    SECTION("Case insensitive - finds lowercase in uppercase") {
        REQUIRE(contains_ci("BED_MESH_CALIBRATE", "bed_mesh"));
    }

    SECTION("Case insensitive - finds uppercase in lowercase") {
        REQUIRE(contains_ci("bed_mesh_calibrate", "BED_MESH"));
    }

    SECTION("Case insensitive - mixed case") {
        REQUIRE(contains_ci("BeD_MeSh_CaLiBrAtE", "bed_MESH"));
    }

    SECTION("Returns false when not found") {
        REQUIRE_FALSE(contains_ci("BED_MESH_CALIBRATE", "QGL"));
    }

    SECTION("Empty needle does not match (guarded foot-gun)") {
        REQUIRE_FALSE(contains_ci("anything", ""));
    }

    SECTION("Empty haystack doesn't match non-empty needle") {
        REQUIRE_FALSE(contains_ci("", "test"));
    }

    SECTION("Empty needle does not match an empty haystack either") {
        // The empty-needle guard is checked before the size comparison, so this
        // stays false rather than degenerating into std::string::find's
        // "found at 0" convention.
        REQUIRE_FALSE(contains_ci("", ""));
    }

    SECTION("Needle longer than haystack is rejected") {
        REQUIRE_FALSE(contains_ci("BED", "BED_MESH"));
    }

    SECTION("Matches at both ends of the haystack") {
        REQUIRE(contains_ci("bed_mesh_calibrate", "BED"));       // position 0
        REQUIRE(contains_ci("bed_mesh_calibrate", "CALIBRATE")); // final byte
        REQUIRE(contains_ci("G28", "g28"));                      // whole string
    }

    SECTION("Searches by length, not to the first NUL") {
        // std::string_view carries its own size; an embedded NUL must not end
        // the scan the way it would for a C string.
        const std::string_view haystack("BED\0MESH", 8);
        REQUIRE(contains_ci(haystack, std::string_view("\0mesh", 5)));
        REQUIRE_FALSE(contains_ci(haystack, std::string_view("\0qgl", 4)));
    }
}

// ============================================================================
// contains_ci fold characterization (229432de3)
//
// contains_ci folds ONLY A-Z/a-z, by arithmetic, on raw bytes. It replaced a
// std::toupper fold that happened to behave identically because this process
// never setlocale()s LC_CTYPE (only LC_TIME — src/ui/locale_formats.cpp:181), so
// LC_CTYPE stays "C". Every other assertion in this file is 7-bit ASCII, which
// leaves that equivalence — and the fold's behaviour on the UTF-8 the AMS
// backends actually feed it — unpinned.
//
// These cases pin the contract for bytes >= 0x80 so a future change to the fold
// (a locale-aware one, an unsigned-char one, a Unicode-aware one) cannot quietly
// alter which macros and filament names match.
// ============================================================================

TEST_CASE("operation_patterns - contains_ci fold is ASCII-only over real UTF-8",
          "[operation_patterns][string][utf8]") {
    SECTION("High bytes match byte-for-byte and are passed through unfolded") {
        // "Düse" — the u-umlaut is C3 BC in UTF-8; the surrounding ASCII still folds.
        REQUIRE(contains_ci("Düse wechseln", "düse"));
        REQUIRE(contains_ci("AMS Löschen", "ms löschen"));
        REQUIRE(contains_ci("Загрузка филамента", "филамента"));
        REQUIRE(contains_ci("フィラメント交換", "交換"));
    }

    SECTION("No case folding happens above 0x7F") {
        // U+00DC vs U+00FC differ in their second UTF-8 byte (9C vs BC). A
        // Unicode-aware fold would call these equal; this one must not.
        REQUIRE_FALSE(contains_ci("DÜSE", "düse"));
        REQUIRE(contains_ci("DÜSE", "DÜSE"));

        // Cyrillic upper/lower likewise stay distinct.
        REQUIRE_FALSE(contains_ci("ФИЛАМЕНТ", "филамент"));
        REQUIRE(contains_ci("ФИЛАМЕНТ", "ФИЛАМЕНТ"));
    }

    SECTION("Raw high bytes are compared, never folded") {
        // 0xC0/0xE0 are A-with-grave and a-with-grave in Latin-1, i.e. exactly
        // 0x20 apart the way 'A'/'a' are. The fold must not reach them —
        // signed char makes them negative, unsigned char makes them > 'Z', so
        // this holds on x86 and on ARM alike.
        REQUIRE_FALSE(contains_ci("\xC0", "\xE0"));
        REQUIRE(contains_ci("\xC0", "\xC0"));
        REQUIRE(contains_ci("PLA\xFF_DRY", "\xFF_dry"));
    }

    SECTION("The A-Z window is exact at both edges") {
        // '@' is 'A'-1 and '[' is 'Z'+1; folding either would make these match
        // their +0x20 partners '`' and '{'.
        REQUIRE_FALSE(contains_ci("@", "`"));
        REQUIRE_FALSE(contains_ci("[", "{"));
        // 'A' and 'Z' themselves are inside the window.
        REQUIRE(contains_ci("@AZ[", "@az["));
    }

    SECTION("Real macro names with accented parameters still match their ASCII token") {
        REQUIRE(contains_ci("SET_FILAMENT_TYPE MATERIAL=Rot-Grün", "set_filament_type"));
        REQUIRE(contains_ci("_UNLOAD_FILAMENT_ÜBERGABE", "unload_filament"));
        REQUIRE_FALSE(contains_ci("SET_FILAMENT_TYPE MATERIAL=Rot-Grün", "grun"));
    }
}

TEST_CASE("operation_patterns - equals_ci utility", "[operation_patterns][string]") {
    SECTION("Exact match") {
        REQUIRE(equals_ci("SKIP_BED_MESH", "SKIP_BED_MESH"));
    }

    SECTION("Case insensitive match - different cases") {
        REQUIRE(equals_ci("skip_bed_mesh", "SKIP_BED_MESH"));
        REQUIRE(equals_ci("SKIP_BED_MESH", "skip_bed_mesh"));
        REQUIRE(equals_ci("Skip_Bed_Mesh", "SKIP_BED_MESH"));
    }

    SECTION("Returns false for different strings") {
        REQUIRE_FALSE(equals_ci("SKIP_BED_MESH", "SKIP_QGL"));
    }

    SECTION("Returns false for substrings") {
        REQUIRE_FALSE(equals_ci("SKIP_BED_MESH", "SKIP_BED"));
        REQUIRE_FALSE(equals_ci("SKIP_BED", "SKIP_BED_MESH"));
    }

    SECTION("Empty strings equal") {
        REQUIRE(equals_ci("", ""));
    }
}

// ============================================================================
// Parameter Matching Tests (Phase 3)
// ============================================================================

TEST_CASE("operation_patterns - match_parameter_to_category", "[operation_patterns][param]") {
    SECTION("Matches PERFORM variations (OPT_IN semantics)") {
        // BED_MESH perform variations
        auto result = match_parameter_to_category("PERFORM_BED_MESH");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);
        REQUIRE(result->semantic == ParameterSemantic::OPT_IN);

        result = match_parameter_to_category("DO_BED_MESH");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);
        REQUIRE(result->semantic == ParameterSemantic::OPT_IN);

        result = match_parameter_to_category("FORCE_LEVELING");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);
        REQUIRE(result->semantic == ParameterSemantic::OPT_IN);
    }

    SECTION("Matches SKIP variations (OPT_OUT semantics)") {
        auto result = match_parameter_to_category("SKIP_BED_MESH");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);
        REQUIRE(result->semantic == ParameterSemantic::OPT_OUT);

        result = match_parameter_to_category("SKIP_QGL");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::QGL);
        REQUIRE(result->semantic == ParameterSemantic::OPT_OUT);
    }

    SECTION("Case insensitive matching") {
        auto result = match_parameter_to_category("skip_bed_mesh");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);

        result = match_parameter_to_category("SKIP_BED_MESH");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);

        result = match_parameter_to_category("Skip_Bed_Mesh");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::BED_MESH);
    }

    SECTION("Returns nullopt for unknown parameters") {
        auto result = match_parameter_to_category("UNKNOWN_PARAM");
        REQUIRE_FALSE(result.has_value());

        result = match_parameter_to_category("BED_TEMP");
        REQUIRE_FALSE(result.has_value());

        result = match_parameter_to_category("EXTRUDER_TEMP");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Matches QGL variations") {
        auto result = match_parameter_to_category("SKIP_QGL");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::QGL);

        result = match_parameter_to_category("PERFORM_QGL");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::QGL);
        REQUIRE(result->semantic == ParameterSemantic::OPT_IN);
    }

    SECTION("Matches Z_TILT variations") {
        auto result = match_parameter_to_category("SKIP_Z_TILT");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::Z_TILT);

        result = match_parameter_to_category("PERFORM_Z_TILT");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::Z_TILT);
    }

    SECTION("Matches NOZZLE_CLEAN variations") {
        auto result = match_parameter_to_category("SKIP_NOZZLE_CLEAN");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::NOZZLE_CLEAN);

        result = match_parameter_to_category("CLEAN_NOZZLE");
        // Note: CLEAN_NOZZLE might be a perform variation
        // Test actual behavior once implemented
    }

    SECTION("Matches PURGE_LINE variations") {
        auto result = match_parameter_to_category("SKIP_PURGE");
        REQUIRE(result.has_value());
        REQUIRE(result->category == OperationCategory::PURGE_LINE);

        result = match_parameter_to_category("DISABLE_PRIMING");
        // This should match PURGE_LINE with OPT_OUT
    }

    SECTION("Matches unified BED_LEVEL variations") {
        // SKIP_BED_LEVEL should match both QGL and Z_TILT
        // This tests the unified handling
        auto result = match_parameter_to_category("SKIP_BED_LEVEL");
        REQUIRE(result.has_value());
        // Should match one of the bed leveling categories
        bool is_bed_level = (result->category == OperationCategory::QGL ||
                             result->category == OperationCategory::Z_TILT ||
                             result->category == OperationCategory::BED_LEVEL);
        REQUIRE(is_bed_level);
    }
}

// ============================================================================
// Existing Helper Function Tests
// ============================================================================

TEST_CASE("operation_patterns - category_name consistency", "[operation_patterns]") {
    SECTION("All categories have human-readable names") {
        REQUIRE(category_name(OperationCategory::BED_MESH) == "Bed mesh");
        REQUIRE(category_name(OperationCategory::QGL) == "Quad gantry leveling");
        REQUIRE(category_name(OperationCategory::Z_TILT) == "Z-tilt adjustment");
        REQUIRE(category_name(OperationCategory::NOZZLE_CLEAN) == "Nozzle cleaning");
        REQUIRE(category_name(OperationCategory::PURGE_LINE) == "Purge line");
        REQUIRE(category_name(OperationCategory::HOMING) == "Homing");
        REQUIRE(category_name(OperationCategory::CHAMBER_SOAK) == "Chamber heat soak");
        REQUIRE(category_name(OperationCategory::SKEW_CORRECT) == "Skew correction");
    }

    SECTION("Unknown category returns fallback") {
        REQUIRE(category_name(OperationCategory::UNKNOWN) == "Unknown");
    }
}
