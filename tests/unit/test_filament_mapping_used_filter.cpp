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
 * built on LVGLUITestFixture + a mock AMS backend): rebuild_compact_view()
 * must be a NO-OP when its inputs are unchanged and children exist, so the
 * late AMS resync that arrives after the print-detail panel opens cannot
 * flash the chips through a destroy/recreate cycle; it must render stacked
 * filament_swatch chips; and set_mappings() must repaint them, because the
 * lane number inside each chip is written imperatively during the rebuild.
 */

#include "ui_filament_mapping_card.h"

#include "../mapping_card_render_fixture.h"
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
// Idempotent rebuild (full render path)
// ============================================================================

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard rebuild is idempotent on identical input",
                 "[filament_mapping][idempotent]") {
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    const std::vector<std::string> materials = {"PLA", "PETG"};

    // First render: one pill per tool.
    card.update(colors, materials);
    REQUIRE(card.should_show());
    const uint32_t count_after_first = lv_obj_get_child_count(rows);
    REQUIRE(count_after_first == 2);
    lv_obj_t* const first_pill = lv_obj_get_child(rows, 0);

    // Second update with IDENTICAL inputs — the late AMS-resync-after-open
    // path this guards. No destroy/recreate: same children, same pointers.
    card.update(colors, materials);
    CHECK(lv_obj_get_child_count(rows) == count_after_first);
    CHECK(lv_obj_get_child(rows, 0) == first_pill);

    // Changed slot color => fingerprint differs => a rebuild really happens
    // (fresh pills, first-child pointer differs). Pin this so an over-eager
    // early-return cannot silently disable the card.
    auto slot = mock->get_slot_info(0);
    slot.color_rgb = 0x123456;
    mock->set_slot_info(0, slot, /*persist=*/false);
    card.update(colors, materials);
    CHECK(lv_obj_get_child_count(rows) == count_after_first);
    CHECK(lv_obj_get_child(rows, 0) != first_pill);

    // Flush the async deletes of the replaced pills (safe_clean_children
    // reparents + lv_obj_delete_async) before teardown.
    process_lvgl(100);
}

// ============================================================================
// Stacked-swatch render + repaint
// ============================================================================

namespace {

// Lane number currently drawn in chip `idx`'s bottom band, or "" when hidden.
// Re-fetches the chip every call: a real rebuild reparents the old children
// away, so a cached pointer would report pre-rebuild text forever and the test
// would pass on a broken card.
std::string rendered_lane_number(lv_obj_t* rows, uint32_t idx) {
    lv_obj_t* const chip = lv_obj_get_child(rows, static_cast<int32_t>(idx));
    if (chip == nullptr) {
        return "<no chip>";
    }
    lv_obj_t* const band = lv_obj_find_by_name(chip, "bottom_band");
    if (band == nullptr) {
        return "<no bottom_band>";
    }
    lv_obj_t* const lbl = lv_obj_find_by_name(band, "slot_label");
    if (lbl == nullptr) {
        return "<no slot_label>";
    }
    if (lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
        return "";
    }
    return lv_label_get_text(lbl);
}

// One explicit mapping per tool, so the rendered lane number is a pure function
// of the slot index chosen here rather than of the seeding heuristic.
std::vector<ToolMapping> mappings_to_slots(const std::vector<int>& slots) {
    std::vector<ToolMapping> m;
    m.reserve(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        ToolMapping tm;
        tm.tool_index = static_cast<int>(i);
        tm.mapped_slot = slots[i];
        tm.mapped_backend = 0;
        tm.is_auto = false;
        tm.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;
        m.push_back(tm);
    }
    return m;
}

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard renders stacked filament_swatch chips",
                 "[filament_mapping][swatch]") {
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // The stacked chip's structure, not the pill's. A pill has gcode_dot/arrow/
    // slot_dot; a swatch has top_band/divider/bottom_band.
    lv_obj_t* const chip = lv_obj_get_child(rows, 0);
    CHECK(lv_obj_find_by_name(chip, "top_band") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "divider") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "bottom_band") != nullptr);
    CHECK(lv_obj_find_by_name(chip, "gcode_dot") == nullptr);

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "FilamentMappingCard::set_mappings repaints the lane number",
                 "[filament_mapping][remap]") {
    // The native-remap flow does not go through the card's own modal callback:
    // the print-detail view overrides the card's tap handler, so an accepted
    // remap arrives here as a bare set_mappings() call. The lane number is
    // written imperatively during rebuild, so a setter that stores without
    // rebuilding leaves the old lane on screen while the print runs the new one.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // Tool 0 on the mock's 4th lane (local_slot_index 3 => displayed "4").
    card.set_mappings(mappings_to_slots({3, 1}));
    CHECK(rendered_lane_number(rows, 0) == "4");

    // The reported scenario: remap tool 0 down to the 3rd lane, same colour but
    // more filament. The chip must follow the mapping.
    card.set_mappings(mappings_to_slots({2, 1}));
    CHECK(rendered_lane_number(rows, 0) == "3");

    // Untouched tools keep their lane — a repaint, not a reset.
    CHECK(rendered_lane_number(rows, 1) == "2");

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Unknown gcode colour draws no fill on the top band",
                 "[filament_mapping][swatch]") {
    // A K2 Plus report: painting the neutral stand-in reads as "this file prints
    // in grey", a claim about the file that nothing has made. The pill fixed this
    // via GcodeToolInfo::color_known; the swatch renderer never had the guard, so
    // it must not be lost in the move. Empty palette entry => colour unknown.
    card.update({"", ""}, {"PLA", "PETG"});
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    lv_obj_t* const band = lv_obj_find_by_name(lv_obj_get_child(rows, 0), "top_band");
    REQUIRE(band != nullptr);
    CHECK(lv_obj_get_style_bg_opa(band, LV_PART_MAIN) == LV_OPA_TRANSP);

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
// Single-row truncation + "+N" overflow pill
// ============================================================================

TEST_CASE("chips_that_fit: capacity is what the row can actually hold",
          "[filament][filament_mapping][mapping][overflow]") {
    // Pure arithmetic at a 4px gap; space_xs is breakpoint-scaled, so the widths
    // below are the two rows measured in the running app and the answers are the
    // ones the app actually reported for them.
    constexpr int32_t gap = 4;
    // filament_mapping_rows is 181px at 480x272. Four 40px chips plus three gaps
    // is 172; a fifth would need 216. The app reports 4 there.
    CHECK(FilamentMappingCard::chips_that_fit(181, gap) == 4);
    CHECK(FilamentMappingCard::chips_that_fit(172, gap) == 4);
    CHECK(FilamentMappingCard::chips_that_fit(171, gap) == 3);
    // 296px at 800x480: six chips need 260, seven need 304. The app reports 6.
    CHECK(FilamentMappingCard::chips_that_fit(296, gap) == 6);

    // A row too narrow for one chip still gets a slot — that slot carries the
    // "+N" pill, which is the whole point of not silently dropping tools.
    CHECK(FilamentMappingCard::chips_that_fit(10, gap) == 1);

    // Layout has not settled: fall back to the floor, never to "everything".
    CHECK(FilamentMappingCard::chips_that_fit(0, gap) == FilamentMappingCard::MIN_VISIBLE_CHIPS);
    CHECK(FilamentMappingCard::chips_that_fit(-1, gap) == FilamentMappingCard::MIN_VISIBLE_CHIPS);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A palette wider than the row truncates behind a +N pill",
                 "[filament_mapping][overflow]") {
    // The row does not wrap and does not scroll, so anything past the right edge
    // is invisible. Six tools in a row that holds three must not quietly become
    // three: the print uses all six, and the card has to say so.
    //
    // space_xs is breakpoint-dependent (4/5/6px), so read the real gap rather
    // than assuming one; 140px holds exactly 3 chips at every one of those.
    const int32_t gap = theme_manager_get_spacing("space_xs");
    lv_obj_set_width(rows, 140);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_update_layout(rows);
    REQUIRE(FilamentMappingCard::chips_that_fit(lv_obj_get_content_width(rows), gap) == 3);

    card.update({"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"},
                {"PLA", "PETG", "PLA", "PETG", "PLA", "PETG"});
    REQUIRE(card.should_show());

    // Two chips plus the pill — the pill occupies a slot rather than being drawn
    // past the edge, so overflowing costs a chip.
    REQUIRE(lv_obj_get_child_count(rows) == 3);
    lv_obj_t* const last = lv_obj_get_child(rows, 2);
    REQUIRE(lv_obj_find_by_name(last, "top_band") == nullptr); // not a chip
    lv_obj_t* const count = lv_obj_find_by_name(last, "count_label");
    REQUIRE(count != nullptr);
    CHECK(std::string(lv_label_get_text(count)) == "+4");

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A palette that fits draws no overflow pill",
                 "[filament_mapping][overflow]") {
    // The complement: the pill must not appear when nothing is hidden, or every
    // multi-tool print grows a "+0" that claims tools are missing.
    lv_obj_set_width(rows, 300); // holds 6 chips at every space_xs value
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_update_layout(rows);
    REQUIRE(FilamentMappingCard::chips_that_fit(lv_obj_get_content_width(rows),
                                                theme_manager_get_spacing("space_xs")) == 6);

    card.update({"#FF0000", "#00FF00", "#0000FF"}, {"PLA", "PETG", "PLA"});
    REQUIRE(lv_obj_get_child_count(rows) == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(lv_obj_find_by_name(lv_obj_get_child(rows, static_cast<int32_t>(i)), "top_band") !=
              nullptr);
    }

    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "A capacity chosen before layout settled is corrected on the next rebuild",
                 "[filament_mapping][overflow]") {
    // The measured width decides how many chips get drawn, so it has to be an
    // input to the render fingerprint too. A first render that lands before
    // layout has settled takes the MIN_VISIBLE_CHIPS fallback; if the width is
    // not in the fingerprint that wrong answer is permanent, because every later
    // rebuild with the same data early-returns and on_ui_destroyed() is the only
    // other thing that clears the fingerprint.
    const std::vector<std::string> colors = {"#FF0000", "#00FF00", "#0000FF",
                                             "#FFFF00", "#FF00FF", "#00FFFF"};
    const std::vector<std::string> materials = {"PLA", "PETG", "PLA", "PETG", "PLA", "PETG"};

    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_width(rows, 0);
    lv_obj_update_layout(rows);
    REQUIRE(lv_obj_get_content_width(rows) <= 0);

    // Unmeasurable row falls back to the floor, and six tools do not fit in four
    // slots: three chips and a "+3".
    card.update(colors, materials);
    REQUIRE(FilamentMappingCard::MIN_VISIBLE_CHIPS == 4);
    REQUIRE(lv_obj_get_child_count(rows) == 4);
    lv_obj_t* const first_before = lv_obj_get_child(rows, 0);
    lv_obj_t* const pill = lv_obj_get_child(rows, 3);
    REQUIRE(lv_obj_find_by_name(pill, "count_label") != nullptr);
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(pill, "count_label"))) == "+3");

    // Layout settles. Tools, mappings and slots are byte-identical to the render
    // above — the width is the only thing that changed.
    lv_obj_set_width(rows, 300);
    lv_obj_update_layout(rows);
    REQUIRE(FilamentMappingCard::chips_that_fit(lv_obj_get_content_width(rows),
                                                theme_manager_get_spacing("space_xs")) == 6);

    card.update(colors, materials);
    CHECK(lv_obj_get_child_count(rows) == 6);         // all six fit now, no pill
    CHECK(lv_obj_get_child(rows, 0) != first_before); // really re-rendered
    CHECK(lv_obj_find_by_name(lv_obj_get_child(rows, 5), "top_band") != nullptr);

    process_lvgl(100);
}
