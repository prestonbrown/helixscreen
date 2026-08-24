// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapper.cpp
 * @brief Unit tests for FilamentMapper — pure logic, no LVGL dependency
 *
 * Tests:
 * - color_distance() weighted RGB metric
 * - colors_match() tolerance boundary
 * - materials_match() case-insensitive comparison
 * - find_closest_color_slot() slot selection with SlotKey
 * - compute_defaults() full mapping pipeline
 * - Multi-backend slot uniqueness
 */

#include "filament_mapper.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// =============================================================================
// color_distance
// =============================================================================

TEST_CASE("color_distance returns 0 for identical colors", "[filament_mapper][color]") {
    CHECK(FilamentMapper::color_distance(0x000000, 0x000000) == 0);
    CHECK(FilamentMapper::color_distance(0xFF0000, 0xFF0000) == 0);
    CHECK(FilamentMapper::color_distance(0xABCDEF, 0xABCDEF) == 0);
}

TEST_CASE("color_distance is symmetric", "[filament_mapper][color]") {
    CHECK(FilamentMapper::color_distance(0xFF0000, 0x00FF00) ==
          FilamentMapper::color_distance(0x00FF00, 0xFF0000));
    CHECK(FilamentMapper::color_distance(0x123456, 0x654321) ==
          FilamentMapper::color_distance(0x654321, 0x123456));
}

TEST_CASE("color_distance uses luminance weighting", "[filament_mapper][color]") {
    // Pure green difference should weigh more than pure blue
    int green_diff = FilamentMapper::color_distance(0x000000, 0x001000);
    int blue_diff = FilamentMapper::color_distance(0x000000, 0x000010);
    CHECK(green_diff >= blue_diff);
}

TEST_CASE("color_distance max is for black vs white", "[filament_mapper][color]") {
    int max_dist = FilamentMapper::color_distance(0x000000, 0xFFFFFF);
    CHECK(max_dist > 200);
    CHECK(max_dist < 300); // Weighted, so less than 441 (unweighted Euclidean)
}

TEST_CASE("color_distance per-channel ranges", "[filament_mapper][color][edge]") {
    SECTION("only red channel differs") {
        int dist = FilamentMapper::color_distance(0x000000, 0xFF0000);
        // sqrt(255^2 * 30 / 100) ~ 139
        CHECK(dist > 130);
        CHECK(dist < 150);
    }

    SECTION("only green channel differs") {
        int dist = FilamentMapper::color_distance(0x000000, 0x00FF00);
        // sqrt(255^2 * 59 / 100) ~ 195
        CHECK(dist > 190);
        CHECK(dist < 200);
    }

    SECTION("only blue channel differs") {
        int dist = FilamentMapper::color_distance(0x000000, 0x0000FF);
        // sqrt(255^2 * 11 / 100) ~ 84
        CHECK(dist > 80);
        CHECK(dist < 90);
    }
}

// =============================================================================
// colors_match
// =============================================================================

TEST_CASE("colors_match tolerance boundary", "[filament_mapper][color]") {
    CHECK(FilamentMapper::colors_match(0xFF0000, 0xFF0000));

    SECTION("known under-tolerance pair matches") {
        // Red shift of 15: sqrt(15^2 * 30 / 100) = sqrt(67.5) ~ 8
        CHECK(FilamentMapper::colors_match(0x800000, 0x8F0000));
    }

    SECTION("known over-tolerance pair does not match") {
        // Red vs green: distance >> 40
        CHECK_FALSE(FilamentMapper::colors_match(0xFF0000, 0x00FF00));
        CHECK_FALSE(FilamentMapper::colors_match(0x000000, 0xFFFFFF));
    }

    SECTION("slightly different colors match") {
        CHECK(FilamentMapper::colors_match(0xFF0000, 0xF00000));
        CHECK(FilamentMapper::colors_match(0x00FF00, 0x00F000));
    }
}

// =============================================================================
// materials_match (case-insensitive)
// =============================================================================

TEST_CASE("materials_match is case-insensitive", "[filament_mapper][material]") {
    CHECK(FilamentMapper::materials_match("PLA", "PLA"));
    CHECK(FilamentMapper::materials_match("PLA", "pla"));
    CHECK(FilamentMapper::materials_match("Pla", "pLA"));
    CHECK(FilamentMapper::materials_match("PETG", "petg"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "PETG"));
    CHECK(FilamentMapper::materials_match("PLA", "PLA+")); // Same compat group
    CHECK(FilamentMapper::materials_match("", ""));
}

TEST_CASE("materials_match handles brand-name variants", "[filament_mapper][material]") {
    // Compound names with known base material
    CHECK(FilamentMapper::materials_match("PLA", "PLA SnapSpeed"));
    CHECK(FilamentMapper::materials_match("PLA SnapSpeed", "PLA"));
    CHECK(FilamentMapper::materials_match("PLA SnapSpeed", "PLA SnapSpeed"));
    CHECK(FilamentMapper::materials_match("PETG", "PETG Pro"));
    CHECK(FilamentMapper::materials_match("ABS", "ABS Premium"));

    // Different base materials still mismatch
    CHECK_FALSE(FilamentMapper::materials_match("PLA SnapSpeed", "PETG"));
    CHECK_FALSE(FilamentMapper::materials_match("ABS Premium", "PLA"));
}

// =============================================================================
// find_closest_color_slot (now returns SlotKey)
// =============================================================================

TEST_CASE("find_closest_color_slot with no slots returns invalid key", "[filament_mapper][slot]") {
    std::vector<AvailableSlot> slots;

    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{-1, -1});
}

TEST_CASE("find_closest_color_slot skips empty slots (stale color must not match)",
          "[filament_mapper][slot]") {
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", true, -1},  // EMPTY, but reports a stale red color
        {1, 0, 0x00FF00, "PLA", false, -1}, // green, loaded
    };

    // An empty slot has no filament — its stale color must never attract a match,
    // even on an exact color hit. The only loaded slot (green) is out of tolerance,
    // so nothing matches.
    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{-1, -1});
}

TEST_CASE("find_closest_color_slot matches a loaded slot over an empty same-color slot",
          "[filament_mapper][slot]") {
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", true, -1},  // EMPTY red (stale)
        {1, 0, 0xFF0000, "PLA", false, -1}, // LOADED red
    };

    // Exact-red target must land on the loaded lane, never the empty one.
    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{1, 0});
}

TEST_CASE("find_closest_color_slot returns closest match", "[filament_mapper][slot]") {
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // exact red
        {1, 0, 0xF00000, "PLA", false, -1}, // slightly off red
        {2, 0, 0x00FF00, "PLA", false, -1}, // green (far)
    };

    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{0, 0});
}

TEST_CASE("find_closest_color_slot returns invalid key when nothing within tolerance",
          "[filament_mapper][slot]") {
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x00FF00, "PLA", false, -1},
        {1, 0, 0x0000FF, "PLA", false, -1},
    };

    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{-1, -1});
}

// =============================================================================
// compute_defaults — empty inputs
// =============================================================================

TEST_CASE("compute_defaults with empty inputs", "[filament_mapper][compute]") {
    SECTION("no tools, no slots") {
        auto result = FilamentMapper::compute_defaults({}, {});
        CHECK(result.empty());
    }

    SECTION("tools but no slots") {
        std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, {});
        REQUIRE(result.size() == 1);
        CHECK(result[0].is_auto);
        CHECK(result[0].reason == ToolMapping::MatchReason::AUTO);
        CHECK(result[0].mapped_slot == -1);
    }

    SECTION("no tools but has slots") {
        std::vector<AvailableSlot> slots = {{0, 0, 0xFF0000, "PLA", false, -1}};
        auto result = FilamentMapper::compute_defaults({}, slots);
        CHECK(result.empty());
    }
}

// =============================================================================
// compute_defaults — single tool
// =============================================================================

TEST_CASE("compute_defaults single tool single slot basic match", "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {{0, 0, 0xFF0000, "PLA", false, -1}};

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].tool_index == 0);
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].mapped_backend == 0);
    CHECK_FALSE(result[0].material_mismatch);
    CHECK_FALSE(result[0].is_auto);
    CHECK(result[0].reason == ToolMapping::MatchReason::COLOR_MATCH);
}

// =============================================================================
// compute_defaults — firmware mapping
// =============================================================================

TEST_CASE("compute_defaults firmware mapping is preferred over color match",
          "[filament_mapper][compute][firmware]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};

    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {1, 0, 0x00FF00, "PLA", false, 0}, // firmware maps to tool 0
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 1);
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
}

TEST_CASE("compute_defaults firmware mapping detects material mismatch",
          "[filament_mapper][compute][firmware]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PETG", false, 0},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
    CHECK(result[0].material_mismatch);
}

TEST_CASE("compute_defaults never matches an empty firmware-mapped slot",
          "[filament_mapper][compute][firmware]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", true, 0}, // firmware-mapped but EMPTY
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    // Empty lanes are skipped by firmware, color, AND fallback matching — an empty
    // lane can't print. With nothing loaded the tool stays unmatched (AUTO).
    CHECK(result[0].mapped_slot == -1);
    CHECK(result[0].is_auto);
    CHECK(result[0].reason == ToolMapping::MatchReason::AUTO);
}

TEST_CASE("compute_defaults duplicate firmware mapping takes first non-empty",
          "[filament_mapper][compute][firmware]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, 0}, // both claim tool 0
        {1, 0, 0x00FF00, "PLA", false, 0},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0); // first one wins
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
}

// =============================================================================
// compute_defaults — color matching
// =============================================================================

TEST_CASE("compute_defaults material mismatch skips color match, uses positional",
          "[filament_mapper][compute][color]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PETG", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    // The tool's own positional lane (slot_index 0 == tool_index 0) is assigned
    // with a material-mismatch flag so PrintStartController can warn.
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].material_mismatch);
}

TEST_CASE("compute_defaults blind fallback refuses a known-incompatible lane",
          "[filament_mapper][compute][material]") {
    // Tool 0 wants PLA; the only loaded lane sits at a non-positional index and is
    // PETG, so neither firmware, color, nor positional matching fires. The
    // material-blind "any unclaimed lane" fallback must NOT grab the incompatible
    // PETG lane — leave the tool unmatched so preflight surfaces it.
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {3, 0, 0xFF0000, "PETG", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == -1);
    CHECK(result[0].is_auto);
    CHECK(result[0].reason == ToolMapping::MatchReason::AUTO);
}

TEST_CASE("compute_defaults blind fallback routes around incompatible to compatible",
          "[filament_mapper][compute][material]") {
    // No positional or color match; the fallback skips the incompatible PETG lane
    // and fills the compatible PLA lane rather than grabbing the first unclaimed.
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {3, 0, 0xFF0000, "PETG", false, -1}, // exact color, wrong material
        {4, 0, 0x0000FF, "PLA", false, -1},  // compatible material, off color
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 4);
    CHECK_FALSE(result[0].material_mismatch);
}

TEST_CASE("compute_defaults blind fallback still fills an unknown-material lane",
          "[filament_mapper][compute][material]") {
    // A non-positional lane with no reported material can't be proven
    // incompatible, so backends that don't publish material keep the fallback.
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {3, 0, 0x00FF00, "", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 3);
}

TEST_CASE("compute_defaults case-insensitive material match no mismatch",
          "[filament_mapper][compute][material]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "pla"}}; // lowercase
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // uppercase
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].material_mismatch);
}

TEST_CASE("compute_defaults no color match falls through to positional",
          "[filament_mapper][compute][color]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x00FF00, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    // No color match, but positional fallback assigns slot 0 to tool 0
    CHECK(result[0].mapped_slot == 0);
    CHECK_FALSE(result[0].is_auto);
}

// =============================================================================
// compute_defaults — multi-tool
// =============================================================================

TEST_CASE("compute_defaults multi-tool no conflicts", "[filament_mapper][compute][multi]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"},
        {1, 0x00FF00, "PLA"},
        {2, 0x0000FF, "PLA"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {1, 0, 0x00FF00, "PLA", false, -1},
        {2, 0, 0x0000FF, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 3);

    CHECK(result[0].mapped_slot == 0);
    CHECK(result[1].mapped_slot == 1);
    CHECK(result[2].mapped_slot == 2);

    for (const auto& m : result) {
        CHECK(m.reason == ToolMapping::MatchReason::COLOR_MATCH);
        CHECK_FALSE(m.material_mismatch);
        CHECK_FALSE(m.is_auto);
    }
}

TEST_CASE("compute_defaults multi-tool with same color both map to best slot",
          "[filament_mapper][compute][multi]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"}, {1, 0xFF0000, "PLA"}, // same color
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0xF00000, "PLA", false, -1}, // slightly off red
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);

    // Both tools map to the best color match (slot re-use allowed)
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[1].mapped_slot == 0);
}

TEST_CASE("compute_defaults multi-tool same color with no close alternative",
          "[filament_mapper][compute][multi]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"},
        {1, 0xFF0000, "PLA"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0x00FF00, "PLA", false, -1}, // green, too far
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);

    // Both tools map to slot 0 (re-use allowed, green is too far)
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[1].mapped_slot == 0);
}

// =============================================================================
// compute_defaults — all empty slots
// =============================================================================

TEST_CASE("compute_defaults does not match empty slots (nothing loaded to print)",
          "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}, {1, 0x00FF00, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", true, -1},
        {1, 0, 0x00FF00, "PLA", true, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);
    // Both lanes are empty — their reported colors are stale, so neither tool
    // matches; each stays unmatched (AUTO) rather than routing to a dead lane.
    CHECK(result[0].mapped_slot == -1);
    CHECK(result[0].is_auto);
    CHECK(result[1].mapped_slot == -1);
    CHECK(result[1].is_auto);
}

// =============================================================================
// compute_defaults — mixed scenarios
// =============================================================================

TEST_CASE("compute_defaults mixed firmware, color, and auto", "[filament_mapper][compute][mixed]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"},
        {1, 0x00FF00, "PLA"},
        {2, 0x0000FF, "PETG"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, 0},
        {1, 0, 0x00FF00, "PLA", false, -1},
        {2, 0, 0xFFFF00, "ABS", false, -1}, // yellow, won't match blue
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 3);

    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);

    CHECK(result[1].mapped_slot == 1);
    CHECK(result[1].reason == ToolMapping::MatchReason::COLOR_MATCH);

    // T2: blue PETG has no color match (material mismatch with ABS),
    // falls to positional: slot_index 2 matches tool_index 2
    CHECK(result[2].mapped_slot == 2);
    CHECK_FALSE(result[2].is_auto);
    CHECK(result[2].material_mismatch); // PETG vs ABS
}

TEST_CASE("compute_defaults empty material strings skip mismatch check",
          "[filament_mapper][compute]") {
    SECTION("empty tool material") {
        std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, ""}};
        std::vector<AvailableSlot> slots = {{0, 0, 0xFF0000, "PLA", false, -1}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        CHECK_FALSE(result[0].material_mismatch);
    }

    SECTION("empty slot material") {
        std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
        std::vector<AvailableSlot> slots = {{0, 0, 0xFF0000, "", false, -1}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        CHECK_FALSE(result[0].material_mismatch);
    }
}

// =============================================================================
// use_current_assignments
// =============================================================================

TEST_CASE("FilamentMapper use_current_assignments", "[filament_mapper][current_assignments]") {
    SECTION("maps tools to slots positionally") {
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"},
            {1, 0x00FF00, "PETG"},
        };
        std::vector<AvailableSlot> slots = {
            {0, 0, 0x0000FF, "ABS", false, -1},  // slot 0
            {1, 0, 0xFF0000, "PLA", false, -1},  // slot 1
            {2, 0, 0x00FF00, "PETG", false, -1}, // slot 2
        };

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 2);

        // T0 → first slot (slot 0)
        CHECK(mappings[0].tool_index == 0);
        CHECK(mappings[0].mapped_slot == 0);
        CHECK(mappings[0].mapped_backend == 0);
        CHECK_FALSE(mappings[0].is_auto);

        // T1 → second slot (slot 1)
        CHECK(mappings[1].tool_index == 1);
        CHECK(mappings[1].mapped_slot == 1);
        CHECK(mappings[1].mapped_backend == 0);
        CHECK_FALSE(mappings[1].is_auto);
    }

    SECTION("more tools than slots results in AUTO for excess") {
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"},
            {1, 0x00FF00, "PETG"},
            {2, 0x0000FF, "ABS"},
        };
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xFF0000, "PLA", false, 0},
            {1, 0, 0x00FF00, "PETG", false, 1},
        };

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 3);

        CHECK(mappings[0].mapped_slot == 0);
        CHECK_FALSE(mappings[0].is_auto);

        CHECK(mappings[1].mapped_slot == 1);
        CHECK_FALSE(mappings[1].is_auto);

        // T2 has no slot — AUTO
        CHECK(mappings[2].mapped_slot == -1);
        CHECK(mappings[2].is_auto);
        CHECK(mappings[2].reason == ToolMapping::MatchReason::AUTO);
    }

    SECTION("detects material mismatches") {
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"},
        };
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xFF0000, "PETG", false, 0},
        };

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 1);
        CHECK(mappings[0].mapped_slot == 0);
        CHECK(mappings[0].material_mismatch);
    }

    SECTION("includes empty slots positionally") {
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"},
            {1, 0x00FF00, "PETG"},
        };
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xFF0000, "PLA", false, 0}, {1, 0, 0x000000, "", true, -1}, // empty slot
        };

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 2);
        CHECK(mappings[0].mapped_slot == 0);
        // T1 maps to empty slot (user's choice to keep it)
        CHECK(mappings[1].mapped_slot == 1);
        CHECK_FALSE(mappings[1].is_auto);
    }
}

// =============================================================================
// compute_defaults — backend index propagation
// =============================================================================

TEST_CASE("compute_defaults propagates backend index", "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 2, 0xFF0000, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_backend == 2);
}

// =============================================================================
// Multi-backend slot uniqueness (critical bug fix)
// =============================================================================

TEST_CASE("compute_defaults allows slot re-use across backends",
          "[filament_mapper][compute][multi_backend]") {
    // Two backends each have slot 0 with red filament
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"}, // red
        {1, 0xFF0000, "PLA"}, // also red
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // slot 0, backend 0
        {0, 1, 0xFF0000, "PLA", false, -1}, // slot 0, backend 1 (different physical slot!)
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);

    // Both tools map to the best color match (slot re-use allowed)
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].mapped_backend == 0);

    CHECK(result[1].mapped_slot == 0);
    CHECK(result[1].mapped_backend == 0);
    CHECK_FALSE(result[1].is_auto);
}

TEST_CASE("compute_defaults multi-backend firmware mapping uses correct backend",
          "[filament_mapper][compute][multi_backend]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // same slot_index, no firmware map
        {0, 1, 0x00FF00, "PLA", false, 0},  // same slot_index, firmware-mapped to tool 0
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].mapped_backend == 1); // firmware mapping is on backend 1
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
}

TEST_CASE("find_closest_color_slot picks first matching slot across backends",
          "[filament_mapper][slot][multi_backend]") {
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {0, 1, 0xFF0000, "PLA", false, -1}, // same slot_index, different backend
    };

    // Slot re-use allowed — always picks the first/best match
    auto result = FilamentMapper::find_closest_color_slot(0xFF0000, "", slots);
    CHECK(result == SlotKey{0, 0});
}

// =============================================================================
// materials_match — edge cases and cross-type detection
// =============================================================================

TEST_CASE("materials_match detects cross-material incompatibility", "[filament_mapper][material]") {
    // Common incompatible pairings that should NOT match
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "ASA"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "PETG"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "ABS"));
    CHECK_FALSE(FilamentMapper::materials_match("ASA", "PETG"));
    CHECK_FALSE(FilamentMapper::materials_match("ASA", "PLA"));
    CHECK_FALSE(FilamentMapper::materials_match("PETG", "TPU"));
}

TEST_CASE("materials_match handles empty and whitespace strings",
          "[filament_mapper][material][edge]") {
    // Two empty strings match (both unknown)
    CHECK(FilamentMapper::materials_match("", ""));

    // Empty vs non-empty never match (different lengths)
    CHECK_FALSE(FilamentMapper::materials_match("", "PLA"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", ""));
}

// =============================================================================
// compute_defaults — per-tool material mismatch scenarios
// =============================================================================

TEST_CASE("compute_defaults detects per-tool material mismatch in multi-tool print",
          "[filament_mapper][compute][material]") {
    // Simulates a 2-color PLA print with an AMS that has PLA and ASA
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"}, // tool 0 wants PLA
        {1, 0x0000FF, "PLA"}, // tool 1 wants PLA
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // red PLA — perfect match
        {1, 0, 0x0000FF, "ASA", false, -1}, // blue ASA — color match but wrong material
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);

    // Tool 0: color match, material match
    CHECK(result[0].mapped_slot == 0);
    CHECK_FALSE(result[0].material_mismatch);

    // Tool 1: color match, material MISMATCH (PLA vs ASA)
    CHECK(result[1].mapped_slot == 1);
    CHECK(result[1].material_mismatch);
}

TEST_CASE("compute_defaults mixed materials with firmware mapping",
          "[filament_mapper][compute][material]") {
    // Firmware maps tool 0 to a slot with wrong material
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "PLA"},
        {1, 0x00FF00, "PETG"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "ASA", false, 0},   // firmware-mapped to T0, but ASA not PLA
        {1, 0, 0x00FF00, "PETG", false, -1}, // color match for T1, material matches
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 2);

    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
    CHECK(result[0].material_mismatch); // ASA != PLA

    CHECK(result[1].reason == ToolMapping::MatchReason::COLOR_MATCH);
    CHECK_FALSE(result[1].material_mismatch); // PETG == PETG
}

TEST_CASE("compute_defaults single tool no material info skips mismatch",
          "[filament_mapper][compute][material]") {
    // When gcode has no material info, never flag mismatches
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, ""}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "ASA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK_FALSE(result[0].material_mismatch);
}

// =============================================================================
// materials_match — additional edge cases
// =============================================================================

TEST_CASE("materials_match handles trailing whitespace", "[filament_mapper][material]") {
    // Trailing spaces: base material extraction finds "PLA" in "PLA "
    CHECK(FilamentMapper::materials_match("PLA ", "PLA"));
    // Leading space: base extraction trims, so " PLA" resolves to PLA
    CHECK(FilamentMapper::materials_match("PLA", " PLA"));
    CHECK(FilamentMapper::materials_match("PLA ", "PLA "));
}

TEST_CASE("materials_match handles long material strings", "[filament_mapper][material]") {
    std::string long_name(200, 'X');
    CHECK(FilamentMapper::materials_match(long_name, long_name));
    // Both unknown → compatible (can't determine incompatibility)
    CHECK(FilamentMapper::materials_match(long_name, long_name + "Y"));
}

// =============================================================================
// compute_defaults — extreme scenarios
// =============================================================================

TEST_CASE("compute_defaults handles zero tools", "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools;
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    CHECK(result.empty());
}

TEST_CASE("compute_defaults handles zero slots", "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots;

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == -1);
}

TEST_CASE("compute_defaults handles many tools (12+)", "[filament_mapper][compute]") {
    // Simulate a 12-lane AMS system
    std::vector<GcodeToolInfo> tools;
    std::vector<AvailableSlot> slots;
    for (int i = 0; i < 12; ++i) {
        uint32_t color = static_cast<uint32_t>(i * 20) << 16;
        tools.push_back({i, color, "PLA"});
        slots.push_back({i, 0, color, "PLA", false, -1});
    }

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 12);
    // Each tool should map to its matching color slot
    for (int i = 0; i < 12; ++i) {
        CHECK(result[static_cast<size_t>(i)].mapped_slot == i);
        CHECK_FALSE(result[static_cast<size_t>(i)].material_mismatch);
    }
}

// =============================================================================
// find_unresolved_tools
// =============================================================================

TEST_CASE("find_unresolved_tools", "[filament_mapper]") {
    using helix::FilamentMapper;
    using helix::ToolMapping;

    SECTION("returns empty when all tools are mapped") {
        std::vector<ToolMapping> mappings = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::COLOR_MATCH},
            {1, 1, 0, false, false, ToolMapping::MatchReason::FIRMWARE_MAPPING},
        };
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.empty());
    }

    SECTION("returns auto tools with AUTO reason") {
        std::vector<ToolMapping> mappings = {
            {0, -1, -1, false, true, ToolMapping::MatchReason::AUTO},
            {1, 1, 0, false, false, ToolMapping::MatchReason::COLOR_MATCH},
            {2, -1, -1, false, true, ToolMapping::MatchReason::AUTO},
        };
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.size() == 2);
        REQUIRE(unresolved[0] == 0);
        REQUIRE(unresolved[1] == 2);
    }

    SECTION("returns empty when no mappings") {
        std::vector<ToolMapping> mappings = {};
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.empty());
    }

    SECTION("firmware-mapped tools are not unresolved") {
        std::vector<ToolMapping> mappings = {
            {0, 0, 0, false, false, ToolMapping::MatchReason::FIRMWARE_MAPPING},
        };
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.empty());
    }

    SECTION("color-matched tools are not unresolved") {
        std::vector<ToolMapping> mappings = {
            {0, 2, 0, false, false, ToolMapping::MatchReason::COLOR_MATCH},
        };
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.empty());
    }

    SECTION("is_auto with non-AUTO reason is not unresolved") {
        // is_auto and reason are always set together in practice, but verify
        // the AND condition is correct — both must be true to flag as unresolved
        std::vector<ToolMapping> mappings = {
            {0, 0, 0, false, true, ToolMapping::MatchReason::COLOR_MATCH},
        };
        auto unresolved = FilamentMapper::find_unresolved_tools(mappings);
        REQUIRE(unresolved.empty());
    }
}

TEST_CASE("compute_defaults all slots empty leaves tool unmatched", "[filament_mapper][compute]") {
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x000000, "", true, -1},
        {1, 0, 0x000000, "", true, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    // Nothing is loaded — color, positional, and blind fallbacks all skip empty
    // lanes, so the tool stays unmatched (AUTO / let firmware decide) instead of
    // being routed to a dead lane.
    CHECK(result[0].mapped_slot == -1);
    CHECK(result[0].is_auto);
}

// =============================================================================
// format_slot_label
// =============================================================================

TEST_CASE("FilamentMapper format_slot_label", "[filament_mapper]") {
    SECTION("single-unit slot with material") {
        helix::AvailableSlot slot;
        slot.slot_index = 2;
        slot.local_slot_index = 2;
        slot.backend_index = 0;
        slot.material = "PLA";
        slot.is_empty = false;
        slot.unit_display_name = ""; // single-unit

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Slot 3: PLA");
    }

    SECTION("single-unit slot without material") {
        helix::AvailableSlot slot;
        slot.slot_index = 0;
        slot.local_slot_index = 0;
        slot.backend_index = 0;
        slot.material = "";
        slot.is_empty = false;
        slot.unit_display_name = "";

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Slot 1");
    }

    SECTION("single-unit empty slot") {
        helix::AvailableSlot slot;
        slot.slot_index = 1;
        slot.local_slot_index = 1;
        slot.backend_index = 0;
        slot.material = "";
        slot.is_empty = true;
        slot.unit_display_name = "";

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Slot 2: Empty");
    }

    SECTION("multi-unit slot with material") {
        helix::AvailableSlot slot;
        slot.slot_index = 4;       // global index
        slot.local_slot_index = 0; // first slot in unit
        slot.backend_index = 0;
        slot.material = "PETG";
        slot.is_empty = false;
        slot.unit_display_name = "Turtle 1";

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Turtle 1 \xc2\xb7 Slot 1: PETG");
    }

    SECTION("multi-unit slot without material") {
        helix::AvailableSlot slot;
        slot.slot_index = 7;       // global index
        slot.local_slot_index = 3; // 4th slot in unit
        slot.backend_index = 0;
        slot.material = "";
        slot.is_empty = false;
        slot.unit_display_name = "Turtle 2";

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Turtle 2 \xc2\xb7 Slot 4");
    }

    SECTION("multi-unit empty slot") {
        helix::AvailableSlot slot;
        slot.slot_index = 9;       // global index
        slot.local_slot_index = 1; // 2nd slot in unit
        slot.backend_index = 0;
        slot.material = "";
        slot.is_empty = true;
        slot.unit_display_name = "Turtle 3";

        auto label = helix::FilamentMapper::format_slot_label(slot);
        CHECK(label == "Turtle 3 \xc2\xb7 Slot 2: Empty");
    }
}

// =============================================================================
// Material mismatch detection for pre-print warnings
// =============================================================================
// These tests verify the material_mismatch flag is set correctly in scenarios
// that the PrintStartController uses to show material compatibility warnings.

TEST_CASE("compute_defaults flags mismatch for single-tool non-AMS-like scenario",
          "[filament_mapper][material_mismatch]") {
    // Simulates non-AMS: single tool, single slot, wrong material loaded
    std::vector<GcodeToolInfo> tools = {{0, 0xFFFFFF, "PETG"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFFFFFF, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].material_mismatch); // PETG vs PLA = incompatible
}

TEST_CASE("compute_defaults no mismatch when materials are compatible",
          "[filament_mapper][material_mismatch]") {
    // PLA+ and PLA are in the same compatibility group
    std::vector<GcodeToolInfo> tools = {{0, 0xFFFFFF, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFFFFFF, "PLA+", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK_FALSE(result[0].material_mismatch); // PLA and PLA+ are compatible
}

TEST_CASE("compute_defaults no mismatch when loaded material is empty",
          "[filament_mapper][material_mismatch]") {
    // If the slot has no material info, we can't warn — not a mismatch
    std::vector<GcodeToolInfo> tools = {{0, 0xFFFFFF, "PETG"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFFFFFF, "", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].material_mismatch);
}

TEST_CASE("compute_defaults no mismatch when gcode material is empty",
          "[filament_mapper][material_mismatch]") {
    // If the gcode doesn't specify a material, we can't warn
    std::vector<GcodeToolInfo> tools = {{0, 0xFFFFFF, ""}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFFFFFF, "PLA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].material_mismatch);
}

TEST_CASE("compute_defaults flags multiple mismatches in multi-tool scenario",
          "[filament_mapper][material_mismatch]") {
    // Multi-tool print where two tools have wrong materials
    std::vector<GcodeToolInfo> tools = {
        {0, 0xFF0000, "ABS"},
        {1, 0x00FF00, "ABS"},
        {2, 0x0000FF, "ABS"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, 0},  // ABS vs PLA = mismatch
        {1, 0, 0x00FF00, "ASA", false, 1},  // ABS vs ASA = compatible (same group)
        {2, 0, 0x0000FF, "PETG", false, 2}, // ABS vs PETG = mismatch
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 3);
    CHECK(result[0].material_mismatch);       // ABS vs PLA
    CHECK_FALSE(result[1].material_mismatch); // ABS vs ASA (same compat group)
    CHECK(result[2].material_mismatch);       // ABS vs PETG
}

TEST_CASE("materials_match handles non-AMS external spool comparison",
          "[filament_mapper][material_mismatch]") {
    // These are the exact comparisons the material_compatibility gate
    // (print_start_checks.cpp) performs for non-AMS printers (gcode
    // filament_type vs external spool material)

    SECTION("exact match is compatible") {
        CHECK(FilamentMapper::materials_match("PLA", "PLA"));
        CHECK(FilamentMapper::materials_match("PETG", "PETG"));
        CHECK(FilamentMapper::materials_match("ABS", "ABS"));
    }

    SECTION("case-insensitive match") {
        CHECK(FilamentMapper::materials_match("pla", "PLA"));
        CHECK(FilamentMapper::materials_match("Petg", "PETG"));
    }

    SECTION("compatible group members match") {
        CHECK(FilamentMapper::materials_match("PLA", "PLA+"));
        CHECK(FilamentMapper::materials_match("PLA", "PLA-CF"));
        CHECK(FilamentMapper::materials_match("ABS", "ASA"));
        CHECK(FilamentMapper::materials_match("ABS", "ABS+"));
    }

    SECTION("incompatible materials do not match") {
        CHECK_FALSE(FilamentMapper::materials_match("PLA", "PETG"));
        CHECK_FALSE(FilamentMapper::materials_match("PLA", "ABS"));
        CHECK_FALSE(FilamentMapper::materials_match("PETG", "ABS"));
        CHECK_FALSE(FilamentMapper::materials_match("PLA", "TPU"));
        CHECK_FALSE(FilamentMapper::materials_match("PETG", "PA"));
    }

    SECTION("compound slicer names resolve to base material") {
        // Slicers often use names like "PLA SnapSpeed" or "Generic PETG"
        CHECK(FilamentMapper::materials_match("PLA SnapSpeed", "PLA"));
        CHECK_FALSE(FilamentMapper::materials_match("PLA SnapSpeed", "PETG"));
    }
}

// =============================================================================
// Filament database temperature lookups (used by material mismatch warnings)
// =============================================================================

#include "filament_database.h"

TEST_CASE("filament database provides temp ranges for mismatch warnings",
          "[filament_mapper][material_mismatch]") {
    SECTION("PLA vs PETG have non-overlapping nozzle ranges") {
        auto pla = filament::find_material("PLA");
        auto petg = filament::find_material("PETG");
        REQUIRE(pla.has_value());
        REQUIRE(petg.has_value());

        // PLA max < PETG min — printing PLA at PETG temps would burn it
        CHECK(pla->nozzle_max < petg->nozzle_min);
        CHECK(pla->bed_temp < petg->bed_temp);
    }

    SECTION("ABS vs PLA have very different temp ranges") {
        auto pla = filament::find_material("PLA");
        auto abs = filament::find_material("ABS");
        REQUIRE(pla.has_value());
        REQUIRE(abs.has_value());

        CHECK(pla->nozzle_max < abs->nozzle_min);
        CHECK(pla->bed_temp < abs->bed_temp);
    }

    SECTION("compatible materials have overlapping ranges") {
        auto abs = filament::find_material("ABS");
        auto asa = filament::find_material("ASA");
        REQUIRE(abs.has_value());
        REQUIRE(asa.has_value());

        // ABS and ASA have the same compat group and similar temps
        CHECK(abs->nozzle_min == asa->nozzle_min);
        CHECK(abs->bed_temp == asa->bed_temp);
    }

    SECTION("unknown material returns nullopt") {
        auto unknown = filament::find_material("SuperPlastic3000");
        CHECK_FALSE(unknown.has_value());
    }

    SECTION("alias resolution works for temp lookup") {
        auto nylon = filament::find_material("Nylon");
        auto pa = filament::find_material("PA");
        REQUIRE(nylon.has_value());
        REQUIRE(pa.has_value());
        CHECK(nylon->nozzle_min == pa->nozzle_min);
        CHECK(nylon->nozzle_max == pa->nozzle_max);
    }
}

// =============================================================================
// resolve_display_colors — DISPLAY color resolution (loaded slot vs slicer)
// =============================================================================

using helix::AvailableSlot;
using helix::FilamentMapper;
using helix::GcodeToolInfo;
using helix::ToolMapping;

namespace {
GcodeToolInfo tool(int idx, uint32_t rgb) {
    GcodeToolInfo t;
    t.tool_index = idx;
    t.color_rgb = rgb;
    return t;
}
AvailableSlot slot(int slot_idx, int backend, uint32_t rgb) {
    AvailableSlot s;
    s.slot_index = slot_idx;
    s.backend_index = backend;
    s.color_rgb = rgb;
    s.is_empty = false;
    return s;
}
ToolMapping mapped(int tool_idx, int slot_idx, int backend) {
    ToolMapping m;
    m.tool_index = tool_idx;
    m.mapped_slot = slot_idx;
    m.mapped_backend = backend;
    m.is_auto = false;
    return m;
}
ToolMapping unresolved(int tool_idx) {
    ToolMapping m;
    m.tool_index = tool_idx;
    m.mapped_slot = -1;
    m.mapped_backend = -1;
    m.is_auto = true;
    return m;
}
} // namespace

TEST_CASE("resolve_display_colors uses loaded slot color for mapped tools", "[filament]") {
    std::vector<GcodeToolInfo> tools = {tool(0, 0xFFFF00)};    // slicer yellow
    std::vector<AvailableSlot> slots = {slot(3, 0, 0x0000FF)}; // slot 3 loaded blue
    std::vector<ToolMapping> mappings = {mapped(0, 3, 0)};     // T0 -> slot 3

    auto colors = FilamentMapper::resolve_display_colors(tools, mappings, slots);
    REQUIRE(colors.size() == 1);
    REQUIRE(colors[0] == 0x0000FF); // blue (loaded), NOT yellow (slicer)
}

TEST_CASE("resolve_display_colors falls back to slicer color when unmapped", "[filament]") {
    std::vector<GcodeToolInfo> tools = {tool(0, 0xFFFF00)};
    std::vector<AvailableSlot> slots = {slot(3, 0, 0x0000FF)};
    std::vector<ToolMapping> mappings = {unresolved(0)}; // is_auto, no slot

    auto colors = FilamentMapper::resolve_display_colors(tools, mappings, slots);
    REQUIRE(colors.size() == 1);
    REQUIRE(colors[0] == 0xFFFF00); // slicer yellow
}

TEST_CASE("resolve_display_colors reflects a slot color change (live re-color)", "[filament]") {
    std::vector<GcodeToolInfo> tools = {tool(0, 0xFFFF00)};
    std::vector<ToolMapping> mappings = {mapped(0, 3, 0)};

    std::vector<AvailableSlot> before = {slot(3, 0, 0x0000FF)};
    REQUIRE(FilamentMapper::resolve_display_colors(tools, mappings, before)[0] == 0x0000FF);

    std::vector<AvailableSlot> after = {slot(3, 0, 0x00FF00)}; // reloaded green
    REQUIRE(FilamentMapper::resolve_display_colors(tools, mappings, after)[0] == 0x00FF00);
}

TEST_CASE("resolve_display_colors preserves a manual mapping when an unrelated slot changes",
          "[filament]") {
    std::vector<GcodeToolInfo> tools = {tool(0, 0xFFFF00)};
    std::vector<ToolMapping> mappings = {mapped(0, 2, 0)}; // T0 -> slot 2 (green)
    std::vector<AvailableSlot> slots = {slot(0, 0, 0xFF0000), slot(2, 0, 0x00FF00)};

    // slot 0 changes color; T0 is mapped to slot 2, so its color must stay green.
    slots[0].color_rgb = 0x123456;
    auto colors = FilamentMapper::resolve_display_colors(tools, mappings, slots);
    REQUIRE(colors[0] == 0x00FF00);
}

// =============================================================================
// effective_mappings / effective_tool_colors — the shared toggle-aware helpers
// =============================================================================

TEST_CASE("effective_mappings auto ON ignores firmware mapping and color-matches",
          "[filament_mapper][effective]") {
    std::vector<GcodeToolInfo> tools = {{0, 0x00FF00, "PLA"}}; // wants green PLA
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, 0},  // red PLA, firmware-mapped to tool 0
        {1, 0, 0x00FF00, "PLA", false, -1}, // green PLA, no firmware mapping
    };

    // Auto ON: firmware mapping is cleared so the color match (green) wins.
    auto on = FilamentMapper::effective_mappings(tools, slots, /*auto_color_map=*/true);
    REQUIRE(on.size() == 1);
    CHECK(on[0].mapped_slot == 1);

    // Auto OFF: positional assignment — tool 0 takes the first slot (red).
    auto off = FilamentMapper::effective_mappings(tools, slots, /*auto_color_map=*/false);
    REQUIRE(off.size() == 1);
    CHECK(off[0].mapped_slot == 0);
}

TEST_CASE("effective_tool_colors scatters a sparse used-set to tool-number indices",
          "[filament_mapper][effective]") {
    // A print using only T0 and T2 must land each color at its tool number, with
    // the unused T1 slot filled with the neutral default.
    std::vector<GcodeToolInfo> tools = {{0, 0xAA0000, "PLA"}, {2, 0x0000BB, "PLA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xAA0000, "PLA", false, -1},
        {1, 0, 0x123456, "PLA", false, -1},
        {2, 0, 0x0000BB, "PLA", false, -1},
    };

    auto colors = FilamentMapper::effective_tool_colors(tools, slots, /*auto_color_map=*/true);
    REQUIRE(colors.size() == 3); // max tool_index (2) + 1
    CHECK(colors[0] == 0xAA0000);
    CHECK(colors[1] == 0x808080); // T1 unused → neutral default
    CHECK(colors[2] == 0x0000BB);
}

TEST_CASE("effective_tool_colors returns empty for no tools", "[filament_mapper][effective]") {
    std::vector<AvailableSlot> slots = {{0, 0, 0xAA0000, "PLA", false, -1}};
    CHECK(FilamentMapper::effective_tool_colors({}, slots, /*auto_color_map=*/true).empty());
}
