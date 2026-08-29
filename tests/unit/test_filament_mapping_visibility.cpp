// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_visibility.cpp
 * @brief Pins the merged FilamentMappingCard::should_show() predicate.
 *
 * The card used to hide itself on backends whose tool mapping is not editable
 * (Snapmaker U1, ACE), because a second surface - the print-detail FILAMENTS
 * card - rendered the same information there. That second surface is gone, so
 * hiding on non-editable backends would show the user nothing at all. The
 * dead-control concern it existed for now lives entirely on the tap affordance
 * (PrintSelectDetailView::color_card_opens_remap).
 *
 * The two gates that MUST survive the merge are pinned here too: the bypass
 * single-lane suppression (a K2 user printed off the bypass spool after reading
 * a chip as a lane claim) and BOTH halves of the multi-tool-printer-aware
 * tool-count rule (any tool on a multi-tool printer, 2+ tools on a single
 * extruder) - the single-extruder half via SingleExtruderMappingCardFixture,
 * which forces a genuine 1-tool ToolState topology rather than relying on
 * whatever the shared 4-slot AmsBackendMock fixture happens to report.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "../mapping_card_render_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <memory>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

namespace {

/// Forces ToolState into a genuine single-extruder topology (tool_count()==1,
/// is_multi_tool()==false), independent of whatever a prior test file left the
/// global ToolState singleton (a Meyers singleton, so its state otherwise
/// survives across the whole binary) holding. Slot count 1 on the AMS side
/// makes the `ams_slots > 1` half of is_multi_tool_printer false too, so both
/// halves of the OR are pinned rather than just one.
///
/// init_tools()/deinit_subjects() sequencing mirrors the established pattern
/// in test_tool_switcher_print_gate.cpp. The object list omits any
/// "toolchanger"/"tool T*" entry and reports exactly one "extruder" heater, so
/// PrinterDiscovery takes ToolState::init_tools()'s "no tool changer" branch
/// and builds exactly one tool.
struct SingleExtruderMappingCardFixture : MappingCardRenderFixture {
    SingleExtruderMappingCardFixture() : MappingCardRenderFixture(/*slot_count=*/1) {
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(false);
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
        helix::ToolState::instance().init_tools(hw);
        REQUIRE(helix::ToolState::instance().tool_count() == 1);
        REQUIRE_FALSE(helix::ToolState::instance().is_multi_tool());
    }

    ~SingleExtruderMappingCardFixture() override {
        helix::ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(MappingCardRenderFixture, "Card shows on a backend whose mapping is not editable",
                 "[filament_mapping][visibility]") {
    // snapmaker_mode_ is the U1 case: RemapStrategy::SnapmakerNative, so the
    // pick IS honored (through the pre-print send) while no tool->slot table is
    // written and owns_tool_mapping_table() is false. Before the merge this hid
    // the card and a second surface drew the chips; that surface is gone.
    // should_show() reads neither, which is the point — card visibility is
    // decided by the tools the file uses, not by what the backend can remap.
    mock->set_snapmaker_mode(true);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Card hides a single-lane print while bypass is engaged",
                 "[filament_mapping][visibility]") {
    // With bypass engaged a single-tool print takes filament from the external
    // spool, so offering a lane mapping claims something the print will not do.
    REQUIRE(mock->enable_bypass().success());
    REQUIRE(mock->is_bypass_active());
    card.update({"#FF0000"}, {"PLA"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Card shows a single-tool file on a multi-tool printer",
                 "[filament_mapping][visibility]") {
    // The multi-tool half of the tool-count rule moved from
    // swatches_card_visible_for(): on a multi-tool printer ANY referenced tool
    // is worth showing, because which lane it routes to is the whole question.
    card.set_used_tools(std::set<int>{0});
    card.update({"#FF0000"}, {"PLA"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 1);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "An explicit empty used-tools set does not hide the card",
                 "[filament_mapping][visibility]") {
    // Pins the fix for a real regression this task introduced and review
    // caught: the tool-count rule originally read used_tools_->size()
    // directly, so an explicit (non-nullopt) EMPTY set forced tool_count to 0
    // and hid the card even with a non-empty tool_info_ — unlike every other
    // reader of used_tools_ in this class (the bypass gate above,
    // apply_used_tools_filter()'s documented contract), which treats an empty
    // set as "no answer, show all". The fix reuses print_lane_requirement(),
    // which has that same fallback built in, so this must SHOW.
    card.set_used_tools(std::set<int>{});
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(SingleExtruderMappingCardFixture,
                 "Card hides a single tool on a single-extruder printer",
                 "[filament_mapping][visibility]") {
    // The single-extruder half of the same rule: an ordinary one-colour print
    // uses exactly one tool, which is not worth a lane-mapping card when there
    // is no lane to route it through.
    card.update({"#FF0000"}, {"PLA"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}

TEST_CASE_METHOD(SingleExtruderMappingCardFixture,
                 "Card shows two tools on a single-extruder printer",
                 "[filament_mapping][visibility]") {
    // 2+ tools on a single extruder is a manual-swap multi-colour file, not an
    // ordinary single-colour print — the KEEP half of the rule this task's
    // brief named as the failure mode of the merge.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 2);
    process_lvgl(100);
}
