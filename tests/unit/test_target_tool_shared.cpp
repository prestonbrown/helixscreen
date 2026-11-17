// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_target_tool_shared.cpp
 * @brief One target tool for the Controls temperature card, the Filament panel
 *        and the temperature graph: a pick on any of them aims all of them,
 *        it follows the carriage on a tool change, and it never changes tools.
 */

#include "ui_overlay_temp_graph.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/update_queue_test_access.h"
#include "printer_state.h"
#include "target_tool.h"
#include "tool_heads_model.h"
#include "tool_picker.h"
#include "tool_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ToolState;
using helix::ui::TargetTool;
using helix::ui::ToolHeadsModel;

namespace {

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

std::string subject_text(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

/// A three-head changer (GateBackend counts every tool change) with each
/// head's extruder at its own target, T0 mounted.
struct ThreeHeads {
    LVGLUITestFixture& fx;
    helix::ToolChangerRig rig{3};

    explicit ThreeHeads(LVGLUITestFixture& f) : fx(f) {
        auto& temps = fx.state().temperature_state();
        temps.init_extruders({"extruder", "extruder1", "extruder2"});
        fx.state().update_from_status(
            {{"extruder", {{"temperature", 25.0}, {"target", 0.0}}},
             {"extruder1", {{"temperature", 150.0}, {"target", 180.0}, {"power", 1.0}}},
             {"extruder2", {{"temperature", 230.0}, {"target", 230.0}}}});
        TargetTool::instance().attach(fx.state());
        ToolHeadsModel::instance().attach(fx.state());
        drain();
    }

    /// Leave T0 on the carriage: ToolState's tool list outlives the case, and a
    /// later one that sets up no tools of its own reads it.
    ~ThreeHeads() {
        ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", 0}}}});
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: a pick aims every surface's nozzle reading",
                 "[target_tool][shared]") {
    ThreeHeads h(*this);
    REQUIRE(TargetTool::instance().tool() == 0);
    REQUIRE(subject_int("target_tool_multi_head") == 1);

    TargetTool::instance().pick(1);
    h.drain();
    // The subjects the Controls card, the Filament Tool card and the graph's
    // card all bind.
    CHECK(subject_int("target_tool") == 1);
    CHECK(subject_text("target_nozzle_label") == "Nozzle 2");
    CHECK(subject_text("target_tool_name") == "Tool 2");
    CHECK(subject_text("target_tool_number") == "2");
    CHECK(subject_int("target_nozzle_temp") == 1500);
    CHECK(subject_int("target_nozzle_target") == 1800);
    CHECK(subject_int("target_nozzle_status_state") ==
          static_cast<int>(helix::ui::temperature::HeaterStatusState::Heating));
    // The tool row highlights the target's head, not the mounted one.
    CHECK(subject_int("tool_switcher_seg_1_target_state") == 1);
    CHECK(subject_int("tool_switcher_seg_0_target_state") == 0);
    CHECK(subject_int("tool_switcher_seg_0_state") == 1);
    // Aiming never changes tools.
    CHECK(h.rig.backend->change_tool_calls == 0);
    CHECK(ToolState::instance().active_tool_index() == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: the graph opened on a nozzle aims the others",
                 "[target_tool][shared][temp_graph_overlay]") {
    ThreeHeads h(*this);
    // A home tile naming its nozzle opens the graph on it; the Controls card
    // and the Filament panel read the same target.
    TempGraphOverlay overlay;
    overlay.open(TempGraphOverlay::Mode::Nozzle, nullptr, "extruder2");
    h.drain();
    CHECK(TargetTool::instance().tool() == 2);
    CHECK(subject_text("target_nozzle_label") == "Nozzle 3");
    CHECK(h.rig.backend->change_tool_calls == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: follows the carriage, keeps a pick otherwise",
                 "[target_tool][shared]") {
    ThreeHeads h(*this);
    TargetTool::instance().pick(2);
    h.drain();

    SECTION("a surface resubscribing is not a tool change") {
        TargetTool::instance().attach(state());
        h.drain();
        CHECK(TargetTool::instance().tool() == 2);
    }
    SECTION("an empty carriage keeps the pick") {
        helix::ToolChangerRig::unmount();
        h.drain();
        CHECK(TargetTool::instance().tool() == 2);
    }
    SECTION("a tool change moves it to the mounted tool") {
        ToolState::instance().update_from_status({{"toolchanger", {{"tool_number", 1}}}});
        h.drain();
        CHECK(TargetTool::instance().tool() == 1);
        CHECK(TargetTool::instance().mounted());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: a target row cell aims without changing tools",
                 "[target_tool][shared][temp_graph_overlay]") {
    ThreeHeads h(*this);
    // The temperature graph's row of heads: the shared segment, one per head.
    const char* attrs[] = {"labels", "_row_graph", nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "tool_target_row", attrs));
    REQUIRE(row != nullptr);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(row) == 3);
    lv_obj_t* cell = lv_obj_find_by_name(row, "tool_target_cell_2");
    REQUIRE(cell != nullptr);

    lv_obj_send_event(cell, LV_EVENT_CLICKED, nullptr);
    h.drain();
    CHECK(TargetTool::instance().tool() == 2);
    CHECK(subject_int("tool_switcher_seg_2_target_state") == 1);
    CHECK(h.rig.backend->change_tool_calls == 0);
    // The row's labels are in the display numbering.
    const std::string label = subject_text("tool_switcher_seg_2_row_graph");
    CHECK((label == "Tool 3" || label == "3"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: the graph's card binds the shared target",
                 "[target_tool][shared][temp_graph_overlay]") {
    ThreeHeads h(*this);
    TempGraphOverlay overlay;
    overlay.init_subjects();
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "temp_graph_overlay", nullptr));
    REQUIRE(root != nullptr);
    process_lvgl(30);
    // The tool row is the shared segment, not a row of pills built in C++.
    lv_obj_t* row = lv_obj_find_by_name(root, "extruder_selector_row");
    REQUIRE(row != nullptr);
    CHECK(lv_obj_find_by_name(row, "tool_target_cell_0") != nullptr);
    CHECK(lv_obj_find_by_name(row, "tool_target_cell_2") != nullptr);
    lv_obj_delete(root);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target picker: highlights the target, no footer, no change",
                 "[target_tool][tool_picker]") {
    ThreeHeads h(*this);
    h.rig.backend->can_park = true;
    // The Home switcher's subjects (Park and the feeder's support) exist with
    // or without a tile placed: the app registers every widget's at startup.
    helix::ToolSwitcherWidget registers_switcher_subjects(state());
    // The changer can park, as the switcher publishes it: Mount mode would
    // show Park under the grid.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "tool_switcher_park_supported"), 1);
    TargetTool::instance().pick(2);
    h.drain();

    lv_obj_t* chip = lv_obj_create(test_screen());
    REQUIRE(helix::ui::open_target_tool_picker(chip));
    process_lvgl(30);
    auto& picker = helix::ui::tool_picker();
    REQUIRE(picker.is_visible());
    CHECK(picker.mode() == helix::ui::ToolPicker::Mode::Target);
    // The target's head is the highlighted cell, not the mounted one.
    CHECK(subject_int("tool_switcher_seg_2_pick_state") == 1);
    CHECK(subject_int("tool_switcher_seg_0_pick_state") == 0);
    // Park and Open feeder belong to mounting, not aiming.
    lv_obj_t* footer = lv_obj_find_by_name(lv_layer_top(), "tool_picker_footer");
    if (!footer) {
        footer = lv_obj_find_by_name(test_screen(), "tool_picker_footer");
    }
    REQUIRE(footer != nullptr);
    CHECK(lv_obj_has_flag(footer, LV_OBJ_FLAG_HIDDEN));
    // No cell is disabled: nothing here changes tools.
    CHECK(subject_int("tool_picker_blocked") == 0);

    picker.choose(1);
    h.drain();
    CHECK_FALSE(picker.is_visible());
    CHECK(TargetTool::instance().tool() == 1);
    CHECK(ToolState::instance().active_tool_index() == 0);
    CHECK(h.rig.backend->change_tool_calls == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Target tool: one head has no chooser",
                 "[target_tool][shared]") {
    // A single hotend: the target is the mounted tool, whatever ToolState lists.
    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(true);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    ToolState::instance().init_tools(hw);
    TargetTool::instance().attach(state());
    ThreeHeads::drain();
    CHECK(subject_int("target_tool_multi_head") == 0);
    CHECK(subject_text("target_tool_number").empty());
    CHECK(subject_text("target_nozzle_label") == "Nozzle");
    CHECK(TargetTool::instance().mounted());
}
