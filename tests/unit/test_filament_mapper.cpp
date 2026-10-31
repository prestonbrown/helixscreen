// Copyright (C) 2025-2026 356C LLC
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

#include <optional>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// =============================================================================
// color_distance / colors_match
// =============================================================================

TEST_CASE("color_distance weighs channels by luminance", "[filament_mapper][color]") {
    for (const uint32_t c : {0x000000u, 0xFF0000u, 0xABCDEFu}) {
        INFO(std::hex << c);
        CHECK(FilamentMapper::color_distance(c, c) == 0);
    }
    CHECK(FilamentMapper::color_distance(0xFF0000, 0x00FF00) ==
          FilamentMapper::color_distance(0x00FF00, 0xFF0000));
    CHECK(FilamentMapper::color_distance(0x123456, 0x654321) ==
          FilamentMapper::color_distance(0x654321, 0x123456));
    CHECK(FilamentMapper::color_distance(0x000000, 0x001000) >=
          FilamentMapper::color_distance(0x000000, 0x000010));

    // sqrt(255^2 * weight / 100) per channel: 30 red, 59 green, 11 blue; black
    // to white stays under the unweighted 441.
    struct Row {
        const char* name;
        uint32_t to;
        int lo, hi;
    };
    const Row rows[] = {
        {"red", 0xFF0000, 130, 150},
        {"green", 0x00FF00, 190, 200},
        {"blue", 0x0000FF, 80, 90},
        {"white", 0xFFFFFF, 200, 300},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const int dist = FilamentMapper::color_distance(0x000000, row.to);
        CHECK(dist > row.lo);
        CHECK(dist < row.hi);
    }
}

TEST_CASE("colors_match holds within the tolerance", "[filament_mapper][color]") {
    struct Row {
        uint32_t a, b;
        bool match;
    };
    const Row rows[] = {
        {0xFF0000, 0xFF0000, true},  {0x800000, 0x8F0000, true}, // red shift of 15: distance ~8
        {0xFF0000, 0xF00000, true},  {0x00FF00, 0x00F000, true},
        {0xFF0000, 0x00FF00, false}, {0x000000, 0xFFFFFF, false},
    };
    for (const auto& row : rows) {
        INFO(std::hex << row.a << " vs " << row.b);
        CHECK(FilamentMapper::colors_match(row.a, row.b) == row.match);
    }
}

// =============================================================================
// materials_match
// =============================================================================

TEST_CASE("materials_match compares base materials and their compatibility groups",
          "[filament_mapper][material]") {
    const std::string long_name(200, 'X');
    struct Row {
        std::string a, b;
        bool match;
    };
    const Row rows[] = {
        {"PLA", "PLA", true},
        // Case-insensitive.
        {"PLA", "pla", true},
        {"Pla", "pLA", true},
        {"PETG", "petg", true},
        // One compatibility group.
        {"PLA", "PLA+", true},
        // A brand's product name over a known base material.
        {"PLA", "PLA SnapSpeed", true},
        {"PLA SnapSpeed", "PLA", true},
        {"PLA SnapSpeed", "PLA SnapSpeed", true},
        {"PETG", "PETG Pro", true},
        {"ABS", "ABS Premium", true},
        {"PLA SnapSpeed", "PETG", false},
        {"ABS Premium", "PLA", false},
        // Different bases.
        {"PLA", "PETG", false},
        {"PLA", "ASA", false},
        {"PLA", "ABS", false},
        {"ASA", "PETG", false},
        {"ASA", "PLA", false},
        {"PETG", "TPU", false},
        // Both unknown match; unknown against known does not.
        {"", "", true},
        {"", "PLA", false},
        {"PLA", "", false},
        {long_name, long_name, true},
        {long_name, long_name + "Y", true},
        // Surrounding whitespace is trimmed.
        {"PLA ", "PLA", true},
        {"PLA", " PLA", true},
        {"PLA ", "PLA ", true},
    };
    for (const auto& row : rows) {
        INFO("\"" << row.a << "\" vs \"" << row.b << "\"");
        CHECK(FilamentMapper::materials_match(row.a, row.b) == row.match);
    }
}

// =============================================================================
// find_closest_color_slot
// =============================================================================

TEST_CASE("find_closest_color_slot takes the closest loaded lane within tolerance",
          "[filament_mapper][slot]") {
    struct Row {
        const char* name;
        std::vector<AvailableSlot> slots;
        SlotKey want;
    };
    const Row rows[] = {
        {"no slots", {}, {-1, -1}},
        // An empty lane's stale colour never attracts a match, even exact.
        {"an empty exact lane and a far loaded one",
         {{0, 0, 0xFF0000, "PLA", true, -1}, {1, 0, 0x00FF00, "PLA", false, -1}},
         {-1, -1}},
        {"a loaded lane over an empty one of the same colour",
         {{0, 0, 0xFF0000, "PLA", true, -1}, {1, 0, 0xFF0000, "PLA", false, -1}},
         {1, 0}},
        {"the closest of several",
         {{0, 0, 0xFF0000, "PLA", false, -1},
          {1, 0, 0xF00000, "PLA", false, -1},
          {2, 0, 0x00FF00, "PLA", false, -1}},
         {0, 0}},
        {"nothing within tolerance",
         {{0, 0, 0x00FF00, "PLA", false, -1}, {1, 0, 0x0000FF, "PLA", false, -1}},
         {-1, -1}},
        {"the first of two backends' equal lanes",
         {{0, 0, 0xFF0000, "PLA", false, -1}, {0, 1, 0xFF0000, "PLA", false, -1}},
         {0, 0}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        CHECK(FilamentMapper::find_closest_color_slot(0xFF0000, "", row.slots) == row.want);
    }
}

// =============================================================================
// compute_defaults
// =============================================================================
// A tool takes, in order: the lane firmware maps to it, the closest colour
// within tolerance among lanes of a compatible material, its own positional
// lane, then any unclaimed lane not known to be incompatible. An empty lane
// prints nothing, so no step takes one; a tool nothing fits stays AUTO. Lanes
// may be shared, and material_mismatch flags a mapped lane whose known material
// differs from the tool's.

TEST_CASE("compute_defaults maps each tool to a lane", "[filament_mapper][compute]") {
    using R = ToolMapping::MatchReason;
    struct Want {
        int slot;
        std::optional<R> reason = std::nullopt;
        std::optional<bool> mismatch = std::nullopt;
        int backend = -1; // checked when >= 0
    };
    struct Row {
        const char* name;
        std::vector<GcodeToolInfo> tools;
        std::vector<AvailableSlot> slots;
        std::vector<Want> want; // one per tool, in tool order
    };
    const Row rows[] = {
        {"no tools, no slots", {}, {}, {}},
        {"no tools but slots", {}, {{0, 0, 0xFF0000, "PLA", false, -1}}, {}},
        {"tools but no slots", {{0, 0xFF0000, "PLA"}}, {}, {{-1, R::AUTO}}},
        {"one tool, one matching slot",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}},
         {{0, R::COLOR_MATCH, false, 0}}},
        {"firmware mapping beats a colour match",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0x00FF00, "PLA", false, 0}},
         {{1, R::FIRMWARE_MAPPING}}},
        {"firmware mapping onto the wrong material",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PETG", false, 0}},
         {{0, R::FIRMWARE_MAPPING, true}}},
        {"an empty firmware-mapped lane",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", true, 0}},
         {{-1, R::AUTO}}},
        {"two lanes firmware-mapped to one tool: the first",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, 0}, {1, 0, 0x00FF00, "PLA", false, 0}},
         {{0, R::FIRMWARE_MAPPING}}},
        {"wrong material skips the colour match for the positional lane",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PETG", false, -1}},
         {{0, std::nullopt, true}}},
        {"the free-lane fallback refuses a known-incompatible lane",
         {{0, 0xFF0000, "PLA"}},
         {{3, 0, 0xFF0000, "PETG", false, -1}},
         {{-1, R::AUTO}}},
        {"the free-lane fallback routes around to a compatible lane",
         {{0, 0xFF0000, "PLA"}},
         {{3, 0, 0xFF0000, "PETG", false, -1}, {4, 0, 0x0000FF, "PLA", false, -1}},
         {{4, std::nullopt, false}}},
        // A lane with no reported material cannot be proven incompatible.
        {"the free-lane fallback fills an unknown-material lane",
         {{0, 0xFF0000, "PLA"}},
         {{3, 0, 0x00FF00, "", false, -1}},
         {{3}}},
        {"material case is ignored",
         {{0, 0xFF0000, "pla"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}},
         {{0, std::nullopt, false}}},
        {"no colour match falls to the positional lane",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0x00FF00, "PLA", false, -1}},
         {{0}}},
        {"three tools, three matching lanes",
         {{0, 0xFF0000, "PLA"}, {1, 0x00FF00, "PLA"}, {2, 0x0000FF, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1},
          {1, 0, 0x00FF00, "PLA", false, -1},
          {2, 0, 0x0000FF, "PLA", false, -1}},
         {{0, R::COLOR_MATCH, false}, {1, R::COLOR_MATCH, false}, {2, R::COLOR_MATCH, false}}},
        {"two same-colour tools share the best lane",
         {{0, 0xFF0000, "PLA"}, {1, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0xF00000, "PLA", false, -1}},
         {{0}, {0}}},
        {"two same-colour tools share the only close lane",
         {{0, 0xFF0000, "PLA"}, {1, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0x00FF00, "PLA", false, -1}},
         {{0}, {0}}},
        // Empty lanes report stale colours.
        {"empty lanes match nothing",
         {{0, 0xFF0000, "PLA"}, {1, 0x00FF00, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", true, -1}, {1, 0, 0x00FF00, "PLA", true, -1}},
         {{-1}, {-1}}},
        {"every lane empty",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0x000000, "", true, -1}, {1, 0, 0x000000, "", true, -1}},
         {{-1}}},
        {"firmware, colour and positional in one print",
         {{0, 0xFF0000, "PLA"}, {1, 0x00FF00, "PLA"}, {2, 0x0000FF, "PETG"}},
         {{0, 0, 0xFF0000, "PLA", false, 0},
          {1, 0, 0x00FF00, "PLA", false, -1},
          {2, 0, 0xFFFF00, "ABS", false, -1}},
         {{0, R::FIRMWARE_MAPPING}, {1, R::COLOR_MATCH}, {2, std::nullopt, true}}},
        {"an unknown tool material flags nothing",
         {{0, 0xFF0000, ""}},
         {{0, 0, 0xFF0000, "ASA", false, -1}},
         {{0, std::nullopt, false}}},
        {"an unknown lane material flags nothing",
         {{0, 0xFFFFFF, "PETG"}},
         {{0, 0, 0xFFFFFF, "", false, -1}},
         {{0, std::nullopt, false}}},
        {"a single tool on the wrong material",
         {{0, 0xFFFFFF, "PETG"}},
         {{0, 0, 0xFFFFFF, "PLA", false, -1}},
         {{0, std::nullopt, true}}},
        {"PLA+ is compatible with PLA",
         {{0, 0xFFFFFF, "PLA"}},
         {{0, 0, 0xFFFFFF, "PLA+", false, -1}},
         {{0, std::nullopt, false}}},
        {"the lane's backend is reported",
         {{0, 0xFF0000, "PLA"}},
         {{0, 2, 0xFF0000, "PLA", false, -1}},
         {{0, std::nullopt, std::nullopt, 2}}},
        {"two backends' slot 0: both tools take the first",
         {{0, 0xFF0000, "PLA"}, {1, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {0, 1, 0xFF0000, "PLA", false, -1}},
         {{0, std::nullopt, std::nullopt, 0}, {0, std::nullopt, std::nullopt, 0}}},
        {"a firmware mapping names its own backend",
         {{0, 0xFF0000, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {0, 1, 0x00FF00, "PLA", false, 0}},
         {{0, R::FIRMWARE_MAPPING, std::nullopt, 1}}},
        {"a colour match onto the wrong material",
         {{0, 0xFF0000, "PLA"}, {1, 0x0000FF, "PLA"}},
         {{0, 0, 0xFF0000, "PLA", false, -1}, {1, 0, 0x0000FF, "ASA", false, -1}},
         {{0, std::nullopt, false}, {1, std::nullopt, true}}},
        {"firmware onto the wrong material beside a clean colour match",
         {{0, 0xFF0000, "PLA"}, {1, 0x00FF00, "PETG"}},
         {{0, 0, 0xFF0000, "ASA", false, 0}, {1, 0, 0x00FF00, "PETG", false, -1}},
         {{0, R::FIRMWARE_MAPPING, true}, {1, R::COLOR_MATCH, false}}},
        // ASA sits in ABS's compatibility group.
        {"per-tool mismatches across firmware mappings",
         {{0, 0xFF0000, "ABS"}, {1, 0x00FF00, "ABS"}, {2, 0x0000FF, "ABS"}},
         {{0, 0, 0xFF0000, "PLA", false, 0},
          {1, 0, 0x00FF00, "ASA", false, 1},
          {2, 0, 0x0000FF, "PETG", false, 2}},
         {{0, std::nullopt, true}, {1, std::nullopt, false}, {2, std::nullopt, true}}},
        // With lanes of one material loaded, matching a stand-in colour is a
        // coin toss, so an unknown colour takes the positional lane (0) and not
        // the exact grey (1).
        {"an unknown colour never takes a colour match",
         {{0, 0x808080, "ASA", /*color_known=*/false}},
         {{0, 0, 0x111111, "ASA", false, -1}, {1, 0, 0x808080, "ASA", false, -1}},
         {{0}}},
        {"a known colour takes the colour match",
         {{0, 0x808080, "ASA"}},
         {{0, 0, 0x111111, "ASA", false, -1}, {1, 0, 0x808080, "ASA", false, -1}},
         {{1, R::COLOR_MATCH}}},
        {"a firmware mapping wins for an unknown colour",
         {{0, 0x808080, "ASA", /*color_known=*/false}},
         {{0, 0, 0x111111, "ASA", false, -1}, {1, 0, 0x222222, "ASA", false, 0}},
         {{1, R::FIRMWARE_MAPPING}}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto result = FilamentMapper::compute_defaults(row.tools, row.slots);
        REQUIRE(result.size() == row.want.size());
        for (size_t i = 0; i < result.size(); ++i) {
            INFO("tool " << i);
            const auto& got = result[i];
            const auto& want = row.want[i];
            CHECK(got.tool_index == row.tools[i].tool_index);
            CHECK(got.mapped_slot == want.slot);
            CHECK(got.is_auto == (want.slot < 0));
            if (want.reason)
                CHECK(got.reason == *want.reason);
            if (want.mismatch)
                CHECK(got.material_mismatch == *want.mismatch);
            if (want.backend >= 0)
                CHECK(got.mapped_backend == want.backend);
        }
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

TEST_CASE("identity_filtered_remap sends only the primary backend's slots",
          "[filament_mapper][remap][multi_backend]") {
    // The remap goes out against backend 0's routing, where slot 2 is backend 0's
    // own lane 3. A pick of backend 1's slot 2 names a different lane.
    std::vector<ToolMapping> m(3);
    m[0].tool_index = 0;
    m[0].mapped_slot = 1;
    m[0].mapped_backend = 0;
    m[1].tool_index = 1;
    m[1].mapped_slot = 2;
    m[1].mapped_backend = 1;
    m[2].tool_index = 2;
    m[2].mapped_slot = 3;
    m[2].mapped_backend = -1; // primary
    const std::map<int, int> expected = {{0, 1}, {2, 3}};
    CHECK(FilamentMapper::identity_filtered_remap(m, helix::FirmwareRouting::identity()) ==
          expected);
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
    red.color_rgb = 0xFE0101; // within kColorMatchTolerance of the lane
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

    SECTION("a tool with no lane to compare against comes back unresolved") {
        // The other half of guard 2, and all that can be asserted on this side of
        // it: with no lanes there is nothing to classify, so the tool is AUTO with
        // no slot. That the surround stays off for such a tool is not assertable
        // here - classify_mismatches() takes a resolved AvailableSlot&, so an
        // unresolved tool cannot reach it and a color_mismatch assertion would be
        // reading a default-initialised bool. The render side is where that
        // becomes observable, and it is pinned there against a real chip.
        std::vector<GcodeToolInfo> tools = {{0, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, {});
        REQUIRE(result.size() == 1);
        CHECK(result[0].is_auto);
        CHECK(result[0].mapped_slot == -1);
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

TEST_CASE("resolve_display_colors shows a mapped tool's lane colour, else the slicer's",
          "[filament]") {
    const std::vector<GcodeToolInfo> tools = {tool(0, 0xFFFF00)}; // slicer yellow
    struct Row {
        const char* name;
        ToolMapping mapping;
        std::vector<AvailableSlot> slots;
        uint32_t want;
    };
    const Row rows[] = {
        {"mapped to a loaded blue lane", mapped(0, 3, 0), {slot(3, 0, 0x0000FF)}, 0x0000FF},
        {"the same lane reloaded green", mapped(0, 3, 0), {slot(3, 0, 0x00FF00)}, 0x00FF00},
        {"unmapped", unresolved(0), {slot(3, 0, 0x0000FF)}, 0xFFFF00},
        {"another lane recoloured",
         mapped(0, 2, 0),
         {slot(0, 0, 0x123456), slot(2, 0, 0x00FF00)},
         0x00FF00},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto colors = FilamentMapper::resolve_display_colors(tools, {row.mapping}, row.slots);
        REQUIRE(colors.size() == 1);
        CHECK(colors[0] == row.want);
    }
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

TEST_CASE("effective_mappings auto ON: a tool of unstated colour keeps the firmware's lane",
          "[filament_mapper][effective][routing]") {
    // The printer routes T1 to lane 3. A file that states no colour gives the
    // matcher nothing to go on, so the tool stays where the firmware sends it;
    // any other lane would go out as a remap over the printer's own table.
    GcodeToolInfo t1{1, 0x808080, "PLA"};
    t1.color_known = false;
    const std::vector<GcodeToolInfo> tools{t1};
    const std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, 0},
        {1, 0, 0x00FF00, "PLA", false, 3},
        {2, 0, 0x0000FF, "PLA", false, 2},
        {3, 0, 0xFFFF00, "PLA", false, 1},
    };
    FirmwareRouting routing;
    routing.head_for_tool = {0, 3, 2, 1};

    auto routed = FilamentMapper::effective_mappings(tools, slots, true, routing);
    REQUIRE(routed.size() == 1);
    CHECK(routed[0].mapped_slot == 3);
    // Following the printer is not a match the matcher made, so print start
    // must not send it.
    CHECK(routed[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);

    SECTION("a tool the routing gives no lane falls back to a free loaded lane") {
        routing.head_for_tool = {0, -1, 2, 3};
        auto unrouted = FilamentMapper::effective_mappings(tools, slots, true, routing);
        REQUIRE(unrouted.size() == 1);
        CHECK(unrouted[0].mapped_slot == 0);
    }

    SECTION("identity routing still takes the tool's own lane") {
        auto identity = FilamentMapper::effective_mappings(tools, slots, true);
        REQUIRE(identity.size() == 1);
        CHECK(identity[0].mapped_slot == 1);
    }
}

TEST_CASE("apply_picks lays only the user's picks over the current mappings",
          "[filament_mapper][routing]") {
    // The remap modal returns the mappings as they stood when it opened. Only
    // the rows the user changed are news; the rest may have followed the
    // printer since, and the snapshot must not drag them back.
    std::vector<ToolMapping> current(2);
    current[0].tool_index = 0;
    current[0].mapped_slot = 3; // followed the printer while the modal was open
    current[1].tool_index = 1;
    current[1].mapped_slot = 1;

    std::vector<ToolMapping> from_modal(2);
    from_modal[0].tool_index = 0;
    from_modal[0].mapped_slot = 0; // the stale snapshot
    from_modal[1].tool_index = 1;
    from_modal[1].mapped_slot = 2;
    from_modal[1].user_picked = true;

    FilamentMapper::apply_picks(current, from_modal);
    CHECK(current[0].mapped_slot == 3);
    CHECK_FALSE(current[0].user_picked);
    CHECK(current[1].mapped_slot == 2);
    CHECK(current[1].user_picked);
}

// The G-code viewer indexes its colour overrides by tool number, so a print
// using T0 and T2 gets a three-entry vector with T1 neutral. Most printers have
// no filament system: their tools keep the slicer's colour, and the 0x808080
// placeholder appears only where the slicer gave none.
TEST_CASE("effective_tool_colors lays each tool's colour at its tool number",
          "[filament_mapper][effective]") {
    struct Row {
        const char* name;
        std::vector<GcodeToolInfo> tools;
        std::vector<AvailableSlot> slots;
        bool auto_map;
        std::vector<uint32_t> want;
    };
    const Row rows[] = {
        {"no tools", {}, {{0, 0, 0xAA0000, "PLA", false, -1}}, true, {}},
        {"T0 and T2 matched to lanes",
         {{0, 0xAA0000, "PLA"}, {2, 0x0000BB, "PLA"}},
         {{0, 0, 0xAA0000, "PLA", false, -1},
          {1, 0, 0x123456, "PLA", false, -1},
          {2, 0, 0x0000BB, "PLA", false, -1}},
         true,
         {0xAA0000, 0x808080, 0x0000BB}},
        {"no lanes: the slicer colour", {{0, 0x00A899, ""}}, {}, false, {0x00A899}},
        {"no lanes and the slicer's default", {{0, 0x808080, ""}}, {}, false, {0x808080}},
        {"no lanes, T0 and T2",
         {{0, 0xED1C24, ""}, {2, 0x00C502, ""}},
         {},
         false,
         {0xED1C24, 0x808080, 0x00C502}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        CHECK(FilamentMapper::effective_tool_colors(row.tools, row.slots, row.auto_map) ==
              row.want);
    }
}

// =============================================================================
// routed_tool_colors — a degenerate routing must not paint over the palette
// =============================================================================

constexpr auto Unvouched = helix::printer::ToolMappingOrigin::Unvouched;
constexpr auto Deliberate = helix::printer::ToolMappingOrigin::Deliberate;
//
// routed_tool_colors() answers the print-status preview's per-tool colour
// overrides. An empty return leaves the renderer's own slicer palette in place;
// a non-empty one replaces it. So an answer that carries no per-tool
// information is worse than no answer: it trades a palette that may be right
// for one that is definitely useless.

TEST_CASE("routed_tool_colors: every routed tool on one head is degenerate",
          "[filament_mapper][routing]") {
    // Observed on a real rig: a 4-tool file whose routing sent every tool to
    // head 0, with head 0 loaded. That resolves to [purple, purple, purple,
    // purple] - not neutral, so it passed the all-grey guard and painted the
    // whole model one solid colour over a perfectly good 4-entry palette.
    const std::vector<AvailableSlot> one_loaded = {
        {0, 0, 0xA03CF7, "PLA", false, -1},
    };

    SECTION("a multi-tool routing with one answer for all returns nothing") {
        REQUIRE(FilamentMapper::routed_tool_colors({0, 0, 0, 0}, one_loaded, Unvouched).empty());
    }

    SECTION("a single-tool print legitimately routes its only tool anywhere") {
        // One routed tool has nothing to tell apart; the lane's colour IS the
        // answer.
        const auto colors = FilamentMapper::routed_tool_colors({0}, one_loaded, Unvouched);
        REQUIRE(colors.size() == 1);
        CHECK(colors[0] == 0xA03CF7);
    }

    SECTION("routed tools collapsing onto one head among unrouted ones is still degenerate") {
        // -1 entries are "no opinion" and count toward neither side; the two
        // tools that DO route share head 0, so the answer carries no per-tool
        // information and the slicer palette stands.
        REQUIRE(FilamentMapper::routed_tool_colors({-1, 0, -1, 0}, one_loaded, Unvouched).empty());
    }
}

TEST_CASE("routed_tool_colors: distinct heads with identical spool colours still publish",
          "[filament_mapper][routing]") {
    // The degeneracy signal is the ROUTING collapsing, never the colours
    // agreeing: two lanes holding the same white spool (a runout backup
    // pair) are a true uniform answer, and suppressing it would put the
    // slicer's multi-colour palette up for a print that comes out solid.
    const std::vector<AvailableSlot> two_white = {
        {0, 0, 0xFFFFFF, "PLA", false, -1},
        {1, 0, 0xFFFFFF, "PLA", false, -1},
    };

    SECTION("all-equal colours on distinct heads are returned, not discarded") {
        const auto colors = FilamentMapper::routed_tool_colors({0, 1}, two_white, Unvouched);
        REQUIRE(colors.size() == 2);
        CHECK(colors[0] == 0xFFFFFF);
        CHECK(colors[1] == 0xFFFFFF);
    }

    SECTION("a four-lane identity routing of same-colour spools is published") {
        const std::vector<AvailableSlot> four_white = {
            {0, 0, 0xFFFFFF, "PLA", false, -1},
            {1, 0, 0xFFFFFF, "PLA", false, -1},
            {2, 0, 0xFFFFFF, "PLA", false, -1},
            {3, 0, 0xFFFFFF, "PLA", false, -1},
        };
        const auto colors = FilamentMapper::routed_tool_colors({0, 1, 2, 3}, four_white, Unvouched);
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == 0xFFFFFF);
        CHECK(colors[3] == 0xFFFFFF);
    }
}

TEST_CASE("routed_tool_colors: a routing that distinguishes tools keeps answering",
          "[filament_mapper][routing]") {
    // Pins the existing behaviour either side of the degeneracy guard so the
    // guard cannot silently widen: distinct routings answer with their lanes'
    // colours, and a partially-unknown routing keeps its neutral gap.
    const std::vector<AvailableSlot> slots = {
        {0, 0, 0x080A0D, "PLA", false, -1},
        {1, 0, 0xE2DEDB, "PLA", false, -1},
        {2, 0, 0xE72F1D, "PLA", false, -1},
        {3, 0, 0xF4C032, "PLA", false, -1},
    };

    SECTION("four tools on four distinct heads return the four colours") {
        const auto colors = FilamentMapper::routed_tool_colors({0, 1, 2, 3}, slots, Unvouched);
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == 0x080A0D);
        CHECK(colors[1] == 0xE2DEDB);
        CHECK(colors[2] == 0xE72F1D);
        CHECK(colors[3] == 0xF4C032);
    }

    SECTION("a tool with no routing entry stays neutral, the rest answer") {
        const auto colors = FilamentMapper::routed_tool_colors({0, -1, 1, 2}, slots, Unvouched);
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == 0x080A0D);
        CHECK(colors[1] == 0x808080);
        CHECK(colors[2] == 0xE2DEDB);
        CHECK(colors[3] == 0xE72F1D);
    }
}

TEST_CASE("routed_tool_colors: a deliberate one-head routing is the answer, not an artifact",
          "[filament_mapper][routing][1422]") {
    // Pointing several tools at one lane is something our own UI offers, with a
    // toast rather than a refusal. The print then comes out one solid colour,
    // and the uniform lane colour is the only honest preview of it. The shape is
    // identical to a table nobody chose, so provenance is the whole difference.
    const std::vector<AvailableSlot> one_loaded = {
        {0, 0, 0xA03CF7, "PLA", false, -1},
    };

    SECTION("four tools deliberately sharing one lane publish that lane's colour") {
        const auto colors =
            FilamentMapper::routed_tool_colors({0, 0, 0, 0}, one_loaded, Deliberate);
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == 0xA03CF7);
        CHECK(colors[1] == 0xA03CF7);
        CHECK(colors[2] == 0xA03CF7);
        CHECK(colors[3] == 0xA03CF7);
    }

    SECTION("the same routing with nothing behind it still stands aside") {
        REQUIRE(FilamentMapper::routed_tool_colors({0, 0, 0, 0}, one_loaded, Unvouched).empty());
    }

    SECTION("a deliberate share among unrouted tools publishes too") {
        const auto colors =
            FilamentMapper::routed_tool_colors({-1, 0, -1, 0}, one_loaded, Deliberate);
        REQUIRE(colors.size() == 4);
        CHECK(colors[0] == 0x808080);
        CHECK(colors[1] == 0xA03CF7);
        CHECK(colors[2] == 0x808080);
        CHECK(colors[3] == 0xA03CF7);
    }
}

TEST_CASE("routed_tool_colors: provenance cannot conjure a colour out of nothing",
          "[filament_mapper][routing][1422]") {
    // Deliberate says who chose the routing, never what the lanes hold. An
    // all-neutral answer is still nothing worth pushing at the renderer.
    const std::vector<AvailableSlot> empty_lane = {
        {0, 0, 0xA03CF7, "PLA", true, -1},
    };
    REQUIRE(FilamentMapper::routed_tool_colors({0, 0}, empty_lane, Deliberate).empty());
}

TEST_CASE("mapped_lane_display_number names the mapped lane within its unit",
          "[filament_mapper][lane_number]") {
    const auto lane = [](int slot, int backend, int local, bool empty = false, int unit = 0) {
        AvailableSlot s{slot, backend, 0xFF0000, "PLA", empty, -1};
        s.local_slot_index = local;
        s.unit_index = unit;
        return s;
    };
    const auto to = [](int slot, int backend, bool is_auto = false) {
        ToolMapping m;
        m.mapped_slot = slot;
        m.mapped_backend = backend;
        m.is_auto = is_auto;
        return m;
    };
    struct Row {
        const char* name;
        std::vector<AvailableSlot> slots;
        ToolMapping mapping;
        int want;
    };
    const Row rows[] = {
        // 1-based, as format_slot_label and the slot badges show it. Two lanes
        // of one colour are told apart only by this number.
        {"the second of two same-colour lanes", {lane(0, 0, 0), lane(1, 0, 1)}, to(1, 0), 2},
        {"the first of two same-colour lanes", {lane(0, 0, 0), lane(1, 0, 1)}, to(0, 0), 1},
        // The second unit's first bay is its slot 1, not slot 5.
        {"a lane of the second unit", {lane(4, 0, 0, false, 1)}, to(4, 0), 1},
        // Slot indices repeat across backends.
        {"slot 0 of the second backend", {lane(0, 0, 0), lane(0, 1, 3)}, to(0, 1), 4},
        // An empty lane is the one the user must go and load.
        {"an empty lane", {lane(0, 0, 2, true)}, to(0, 0), 3},
        // Auto is the firmware choosing at print time.
        {"an auto tool", {lane(0, 0, 0)}, to(0, 0, true), -1},
        {"an unmapped tool", {lane(0, 0, 0)}, to(-1, -1), -1},
        // A lane unplugged since the mapping was made.
        {"a lane no longer listed", {lane(0, 0, 0)}, to(7, 0), -1},
        {"no lanes at all", {}, to(7, 0), -1},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        CHECK(FilamentMapper::mapped_lane_display_number(row.mapping, row.slots) == row.want);
    }
}

// =============================================================================
// compute_defaults - the free-lane fallback is a guess, not a colour match
// (prestonbrown/helixscreen#1862)
// =============================================================================

namespace {
// T1 (red) colour-matches lane 0, which is T0's routed lane; T0 states no colour.
std::vector<GcodeToolInfo> claimed_route_tools() {
    GcodeToolInfo t0{0, 0x808080, "PLA"};
    t0.color_known = false;
    return {t0, {1, 0xFF0000, "PLA"}};
}

std::vector<AvailableSlot> claimed_route_slots() {
    return {
        {0, 0, 0xFF0000, "PLA", false, -1}, // red, T0's routed lane
        {1, 0, 0x00FF00, "PLA", false, -1}, // green, free
    };
}
} // namespace

TEST_CASE("compute_defaults: a free-lane fallback is not labelled a colour match",
          "[filament_mapper][compute][fallback]") {
    // Tools are seeded in list order, so T1 goes first: its colour match claims
    // lane 0 before T0 asks for its routed lane.
    auto tools = claimed_route_tools();
    std::swap(tools[0], tools[1]);

    auto result = FilamentMapper::compute_defaults(tools, claimed_route_slots());
    REQUIRE(result.size() == 2);
    CHECK(result[0].tool_index == 1);
    CHECK(result[0].mapped_slot == 0);
    CHECK(result[0].reason == ToolMapping::MatchReason::COLOR_MATCH);
    CHECK(result[1].tool_index == 0);
    CHECK(result[1].mapped_slot == 1); // the guess, still made where it is allowed
    CHECK(result[1].reason == ToolMapping::MatchReason::FREE_LANE_FALLBACK);
}

TEST_CASE("compute_defaults: a refused guess keeps the tool on its routed lane",
          "[filament_mapper][compute][fallback]") {
    SECTION("routed lane claimed by another tool's colour match") {
        auto tools = claimed_route_tools();
        std::swap(tools[0], tools[1]);
        auto result = FilamentMapper::compute_defaults(tools, claimed_route_slots(), {},
                                                       FreeLaneGuess::Refused);
        REQUIRE(result.size() == 2);
        CHECK(result[0].reason == ToolMapping::MatchReason::COLOR_MATCH);
        CHECK(result[1].tool_index == 0);
        CHECK(result[1].mapped_slot == 0);
        CHECK(result[1].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
    }

    SECTION("routed lane empty: the seed names it, so preflight sees it empty") {
        std::vector<GcodeToolInfo> tools = {{0, 0x0000FF, "PLA"}};
        std::vector<AvailableSlot> slots = {
            {0, 0, 0xFF0000, "PLA", true, -1},  // routed lane, empty
            {1, 0, 0x00FF00, "PLA", false, -1}, // loaded, would be the guess
        };
        auto allowed = FilamentMapper::compute_defaults(tools, slots);
        REQUIRE(allowed.size() == 1);
        CHECK(allowed[0].mapped_slot == 1);
        CHECK(allowed[0].reason == ToolMapping::MatchReason::FREE_LANE_FALLBACK);

        auto refused = FilamentMapper::compute_defaults(tools, slots, {}, FreeLaneGuess::Refused);
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].mapped_slot == 0);
        CHECK(refused[0].reason == ToolMapping::MatchReason::FIRMWARE_MAPPING);
        CHECK_FALSE(refused[0].is_auto);
    }

    SECTION("no routed lane at all leaves the tool unresolved") {
        std::vector<GcodeToolInfo> tools = {{3, 0x0000FF, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, claimed_route_slots(), {},
                                                       FreeLaneGuess::Refused);
        REQUIRE(result.size() == 1);
        CHECK(result[0].is_auto);
        CHECK(result[0].reason == ToolMapping::MatchReason::AUTO);
    }

    SECTION("a real colour match is unaffected") {
        std::vector<GcodeToolInfo> tools = {{0, 0x00FF00, "PLA"}};
        auto result = FilamentMapper::compute_defaults(tools, claimed_route_slots(), {},
                                                       FreeLaneGuess::Refused);
        REQUIRE(result.size() == 1);
        CHECK(result[0].mapped_slot == 1);
        CHECK(result[0].reason == ToolMapping::MatchReason::COLOR_MATCH);
    }
}

// =============================================================================
// pick_slot
// =============================================================================

TEST_CASE("pick_slot: a picked lane is marked user_picked and re-classified",
          "[filament_mapper][pick_slot]") {
    const GcodeToolInfo tool{0, 0xFF0000, "PLA"};
    const std::vector<AvailableSlot> slots = {
        {0, 0, 0xFF0000, "PLA", false, -1},
        {1, 0, 0xFF0000, "PETG", false, -1},
    };
    ToolMapping m;
    m.tool_index = 0;

    FilamentMapper::pick_slot(m, tool, slots, 1, 0, false);
    CHECK(m.user_picked);
    CHECK(m.mapped_slot == 1);
    CHECK(m.mapped_backend == 0);
    CHECK_FALSE(m.is_auto);
    CHECK(m.material_mismatch);

    FilamentMapper::pick_slot(m, tool, slots, 0, 0, false);
    CHECK_FALSE(m.material_mismatch);
}

TEST_CASE("pick_slot: Auto, or a lane no longer present, leaves no warnings",
          "[filament_mapper][pick_slot]") {
    const GcodeToolInfo tool{0, 0xFF0000, "PLA"};
    const std::vector<AvailableSlot> slots = {
        {0, 0, 0x0000FF, "PETG", false, -1},
    };

    ToolMapping a;
    a.material_mismatch = true;
    a.color_mismatch = true;
    a.reason = ToolMapping::MatchReason::COLOR_MATCH;
    FilamentMapper::pick_slot(a, tool, slots, -1, -1, true);
    CHECK(a.is_auto);
    CHECK(a.user_picked);
    CHECK(a.reason == ToolMapping::MatchReason::AUTO);
    CHECK_FALSE(a.material_mismatch);
    CHECK_FALSE(a.color_mismatch);

    ToolMapping gone;
    gone.material_mismatch = true;
    gone.color_mismatch = true;
    FilamentMapper::pick_slot(gone, tool, slots, 7, 0, false);
    CHECK(gone.user_picked);
    CHECK(gone.mapped_slot == 7);
    CHECK_FALSE(gone.material_mismatch);
    CHECK_FALSE(gone.color_mismatch);
}
