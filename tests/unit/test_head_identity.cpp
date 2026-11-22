// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "display_numbering.h"
#include "lvgl_test_fixture.h"
#include "printer_discovery.h"
#include "test_helpers/tool_topologies.h"
#include "tool_heads_model.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::HeadIdentity;
using helix::PhysicalHead;
using helix::ToolInfo;
using helix::ToolState;
using helix::ui::LaneNoun;
using helix::ui::ToolLabelForm;
using helix::ui::UpdateQueue;

namespace {

ToolInfo tool_on(int index, const char* extruder, std::string head_name = {}) {
    ToolInfo t;
    t.index = index;
    t.name = helix::ui::tool_label(index);
    t.extruder_name = extruder;
    t.head_name = std::move(head_name);
    return t;
}

/// Five lanes on three heads: T0 alone on the first, T1..T3 through a hub
/// into the second, T4 alone on the third. A head's tools are lanes here
/// even where it has one.
std::vector<ToolInfo> hub_lanes(bool changer_names) {
    const auto name = [&](const char* n) { return changer_names ? std::string(n) : std::string(); };
    return {tool_on(0, "extruder", name("T0")), tool_on(1, "extruder1", name("T1")),
            tool_on(2, "extruder1", name("T1")), tool_on(3, "extruder1", name("T1")),
            tool_on(4, "extruder2", name("T2"))};
}

std::string label(const HeadIdentity& head, ToolLabelForm form) {
    return helix::ui::head_label_in_form(LaneNoun::Tool, head, form);
}

struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        ToolState::instance().clear_ams_topology();
        UpdateQueue::instance().drain();
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE("A head that is its own tool is named and numbered as that tool", "[head-identity]") {
    // klipper-toolchanger, a U1, MedusaHC, INDX: every tool is a head, and the
    // head's labels are the tool's, letter for letter.
    std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1"),
                                   tool_on(2, "extruder2"), tool_on(3, "extruder3")};
    tools[1].name = "Left";
    for (const bool shared : {false, true}) {
        const auto heads = helix::physical_heads(tools, shared);
        REQUIRE(heads.size() == tools.size());
        for (int i = 0; i < static_cast<int>(heads.size()); ++i) {
            CAPTURE(shared, i);
            const HeadIdentity head = helix::head_identity(tools, heads, i, 2);
            CHECK(head.name == tools[static_cast<size_t>(i)].name);
            CHECK(head.index == i);
            CHECK(head.lane_code.empty());
            const std::string& name = tools[static_cast<size_t>(i)].name;
            CHECK(helix::ui::head_name_label(LaneNoun::Tool, head) ==
                  helix::ui::tool_name_label(LaneNoun::Tool, name, i));
            CHECK(helix::ui::head_number_or_name(head) == helix::ui::tool_number_or_name(name, i));
            for (const ToolLabelForm form : helix::ui::kToolLabelForms) {
                CHECK(label(head, form) ==
                      helix::ui::tool_label_in_form(LaneNoun::Tool, name, i, form));
            }
        }
    }
    CHECK(label(helix::head_identity(tools, helix::physical_heads(tools), 2, 0),
                ToolLabelForm::Full) == "Tool 3 (T2)");
}

TEST_CASE("A hub-fed head with no name of its own goes by its number, its lane beside it",
          "[head-identity]") {
    const auto tools = hub_lanes(/*changer_names=*/false);
    const auto heads = helix::physical_heads(tools);
    REQUIRE(heads.size() == 3);

    // Lane T3 is mounted on the second head: that head is head 2, holding T3.
    const HeadIdentity second = helix::head_identity(tools, heads, 1, 3);
    CHECK(second.name.empty());
    CHECK(second.index == 1);
    CHECK(second.lane_code == "T3");
    // No G-code name selects the head: T1 would load lane T1, so no form
    // spells one.
    CHECK(label(second, ToolLabelForm::Code) == "2");
    CHECK(label(second, ToolLabelForm::Full) == "Tool 2");
    CHECK(helix::ui::head_name_label(LaneNoun::Tool, second) == "Tool 2");
    CHECK(helix::ui::head_number_or_name(second) == "2");

    // A head fed by one lane is still a head of lanes: the third head is not "T4".
    const HeadIdentity third = helix::head_identity(tools, heads, 2, 3);
    CHECK(third.lane_code == "T4");
    CHECK(label(third, ToolLabelForm::Code) == "3");
}

TEST_CASE("A hub-fed head behind a changer tool is named by that tool, its lane beside it",
          "[head-identity]") {
    const auto tools = hub_lanes(/*changer_names=*/true);
    const auto heads = helix::physical_heads(tools);

    const HeadIdentity second = helix::head_identity(tools, heads, 1, 3);
    CHECK(second.name == "T1");
    CHECK(second.index == 1);
    CHECK(second.lane_code == "T3");
    CHECK(label(second, ToolLabelForm::Code) == "T1");
    CHECK(label(second, ToolLabelForm::Full) == "Tool 2 (T1)");

    const HeadIdentity third = helix::head_identity(tools, heads, 2, 3);
    CHECK(third.name == "T2");
    CHECK(third.lane_code == "T4");
}

TEST_CASE("A single head fed by lanes is head 1 whichever lane it holds", "[head-identity]") {
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder"),
                                         tool_on(2, "extruder"), tool_on(3, "extruder")};
    const auto heads = helix::physical_heads(tools);
    REQUIRE(heads.size() == 1);
    const HeadIdentity head = helix::head_identity(tools, heads, 0, 2);
    CHECK(head.name.empty());
    CHECK(head.index == 0);
    CHECK(head.lane_code == "T2");
    CHECK(label(head, ToolLabelForm::Full) == "Tool 1");
    CHECK(label(head, ToolLabelForm::Code) == "1");

    CHECK(helix::head_identity(tools, heads, 1, 2).index == -1);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "Lanes on a tool changer keep each head's changer name through every rebuild",
                 "[head-identity][tool-state][ams-topology]") {
    auto& ts = ToolState::instance();
    ts.init_tools(helix::test::make_head_discovery(7));
    // Lane 9 is mounted on the sixth head; lane 14 is seated in the parked seventh.
    ts.set_ams_topology(helix::test::make_hub_topology(9, {14}));
    UpdateQueue::instance().drain();

    const auto check = [&] {
        const auto& heads = ts.physical_heads();
        REQUIRE(heads.size() == 7);
        const HeadIdentity sixth = ts.head_identity(heads[5]);
        CHECK(sixth.name == "T5");
        CHECK(sixth.index == 5);
        CHECK(sixth.lane_code == "T9");
        const HeadIdentity seventh = ts.head_identity(heads[6]);
        CHECK(seventh.name == "T6");
        CHECK(seventh.lane_code == "T14");
        CHECK(ts.head_identity(heads[0]).name == "T0");
    };
    check();

    // A remap rebuilds the tools from the lane list itself.
    auto topo = helix::test::make_hub_topology(9, {14});
    topo.tool_to_slot[0] = 1;
    topo.tool_to_slot[1] = 0;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    check();

    // The heated heads are the same heads.
    const auto heated = ts.heated_heads();
    REQUIRE(heated.size() == 7);
    CHECK(ts.head_identity(heated[5]).lane_code == "T9");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "The tool chooser names a hub-fed head by the head, the lane on its second line",
                 "[head-identity][tool-state][ams-topology]") {
    auto& ts = ToolState::instance();
    // Seven extruders and no changer: nothing names a head.
    nlohmann::json objects = nlohmann::json::array({"extruder"});
    for (int i = 1; i < 7; ++i) {
        objects.push_back("extruder" + std::to_string(i));
    }
    helix::PrinterDiscovery disc;
    disc.parse_objects(objects);
    ts.init_tools(disc);
    ts.set_ams_topology(helix::test::make_hub_topology(9));
    UpdateQueue::instance().drain();

    const auto& model = helix::ui::ToolHeadsModel::instance();
    REQUIRE(model.head_count() == 7);
    REQUIRE(ts.tools()[9].head_name.empty());
    CHECK(model.picker_lines(5, ToolLabelForm::Code).first == "6");
    CHECK(model.picker_lines(5, ToolLabelForm::Code).second == "T9");
    CHECK(model.picker_lines(5, ToolLabelForm::Full).first == "Tool 6");
    CHECK(model.picker_lines(5, ToolLabelForm::Full).second == "T9");
    CHECK(model.row_label(5, true) == "Tool 6");
    CHECK(model.row_label(5, false) == "6");
    // A head of one lane on the same printer is numbered as a head too.
    CHECK(model.picker_lines(1, ToolLabelForm::Code).first == "2");
    CHECK(model.picker_lines(1, ToolLabelForm::Code).second == "T1");
}
