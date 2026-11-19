// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "printer_discovery.h"
#include "target_tool_selection.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ToolState;
using helix::ui::decide_target_tool_action;
using helix::ui::TargetToolAction;
using helix::ui::TargetToolSelection;
using helix::ui::ToolArrivalWait;

TEST_CASE("TargetToolSelection follows the carriage and keeps a pick", "[target_tool]") {
    TargetToolSelection sel;
    CHECK(sel.current(6) == 0);
    sel.pick(3);
    CHECK(sel.current(6) == 3);
    sel.on_active_tool_changed(1);
    CHECK(sel.current(6) == 1);
    sel.pick(4);
    sel.on_active_tool_changed(-1);
    CHECK(sel.current(6) == 4);
    sel.pick(5);
    CHECK(sel.current(3) == 0);
}

TEST_CASE("decide_target_tool_action offers pick up or park", "[target_tool]") {
    CHECK(decide_target_tool_action(3, 1, true, true) == TargetToolAction::PickUp);
    CHECK(decide_target_tool_action(3, -1, true, true) == TargetToolAction::PickUp);
    CHECK(decide_target_tool_action(3, 3, true, true) == TargetToolAction::Park);
    CHECK(decide_target_tool_action(3, 3, true, false) == TargetToolAction::None);
    CHECK(decide_target_tool_action(3, 1, true, false) == TargetToolAction::PickUp);
    CHECK(decide_target_tool_action(3, 1, false, true) == TargetToolAction::None);
}

namespace {

const std::vector<std::string> kExtruders = {"extruder", "extruder1", "extruder2"};

void seed_three_heads() {
    ToolState::instance().init_subjects(true);
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& e : kExtruders) {
        objects.push_back(e);
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    ToolState::instance().init_tools(hw);
}

void mount(int tool) {
    ToolState::instance().update_from_status(
        {{"toolhead", {{"extruder", kExtruders[static_cast<size_t>(tool)]}}}});
    REQUIRE(ToolState::instance().active_tool_index() == tool);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ToolArrivalWait runs the step only once the tool is mounted",
                 "[target_tool]") {
    seed_three_heads();
    mount(0);
    int arrived = 0;
    int timed_out = 0;
    auto on_arrived = [&]() { ++arrived; };
    auto on_timeout = [&]() { ++timed_out; };

    SECTION("arrival") {
        ToolArrivalWait wait;
        wait.start(2, 1000, on_arrived, on_timeout);
        process_lvgl(50);
        CHECK(arrived == 0);
        mount(2);
        process_lvgl(50);
        CHECK(arrived == 1);
        process_lvgl(1200);
        CHECK(timed_out == 0);
        CHECK_FALSE(wait.armed());
    }
    SECTION("already mounted runs at once") {
        ToolArrivalWait wait;
        wait.start(0, 1000, on_arrived, on_timeout);
        CHECK(arrived == 1);
        CHECK_FALSE(wait.armed());
    }
    SECTION("timeout") {
        ToolArrivalWait wait;
        wait.start(2, 1000, on_arrived, on_timeout);
        process_lvgl(1200);
        CHECK(timed_out == 1);
        mount(2);
        process_lvgl(50);
        CHECK(arrived == 0);
    }
    SECTION("another tool settling on the carriage drops the step") {
        ToolArrivalWait wait;
        wait.start(2, 1000, on_arrived, on_timeout);
        mount(1);
        process_lvgl(50);
        CHECK(timed_out == 1);
        mount(2);
        process_lvgl(50);
        CHECK(arrived == 0);
    }
    SECTION("cancel") {
        ToolArrivalWait wait;
        wait.start(2, 1000, on_arrived, on_timeout);
        wait.cancel();
        mount(2);
        process_lvgl(1200);
        CHECK(arrived == 0);
        CHECK(timed_out == 0);
    }
    SECTION("destroyed while armed") {
        {
            ToolArrivalWait wait;
            wait.start(2, 1000, on_arrived, on_timeout);
        }
        mount(2);
        process_lvgl(1200);
        CHECK(arrived == 0);
        CHECK(timed_out == 0);
    }
}
