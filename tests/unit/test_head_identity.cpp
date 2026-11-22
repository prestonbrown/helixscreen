// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "core/update_queue.h"
#include "display_numbering.h"
#include "lvgl_test_fixture.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/nozzle_temps_widget.h"
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
using helix::ui::ToolNumberStyle;
using helix::ui::UpdateQueue;

namespace {

ToolInfo tool_on(int index, const char* extruder) {
    ToolInfo t;
    t.index = index;
    t.name = helix::ui::tool_label(index);
    t.extruder_name = extruder;
    return t;
}

std::string label(const HeadIdentity& head, ToolNumberStyle style, ToolLabelForm form) {
    return helix::ui::head_label_in_form(LaneNoun::Tool, style, head, form);
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
            const HeadIdentity head = helix::head_identity(tools, heads, i, 2, false);
            CHECK(head.name == tools[static_cast<size_t>(i)].name);
            CHECK(head.index == i);
            CHECK(head.lane_code.empty());
            for (const auto style : {ToolNumberStyle::Physical, ToolNumberStyle::Gcode}) {
                const std::string& name = tools[static_cast<size_t>(i)].name;
                CHECK(helix::ui::head_name_label(LaneNoun::Tool, style, head) ==
                      helix::ui::tool_name_label(LaneNoun::Tool, style, name, i));
                CHECK(helix::ui::head_badge_label(style, head) ==
                      helix::ui::tool_badge_label(style, name, i));
                CHECK(helix::ui::head_number_text(style, head) ==
                      helix::ui::tool_number_text(style, name, i));
                for (const ToolLabelForm form : helix::ui::kToolLabelForms) {
                    CHECK(label(head, style, form) ==
                          helix::ui::tool_label_in_form(LaneNoun::Tool, style, name, i, form));
                }
            }
        }
    }
    CHECK(label(helix::head_identity(tools, helix::physical_heads(tools), 2, 0, false),
                ToolNumberStyle::Physical, ToolLabelForm::Full) == "Tool 3 (T2)");
}

TEST_CASE("Where tool numbers name lanes a head goes by its extruder, its lane beside it",
          "[head-identity]") {
    // T0 alone on extruder, T1..T3 through a hub into extruder2 (no extruder1),
    // T4 alone on extruder3.
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder2"),
                                         tool_on(2, "extruder2"), tool_on(3, "extruder2"),
                                         tool_on(4, "extruder3")};
    const auto heads = helix::physical_heads(tools, false, true);
    REQUIRE(heads.size() == 3);

    // Lane T3 is mounted on the second head, which is extruder2's.
    const HeadIdentity second = helix::head_identity(tools, heads, 1, 3, true);
    CHECK(second.name.empty());
    CHECK(second.index == 2);
    CHECK(second.lane_code == "T3");
    CHECK(label(second, ToolNumberStyle::Gcode, ToolLabelForm::Code) == "E3");

    // A head fed by one lane is still a head of lanes: the third head is not "T4".
    const HeadIdentity third = helix::head_identity(tools, heads, 2, 3, true);
    CHECK(third.lane_code == "T4");
    CHECK(label(third, ToolNumberStyle::Gcode, ToolLabelForm::Code) == "E4");

    // A single head fed by lanes is extruder's, whichever lane it holds.
    const std::vector<ToolInfo> one = {tool_on(0, "extruder"), tool_on(1, "extruder")};
    const auto one_head = helix::physical_heads(one, false, true);
    REQUIRE(one_head.size() == 1);
    const HeadIdentity only = helix::head_identity(one, one_head, 0, 1, true);
    CHECK(only.index == 0);
    CHECK(only.lane_code == "T1");
    CHECK(helix::head_identity(one, one_head, 1, 1, true).index == -1);
}

TEST_CASE("Every head label in each style and numbering, lane rig or tool changer",
          "[head-identity]") {
    struct Row {
        ToolNumberStyle style;
        bool from_zero;
        bool lanes;
        const char* code;
        const char* full;
        const char* name;
        const char* badge;
    };
    // The head on extruder1: a tool changer's T1, or on a lane rig a head no
    // T number selects, so E<n> in the screen's numbering, never T<n>.
    const Row rows[] = {
        {ToolNumberStyle::Physical, false, false, "T1", "Tool 2 (T1)", "Tool 2", "2"},
        {ToolNumberStyle::Physical, true, false, "T1", "Tool 1", "Tool 1", "1"},
        {ToolNumberStyle::Gcode, false, false, "T1", "T1", "T1", "T1"},
        {ToolNumberStyle::Gcode, true, false, "T1", "T1", "T1", "T1"},
        {ToolNumberStyle::Physical, false, true, "2", "Tool 2 (E2)", "Tool 2", "2"},
        {ToolNumberStyle::Physical, true, true, "1", "Tool 1 (E1)", "Tool 1", "1"},
        {ToolNumberStyle::Gcode, false, true, "E2", "E2", "E2", "E2"},
        {ToolNumberStyle::Gcode, true, true, "E1", "E1", "E1", "E1"},
    };
    struct NumberingReset {
        ~NumberingReset() {
            helix::ui::set_lane_numbering_from_zero(false);
        }
    } reset;
    const std::vector<ToolInfo> tools = {tool_on(0, "extruder"), tool_on(1, "extruder1"),
                                         tool_on(2, "extruder2")};
    for (const Row& r : rows) {
        CAPTURE(static_cast<int>(r.style), r.from_zero, r.lanes);
        helix::ui::set_lane_numbering_from_zero(r.from_zero);
        const auto heads = helix::physical_heads(tools, false, r.lanes);
        const HeadIdentity head = helix::head_identity(tools, heads, 1, 0, r.lanes);
        CHECK(label(head, r.style, ToolLabelForm::Code) == r.code);
        CHECK(label(head, r.style, ToolLabelForm::Full) == r.full);
        CHECK(helix::ui::head_name_label(LaneNoun::Tool, r.style, head) == r.name);
        CHECK(helix::ui::head_badge_label(r.style, head) == r.badge);
        CHECK(helix::ui::head_number_text(r.style, head) == r.badge);
    }
}

TEST_CASE_METHOD(ToolStateFixture,
                 "Lanes on a tool changer name each head by its extruder through every rebuild",
                 "[head-identity][tool-state][ams-topology]") {
    // klipper-toolchanger tools T0..T6 sit on the heads, but T<n> loads lane
    // T<n>: the changer's T5 does not select the sixth head here.
    auto& ts = ToolState::instance();
    ts.init_tools(helix::test::make_head_discovery(7));
    // Lane 9 is mounted on the sixth head; lane 14 is seated in the parked seventh.
    ts.set_ams_topology(helix::test::make_hub_topology(9, {14}));
    UpdateQueue::instance().drain();

    const auto check = [&] {
        const auto& heads = ts.physical_heads();
        REQUIRE(heads.size() == 7);
        const HeadIdentity sixth = ts.head_identity(heads[5]);
        CHECK(sixth.name.empty());
        CHECK(sixth.index == 5);
        CHECK(sixth.lane_code == "T9");
        const HeadIdentity seventh = ts.head_identity(heads[6]);
        CHECK(seventh.index == 6);
        CHECK(seventh.lane_code == "T14");
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

    // The tool chooser: the head over the lane it holds.
    const auto& model = helix::ui::ToolHeadsModel::instance();
    CHECK(model.picker_lines(5, ToolLabelForm::Code).first == "6");
    CHECK(model.picker_lines(5, ToolLabelForm::Code).second == "T9");
    CHECK(model.picker_lines(5, ToolLabelForm::Full).first == "Tool 6");
    CHECK(model.picker_lines(5, ToolLabelForm::Full).second == "T9");
    CHECK(model.row_label(5, true) == "Tool 6");
    CHECK(model.row_label(5, false) == "6");
}

namespace {

/// The reporter's Voron 2.4 (debug bundle LB6LEZ44): klipper-toolchanger tools
/// T0..T4 on extruder..extruder4, a sixth AFC head on extruder5 with no tool
/// object, and twelve AFC lanes whose T numbers follow AFC's lane map.
helix::PrinterDiscovery lb6lez44_discovery() {
    nlohmann::json objects = nlohmann::json::array({"toolchanger"});
    for (int i = 0; i < 5; ++i) {
        objects.push_back("tool T" + std::to_string(i));
    }
    objects.push_back("extruder");
    for (int i = 1; i < 6; ++i) {
        objects.push_back("extruder" + std::to_string(i));
    }
    helix::PrinterDiscovery disc;
    disc.parse_objects(objects);
    return disc;
}

/// AFC's lanes in its own order, each with the T number it maps and the head
/// it feeds; tool Tn sources the lane mapped Tn. lane4 is Tooled in extruder2
/// (mounted), lane2 is loaded in extruder1, the extruder4 lane is Tooled.
helix::ToolTopology lb6lez44_topology() {
    struct Lane {
        int tool;
        const char* extruder;
        bool seated;
    };
    const Lane lanes[] = {
        {5, "extruder4", true},   // extruder4
        {6, "extruder5", false},  // extruder5
        {8, "extruder", false},   // lane1
        {2, "extruder1", true},   // lane2
        {7, "extruder2", false},  // lane3, HTLF_1
        {0, "extruder2", true},   // lane4, HTLF_1
        {4, "extruder", false},   // lane5, NightOwl
        {1, "extruder", false},   // lane6, NightOwl
        {10, "extruder3", false}, // lane7, Turtle_1
        {9, "extruder3", false},  // lane8
        {3, "extruder3", false},  // lane9
        {11, "extruder3", false}, // lane10
    };
    helix::ToolTopology topo;
    topo.tool_count = 12;
    topo.active_tool = 0;
    topo.allows_empty_carriage = true;
    topo.tools_are_lanes = true;
    topo.tool_to_slot.assign(12, -1);
    topo.tool_extruder.assign(12, "");
    topo.tool_seated.assign(12, false);
    for (int slot = 0; slot < 12; ++slot) {
        const Lane& lane = lanes[slot];
        topo.tool_to_slot[static_cast<size_t>(lane.tool)] = slot;
        topo.tool_extruder[static_cast<size_t>(lane.tool)] = lane.extruder;
        topo.tool_seated[static_cast<size_t>(lane.tool)] = lane.seated;
    }
    return topo;
}

} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "LB6LEZ44: every head is the physical toolhead, its lane beside it",
                 "[head-identity][tool-state][ams-topology][physical-heads]") {
    auto& ts = ToolState::instance();
    ts.init_tools(lb6lez44_discovery());
    ts.set_ams_topology(lb6lez44_topology());
    UpdateQueue::instance().drain();

    // Twelve lane tools, six heads: one per extruder, in the order they stand.
    REQUIRE(ts.tool_count() == 12);
    const auto& heads = ts.physical_heads();
    REQUIRE(heads.size() == 6);
    const char* extruders[] = {"extruder",  "extruder1", "extruder2",
                               "extruder3", "extruder4", "extruder5"};
    // The lane each head stands for: the mounted one (T0 in extruder2), the
    // seated one (T2, T5), else the head's lowest.
    const char* lanes[] = {"T1", "T2", "T0", "T3", "T5", "T6"};
    for (size_t i = 0; i < heads.size(); ++i) {
        CAPTURE(i);
        REQUIRE(heads[i].extruder_name == std::optional<std::string>(extruders[i]));
        const HeadIdentity head = ts.head_identity(heads[i]);
        CHECK(head.index == static_cast<int>(i));
        CHECK(head.lane_code == lanes[i]);

        // A tap sends the head's representative tool, and AFC mounts the head
        // that tool's lane feeds (AFC_SELECT_TOOL TOOL=<extruder>): this head,
        // keeping the lane it holds.
        const int sent = heads[i].representative_tool(ts.active_tool_index());
        CHECK(ts.tools()[static_cast<size_t>(sent)].name == lanes[i]);
        CHECK(ts.tools()[static_cast<size_t>(sent)].extruder_name == heads[i].extruder_name);
    }

    // No head is a T number, though tool objects T0..T4 sit on five of them:
    // the T0 G-code loads lane4 on extruder2, so a "T0" on the extruder head
    // would promise what typing it does not do. Each head is its extruder.
    const char* gcode[] = {"E1", "E2", "E3", "E4", "E5", "E6"};
    for (size_t i = 0; i < heads.size(); ++i) {
        CAPTURE(i);
        const HeadIdentity head = ts.head_identity(heads[i]);
        CHECK(head.name.empty());
        CHECK(label(head, ToolNumberStyle::Gcode, ToolLabelForm::Code) == gcode[i]);
        CHECK(label(head, ToolNumberStyle::Physical, ToolLabelForm::Code) == std::to_string(i + 1));
    }
    CHECK(label(ts.head_identity(heads[2]), ToolNumberStyle::Physical, ToolLabelForm::Full) ==
          "Tool 3 (E3)");

    const auto& model = helix::ui::ToolHeadsModel::instance();
    CHECK(model.picker_lines(2, ToolLabelForm::Code).first == "3");
    CHECK(model.picker_lines(2, ToolLabelForm::Code).second == "T0");
    CHECK(model.picker_lines(5, ToolLabelForm::Full).first == "Tool 6");
    CHECK(model.mounted_head() == 2);

    // Every other surface naming the mounted nozzle or a head by its
    // extruder names the head: lane T0 is mounted, on extruder2's head.
    CHECK(ts.nozzle_label() == "Nozzle 3");
    CHECK(ts.display_label_for_extruder("extruder2") == "Tool 3");
    CHECK(ts.display_label_for_extruder("extruder5") == "Tool 6");
    CHECK(ts.tool_name_for_extruder("extruder2").empty());
    // The temperature graph's legend: the head, not discovery's T code.
    CHECK(helix::ui::temperature::nozzle_display_name("extruder2", "Nozzle T2") == "Nozzle 3");
    {
        // Under the G-code style the legend says which head, never which lane.
        auto& settings = helix::SettingsManager::instance();
        settings.init_subjects();
        settings.set_tool_number_style_setting(helix::ui::ToolNumberStyleSetting::Gcode);
        struct StyleReset {
            ~StyleReset() {
                helix::SettingsManager::instance().set_tool_number_style_setting(
                    helix::ui::ToolNumberStyleSetting::Auto);
            }
        } reset;
        CHECK(helix::ui::temperature::nozzle_display_name("extruder", "Nozzle T0") == "Nozzle E1");
        CHECK(helix::ui::temperature::nozzle_display_name("extruder5", "Nozzle T5") == "Nozzle E6");
    }

    // The nozzle temperature rows stand in the same order as the heads.
    const std::vector<std::string> by_extruder(std::begin(extruders), std::end(extruders));
    CHECK(helix::distinct_extruder_names(ts.tools(), ts.tools_are_lanes()) == by_extruder);
    CHECK(helix::distinct_extruder_names(ts.tools()).front() == "extruder2");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "One lane per head still names each head by the head when tool numbers name lanes",
                 "[head-identity][tool-state][ams-topology][physical-heads]") {
    // Four lanes on four changer heads (tool objects T0..T3), lane T2 mapped to
    // the head on extruder3 and T3 to extruder2: T2 loads the extruder3 head.
    auto& ts = ToolState::instance();
    ts.init_tools(helix::test::make_head_discovery(4));
    helix::ToolTopology topo = helix::test::make_head_topology(4, 2);
    topo.tool_extruder[2] = "extruder3";
    topo.tool_extruder[3] = "extruder2";

    SECTION("lane maps: each head is its extruder, the lane beside it") {
        topo.tools_are_lanes = true;
        ts.set_ams_topology(topo);
        UpdateQueue::instance().drain();
        const auto& heads = ts.physical_heads();
        REQUIRE(heads.size() == 4);
        REQUIRE(heads[3].extruder_name == std::optional<std::string>("extruder3"));
        const HeadIdentity fourth = ts.head_identity(heads[3]);
        CHECK(fourth.name.empty());
        CHECK(fourth.index == 3);
        CHECK(fourth.lane_code == "T2");
        CHECK(label(fourth, ToolNumberStyle::Gcode, ToolLabelForm::Code) == "E4");
        CHECK(label(fourth, ToolNumberStyle::Physical, ToolLabelForm::Full) == "Tool 4 (E4)");
        const HeadIdentity third = ts.head_identity(heads[2]);
        CHECK(third.index == 2);
        CHECK(third.lane_code == "T3");
        // The mounted lane T2 is on the fourth head, and a tap there mounts it.
        CHECK(helix::ui::ToolHeadsModel::instance().mounted_head() == 3);
        CHECK(heads[3].representative_tool(ts.active_tool_index()) == 2);
    }

    SECTION("a tool changer's tools are its heads, so the same shape reads as before") {
        ts.set_ams_topology(topo);
        UpdateQueue::instance().drain();
        const auto& heads = ts.physical_heads();
        REQUIRE(heads.size() == 4);
        const HeadIdentity third = ts.head_identity(heads[2]);
        CHECK(third.name == "T2");
        CHECK(third.index == 2);
        CHECK(third.lane_code.empty());
        CHECK(helix::ui::temperature::nozzle_display_name("extruder2", "Nozzle T2") == "Nozzle T2");
    }
}
