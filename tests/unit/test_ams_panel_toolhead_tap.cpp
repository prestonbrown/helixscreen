// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_panel_toolhead_tap.cpp
 * @brief On a one-head-per-tool path, a head tap mounts that tool and a tube
 *        tap opens the slot menu, never a tool change.
 */

#include "ui_filament_path_canvas.h"
#include "ui_modal.h"
#include "ui_panel_ams.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "safety_settings_manager.h"
#include "tool_change_request.h"
#include "tool_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::parallel_tap_at;
using helix::ui::ParallelTapTarget;

// parallel_tap_at(): pure
//
// Three columns at x = 100, 200, 300, each lane 30 px either side of its x and
// owning everything above sensor_y 250. Each head glyph spans x -12..+12 of
// its column and y 380..430.

namespace {
constexpr int32_t kSlotX[] = {100, 200, 300};

helix::ui::ParallelTapLayout layout(const int32_t* xs = kSlotX, int count = 3) {
    helix::ui::ParallelTapLayout l;
    l.slot_x = xs;
    l.slot_count = count;
    l.sensor_y = 250;
    l.lane_half_width = 30;
    l.head = {-12, 380, 12, 430};
    return l;
}

helix::ui::ParallelTap tap(int32_t x, int32_t y) {
    return parallel_tap_at({x, y}, layout());
}
} // namespace

TEST_CASE("parallel_tap_at: a head glyph answers as its toolhead", "[ams][path][hit_test]") {
    const auto t = tap(200, 410);
    CHECK(t.target == ParallelTapTarget::Toolhead);
    CHECK(t.slot == 1);
}

TEST_CASE("parallel_tap_at: the glyph's edges are the head, one pixel out is not",
          "[ams][path][hit_test]") {
    CHECK(tap(200, 380).target == ParallelTapTarget::Toolhead);
    CHECK(tap(200, 430).target == ParallelTapTarget::Toolhead);
    CHECK(tap(188, 400).target == ParallelTapTarget::Toolhead);
    CHECK(tap(212, 400).target == ParallelTapTarget::Toolhead);
    CHECK(tap(200, 379).target == ParallelTapTarget::Lane);
    CHECK(tap(200, 431).target == ParallelTapTarget::None);
    CHECK(tap(187, 400).target == ParallelTapTarget::None);
    CHECK(tap(213, 400).target == ParallelTapTarget::None);
}

TEST_CASE("parallel_tap_at: a tube or spool answers as its lane", "[ams][path][hit_test]") {
    for (int32_t y : {10, 120, 249}) {
        CAPTURE(y);
        const auto t = tap(100, y);
        CHECK(t.target == ParallelTapTarget::Lane);
        CHECK(t.slot == 0);
    }
    // Below the sensor the tube still runs down to the glyph: still the lane's.
    CHECK(tap(100, 250).target == ParallelTapTarget::Lane);
    CHECK(tap(100, 379).target == ParallelTapTarget::Lane);
}

TEST_CASE("parallel_tap_at: between columns is nothing", "[ams][path][hit_test]") {
    CHECK(tap(150, 400).target == ParallelTapTarget::None);
    CHECK(tap(150, 100).target == ParallelTapTarget::None);
}

TEST_CASE("parallel_tap_at: a glyph reaching above the sensor row leaves the tube to the lane",
          "[ams][path][hit_test]") {
    auto l = layout();
    l.head = {-12, 200, 12, 430};
    const auto above = parallel_tap_at({200, 240}, l);
    CHECK(above.target == ParallelTapTarget::Lane);
    CHECK(above.slot == 1);
    const auto below = parallel_tap_at({200, 260}, l);
    CHECK(below.target == ParallelTapTarget::Toolhead);
    CHECK(below.slot == 1);
}

TEST_CASE("parallel_tap_at: overlapping columns answer the nearest", "[ams][path][hit_test]") {
    // 31 px pitch: every lane reach and head glyph overlaps its neighbours.
    constexpr int32_t xs[] = {100, 131, 162};
    auto l = layout(xs, 3);
    l.head = {-20, 380, 20, 430};
    for (int32_t y : {100, 400}) {
        CAPTURE(y);
        CHECK(parallel_tap_at({120, y}, l).slot == 1); // 11 px from 131, 20 from 100
        CHECK(parallel_tap_at({115, y}, l).slot == 0); // 15 px from 100, 16 from 131
        CHECK(parallel_tap_at({150, y}, l).slot == 2); // 12 px from 162, 19 from 131
    }
}

// AmsPanel: what each tap does

namespace {

struct ToolheadTapFixture : public XMLTestFixture {
    ToolheadTapFixture() {
        test::set_wire_state(state(), PrintJobState::STANDBY);
        ensure_ams_widgets_registered();
        panel = std::make_unique<AmsPanel>(state(), &api());
        panel->init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel->setup(panel_obj, test_screen());
        panel->on_activate();
        lv_obj_update_layout(test_screen());
        process_lvgl(50);
        REQUIRE(rig.backend->get_topology() == PathTopology::PARALLEL);
        REQUIRE(ToolState::instance().active_tool_index() == 0);
    }

    ~ToolheadTapFixture() override {
        ModalStack::instance().clear();
        panel->on_deactivate(DeactivateReason::NavigateAway);
        panel->clear_panel_reference();
        lv_obj_delete(panel_obj);
        process_lvgl(10);
        panel.reset();
    }

    void set_confirm(bool on) {
        SafetySettingsManager::instance().set_tool_change_require_confirmation(on);
    }

    ToolChangerRig rig{3};
    std::unique_ptr<AmsPanel> panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ToolheadTapFixture, "a tap on another tool's head asks, then mounts it",
                 "[ams][path][toolhead_tap]") {
    set_confirm(true);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK_FALSE(AmsPanelTestAccess::context_menu_open(*panel));

    test::answer_top_modal("btn_primary");
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 1);
}

TEST_CASE_METHOD(ToolheadTapFixture, "with confirmation off a head tap mounts it at once",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    AmsPanelTestAccess::tap_toolhead(*panel, 2);
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 2);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(ToolheadTapFixture, "a tube tap never changes tool, whatever the setting",
                 "[ams][path][toolhead_tap]") {
    for (bool confirm : {false, true}) {
        CAPTURE(confirm);
        set_confirm(confirm);
        AmsPanelTestAccess::tap_lane(*panel, 1);
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(ModalStack::instance().stack_empty());
        CHECK(AmsPanelTestAccess::context_menu_open(*panel));
    }
}

TEST_CASE_METHOD(ToolheadTapFixture, "the mounted head's tap opens the slot menu",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    AmsPanelTestAccess::tap_toolhead(*panel, 0);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(AmsPanelTestAccess::context_menu_open(*panel));
}

TEST_CASE_METHOD(ToolheadTapFixture,
                 "a head tap while the filament system is busy is ignored, with a toast",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    rig.backend->report_action = GENERATE(AmsAction::LOADING, AmsAction::CHECKING);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    CHECK_FALSE(AmsPanelTestAccess::context_menu_open(*panel));
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find(ui::busy_refusal().user_msg) != std::string::npos);
}

TEST_CASE_METHOD(ToolheadTapFixture, "a head tap while printing is refused with a reason",
                 "[ams][path][toolhead_tap]") {
    set_confirm(true);
    test::set_wire_state(state(), PrintJobState::PRINTING);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("Cannot run filament operation while printing") !=
          std::string::npos);
}

TEST_CASE_METHOD(ToolheadTapFixture, "a head tap with spools on the bed is refused",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    state().print_state().set_spool_latch(true);
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(rig.warnings.size() == 1);
    state().print_state().set_spool_latch(false);
}

TEST_CASE_METHOD(ToolheadTapFixture,
                 "a head tap while the filament system waits on the user is refused with a reason",
                 "[ams][path][toolhead_tap]") {
    set_confirm(false);
    rig.backend->report_action = AmsAction::PAUSED;
    AmsPanelTestAccess::tap_toolhead(*panel, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("The filament system needs attention") != std::string::npos);
}
