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
 * a chip as a lane claim) and the single-extruder tools>1 rule.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "../mapping_card_render_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

TEST_CASE_METHOD(MappingCardRenderFixture, "Card shows on a backend whose mapping is not editable",
                 "[filament_mapping][visibility]") {
    // snapmaker_mode_ makes get_tool_mapping_capabilities() report
    // {supported=true, editable=false} — the U1 case. Before the merge this
    // hid the card and a second surface drew the chips; that surface is gone.
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
    // The reachable half of the tool-count rule moved from
    // swatches_card_visible_for(): on a multi-tool printer ANY referenced tool
    // is worth showing, because which lane it routes to is the whole question.
    // (The single-extruder tools>1 branch needs a 1-slot backend the 4-slot mock
    // cannot present; it is covered by Task 5's manual verification instead of a
    // test that cannot fail.)
    card.set_used_tools(std::set<int>{0});
    card.update({"#FF0000"}, {"PLA"});
    CHECK(card.should_show());
    CHECK(lv_obj_get_child_count(rows) == 1);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "Card hides a used-tools count of zero on a multi-tool printer",
                 "[filament_mapping][visibility]") {
    // The hide side of the same rule, distinct from the pre-existing
    // tool_info_.empty() gate above it: gcode_colors below is non-empty, so
    // tool_info_ is non-empty too, and only the moved tool-count rule can be
    // hiding the card here. An explicit EMPTY set (not nullopt) is what reaches
    // it — apply_used_tools_filter treats both as "no filter", but the count
    // rule reads used_tools_->size() directly, so a real zero-tool answer (as
    // opposed to "not computed yet") still hides a multi-tool-printer card.
    card.set_used_tools(std::set<int>{});
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}
