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
        REQUIRE(helix::text_io::to_upper("hello") == "HELLO");
        REQUIRE(helix::text_io::to_upper("world") == "WORLD");
    }

    SECTION("Preserves uppercase") {
        REQUIRE(helix::text_io::to_upper("HELLO") == "HELLO");
    }

    SECTION("Handles mixed case") {
        REQUIRE(helix::text_io::to_upper("HeLLo WoRLd") == "HELLO WORLD");
    }

    SECTION("Handles empty string") {
        REQUIRE(helix::text_io::to_upper("") == "");
    }

    SECTION("Preserves non-alphabetic characters") {
        REQUIRE(helix::text_io::to_upper("123_test!@#") == "123_TEST!@#");
    }
}

TEST_CASE("operation_patterns - to_lower utility", "[operation_patterns][string]") {
    SECTION("Converts uppercase to lowercase") {
        REQUIRE(helix::text_io::to_lower("HELLO") == "hello");
        REQUIRE(helix::text_io::to_lower("WORLD") == "world");
    }

    SECTION("Preserves lowercase") {
        REQUIRE(helix::text_io::to_lower("hello") == "hello");
    }

    SECTION("Handles mixed case") {
        REQUIRE(helix::text_io::to_lower("HeLLo WoRLd") == "hello world");
    }

    SECTION("Handles empty string") {
        REQUIRE(helix::text_io::to_lower("") == "");
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
