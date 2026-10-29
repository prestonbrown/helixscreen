// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_used_filter.cpp
 * @brief Unit tests for FilamentMappingCard::apply_used_tools_filter (the pure
 *        used-tools compaction helper) and the card's idempotent rebuild.
 *
 * The mapping card renders one chip (and one modal row) per slicer palette
 * entry. A 4-filament OrcaSlicer project that only uses tools 2 and 3 should
 * show only T2 and T3, not all four. apply_used_tools_filter compacts the
 * card's parallel tool_info_ / mappings_ vectors down to the tools the gcode
 * actually uses.
 *
 * Contract pinned here (test the pure seam directly with hand-built vectors —
 * no LVGL widgets or AMS state required):
 *  - a non-empty `used` set keeps only entries whose .tool_index is in the set,
 *    in BOTH vectors, in LOCKSTEP, preserving order and .tool_index;
 *  - nullopt  => no filter (show all);
 *  - empty set => no filter (show all, NOT zero — the safety rule that avoids
 *    blanking the card pre-parse / on the headless single-extruder path).
 *
 * The later test cases exercise the full render path (MappingCardRenderFixture,
 * built on LVGLUITestFixture + a mock AMS backend): the filament_chip_*
 * subjects the card publishes for each used tool, the filament_swatch chips
 * that bind them, and the "+N" pill past what the row or the subject cap holds.
 */

#include "ui_filament_mapping_card.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"

#include "../mapping_card_render_fixture.h"
#include "../test_helpers/mock_bypass.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "filament_chip_model.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"
#include "settings_manager.h"
#include "system_settings_manager.h"
#include "theme_manager.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::GcodeToolInfo;
using helix::ToolMapping;
using helix::ui::FilamentMappingCard;

namespace {

// Build a 4-entry palette: tool_info_[i].tool_index == i, mappings_[i].tool_index == i.
// mapped_slot is set to 100+i so lockstep/order can be verified independently.
std::vector<GcodeToolInfo> make_tool_info() {
    std::vector<GcodeToolInfo> ti;
    for (int i = 0; i < 4; ++i) {
        GcodeToolInfo t;
        t.tool_index = i;
        t.color_rgb = 0x100000u * static_cast<uint32_t>(i + 1);
        t.material = "PLA";
        ti.push_back(t);
    }
    return ti;
}

std::vector<ToolMapping> make_mappings() {
    std::vector<ToolMapping> m;
    for (int i = 0; i < 4; ++i) {
        ToolMapping tm;
        tm.tool_index = i;
        tm.mapped_slot = 100 + i;
        m.push_back(tm);
    }
    return m;
}

} // namespace

TEST_CASE("apply_used_tools_filter: keeps only used tools in lockstep",
          "[filament][mapping][filament_mapping][used_filter]") {
    auto ti = make_tool_info();
    auto m = make_mappings();

    FilamentMappingCard::apply_used_tools_filter(ti, m, std::set<int>{2, 3});

    REQUIRE(ti.size() == 2);
    REQUIRE(m.size() == 2);
    // Order + tool_index preserved.
    CHECK(ti[0].tool_index == 2);
    CHECK(ti[1].tool_index == 3);
    CHECK(m[0].tool_index == 2);
    CHECK(m[1].tool_index == 3);
    // Lockstep: mappings compacted to the SAME positions (mapped_slot 102, 103).
    CHECK(m[0].mapped_slot == 102);
    CHECK(m[1].mapped_slot == 103);
}

TEST_CASE("apply_used_tools_filter: nullopt leaves both vectors untouched (show all)",
          "[filament][mapping][filament_mapping][used_filter]") {
    auto ti = make_tool_info();
    auto m = make_mappings();

    FilamentMappingCard::apply_used_tools_filter(ti, m, std::nullopt);

    REQUIRE(ti.size() == 4);
    REQUIRE(m.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(ti[static_cast<size_t>(i)].tool_index == i);
        CHECK(m[static_cast<size_t>(i)].tool_index == i);
    }
}

TEST_CASE("apply_used_tools_filter: empty set leaves both vectors untouched (show all, not zero)",
          "[filament][mapping][filament_mapping][used_filter]") {
    auto ti = make_tool_info();
    auto m = make_mappings();

    FilamentMappingCard::apply_used_tools_filter(ti, m, std::set<int>{});

    // Empty => show all. Must NOT compact to zero.
    REQUIRE(ti.size() == 4);
    REQUIRE(m.size() == 4);
}

TEST_CASE("apply_used_tools_filter: single used tool preserves its real tool_index",
          "[filament][mapping][filament_mapping][used_filter]") {
    auto ti = make_tool_info();
    auto m = make_mappings();

    // Only T2 used — a genuinely single-tool file must keep the real index (2),
    // not collapse to a palette ordinal of 0.
    FilamentMappingCard::apply_used_tools_filter(ti, m, std::set<int>{2});

    REQUIRE(ti.size() == 1);
    REQUIRE(m.size() == 1);
    CHECK(ti[0].tool_index == 2);
    CHECK(m[0].tool_index == 2);
    CHECK(m[0].mapped_slot == 102);
}

TEST_CASE("apply_used_tools_filter: used tools not present in the palette are ignored",
          "[filament][mapping][filament_mapping][used_filter]") {
    auto ti = make_tool_info();
    auto m = make_mappings();

    // used contains a stray index (9) that has no palette entry — the filter
    // keeps the intersection (just T1) and never fabricates an entry.
    FilamentMappingCard::apply_used_tools_filter(ti, m, std::set<int>{1, 9});

    REQUIRE(ti.size() == 1);
    REQUIRE(m.size() == 1);
    CHECK(ti[0].tool_index == 1);
    CHECK(m[0].tool_index == 1);
}

TEST_CASE("find_by_tool_index: resolves by real tool_index on a compacted vector",
          "[filament][mapping][filament_mapping][used_filter]") {
    // After compaction the vector position no longer equals .tool_index. The
    // print-start mismatch dialogs look tools up by tool_index; positional
    // access (tool_info[tool_index]) would miss or mislabel. This pins the
    // lookup that replaced it.
    auto ti = make_tool_info();
    auto m = make_mappings();
    FilamentMappingCard::apply_used_tools_filter(ti, m, std::set<int>{2, 3}); // -> positions 0,1

    const auto* t2 = FilamentMappingCard::find_by_tool_index(ti, 2);
    const auto* t3 = FilamentMappingCard::find_by_tool_index(ti, 3);
    REQUIRE(t2 != nullptr);
    REQUIRE(t3 != nullptr);
    // Correct entries by identity (color set to 0x100000*(i+1) in make_tool_info).
    CHECK(t2->tool_index == 2);
    CHECK(t2->color_rgb == 0x100000u * 3u); // original palette index 2
    CHECK(t3->tool_index == 3);
    CHECK(t3->color_rgb == 0x100000u * 4u); // original palette index 3

    // Tools filtered out (0, 1) are no longer found — the dialogs iterate only
    // the used/unresolved tools, so a nullptr here means "not used", not "wrong".
    CHECK(FilamentMappingCard::find_by_tool_index(ti, 0) == nullptr);
    CHECK(FilamentMappingCard::find_by_tool_index(ti, 1) == nullptr);
    CHECK(FilamentMappingCard::find_by_tool_index(ti, 99) == nullptr);
}

TEST_CASE("find_by_tool_index: full palette resolves each tool to its own entry",
          "[filament][mapping][filament_mapping][used_filter]") {
    // Behaviour-preserving for the non-compacted case: position == tool_index,
    // so the lookup matches what the old positional access returned.
    auto ti = make_tool_info();
    for (int i = 0; i < 4; ++i) {
        const auto* t = FilamentMappingCard::find_by_tool_index(ti, i);
        REQUIRE(t != nullptr);
        CHECK(t->tool_index == i);
        CHECK(t->color_rgb == 0x100000u * static_cast<uint32_t>(i + 1));
    }
}

// ============================================================================
// The card holds what compute_mapping answers
// ============================================================================

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard holds what compute_mapping answers for the file",
                 "[filament_mapping][compute_mapping]") {
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF"};
    const std::vector<std::string> materials = {"PLA", "PETG", "PLA"};
    const std::optional<std::set<int>> used = std::set<int>{0, 2};

    const auto m = FilamentMappingCard::compute_mapping(colors, materials, used);
    REQUIRE(m.shows);
    CHECK(m.palette_colour_count == 3);
    CHECK(m.palette_tool_count == 3);
    // Seeded from the whole palette, then narrowed to the used tools.
    REQUIRE(m.tool_info.size() == 2);
    CHECK(m.tool_info[1].tool_index == 2);
    REQUIRE(m.mappings.size() == 2);
    CHECK(m.mappings[1].tool_index == 2);
    CHECK_FALSE(m.slots.empty());

    // The used set arriving before the palette, or after it: the same answer.
    SECTION("used tools first") {
        card.set_used_tools(used);
        card.update(colors, materials);
    }
    SECTION("palette first") {
        card.update(colors, materials);
        card.set_used_tools(used);
    }
    REQUIRE(card.should_show());
    const auto tools = card.get_tool_info();
    const auto mappings = card.get_mappings();
    REQUIRE(tools.size() == m.tool_info.size());
    REQUIRE(mappings.size() == m.mappings.size());
    for (size_t i = 0; i < tools.size(); ++i) {
        CHECK(tools[i].tool_index == m.tool_info[i].tool_index);
        CHECK(mappings[i].tool_index == m.mappings[i].tool_index);
        CHECK(mappings[i].mapped_slot == m.mappings[i].mapped_slot);
    }
    CHECK(card.get_available_slots().size() == m.slots.size());
    process_lvgl(100);
}

// ============================================================================
// Chip subjects (full render path)
// ============================================================================

namespace {

/// Load lane @p index with a colour and material. The card reads lanes through
/// AmsState, so this is the only way to drive a chip's severity: the card
/// classifies (tool, lane) itself rather than trusting a flag on the mapping.
void load_lane(helix::AmsBackendMock* mock, int index, uint32_t rgb, const char* material) {
    auto slot = mock->get_slot_info(index);
    slot.color_rgb = rgb;
    slot.material = material;
    mock->sync_external_identity(index, slot);
}

// One explicit mapping per tool (-1: none), so what a chip shows is a pure
// function of the lane chosen here rather than of the seeding heuristic.
std::vector<ToolMapping> mappings_to_slots(const std::vector<int>& slots) {
    std::vector<ToolMapping> m;
    m.reserve(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        ToolMapping tm;
        tm.tool_index = static_cast<int>(i);
        tm.mapped_slot = slots[i];
        tm.mapped_backend = slots[i] < 0 ? -1 : 0;
        tm.is_auto = false;
        tm.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;
        m.push_back(tm);
    }
    return m;
}

/// Give `rows` room for exactly @p n chips at this breakpoint's chip width.
void size_rows_for(lv_obj_t* rows, int n) {
    const int32_t gap = theme_manager_get_spacing("space_xs");
    const int32_t chip = theme_manager_get_spacing("filament_chip_width");
    REQUIRE(chip > 0);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_width(rows, n * chip + (n - 1) * gap);
    lv_obj_update_layout(rows);
    REQUIRE(FilamentMappingCard::chips_that_fit(lv_obj_get_content_width(rows), gap, chip) ==
            static_cast<size_t>(n));
}

lv_color_t border_of(lv_obj_t* obj) {
    return lv_obj_get_style_border_color(obj, LV_PART_MAIN);
}

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture, "The chip subjects for a three-tool file",
                 "[filament_mapping][chip]") {
    // One render, all three outlines: a matched pair, a colour that differs and
    // a tool with no lane. An outline that is always one colour fails here.
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x0000FF, "PETG");
    card.update({"#FF0000", "#00FF00", "#0000FF"}, {"PLA", "PETG", ""});
    card.set_mappings(mappings_to_slots({0, 1, -1}));
    REQUIRE(chip_count() == 3);

    // T0: exactly what the file asked for.
    CHECK(chip_string(0, "_material") == "PLA");
    CHECK(chip_int(0, "_file_color_known") == 1);
    CHECK((lv_color_to_u32(lv_subject_get_color(chip_subject(0, "_file_color"))) & 0xFFFFFF) ==
          0xFF0000u);
    CHECK(chip_string(0, "_slot") == "1");
    CHECK(chip_int(0, "_slot_state") == 1);
    CHECK((lv_color_to_u32(lv_subject_get_color(chip_subject(0, "_slot_color"))) & 0xFFFFFF) ==
          0xFF0000u);
    CHECK(chip_int(0, "_severity") == 0);

    // T1: right material, wrong colour.
    CHECK(chip_string(1, "_slot") == "2");
    CHECK(chip_int(1, "_severity") == 1);

    // T2: no material in the file, so the header names the tool; no lane.
    CHECK(chip_string(2, "_material") == "T2");
    CHECK(chip_string(2, "_slot").empty());
    CHECK(chip_int(2, "_slot_state") == 0);
    CHECK(chip_int(2, "_severity") == 2);

    // The XML half: the outline and the slot text follow the severity subject.
    lv_obj_t* const ok = chip(0);
    lv_obj_t* const warn = chip(1);
    lv_obj_t* const block = chip(2);
    REQUIRE(ok != nullptr);
    REQUIRE(warn != nullptr);
    REQUIRE(block != nullptr);
    CHECK(lv_color_eq(border_of(ok), theme_manager_get_color("text_muted")));
    CHECK(lv_color_eq(border_of(warn), theme_manager_get_color("warning")));
    CHECK(lv_color_eq(border_of(block), theme_manager_get_color("danger")));
    // No lane: the empty-spool icon stands in, and no word sits in the body.
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(block, "chip_pick"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(block, "chip_slot"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(ok, "chip_pick"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* const warn_slot = lv_obj_find_by_name(warn, "chip_slot");
    REQUIRE(warn_slot != nullptr);
    CHECK_FALSE(lv_obj_has_flag(warn_slot, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(warn_slot)) == "2");
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(block, "chip_spool"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(ok, "chip_spool"), LV_OBJ_FLAG_HIDDEN));

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "An empty slot outlines its chip red",
                 "[filament_mapping][chip]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    mock->force_slot_status(1, helix::SlotStatus::EMPTY);
    card.update({"#FF0000", "#FF0000"}, {"PLA", "PLA"});
    card.set_mappings(mappings_to_slots({0, 1}));
    REQUIRE(chip_count() == 2);

    CHECK(chip_int(0, "_severity") == 0);
    CHECK(chip_int(1, "_severity") == 2);
    CHECK(chip_int(1, "_slot_state") == 2);
    CHECK(chip_string(1, "_slot") == "2");
    lv_obj_t* const empty = chip(1);
    REQUIRE(empty != nullptr);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(empty, "chip_spool"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(empty, "chip_empty_ring"), LV_OBJ_FLAG_HIDDEN));

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A chip's severity and spool follow a spool swap rather than being decided once",
                 "[filament_mapping][chip]") {
    // refresh_slot_data() replaces available_slots_ and keeps mappings_, so a
    // verdict cached on the mapping would outlive the lane it judged. T1 is the
    // control: its lane matches throughout.
    load_lane(mock, 0, 0x0000FF, "PLA"); // blue lane, red file
    load_lane(mock, 1, 0x00FF00, "PLA"); // green lane, green file
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    card.set_mappings(mappings_to_slots({0, 1}));
    REQUIRE(chip_int(0, "_severity") == 1);
    REQUIRE(chip_int(1, "_severity") == 0);

    load_lane(mock, 0, 0xFF0000, "PLA");
    helix::ui::UpdateQueue::instance().drain();
    card.refresh_slot_data();
    CHECK(chip_int(0, "_severity") == 0);
    CHECK((lv_color_to_u32(lv_subject_get_color(chip_subject(0, "_slot_color"))) & 0xFFFFFF) ==
          0xFF0000u);
    CHECK(chip_int(1, "_severity") == 0);

    // And back, so the fix cannot be "severity only ever clears".
    load_lane(mock, 0, 0x0000FF, "PLA");
    helix::ui::UpdateQueue::instance().drain();
    card.refresh_slot_data();
    CHECK(chip_int(0, "_severity") == 1);
    CHECK((lv_color_to_u32(lv_subject_get_color(chip_subject(0, "_slot_color"))) & 0xFFFFFF) ==
          0x0000FFu);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "set_mappings republishes the slot label",
                 "[filament_mapping][remap]") {
    // A remap accepted from the modal or the picker arrives as set_mappings().
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(chip_count() == 2);

    card.set_mappings(mappings_to_slots({3, 1}));
    CHECK(chip_string(0, "_slot") == "4");
    card.set_mappings(mappings_to_slots({2, 1}));
    CHECK(chip_string(0, "_slot") == "3");
    CHECK(chip_string(1, "_slot") == "2");
    lv_obj_t* const label = lv_obj_find_by_name(chip(0), "chip_slot");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "3");

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Unknown gcode colour draws no fill on the header",
                 "[filament_mapping][chip]") {
    // Painting a stand-in reads as "this file prints in grey", a claim about
    // the file that nothing has made.
    card.update({"", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(chip_count() == 2);
    CHECK(chip_int(0, "_file_color_known") == 0);
    CHECK(chip_int(0, "_ink") == 0);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(chip(0), "chip_header_fill"), LV_OBJ_FLAG_HIDDEN));
    CHECK(chip_int(1, "_file_color_known") == 1);
    CHECK(chip_int(1, "_ink") != 0);
    CHECK_FALSE(
        lv_obj_has_flag(lv_obj_find_by_name(chip(1), "chip_header_fill"), LV_OBJ_FLAG_HIDDEN));

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "The chip's spool paints the slot's split and finish",
                 "[filament_mapping][chip]") {
    auto slot = mock->get_slot_info(0);
    slot.color_rgb = 0xFF0000;
    slot.multi_color_hexes = "#FF0000,#00FF00,#0000FF";
    slot.material = "PLA";
    slot.product_name = "Silk Rainbow";
    mock->sync_external_identity(0, slot);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    card.set_mappings(mappings_to_slots({0, 1}));
    REQUIRE(chip_count() == 2);

    lv_obj_t* const spool = lv_obj_find_by_name(chip(0), "chip_spool");
    REQUIRE(spool != nullptr);
    CHECK(helix::ui::swatch_paint_colors(spool).size() == 3);
    CHECK(helix::ui::swatch_paint_finish(spool) == helix::ui::SwatchFinish::Silk);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Republishing the same file keeps the chips",
                 "[filament_mapping][idempotent]") {
    // The late AMS resync after the panel opens republishes identical data; the
    // chips must not be torn down and rebuilt for it.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(chip_count() == 2);
    lv_obj_t* const first = chip(0);
    REQUIRE(first != nullptr);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(chip_count() == 2);
    CHECK(chip(0) == first);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "An unmapped chip carries no word in any language",
                 "[filament_mapping][chip]") {
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    card.set_mappings(mappings_to_slots({0, -1}));
    REQUIRE(chip_int(1, "_slot_state") == 0);
    REQUIRE(chip_string(1, "_slot").empty());

    struct RestoreEnglish {
        ~RestoreEnglish() {
            helix::SystemSettingsManager::instance().set_language("en");
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        }
    } restore;
    helix::SystemSettingsManager::instance().set_language("de");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Pick")) != "Pick");
    CHECK(chip_string(1, "_slot").empty());
    CHECK(chip_int(1, "_slot_state") == 0);
    process_lvgl(100);
}

// ============================================================================
// Single-row truncation + "+N" overflow pill
// ============================================================================

TEST_CASE("chips_that_fit: capacity is what the row can actually hold",
          "[filament][filament_mapping][mapping][overflow]") {
    constexpr int32_t gap = 2;
    // filament_mapping_rows is 181px at 480x272, where a chip is 52px: three
    // chips and two gaps need 160, a fourth would need 214.
    CHECK(FilamentMappingCard::chips_that_fit(181, gap, 52) == 3);
    CHECK(FilamentMappingCard::chips_that_fit(160, gap, 52) == 3);
    CHECK(FilamentMappingCard::chips_that_fit(159, gap, 52) == 2);
    // 274px at 800x480 with 64px chips and a 5px gap: four need 271.
    CHECK(FilamentMappingCard::chips_that_fit(274, 5, 64) == 4);

    // A row too narrow for one chip still gets a slot: that slot carries the
    // "+N" pill, which is the whole point of not silently dropping tools.
    CHECK(FilamentMappingCard::chips_that_fit(10, gap, 52) == 1);

    // Layout has not settled: fall back to the floor, never to "everything".
    CHECK(FilamentMappingCard::chips_that_fit(0, gap, 52) == FilamentMappingCard::kMinVisibleChips);
    CHECK(FilamentMappingCard::chips_that_fit(-1, gap, 52) ==
          FilamentMappingCard::kMinVisibleChips);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A palette wider than the row truncates behind a +N pill",
                 "[filament_mapping][overflow]") {
    size_rows_for(rows, 3);
    card.update({"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"},
                {"PLA", "PETG", "PLA", "PETG", "PLA", "PETG"});
    REQUIRE(card.should_show());

    // Two chips plus the pill: the pill takes a chip's place.
    CHECK(chip_count() == 2);
    CHECK(chip(1) != nullptr);
    CHECK(chip(2) == nullptr);
    lv_obj_t* const pill = lv_obj_find_by_name(rows, "filament_chip_more");
    REQUIRE(pill != nullptr);
    CHECK_FALSE(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(pill, "count_label"))) == "+4");

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A palette that fits draws no overflow pill",
                 "[filament_mapping][overflow]") {
    // A "+0" would claim tools are missing on every multi-tool print.
    size_rows_for(rows, 4);
    card.update({"#FF0000", "#00FF00", "#0000FF"}, {"PLA", "PETG", "PLA"});
    CHECK(chip_count() == 3);
    lv_obj_t* const pill = lv_obj_find_by_name(rows, "filament_chip_more");
    REQUIRE(pill != nullptr);
    CHECK(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A capacity chosen before layout settled is corrected on the next publish",
                 "[filament_mapping][overflow]") {
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF",
                                             "#FFFF00", "#FF00FF", "#00FFFF"};
    const std::vector<std::string> materials = {"PLA", "PETG", "PLA", "PETG", "PLA", "PETG"};

    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_width(rows, 0);
    lv_obj_update_layout(rows);
    REQUIRE(lv_obj_get_content_width(rows) <= 0);

    // Unmeasurable row: the floor, and six tools do not fit in it.
    card.update(colors, materials);
    CHECK(chip_count() == static_cast<int>(FilamentMappingCard::kMinVisibleChips) - 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_chip_more_count")) ==
          6 - (static_cast<int>(FilamentMappingCard::kMinVisibleChips) - 1));

    // Layout settles; the data is identical, only the width changed.
    size_rows_for(rows, 6);
    card.update(colors, materials);
    CHECK(chip_count() == 6);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_chip_more_count")) == 0);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "More tools than the subject cap show a pill and write no subject past it",
                 "[filament_mapping][overflow]") {
    // A row wide enough for every tool still stops at kMaxFilamentChips.
    size_rows_for(rows, 24);
    std::vector<std::string> colors(20, "#FF0000");
    std::vector<std::string> materials(20, "PLA");
    card.update(colors, materials);
    REQUIRE(card.should_show());

    const int cap = static_cast<int>(FilamentMappingCard::kMaxFilamentChips);
    CHECK(chip_count() == cap - 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_chip_more_count")) ==
          20 - (cap - 1));
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(
              nullptr, "filament_chip_more_text"))) == "+" + std::to_string(20 - (cap - 1)));
    // No subject exists past the cap, so none can have been written.
    CHECK(chip_subject(cap - 1, "_material") != nullptr);
    CHECK(chip_subject(cap, "_material") == nullptr);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A mapping the user never picked follows the printer; a picked one stays",
                 "[filament_mapping][routing]") {
    // The card seeds once, when the file opens. Lanes that change while it is
    // open (a table that arrives late, a spool moved on the printer) must move
    // every tool the user left alone, or print start sends the stale seed as
    // remaps. A lane the user chose is theirs and stays put.
    struct AutoColorOn {
        bool prev;
        AutoColorOn() {
            helix::SettingsManager::instance().init_subjects();
            prev = helix::SettingsManager::instance().get_auto_color_map();
            helix::SettingsManager::instance().set_auto_color_map(true);
        }
        ~AutoColorOn() {
            helix::SettingsManager::instance().set_auto_color_map(prev);
        }
    } auto_color_on;

    load_lane(mock, 0, 0x0000FF, "PLA");
    load_lane(mock, 1, 0x00FF00, "PLA");
    load_lane(mock, 2, 0xFF0000, "PLA");
    helix::ui::UpdateQueue::instance().drain();
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    auto seeded = card.get_mappings();
    REQUIRE(seeded.size() == 2);
    REQUIRE(seeded[0].mapped_slot == 2); // red
    REQUIRE(seeded[1].mapped_slot == 1); // green

    // The user puts T1 on lane 0; T0 is left to the seed.
    seeded[1].mapped_slot = 0;
    seeded[1].user_picked = true;
    card.set_mappings(seeded);

    // Red moves from lane 2 to lane 1, where green was.
    load_lane(mock, 2, 0xFFFFFF, "PLA");
    load_lane(mock, 1, 0xFF0000, "PLA");
    helix::ui::UpdateQueue::instance().drain();
    card.refresh_slot_data();

    const auto after = card.get_mappings();
    REQUIRE(after.size() == 2);
    CHECK(after[0].mapped_slot == 1);
    CHECK_FALSE(after[0].user_picked);
    CHECK(after[1].mapped_slot == 0);
    CHECK(after[1].user_picked);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A hidden card keeps nothing from the previous file",
                 "[filament_mapping][routing]") {
    // File A gets a pick. File B's card is hidden (no palette), so B has no
    // mappings of its own, and A's pick must not survive to be sent as B's.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    auto picked = card.get_mappings();
    REQUIRE(picked.size() == 2);
    picked[1].mapped_slot = 3;
    picked[1].user_picked = true;
    card.set_mappings(picked);

    card.update({}, {});
    REQUIRE_FALSE(card.should_show());
    CHECK(card.get_mappings().empty());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A palette arriving late for the same file keeps the user's picks",
                 "[filament_mapping][routing]") {
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    auto picked = card.get_mappings();
    REQUIRE(picked.size() == 2);
    picked[1].mapped_slot = 3;
    picked[1].user_picked = true;
    card.set_mappings(picked);

    // The same file's palette re-read (footer or parse backfill).
    card.refresh_palette({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    auto kept = card.get_mappings();
    REQUIRE(kept.size() == 2);
    CHECK(kept[1].mapped_slot == 3);
    CHECK(kept[1].user_picked);

    // A different file starts from its own seed.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    auto fresh = card.get_mappings();
    REQUIRE(fresh.size() == 2);
    CHECK_FALSE(fresh[1].user_picked);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A card hidden after it showed keeps no mappings",
                 "[filament_mapping][routing][bypass]") {
    // Hidden means print start must find nothing here to send, whichever path
    // hid the card.
    SECTION("bypass engaging hides it on a slot refresh") {
        card.update({"#FF0000"}, {"PLA"});
        REQUIRE(card.should_show());
        auto picked = card.get_mappings();
        REQUIRE(picked.size() == 1);
        picked[0].mapped_slot = 2;
        picked[0].user_picked = true;
        card.set_mappings(picked);

        REQUIRE(helix::test::unload_for_bypass(*mock));
        REQUIRE(mock->enable_bypass().success());
        helix::ui::UpdateQueue::instance().drain();
        card.refresh_slot_data();
        REQUIRE_FALSE(card.should_show());
        CHECK(card.get_mappings().empty());
    }

    SECTION("the used-tool set narrowing a bypassed print hides it") {
        REQUIRE(helix::test::unload_for_bypass(*mock));
        REQUIRE(mock->enable_bypass().success());
        helix::ui::UpdateQueue::instance().drain();
        card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
        REQUIRE(card.should_show());
        auto picked = card.get_mappings();
        REQUIRE_FALSE(picked.empty());
        picked[0].user_picked = true;
        card.set_mappings(picked);

        card.set_used_tools(std::set<int>{0});
        REQUIRE_FALSE(card.should_show());
        CHECK(card.get_mappings().empty());
    }
    process_lvgl(100);
}

TEST_CASE("resolve_mapped_slot: a mapping that outlived its lane resolves to nothing",
          "[filament][mapping][mapper]") {
    // Unit unplugged between the mapping and the render. Better to show no
    // number than a stale one — and the swatch renderer used to re-derive
    // local_slot_index + 1 inline, with no such guard.
    std::vector<helix::AvailableSlot> slots;
    helix::AvailableSlot s{};
    s.slot_index = 0;
    s.backend_index = 0;
    s.local_slot_index = 0;
    slots.push_back(s);

    ToolMapping gone;
    gone.tool_index = 0;
    gone.mapped_slot = 7; // no such lane
    gone.mapped_backend = 0;
    CHECK(helix::FilamentMapper::resolve_mapped_slot(gone, slots) == nullptr);
    CHECK(helix::FilamentMapper::mapped_lane_display_number(gone, slots) == -1);

    ToolMapping automatic;
    automatic.tool_index = 0;
    automatic.is_auto = true;
    CHECK(helix::FilamentMapper::resolve_mapped_slot(automatic, slots) == nullptr);

    ToolMapping ok;
    ok.tool_index = 0;
    ok.mapped_slot = 0;
    ok.mapped_backend = 0;
    REQUIRE(helix::FilamentMapper::resolve_mapped_slot(ok, slots) == &slots[0]);
    CHECK(helix::FilamentMapper::mapped_lane_display_number(ok, slots) == 1);
}

// ============================================================================
// A chip tap opens the slot dropdown
// ============================================================================

namespace {

lv_obj_t* on_screen(const char* name) {
    return lv_obj_find_by_name(lv_screen_active(), name);
}

std::string picker_string(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

void tap(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture, "A chip tap opens that tool's ranked slot dropdown",
                 "[filament_mapping][chip_tap]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x00FF00, "PETG");
    load_lane(mock, 2, 0x0000FF, "PETG");
    load_lane(mock, 3, 0x00FF00, "PLA");
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    card.set_mappings(mappings_to_slots({0, 2}));
    REQUIRE(chip_count() == 2);

    // No gate installed on the card: a tap goes straight to the dropdown.
    tap(chip(1));
    REQUIRE(card.slot_picker().is_visible());

    CHECK(picker_string("filament_picker_title") == "T1 · PETG");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_offer_auto")) == 1);
    CHECK(picker_string("filament_picker_extra_text") == "More options…");

    // Rows in rank_slots_for_tool's order, each with slot_fit_text.
    const auto ranked =
        helix::rank_slots_for_tool(card.get_tool_info()[1], card.get_available_slots(),
                                   card.get_tool_info(), card.get_mappings());
    REQUIRE(ranked.size() == 4);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_picker_row_count")) == 4);
    // Lane 2 (index 1) is the exact match; lane 3, mapped now, only differs in colour.
    CHECK(ranked[0].slot_pos == 1);
    for (size_t k = 0; k < ranked.size(); ++k) {
        const std::string reason = "filament_picker_row_" + std::to_string(k) + "_reason";
        CHECK(picker_string(reason.c_str()) == helix::slot_fit_text(ranked[k]));
    }

    // The current pick is highlighted, on screen and not only in its subject.
    size_t current_row = ranked.size();
    for (size_t k = 0; k < ranked.size(); ++k) {
        if (ranked[k].slot_pos == 2) {
            current_row = k;
        }
    }
    REQUIRE(current_row < ranked.size());
    const std::string row_name = "filament_picker_row_" + std::to_string(current_row);
    lv_obj_t* const row = on_screen(row_name.c_str());
    REQUIRE(row != nullptr);
    CHECK(lv_obj_get_style_bg_opa(row, LV_PART_MAIN) == 40);
    lv_obj_t* const other =
        on_screen(current_row == 0 ? "filament_picker_row_1" : "filament_picker_row_0");
    REQUIRE(other != nullptr);
    CHECK(lv_obj_get_style_bg_opa(other, LV_PART_MAIN) == 0);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Picking a row records it once, the way the modal does",
                 "[filament_mapping][chip_tap]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x00FF00, "PETG");
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    card.set_mappings(mappings_to_slots({0, 3}));
    int changed = 0;
    card.set_on_mappings_changed([&changed] { ++changed; });

    card.open_slot_picker(1, chip(1));
    REQUIRE(card.slot_picker().is_visible());
    // Row 0 is the best fit: lane 2, exactly T1's green PETG.
    tap(on_screen("filament_picker_row_0"));

    CHECK_FALSE(card.slot_picker().is_visible());
    CHECK(changed == 1);
    const auto mappings = card.get_mappings();
    REQUIRE(mappings.size() == 2);
    CHECK(mappings[1].mapped_slot == 1);
    CHECK(mappings[1].user_picked);
    CHECK_FALSE(mappings[1].is_auto);
    CHECK(mappings[0].mapped_slot == 0); // the other tool is untouched
    CHECK(chip_string(1, "_slot") == "2");
    CHECK(chip_int(1, "_severity") == 0);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "More options opens the full mapping",
                 "[filament_mapping][chip_tap]") {
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    const auto before = card.get_mappings();
    int full = 0;
    card.set_on_tap([&full] { ++full; });
    int changed = 0;
    card.set_on_mappings_changed([&changed] { ++changed; });

    card.open_slot_picker(0, chip(0));
    REQUIRE(card.slot_picker().is_visible());
    tap(on_screen("extra_choice"));

    CHECK(full == 1);
    CHECK(changed == 0);
    CHECK_FALSE(card.slot_picker().is_visible());
    CHECK(card.get_mappings().size() == before.size());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Fresh slot data closes an open dropdown",
                 "[filament_mapping][chip_tap]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x00FF00, "PETG");
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    card.open_slot_picker(0, chip(0));
    REQUIRE(card.slot_picker().is_visible());

    // Nothing moved: the dropdown stays.
    card.refresh_slot_data();
    REQUIRE(card.slot_picker().is_visible());

    load_lane(mock, 1, 0x0000FF, "PETG");
    card.refresh_slot_data();
    CHECK_FALSE(card.slot_picker().is_visible());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A pick the gate refuses is not recorded",
                 "[filament_mapping][chip_tap]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x00FF00, "PETG");
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    const auto before = card.get_mappings();
    int changed = 0;
    card.set_on_mappings_changed([&changed] { ++changed; });
    bool allowed = true;
    card.set_pick_gate([&allowed] { return allowed; });

    card.open_slot_picker(1, chip(1));
    REQUIRE(card.slot_picker().is_visible());
    allowed = false; // the remap went away while the dropdown was open
    tap(on_screen("filament_picker_row_0"));

    CHECK_FALSE(card.slot_picker().is_visible());
    CHECK(changed == 0);
    const auto after = card.get_mappings();
    REQUIRE(after.size() == before.size());
    CHECK_FALSE(after[1].user_picked);
    CHECK(after[1].mapped_slot == before[1].mapped_slot);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Picking Auto shows the lane the tool routes to",
                 "[filament_mapping][chip_tap]") {
    load_lane(mock, 0, 0xFF0000, "PLA");
    load_lane(mock, 1, 0x00FF00, "PETG");
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    card.set_mappings(mappings_to_slots({0, 0}));
    REQUIRE(chip_int(1, "_severity") == 2); // T1's PETG on lane 1's PLA

    card.open_slot_picker(1, chip(1));
    REQUIRE(card.slot_picker().is_visible());
    tap(on_screen("auto_choice"));

    REQUIRE(card.get_mappings()[1].is_auto);
    // The mock routes lane-per-tool: T1 prints from lane 2, its own PETG.
    CHECK(chip_int(1, "_severity") != 2);
    CHECK(chip_int(1, "_severity") == 0);
    CHECK(chip_string(1, "_slot") == "2");
    CHECK(chip_int(1, "_slot_state") == 1);
    // The print-start gate judges the same lane and lets it print.
    CHECK_FALSE(
        helix::file_preflight(card.get_tool_info(), card.get_mappings(), false).has_block());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "An Auto that routes to no lane is red with the no-slot body, as the gate blocks",
                 "[filament_mapping][chip_tap][preflight]") {
    // T5 on four lanes: identity routing gives it head 5, which no lane is.
    card.set_used_tools(std::set<int>{0, 5});
    card.update({"#FF0000", "#00FF00", "#0000FF", "#FFFFFF", "#000000", "#00FFFF"},
                {"PLA", "PLA", "PLA", "PLA", "PLA", "PLA"});
    auto mappings = card.get_mappings();
    REQUIRE(mappings.size() == 2);
    REQUIRE(mappings[1].tool_index == 5);
    helix::FilamentMapper::pick_slot(mappings[1], card.get_tool_info()[1],
                                     card.get_available_slots(), -1, -1, true);
    card.set_mappings(mappings);
    REQUIRE(chip_count() == 2);
    CHECK(chip_int(1, "_severity") == 2);
    CHECK(chip_int(1, "_slot_state") == 0);
    // The print-start gate reads the same mapping and blocks it.
    CHECK(helix::file_preflight(card.get_tool_info(), card.get_mappings(), false).has_block());
    process_lvgl(100);
}

TEST_CASE_METHOD(
    MappingCardRenderFixture,
    "Where Print carries out no stored pick, the card seeds each tool on its routed head",
    "[filament_mapping][chip][preflight]") {
    // GcodeRewrite: the Print button prints the file as sliced, so T0 prints
    // from the head it routes to even though another lane holds its colour.
    struct AutoColorOn {
        bool prev;
        AutoColorOn() {
            helix::SettingsManager::instance().init_subjects();
            prev = helix::SettingsManager::instance().get_auto_color_map();
            helix::SettingsManager::instance().set_auto_color_map(true);
        }
        ~AutoColorOn() {
            helix::SettingsManager::instance().set_auto_color_map(prev);
        }
    } auto_color_on;

    mock->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
    mock->force_slot_status(0, helix::SlotStatus::EMPTY);
    load_lane(mock, 1, 0x00FF00, "PLA");
    load_lane(mock, 2, 0xFF0000, "PLA");
    helix::ui::UpdateQueue::instance().drain();
    // Colour matching is on, so a seed that colour-matched would pick lane 3.
    REQUIRE(helix::AmsState::instance().effective_auto_match());

    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    const auto seeded = card.get_mappings();
    REQUIRE(seeded.size() == 2);
    CHECK(seeded[0].mapped_slot == 0);
    CHECK(seeded[1].mapped_slot == 1);

    const auto preflight = helix::file_preflight(card.get_tool_info(), seeded, false);
    REQUIRE(preflight.checks.size() == 2);
    CHECK(preflight.checks[0].severity == helix::ToolCheck::Severity::EmptySlot);
    CHECK(preflight.has_block());

    REQUIRE(chip_count() == 2);
    CHECK(chip_int(0, "_severity") == 2);
    CHECK(chip_int(0, "_slot_state") == 2);

    // The rewrite modal still suggests the colour match: its apply rewrites the
    // job, so the suggestion prints as shown.
    const auto suggested = helix::remap_modal_mappings(seeded, card.get_tool_info());
    REQUIRE(suggested.size() == 2);
    CHECK(suggested[0].mapped_slot == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Where the firmware picks lanes at print time, the card claims no lane",
                 "[filament_mapping][chip][preflight]") {
    mock->set_remap_strategy(helix::AmsBackend::RemapStrategy::None);
    mock->set_firmware_picks_lanes_at_print(true);
    mock->force_slot_status(0, helix::SlotStatus::EMPTY);
    load_lane(mock, 1, 0x00FF00, "PETG");
    helix::ui::UpdateQueue::instance().drain();

    card.update({"#FF0000", "#00FF00"}, {"PLA", "PLA"});
    const auto seeded = card.get_mappings();
    REQUIRE(seeded.size() == 2);
    for (const auto& m : seeded) {
        CHECK(m.is_auto);
        CHECK(m.mapped_slot == -1);
    }

    // The file's filament over the unknown body, in a grey outline.
    REQUIRE(chip_count() == 2);
    for (int i = 0; i < 2; ++i) {
        CHECK(chip_int(i, "_severity") == 0);
        CHECK(chip_int(i, "_slot_state") == 4);
        CHECK(chip_string(i, "_slot").empty());
    }

    // The tap gate offers no dropdown.
    CHECK(helix::printer::remap_block(*mock, helix::GcodeRewriteBlock::None, 2) ==
          helix::printer::RemapBlock::NoStrategy);

    // Neither the empty lane 1 nor the PETG in lane 2 is held against a tool.
    const auto preflight = helix::file_preflight(card.get_tool_info(), seeded, false);
    CHECK(preflight.checks.empty());
    CHECK_FALSE(preflight.has_block());
    CHECK_FALSE(preflight.has_advisory());

    helix::PrintStartContext ctx;
    ctx.filament_color_count = 2;
    ctx.mappings = seeded;
    ctx.lanes_picked_at_print =
        helix::AmsState::instance().collect_firmware_routing().chosen_at_print;
    CHECK(ctx.lanes_picked_at_print);
    CHECK(helix::unresolved_tools_in(ctx).empty());
    process_lvgl(100);
}
