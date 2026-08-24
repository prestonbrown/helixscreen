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
 * The last test case exercises the full render path (MappingCardRenderFixture,
 * built on LVGLUITestFixture + a mock AMS backend): rebuild_compact_view()
 * must be a NO-OP when its inputs are unchanged and children exist, so the
 * late AMS resync that arrives after the print-detail panel opens cannot
 * flash the pills through a destroy/recreate cycle.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>
#include <optional>
#include <set>
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

namespace {

// The pure-seam tests above need no LVGL; this one renders real pills, so it
// needs LVGLUITestFixture (XML component registration — lv_xml_create must
// build filament_mapping_pill) plus a started AmsBackendMock so update()
// passes its availability/editable-mapping gates. Widget wiring mirrors
// MappingCardFixture (test_filament_mapping_drain_reentrancy.cpp); AMS wiring
// mirrors test_ams_available_slots.cpp.
struct MappingCardRenderFixture : LVGLUITestFixture {
    FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;
    AmsBackendMock* mock = nullptr;

    MappingCardRenderFixture() {
        // AmsState subjects must exist before backend events fire; the base
        // fixture already initialized PrinterState's (the init order
        // subject_initializer.cpp enforces in production).
        auto& ams = AmsState::instance();
        ams.init_subjects(false);

        auto owned = std::make_unique<AmsBackendMock>(4);
        mock = owned.get();
        mock->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        mock->start();

        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }

    ~MappingCardRenderFixture() override {
        // Detach while LVGL still runs and before members die (base-class
        // teardown has not happened yet). Drain first so queued backend-event
        // syncs don't leak into the next test.
        helix::ui::UpdateQueue::instance().drain();
        if (mock) {
            mock->stop();
        }
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

} // namespace

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
