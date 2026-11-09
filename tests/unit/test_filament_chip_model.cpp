// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_chip_model.cpp
 * @brief What a FILAMENTS-card chip says: severity, slot label, dropdown ranking and reasons,
 *        the details row's material text, and the no-filament-system loaded comparison.
 *
 * Run with: ./build/bin/helix-tests "[filament_chip]"
 */

#include "filament_chip_model.h"
#include "test_helpers/zero_lane_numbering.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

AvailableSlot make_slot(int slot_index, uint32_t color, const std::string& material,
                        bool empty = false, int backend = 0, int unit = 0, int local = -1) {
    AvailableSlot s{slot_index, backend, color, material, empty, -1};
    s.unit_index = unit;
    s.local_slot_index = local < 0 ? slot_index : local;
    return s;
}

GcodeToolInfo make_tool(int index, uint32_t color, const std::string& material,
                        bool color_known = true) {
    return GcodeToolInfo{index, color, material, color_known};
}

ToolMapping mapped(int tool, int slot, int backend = 0) {
    ToolMapping m;
    m.tool_index = tool;
    m.mapped_slot = slot;
    m.mapped_backend = backend;
    return m;
}

} // namespace

// =============================================================================
// chip_severity
// =============================================================================

TEST_CASE("chip_severity: no lane, an empty lane and a different material block",
          "[filament_chip]") {
    const auto tool = make_tool(0, 0xFF0000, "PLA");
    CHECK(chip_severity(tool, nullptr) == ChipSeverity::Block);

    const auto empty = make_slot(0, 0xFF0000, "PLA", true);
    CHECK(chip_severity(tool, &empty) == ChipSeverity::Block);

    const auto petg = make_slot(0, 0xFF0000, "PETG");
    CHECK(chip_severity(tool, &petg) == ChipSeverity::Block);
}

TEST_CASE("chip_severity: a colour that differs warns only when the file states a colour",
          "[filament_chip]") {
    const auto blue = make_slot(0, 0x0000FF, "PLA");
    REQUIRE(FilamentMapper::color_distance(0xFF0000, 0x0000FF) >
            FilamentMapper::kColorMatchTolerance);

    CHECK(chip_severity(make_tool(0, 0xFF0000, "PLA"), &blue) == ChipSeverity::Warn);
    CHECK(chip_severity(make_tool(0, 0xFF0000, "PLA", false), &blue) == ChipSeverity::Ok);
}

TEST_CASE("chip_severity: an exact match, and Auto resolved to a matching lane, are Ok",
          "[filament_chip]") {
    const auto red = make_slot(0, 0xFF0000, "pla");
    CHECK(chip_severity(make_tool(0, 0xFF0000, "PLA"), &red) == ChipSeverity::Ok);

    // Auto: the caller resolves where the firmware will feed from, then asks.
    std::vector<AvailableSlot> slots = {make_slot(0, 0x00FF00, "PETG"),
                                        make_slot(1, 0xFF0000, "PLA")};
    ToolMapping m = mapped(0, 1);
    const auto* resolved = FilamentMapper::resolve_mapped_slot(m, slots);
    REQUIRE(resolved == &slots[1]);
    CHECK(chip_severity(make_tool(0, 0xFF0000, "PLA"), resolved) == ChipSeverity::Ok);
}

TEST_CASE("Auto prints from the lane the routing sends the tool to",
          "[filament_chip][filament_mapper]") {
    std::vector<AvailableSlot> slots = {make_slot(0, 0x00FF00, "PETG"),
                                        make_slot(1, 0xFF0000, "PLA")};
    ToolMapping automatic;
    automatic.tool_index = 1;
    automatic.is_auto = true;
    CHECK(FilamentMapper::resolve_printing_slot(automatic, slots, FirmwareRouting::identity()) ==
          &slots[1]);
    // A fixed-head table sending T1 to head 0.
    FirmwareRouting to_head_0;
    to_head_0.head_for_tool = {0, 0};
    CHECK(FilamentMapper::resolve_printing_slot(automatic, slots, to_head_0) == &slots[0]);
    // A tool the routing gives no head, and a head that is not among the slots.
    FirmwareRouting none;
    none.head_for_tool = {0, -1};
    CHECK(FilamentMapper::resolve_printing_slot(automatic, slots, none) == nullptr);
    automatic.tool_index = 5;
    CHECK(FilamentMapper::resolve_printing_slot(automatic, slots, FirmwareRouting::identity()) ==
          nullptr);
    // A pick resolves to the picked lane, whatever the routing says.
    CHECK(FilamentMapper::resolve_printing_slot(mapped(1, 0), slots, to_head_0) == &slots[0]);
}

// =============================================================================
// chip_slot_label
// =============================================================================

TEST_CASE("chip_slot_label: one unit prints the bare 1-based number", "[filament_chip]") {
    std::vector<AvailableSlot> slots = {make_slot(0, 0, "PLA"), make_slot(1, 0, "PLA"),
                                        make_slot(2, 0, "PLA"), make_slot(3, 0, "PLA")};
    CHECK(chip_slot_label(slots[2], slots) == "3");
}

TEST_CASE("chip_slot_label: counting from zero renumbers the slot, not the unit",
          "[filament_chip][numbering]") {
    std::vector<AvailableSlot> one_unit = {make_slot(0, 0, "PLA"), make_slot(1, 0, "PLA")};
    ScopedZeroLaneNumbering zero;
    CHECK(chip_slot_label(one_unit[0], one_unit) == "0");

    std::vector<AvailableSlot> two_units = one_unit;
    two_units[1].unit_index = 1;
    two_units[1].local_slot_index = 0;
    CHECK(chip_slot_label(two_units[1], two_units) ==
          std::string("2") + helix::ui::kUnitSeparator + "0");
}

TEST_CASE("chip_slot_label: two units in one backend name the unit", "[filament_chip]") {
    std::vector<AvailableSlot> slots = {
        make_slot(0, 0, "PLA", false, 0, 0, 0), make_slot(1, 0, "PLA", false, 0, 0, 1),
        make_slot(2, 0, "PLA", false, 0, 1, 0), make_slot(3, 0, "PLA", false, 0, 1, 1)};
    CHECK(chip_slot_label(slots[2], slots) == std::string("2") + helix::ui::kUnitSeparator + "1");
    CHECK(chip_slot_label(slots[1], slots) == std::string("1") + helix::ui::kUnitSeparator + "2");
}

TEST_CASE("chip_slot_label: two backends whose units are both index 0 still differ",
          "[filament_chip]") {
    std::vector<AvailableSlot> slots = {make_slot(0, 0, "PLA", false, 0, 0, 0),
                                        make_slot(0, 0, "PLA", false, 1, 0, 0)};
    CHECK(chip_slot_label(slots[0], slots) == std::string("1") + helix::ui::kUnitSeparator + "1");
    CHECK(chip_slot_label(slots[1], slots) == std::string("2") + helix::ui::kUnitSeparator + "1");
}

TEST_CASE("slot_row_label: the dropdown names the unit the chip numbers", "[filament_chip]") {
    // One unit: the lane alone.
    std::vector<AvailableSlot> one = {make_slot(0, 0, "PLA"), make_slot(1, 0, "PLA")};
    CHECK(slot_row_label(one[1], one) == "Slot 2");

    // Several units of one backend: the backend's own unit name.
    auto a = make_slot(0, 0, "PLA", false, 0, 0, 0);
    a.unit_display_name = "Turtle 1";
    auto b = make_slot(4, 0, "PLA", false, 0, 1, 0);
    b.unit_display_name = "Turtle 2";
    std::vector<AvailableSlot> turtles = {a, b};
    CHECK(chip_slot_label(turtles[1], turtles) == "2\xc2\xb7"
                                                  "1");
    CHECK(slot_row_label(turtles[1], turtles) == "Turtle 2 \xc2\xb7 Slot 1");

    // Two single-unit backends: no unit names, so the chip's unit number.
    std::vector<AvailableSlot> two = {make_slot(0, 0, "PLA", false, 0, 0, 0),
                                      make_slot(0, 0, "PLA", false, 1, 0, 0)};
    CHECK(chip_unit_number(two[0], two) == 1);
    CHECK(chip_unit_number(two[1], two) == 2);
    CHECK(slot_row_label(two[0], two) == "1 \xc2\xb7 Slot 1");
    CHECK(slot_row_label(two[1], two) == "2 \xc2\xb7 Slot 1");
    CHECK(slot_row_label(two[0], two) != slot_row_label(two[1], two));
}

// =============================================================================
// rank_slots_for_tool / slot_fit_text
// =============================================================================

TEST_CASE("rank_slots_for_tool: tiers come in order and keep the input order inside one",
          "[filament_chip]") {
    const auto tool = make_tool(0, 0xFF0000, "PLA");
    std::vector<AvailableSlot> slots = {
        make_slot(0, 0xFF0000, "PLA", true), // empty
        make_slot(1, 0xFF0000, "PETG"),      // material differs
        make_slot(2, 0x0000FF, "PLA"),       // colour differs
        make_slot(3, 0xFF0000, "PLA"),       // exact
        make_slot(4, 0x00FF00, "PLA"),       // colour differs
        make_slot(5, 0xFE0000, "PLA"),       // exact
    };
    const auto ranked = rank_slots_for_tool(tool, slots, {tool}, {mapped(0, 3)});
    REQUIRE(ranked.size() == slots.size());

    std::vector<std::size_t> order;
    std::vector<SlotFit> fits;
    for (const auto& r : ranked) {
        order.push_back(r.slot_pos);
        fits.push_back(r.fit);
    }
    CHECK(order == std::vector<std::size_t>{3, 5, 2, 4, 1, 0});
    CHECK(fits == std::vector<SlotFit>{SlotFit::Exact, SlotFit::Exact, SlotFit::ColorDiffers,
                                       SlotFit::ColorDiffers, SlotFit::MaterialDiffers,
                                       SlotFit::Empty});
}

TEST_CASE("rank_slots_for_tool: an unknown file colour never demotes a slot", "[filament_chip]") {
    const auto tool = make_tool(0, 0x808080, "PLA", false);
    std::vector<AvailableSlot> slots = {make_slot(0, 0x0000FF, "PLA"),
                                        make_slot(1, 0xFF0000, "PLA")};
    const auto ranked = rank_slots_for_tool(tool, slots, {tool}, {});
    REQUIRE(ranked.size() == 2);
    CHECK(ranked[0].slot_pos == 0);
    CHECK(ranked[0].fit == SlotFit::Exact);
    CHECK(ranked[1].fit == SlotFit::Exact);
}

TEST_CASE("rank_slots_for_tool: also_tools names the other tools on a slot, never self",
          "[filament_chip]") {
    const auto t0 = make_tool(0, 0xFF0000, "PLA");
    const auto t2 = make_tool(2, 0xFF0000, "PLA");
    const auto t3 = make_tool(3, 0xFF0000, "PLA");
    std::vector<AvailableSlot> slots = {make_slot(0, 0xFF0000, "PLA"),
                                        make_slot(1, 0xFF0000, "PLA")};
    const std::vector<GcodeToolInfo> tools = {t0, t2, t3};
    const std::vector<ToolMapping> mappings = {mapped(0, 0), mapped(2, 0), mapped(3, 1)};

    const auto ranked = rank_slots_for_tool(t0, slots, tools, mappings);
    REQUIRE(ranked.size() == 2);
    REQUIRE(ranked[0].slot_pos == 0);
    CHECK(ranked[0].also_tools == std::vector<int>{2});
    CHECK(ranked[1].also_tools == std::vector<int>{3});
}

TEST_CASE("slot_fit_text: the reason, then the other tools sharing the slot", "[filament_chip]") {
    RankedSlot r;
    r.fit = SlotFit::MaterialDiffers;
    r.also_tools = {2};
    CHECK(slot_fit_text(r) == "material differs, also T2");

    r.fit = SlotFit::Exact;
    r.also_tools = {};
    CHECK(slot_fit_text(r) == "exact match");
    r.fit = SlotFit::ColorDiffers;
    CHECK(slot_fit_text(r) == "color differs");
    r.fit = SlotFit::Empty;
    r.also_tools = {1, 3};
    CHECK(slot_fit_text(r) == "empty, also T1, T3");
}

// =============================================================================
// used_materials_text
// =============================================================================

TEST_CASE("used_materials_text: the used tools' materials, deduplicated in tool order",
          "[filament_chip]") {
    const std::vector<std::string> mats = {"ABS", "ABS", "ABS", "ABS", "ABS-GF"};
    CHECK(used_materials_text(mats, std::set<int>{4}) == "ABS-GF");
    CHECK(used_materials_text(mats, std::set<int>{0, 4}) == "ABS, ABS-GF");
    CHECK(used_materials_text(mats, std::nullopt) == "ABS, ABS-GF");
    CHECK(used_materials_text({}, std::nullopt).empty());
    CHECK(used_materials_text({}, std::set<int>{0}).empty());
}

// =============================================================================
// split_filament_types / filament_type_text
// =============================================================================

TEST_CASE("split_filament_types: one trimmed entry per tool, empties kept in place",
          "[filament_chip]") {
    CHECK(split_filament_types("PLA;PLA;PETG") == std::vector<std::string>{"PLA", "PLA", "PETG"});
    CHECK(split_filament_types(" PLA ; ;PETG") == std::vector<std::string>{"PLA", "", "PETG"});
    CHECK(split_filament_types("ABS") == std::vector<std::string>{"ABS"});
    CHECK(split_filament_types("").empty());
}

TEST_CASE("filament_type_text: each material once, in tool order", "[filament_chip]") {
    CHECK(filament_type_text("ABS;ABS;ABS;ABS") == "ABS");
    CHECK(filament_type_text("PLA;PETG;PLA") == "PLA, PETG");
    CHECK(filament_type_text("PLA") == "PLA");
    CHECK(filament_type_text("ABS;ABS;") == "ABS");
    CHECK(filament_type_text(";;PETG; ;") == "PETG");
    CHECK(filament_type_text(" PLA; PETG ") == "PLA, PETG");
    CHECK(filament_type_text("").empty());
    CHECK(filament_type_text(";;").empty());
}

// =============================================================================
// loaded_severity
// =============================================================================

TEST_CASE("loaded_severity: unknown and Swap are Ok; a Loaded material that differs blocks",
          "[filament_chip]") {
    const auto tool = make_tool(0, 0xFF0000, "PETG");

    LoadedFilament unknown;
    CHECK(loaded_severity(tool, unknown, LoadedRole::Loaded) == ChipSeverity::Ok);

    LoadedFilament pla;
    pla.known = true;
    pla.material = "PLA";
    pla.color_rgb = 0xFF0000;
    pla.color_known = true;
    CHECK(loaded_severity(tool, pla, LoadedRole::Swap) == ChipSeverity::Ok);
    CHECK(loaded_severity(tool, pla, LoadedRole::Loaded) == ChipSeverity::Block);

    LoadedFilament petg_blue = pla;
    petg_blue.material = "PETG";
    petg_blue.color_rgb = 0x0000FF;
    CHECK(loaded_severity(tool, petg_blue, LoadedRole::Loaded) == ChipSeverity::Warn);
    petg_blue.color_known = false;
    CHECK(loaded_severity(tool, petg_blue, LoadedRole::Loaded) == ChipSeverity::Ok);
}
