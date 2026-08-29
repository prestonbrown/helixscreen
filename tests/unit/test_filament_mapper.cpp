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

    SECTION("a sparse tool set pairs each tool with its own head, not the list index") {
        // The U1 case (calicat_PLA_37m55s.gcode): the file uses T0 and T2 only.
        // Pairing the i-th used tool with slots[i] walks the slot list densely,
        // so T2 — the second USED tool — lands on lane 1 instead of lane 2. That
        // is a remap the user never asked for, and because it is not the
        // firmware default identity_filtered_remap() emits it, sending the
        // print to a lane holding the wrong filament.
        //
        // Positional means "each tool keeps its own head". Only a dense tool
        // set makes index-pairing and head-pairing agree, and every other case
        // here is dense, which is why this went unnoticed.
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"}, // red   — the file's body
            {2, 0x000000, "PLA"}, // black — the file's tail
        };
        std::vector<AvailableSlot> slots = {
            {0, 0, 0x080A0D, "PLA", false, -1}, // lane 0 — black
            {1, 0, 0xE2DEDB, "PLA", false, -1}, // lane 1 — off-white
            {2, 0, 0xE72F1D, "PLA", false, -1}, // lane 2 — red
            {3, 0, 0xF4C032, "PLA", false, -1}, // lane 3 — yellow
        };

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 2);

        CHECK(mappings[0].tool_index == 0);
        CHECK(mappings[0].mapped_slot == 0);

        CHECK(mappings[1].tool_index == 2);
        CHECK(mappings[1].mapped_slot == 2); // NOT 1

        // And the whole point: a positional map is the firmware default, so
        // nothing goes out on the wire.
        CHECK(FilamentMapper::identity_filtered_remap(mappings).empty());
    }

    SECTION("a lane-per-tool AMS keeps tools above 3 on their own lanes") {
        // An 8-lane AFC (two Box Turtles) numbers global slots 0..7 and maps
        // them T0..T7, so tool 5 genuinely belongs on lane 5. Seeding through
        // A four-head routing would collapse every tool above 3 onto head 0.
        // This case is stated for a lane-per-tool AMS, which is the default and
        // the majority shape; the four-head cases say so explicitly.
        std::vector<GcodeToolInfo> tools = {
            {0, 0xFF0000, "PLA"},
            {5, 0x00FF00, "PLA"},
        };
        std::vector<AvailableSlot> slots;
        for (int i = 0; i < 8; ++i) {
            slots.push_back({i, 0, 0x101010u * static_cast<uint32_t>(i + 1), "PLA", false, -1});
        }

        auto mappings = FilamentMapper::use_current_assignments(tools, slots);

        REQUIRE(mappings.size() == 2);
        CHECK(mappings[0].mapped_slot == 0);
        CHECK(mappings[1].tool_index == 5);
        CHECK(mappings[1].mapped_slot == 5); // NOT 0
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

TEST_CASE("FilamentMapper use_current_assignments: seed matrix over tool numbering and lane count",
          "[filament_mapper][current_assignments][matrix]") {
    // WHY THIS MATRIX EXISTS. Two separate bugs shipped through the sections
    // above, and both survived because every one of those cases used tools 0-3
    // on a 4-lane system. In that corner three different rules agree:
    //     pair by list position | pair by tool index | pair by default head
    // so no assertion there can tell them apart. The axes that DO separate them
    // are tool SPARSITY (a file using T0+T2 with no T1) and tool NUMBER ABOVE 3
    // (an 8-lane AFC maps lanes 0..7 to T0..T7, while a four-head routing sends
    // everything above 3 to head 0). Each case names the shape it is stated for.
    // Any new rule must be stated across this whole grid, not one corner of it.
    struct Case {
        const char* name;
        std::vector<int> tools;         // tool indices the file actually uses
        int lane_count;                 // lanes the AMS/toolchanger reports
        std::vector<int> expect;        // expected mapped_slot per tool, -1 = AUTO
        bool expect_identity;           // seed is the firmware default => nothing emitted
        helix::FirmwareRouting routing; // which AMS SHAPE the case is stated for
    };

    const auto lane_per_tool = helix::FirmwareRouting::identity();
    const auto four_head = helix::FirmwareRouting::fixed_heads(4, 0);

    const std::vector<Case> cases = {
        {"dense tools, 4 lanes", {0, 1, 2}, 4, {0, 1, 2}, true, lane_per_tool},
        {"sparse skipping T1, 4 lanes", {0, 2}, 4, {0, 2}, true, lane_per_tool},
        {"sparse skipping T1 and T2, 4 lanes", {0, 3}, 4, {0, 3}, true, lane_per_tool},
        {"lane-per-tool AMS, sparse high tool", {0, 5}, 8, {0, 5}, true, lane_per_tool},
        {"lane-per-tool AMS, all eight lanes",
         {0, 1, 2, 3, 4, 5, 6, 7},
         8,
         {0, 1, 2, 3, 4, 5, 6, 7},
         true,
         lane_per_tool},
        {"four-head: extended tool falls back to head 0", {5}, 4, {0}, true, four_head},
        {"four-head: tools 0-3 are still their own heads", {0, 2}, 4, {0, 2}, true, four_head},
        {"tool beyond the lane count is unresolved", {0, 1, 2}, 2, {0, 1, -1}, true, lane_per_tool},
        {"single tool", {0}, 4, {0}, true, lane_per_tool},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);

        std::vector<GcodeToolInfo> tools;
        for (int t : c.tools) {
            tools.push_back({t, 0xFF0000, "PLA"});
        }
        std::vector<AvailableSlot> slots;
        for (int i = 0; i < c.lane_count; ++i) {
            slots.push_back({i, 0, 0x111111u * static_cast<uint32_t>(i + 1), "PLA", false, -1});
        }

        auto mappings = FilamentMapper::use_current_assignments(tools, slots, c.routing);
        REQUIRE(mappings.size() == c.tools.size());

        for (size_t i = 0; i < c.tools.size(); ++i) {
            CAPTURE(c.tools[i]);
            CHECK(mappings[i].tool_index == c.tools[i]);
            CHECK(mappings[i].mapped_slot == c.expect[i]);
            CHECK(mappings[i].is_auto == (c.expect[i] < 0));
        }

        // A positional seed is what the firmware would do unaided, so it must
        // filter to an empty remap. If this fails where it is expected to hold,
        // the seed is quietly emitting a route the user never asked for - the
        // original bug. Because the seed and the filter now share ONE routing,
        // this holds for every shape - it used to fail above tool 3.
        if (c.expect_identity) {
            CHECK(FilamentMapper::identity_filtered_remap(mappings, c.routing).empty());
        }
    }
}

TEST_CASE("use_current_assignments honours the backend's firmware routing",
          "[filament_mapper][current_assignments][routing]") {
    // The same file and the same lanes, seeded for two different AMS shapes.
    // Before backends declared their own routing, a four-head constant decided
    // this for every backend, so the lane-per-tool case below was simply wrong.
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}, {5, 0x00FF00, "PLA"}};
    std::vector<AvailableSlot> slots;
    for (int i = 0; i < 8; ++i) {
        slots.push_back({i, 0, 0x111111u * static_cast<uint32_t>(i + 1), "PLA", false, -1});
    }

    SECTION("lane-per-tool (AFC, Happy Hare): T5 owns lane 5") {
        auto m = FilamentMapper::use_current_assignments(tools, slots,
                                                         helix::FirmwareRouting::identity());
        REQUIRE(m.size() == 2);
        CHECK(m[0].mapped_slot == 0);
        CHECK(m[1].mapped_slot == 5);
    }

    SECTION("fixed-head (Snapmaker U1): T5 falls to head 0") {
        // The U1 has four heads and 32 logical tools; its live table reads
        // [0,1,2,3,0,0,...], so T5 genuinely prints from head 0.
        auto m = FilamentMapper::use_current_assignments(tools, slots,
                                                         helix::FirmwareRouting::fixed_heads(4, 0));
        REQUIRE(m.size() == 2);
        CHECK(m[0].mapped_slot == 0);
        CHECK(m[1].mapped_slot == 0);
    }
}

TEST_CASE("identity_filtered_remap agrees with the seed under the same routing",
          "[filament_mapper][remap][routing]") {
    // These two functions used to disagree above tool 3: the seed said T5 owns
    // lane 5, the filter said the firmware default for T5 was head 0 and so
    // emitted the seed as a real remap. Sharing one routing makes agreement
    // structural instead of something a test has to pin.
    std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}, {5, 0x00FF00, "PLA"}};
    std::vector<AvailableSlot> slots;
    for (int i = 0; i < 8; ++i) {
        slots.push_back({i, 0, 0x111111u * static_cast<uint32_t>(i + 1), "PLA", false, -1});
    }

    SECTION("lane-per-tool: the seed IS the default, so nothing is emitted") {
        auto routing = helix::FirmwareRouting::identity();
        auto m = FilamentMapper::use_current_assignments(tools, slots, routing);
        CHECK(FilamentMapper::identity_filtered_remap(m, routing).empty());
    }

    SECTION("fixed-head: a hand-placed T5 on lane 5 IS a real remap") {
        auto routing = helix::FirmwareRouting::fixed_heads(4, 0);
        std::vector<ToolMapping> m(2);
        m[0].tool_index = 0;
        m[0].mapped_slot = 0; // identity for this routing -> dropped
        m[1].tool_index = 5;
        m[1].mapped_slot = 5; // default head is 0, so this is a genuine remap
        std::map<int, int> expected = {{5, 5}};
        CHECK(FilamentMapper::identity_filtered_remap(m, routing) == expected);
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

// =============================================================================
// Colour mismatch — the per-chip surround's claim
// =============================================================================
// The stacked chip already SHOWS both colours, so this flag only decides
// whether the chip is surrounded. Every guard below is a claim we must not make
// falsely: a surround that fires on prints that are fine trains the user to
// ignore it.

TEST_CASE("colour mismatch: set only when the comparison is real",
          "[filament][mapping][mapper][colour_mismatch]") {
    AvailableSlot lane{};
    lane.slot_index = 0;
    lane.backend_index = 0;
    lane.local_slot_index = 0;
    lane.color_rgb = 0xFF0000; // red lane
    lane.material = "PLA";

    GcodeToolInfo red{};
    red.tool_index = 0;
    red.color_rgb = 0xFE0101; // within COLOR_MATCH_TOLERANCE of the lane
    red.color_known = true;
    red.material = "PLA";

    GcodeToolInfo blue = red;
    blue.color_rgb = 0x0000FF; // far outside tolerance

    GcodeToolInfo unknown = blue;
    unknown.color_known = false; // nothing was claimed about this tool's colour

    // Close enough => no warning, using the SAME predicate auto-match uses, so a
    // lane the matcher would have picked never draws a warning.
    CHECK_FALSE(FilamentMapper::classify_mismatches(red, lane).color_mismatch);
    // Genuinely different => warn.
    CHECK(FilamentMapper::classify_mismatches(blue, lane).color_mismatch);
    // Unknown gcode colour => no claim to contradict.
    CHECK_FALSE(FilamentMapper::classify_mismatches(unknown, lane).color_mismatch);

    // An EMPTY lane has no colour; its own border already says so, and warning
    // twice for one cause reads as two faults.
    AvailableSlot empty = lane;
    empty.is_empty = true;
    CHECK_FALSE(FilamentMapper::classify_mismatches(blue, empty).color_mismatch);
}

TEST_CASE("classify_mismatches answers material the way every seeding site did",
          "[filament][mapping][mapper][colour_mismatch]") {
    // The four inline copies this replaced all read: both sides named, and not
    // compatible. Colour never enters into it, and an empty lane is not special
    // here - only the colour half suppresses on empty.
    AvailableSlot lane{};
    lane.color_rgb = 0xFF0000;
    lane.material = "PLA";

    GcodeToolInfo tool{};
    tool.color_rgb = 0xFF0000;
    tool.material = "PETG";

    CHECK(FilamentMapper::classify_mismatches(tool, lane).material_mismatch);

    tool.material = "PLA+"; // same compatibility group
    CHECK_FALSE(FilamentMapper::classify_mismatches(tool, lane).material_mismatch);

    tool.material = "PETG";
    lane.material = ""; // lane says nothing -> nothing to contradict
    CHECK_FALSE(FilamentMapper::classify_mismatches(tool, lane).material_mismatch);

    lane.material = "PLA";
    tool.material = ""; // file says nothing -> nothing to contradict
    CHECK_FALSE(FilamentMapper::classify_mismatches(tool, lane).material_mismatch);
}

TEST_CASE("colour mismatch reaches the mapping on every seeding path",
          "[filament][mapping][mapper][colour_mismatch]") {
    // One shared classifier is only worth having if every path that resolves a
    // lane runs it. Each SECTION drives one of them through its public entry.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1}, // red lane 0
        {1, 0, 0x00FF00, "PLA", false, -1}, // green lane 1
    };

    SECTION("firmware mapping onto a wrong-coloured lane warns") {
        slots[1].current_tool_mapping = 0; // firmware routes T0 to the green lane
        std::vector<GcodeToolInfo> tools = {{0, 0xFF0000, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 1);
        CHECK(result[0].color_mismatch);
    }

    SECTION("a colour-matched lane never warns") {
        std::vector<GcodeToolInfo> tools = {{0, 0x00FF00, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 1); // matched the green lane
        CHECK_FALSE(result[0].color_mismatch);
    }

    SECTION("the positional fallback warns about the lane it settles for") {
        // Blue is nowhere near either lane, so nothing colour-matches and T0
        // falls back to its own positional lane 0, which is red.
        std::vector<GcodeToolInfo> tools = {{0, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 0);
        CHECK(result[0].color_mismatch);
    }

    SECTION("the last-resort unclaimed lane warns too") {
        // T3 has no positional lane, so it takes the first unclaimed compatible
        // one. The chip shows that lane's colour next to the file's; nothing
        // about "we had to guess" makes the difference less real.
        std::vector<GcodeToolInfo> tools = {{3, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, slots);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 0);
        CHECK(result[0].color_mismatch);
    }

    SECTION("use_current_assignments warns on the head the firmware would pick") {
        // Auto-colour-map OFF: T0 keeps head 0 (red) whatever the file asked for.
        std::vector<GcodeToolInfo> tools = {{0, 0x00FF00, "PLA"}};
        auto result = FilamentMapper::use_current_assignments(tools, slots);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 0);
        CHECK(result[0].color_mismatch);
    }

    SECTION("an unresolved tool makes no claim at all") {
        // No lanes, so no lane was chosen, so there is no comparison to fail.
        std::vector<GcodeToolInfo> tools = {{0, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, {});
        REQUIRE(result.size() == 1);
        CHECK(result[0].is_auto);
        CHECK_FALSE(result[0].color_mismatch);
    }

    SECTION("a tool mapped onto an empty lane warns once, not twice") {
        // The lane's own empty marker is the real problem; its reported colour
        // is stale left-over data. Two borders for one cause read as two faults.
        std::vector<AvailableSlot> with_empty = {
            {0, 0, 0xFF0000, "", true, 0}, // empty lane, firmware routes T0 here
        };
        std::vector<GcodeToolInfo> tools = {{0, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::use_current_assignments(tools, with_empty);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 0);
        CHECK_FALSE(result[0].color_mismatch);
    }
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

// =============================================================================
// effective_tool_colors with no filament system present
// =============================================================================
//
// Most printers have no AMS and no Spoolman, so collect_available_slots()
// returns nothing. What the mapper hands back in that case decides what the
// G-code preview renders, and an AD5M rendered every file - a #000000 cube and
// a #F7F7F7 cube alike - in the same 0x808080 grey.

TEST_CASE("effective_tool_colors keeps the slicer color when no slots exist",
          "[filament_mapper][color]") {
    std::vector<GcodeToolInfo> tools(1);
    tools[0].tool_index = 0;
    tools[0].color_rgb = 0x00A899; // what the slicer declared

    const auto colors = FilamentMapper::effective_tool_colors(tools, {}, /*auto_color_map=*/false);

    REQUIRE(colors.size() == 1);
    // An unmapped tool resolves to its own slicer color. If this ever returns
    // the 0x808080 placeholder instead, every preview on a printer without a
    // filament system renders flat grey.
    CHECK(colors[0] == 0x00A899);
}

TEST_CASE("effective_tool_colors falls back to the placeholder only when the slicer gave nothing",
          "[filament_mapper][color]") {
    std::vector<GcodeToolInfo> tools(1);
    tools[0].tool_index = 0;
    tools[0].color_rgb = 0x808080; // build_tool_info's value_or default

    const auto colors = FilamentMapper::effective_tool_colors(tools, {}, /*auto_color_map=*/false);

    REQUIRE(colors.size() == 1);
    CHECK(colors[0] == 0x808080);
}

TEST_CASE("effective_tool_colors leaves gaps neutral without disturbing used tools",
          "[filament_mapper][color]") {
    // A print that uses only T0 and T2 must land T2's color at index 2 - the
    // viewer's override vector is indexed by logical tool number, so a dense
    // 2-entry vector would paint T2's toolpaths with T1's color.
    std::vector<GcodeToolInfo> tools(2);
    tools[0].tool_index = 0;
    tools[0].color_rgb = 0xED1C24;
    tools[1].tool_index = 2;
    tools[1].color_rgb = 0x00C502;

    const auto colors = FilamentMapper::effective_tool_colors(tools, {}, /*auto_color_map=*/false);

    REQUIRE(colors.size() == 3);
    CHECK(colors[0] == 0xED1C24);
    CHECK(colors[1] == 0x808080); // unused tool number stays neutral
    CHECK(colors[2] == 0x00C502);
}

// =============================================================================
// compute_defaults — a tool whose color is unknown
// =============================================================================
//
// GcodeToolInfo::color_known is false when NO source could say what color a
// tool prints in — Moonraker omitted filament_colors, and neither the G-code
// footer nor a viewer parse has backfilled it yet. color_rgb then holds a
// neutral stand-in, and running the nearest-color search against it picks a
// lane on the strength of a value the file never stated.
//
// Positional fallback is the honest answer there: it is what the mapper already
// does when no lane is within COLOR_MATCH_TOLERANCE, and it does not pretend the
// file expressed a preference. Firmware mappings still win — those are the
// printer's own statement of fact and owe nothing to the palette.

TEST_CASE("compute_defaults: an unknown color never takes a color match",
          "[filament_mapper][compute][color_known]") {
    // NOTE ON THE ASSERTION: reason cannot discriminate here — the positional
    // fallback below stamps MatchReason::COLOR_MATCH too (it has since before
    // this change), so both outcomes report COLOR_MATCH. The slot actually
    // chosen is the only honest observable, so the fixture is built to make the
    // two paths pick DIFFERENT slots:
    //
    //   tool 0, stand-in colour 0x808080  ->  color match wants slot 1 (exact)
    //                                     ->  positional wants slot 0 (index 0)
    //
    // Both lanes are ASA, so the material gate lets the colour search reach
    // either one — which is the point: with several lanes of the same material
    // loaded, matching against a stand-in is pure coin-toss.
    std::vector<GcodeToolInfo> tools = {{0, 0x808080, "ASA", /*color_known=*/false}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x111111, "ASA", false, -1},
        {1, 0, 0x808080, "ASA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 0); // positional, NOT the exact grey match
}

TEST_CASE("compute_defaults: a known color still takes the color match",
          "[filament_mapper][compute][color_known]") {
    // Identical fixture with color_known defaulted true — the pre-existing
    // behavior must be untouched, or every file that DOES declare its colors
    // regresses to positional assignment.
    std::vector<GcodeToolInfo> tools = {{0, 0x808080, "ASA"}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x111111, "ASA", false, -1},
        {1, 0, 0x808080, "ASA", false, -1},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].mapped_slot == 1); // exact colour match wins
    CHECK(result[0].reason == ToolMapping::MatchReason::COLOR_MATCH);
}

TEST_CASE("compute_defaults: a firmware mapping still wins for an unknown color",
          "[filament_mapper][compute][color_known]") {
    // The firmware's own tool assignment is not a guess about the palette, so
    // an unknown color must not suppress it.
    std::vector<GcodeToolInfo> tools = {{0, 0x808080, "ASA", /*color_known=*/false}};
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x111111, "ASA", false, -1},
        {1, 0, 0x222222, "ASA", false, /*current_tool_mapping=*/0},
    };

    auto result = FilamentMapper::compute_defaults(tools, slots);
    REQUIRE(result.size() == 1);
    CHECK(result[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
    CHECK(result[0].mapped_slot == 1);
}

// =============================================================================
// mapped_lane_display_number
// =============================================================================
//
// The mapping chip draws the gcode colour and the mapped lane's colour. When
// two bays hold the same filament those swatches are identical, so the chip
// says which colour will print without saying which spool it comes from —
// precisely the case where the user needs to know. These pin the number that
// disambiguates them, and the rules about when there is no number to show.

TEST_CASE("mapped_lane_display_number: a mapped lane reports its 1-based position",
          "[filament_mapper][lane_number]") {
    // Slot indices are 0-based internally and 1-based everywhere they are shown
    // (format_slot_label, the AMS slot badges). The chip must agree with them.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {1, 0, 0x00FF00, "PLA", false, -1},
        {2, 0, 0x0000FF, "PLA", false, -1},
    };
    slots[0].local_slot_index = 0;
    slots[1].local_slot_index = 1;
    slots[2].local_slot_index = 2;

    ToolMapping m;
    m.tool_index = 0;
    m.mapped_slot = 1;
    m.mapped_backend = 0;

    CHECK(FilamentMapper::mapped_lane_display_number(m, slots) == 2);
}

TEST_CASE("mapped_lane_display_number: two lanes of the same colour stay distinguishable",
          "[filament_mapper][lane_number]") {
    // The reason this function exists. Both lanes are the same red; only the
    // number tells the user which spool the print will pull from.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {1, 0, 0xFF0000, "PLA", false, -1},
    };
    slots[0].local_slot_index = 0;
    slots[1].local_slot_index = 1;

    ToolMapping first;
    first.mapped_slot = 0;
    first.mapped_backend = 0;
    ToolMapping second;
    second.mapped_slot = 1;
    second.mapped_backend = 0;

    CHECK(FilamentMapper::mapped_lane_display_number(first, slots) == 1);
    CHECK(FilamentMapper::mapped_lane_display_number(second, slots) == 2);
}

TEST_CASE("mapped_lane_display_number: a multi-unit lane is numbered within its own unit",
          "[filament_mapper][lane_number]") {
    // Second unit's first bay is "Slot 1" on the hardware, not "Slot 5".
    // Reporting the global index would name a lane the printer does not have,
    // and would disagree with format_slot_label() on the same slot.
    std::vector<AvailableSlot> slots = {
        {4, 0, 0xFF0000, "PLA", false, -1},
    };
    slots[0].unit_index = 1;
    slots[0].local_slot_index = 0;
    slots[0].unit_display_name = "Turtle 2";

    ToolMapping m;
    m.mapped_slot = 4;
    m.mapped_backend = 0;

    CHECK(FilamentMapper::mapped_lane_display_number(m, slots) == 1);
}

TEST_CASE("mapped_lane_display_number: the backend must match, not just the slot index",
          "[filament_mapper][lane_number]") {
    // Slot indices are unique within a backend, not across them. Matching on
    // the index alone would return the first backend's lane for a tool mapped
    // to the second one's.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {0, 1, 0x00FF00, "PLA", false, -1},
    };
    slots[0].local_slot_index = 0;
    slots[1].local_slot_index = 3;

    ToolMapping m;
    m.mapped_slot = 0;
    m.mapped_backend = 1;

    CHECK(FilamentMapper::mapped_lane_display_number(m, slots) == 4);
}

TEST_CASE("mapped_lane_display_number: an auto or unmapped tool has no lane to name",
          "[filament_mapper][lane_number]") {
    // "Auto" is a deliberate absence — the firmware chooses at print time.
    // Printing a number here would claim a decision nothing has made.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
    };

    ToolMapping automatic;
    automatic.is_auto = true;
    automatic.mapped_slot = 0;
    automatic.mapped_backend = 0;
    CHECK(FilamentMapper::mapped_lane_display_number(automatic, slots) == -1);

    ToolMapping unmapped;
    unmapped.mapped_slot = -1;
    unmapped.mapped_backend = -1;
    CHECK(FilamentMapper::mapped_lane_display_number(unmapped, slots) == -1);
}

TEST_CASE("mapped_lane_display_number: a mapping that outlived its lane shows nothing",
          "[filament_mapper][lane_number]") {
    // Unit unplugged between the mapping being computed and the chip rendering.
    // A stale number is worse than none: it names a lane that is not there.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
    };

    ToolMapping m;
    m.mapped_slot = 7;
    m.mapped_backend = 0;

    CHECK(FilamentMapper::mapped_lane_display_number(m, slots) == -1);

    std::vector<AvailableSlot> none;
    CHECK(FilamentMapper::mapped_lane_display_number(m, none) == -1);
}

TEST_CASE("mapped_lane_display_number: an empty lane still reports its number",
          "[filament_mapper][lane_number]") {
    // The chip already flags an empty mapped lane with a warning border. The
    // number is what tells the user which bay to go load.
    std::vector<AvailableSlot> slots = {
        {0, 0, 0x000000, "", true, -1},
    };
    slots[0].local_slot_index = 2;

    ToolMapping m;
    m.mapped_slot = 0;
    m.mapped_backend = 0;

    CHECK(FilamentMapper::mapped_lane_display_number(m, slots) == 3);
}
