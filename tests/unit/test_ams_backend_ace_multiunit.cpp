// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Several ACE units on the ACEPRO driver (ace_instance_0, ace_instance_1, ...):
// each instance is a unit of its own, its slots numbered globally after the
// units before it. A slot's tool is the `tool` the driver publishes for it
// (instance*4 + slot on ACEPRO), never assumed from the slot's position.

#include "ui_update_queue.h"

#include "../test_helpers/ace_backend_test_helper.h"
#include "../test_helpers/ace_test_access.h"
#include "ams_backend_ace.h"
#include "ams_state.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "fake_moonraker_client.h"
#include "filament_chip_model.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "lvgl_ui_test_fixture.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "test_helpers/registered_backend.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <functional>
#include <json.hpp> // nlohmann/json from libhv
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsAction;
using helix::PathSegment;
using helix::SlotStatus;
using json = nlohmann::json;
using namespace ace_fixtures;

namespace {

constexpr uint32_t kUnit0Slot0Color = 0x00E676u; // make_kobra_slots_array, slot 0
constexpr uint32_t kUnit1Color = 0x3F51B5u;      // make_kobra_second_unit_object

void send_frame(AmsBackendAceTestHelper& helper, const json& frame) {
    helper.test_handle_status_update({{"params", json::array({frame, 6000.0})}});
}

json manager_delta(const json& fields) {
    json frame = json::object();
    frame["ace"] = fields;
    return frame;
}

/// The bootstrap of a rig with @p unit_count ACE units, unit N's tools at
/// 4N..4N+3 (ACEPRO numbers tools across its instances in instance order).
/// Unit 0 and the first @p ace_pro_units - 1 further units are ACE Pros, the
/// rest ACE 2s. Like Moonraker, the answer holds only the objects the query
/// asked for.
/// @p edit, when given, changes the printer's full status before it answers.
void start_units(AmsBackendAceTestHelper& helper, helix::test::FakeMoonrakerClient& client,
                 int unit_count, int current_index, int ace_pro_units = 1,
                 const std::function<void(json&)>& edit = nullptr) {
    helper.test_on_started();
    REQUIRE_FALSE(client.rpc_calls.empty());
    const auto& call = client.rpc_calls.back();
    REQUIRE(call.method == "printer.objects.query");
    REQUIRE(call.success_cb);
    json response = make_kobra_objects_query_response(current_index);
    json& status = response["result"]["status"];
    status["ace"]["ace_instances"] = unit_count;
    for (int unit = 1; unit < unit_count; ++unit) {
        json instance = make_kobra_second_unit_object();
        instance["instance"] = unit;
        if (unit < ace_pro_units) {
            instance["protocol"] = "ace1_json";
            instance["model"] = "Anycubic Color Engine Pro";
        }
        for (int i = 0; i < 4; ++i) {
            instance["slots"][i]["tool"] = 4 * unit + i;
        }
        status["ace_instance_" + std::to_string(unit)] = instance;
    }
    if (edit) {
        edit(status);
    }
    json answered = json::object();
    for (const auto& [name, value] : status.items()) {
        if (call.params["objects"].contains(name)) {
            answered[name] = value;
        }
    }
    status = answered;
    call.success_cb(response);
    helix::ui::UpdateQueue::instance().drain();
}

/// What objects.list reports for ACEPRO with @p unit_count units.
helix::PrinterDiscovery ace_discovery(int unit_count) {
    json objects = json::array({"ace", "extruder", "heater_bed"});
    for (int unit = 0; unit < unit_count; ++unit) {
        objects.push_back("ace_instance_" + std::to_string(unit));
    }
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(objects);
    return discovery;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ACE shows each ACEPRO instance as its own unit",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.total_slots == 8);
    CHECK(info.units[0].first_slot_global_index == 0);
    CHECK(info.units[1].first_slot_global_index == 4);
    CHECK(info.units[1].slot_count == 4);
    CHECK(info.units[1].unit_index == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(info.units[1].slots[static_cast<size_t>(i)].global_index == 4 + i);
    }

    // Tools 4-7 carry the second unit's spools; tools 0-3 keep the first's.
    CHECK(helper.get_slot_info(5).material == "ABS");
    CHECK(helper.get_slot_info(5).color_rgb == kUnit1Color);
    CHECK(helper.get_slot_info(0).material == "PLA");
    CHECK(helper.get_slot_info(0).color_rgb == kUnit0Slot0Color);

    // Each unit reports its own environment.
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[1].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == 34.0f);
    CHECK(info.units[1].environment->temperature_c == 51.0f);
    CHECK(info.units[1].environment->humidity_pct == 61.0f);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE units all feed the one toolhead through one hub",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    const auto info = helper.get_test_system_info();
    const ams_draw::SystemToolLayout layout = ams_draw::compute_system_tool_layout(info, &helper);
    REQUIRE(layout.units.size() == 2);
    CHECK(layout.total_physical_tools == 1);
    CHECK(layout.units[0].tool_count == 1);
    CHECK(layout.units[1].tool_count == 1);
    CHECK(layout.units[1].first_physical_tool == layout.units[0].first_physical_tool);
    CHECK(layout.hub_groups.size() == 1);
    CHECK(layout.units[1].hub_group == layout.units[0].hub_group);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE with three units numbers the third unit's slots 8-11",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, 10);

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 3);
    CHECK(info.total_slots == 12);
    CHECK(info.units[2].first_slot_global_index == 8);
    CHECK(info.current_slot == 10);
    CHECK(helper.get_slot_info(10).status == SlotStatus::LOADED);
    CHECK(helper.get_slot_info(10).material == "ABS");
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE second-unit delta updates the second unit only",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    json slots = make_kobra_second_unit_object()["slots"];
    slots[0]["material"] = "PETG";
    slots[0]["color"] = json::array({255, 0, 0});
    json frame = json::object();
    frame["ace_instance_1"] = json{{"slots", slots}, {"temp", 52}};
    send_frame(helper, frame);

    CHECK(helper.get_slot_info(4).material == "PETG");
    CHECK(helper.get_slot_info(4).color_rgb == 0xFF0000u);
    CHECK(helper.get_dryer_info(1).current_temp_c == 52.0f);
    // Sibling: unit 0 is untouched.
    CHECK(helper.get_slot_info(0).material == "PLA");
    CHECK(helper.get_dryer_info(0).current_temp_c == 34.0f);
}

// T0 seated, a toolchange to T5 on the second unit, in the order the ACEPRO driver
// reported it on a two-unit Kobra S1: target_index first, the outgoing strand
// retracting past the toolhead and hub sensors, unit 1 feeding until both
// sensors are made again, then current_index.
TEST_CASE_METHOD(LVGLTestFixture, "ACE toolchange to a tool on the second unit",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, 0);
    REQUIRE(helper.get_test_system_info().current_slot == 0);

    send_frame(helper, manager_delta({{"target_index", 5}, {"toolchange_in_progress", true}}));
    {
        const auto info = helper.get_test_system_info();
        CHECK(info.action == AmsAction::UNLOADING);
        CHECK(info.pending_target_slot == 5);
        // The outgoing lane retracts first.
        CHECK(info.path_active_slot() == 0);
    }

    send_frame(helper, manager_delta({{"toolhead_sensor", false}}));
    send_frame(helper, manager_delta({{"rdm_sensor", false}}));
    {
        const auto info = helper.get_test_system_info();
        CHECK(info.action == AmsAction::LOADING);
        CHECK(info.path_active_slot() == 5);
    }

    json feeding = make_kobra_second_unit_object()["slots"];
    feeding[1]["status"] = "feeding";
    json unit1_frame = json::object();
    unit1_frame["ace_instance_1"] = json{{"status", "busy"}, {"slots", feeding}};
    send_frame(helper, unit1_frame);
    send_frame(helper, manager_delta({{"rdm_sensor", true}}));
    send_frame(helper, manager_delta({{"toolhead_sensor", true}}));
    {
        const auto info = helper.get_test_system_info();
        CHECK(info.action == AmsAction::LOADING);
        CHECK(info.path_active_slot() == 5);
        CHECK(helper.get_slot_info(0).material == "PLA");
    }

    send_frame(helper, manager_delta({{"current_index", 5}, {"target_index", -1}}));
    const auto info = helper.get_test_system_info();
    CHECK(info.action == AmsAction::IDLE);
    CHECK(info.current_slot == 5);
    CHECK(info.current_tool == 5);
    CHECK(info.filament_loaded);
    CHECK(helper.get_slot_info(5).status == SlotStatus::LOADED);
    CHECK(helper.get_slot_info(0).status != SlotStatus::LOADED);
    CHECK(helper.get_filament_segment() == PathSegment::NOZZLE);
    CHECK(helper.get_slot_filament_segment(5) == PathSegment::NOZZLE);
}

// A notify frame carries only the fields that changed: a unit's frame for a
// slot moving says nothing about its humidity, which still stands.
TEST_CASE_METHOD(LVGLTestFixture, "ACE unit keeps its humidity through a frame without it",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, 0);
    REQUIRE(helper.get_test_system_info().units[1].environment->has_humidity);

    json moving = make_kobra_second_unit_object()["slots"];
    moving[1]["status"] = "feeding";
    json frame = json::object();
    frame["ace_instance_1"] = json{{"status", "busy"}, {"slots", moving}};
    send_frame(helper, frame);

    const auto env = helper.get_test_system_info().units[1].environment;
    REQUIRE(env.has_value());
    CHECK(env->has_humidity);
    CHECK(env->humidity_pct == 61.0f);
    CHECK(env->temperature_c == 51.0f);
}

// A toolchange started from this screen: the load sets LOADING before the
// driver says anything, then the driver names its target while the seated
// tool is still in the toolhead. The path stays on the outgoing tool.
TEST_CASE_METHOD(LVGLTestFixture, "ACE load started here retracts the seated tool first",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, 7);
    REQUIRE(helper.get_test_system_info().current_slot == 7);

    helper.set_running(true);
    REQUIRE(helper.load_filament(4).success());
    send_frame(helper, manager_delta({{"target_index", 4}, {"toolchange_in_progress", true}}));

    const auto info = helper.get_test_system_info();
    CHECK(info.pending_target_slot == 4);
    CHECK(info.action == AmsAction::UNLOADING);
    CHECK(info.path_active_slot() == 7);
}

// The driver names each slot's tool, so a Load from the screen with a tool
// seated is planned as the toolchange it is: the step bar then shows the cut
// and retract before the feed.
TEST_CASE_METHOD(LVGLTestFixture, "ACE load with a tool seated is planned as a toolchange",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);

    SECTION("T7 seated: loading slot 3 changes to T3") {
        start_units(helper, client, 2, 7);
        helix::AmsSystemInfo sys;
        const auto caps = helix::ui::read_backend_caps(&helper, sys, 3);
        const auto plan = helix::ui::plan_load(sys, caps, 3, false, false);
        CHECK(plan.ams_call == helix::ui::AmsCall::ChangeTool);
        CHECK(plan.ams_arg == 3);
    }

    SECTION("nothing seated: a plain load") {
        start_units(helper, client, 2, -1);
        helix::AmsSystemInfo sys;
        const auto caps = helix::ui::read_backend_caps(&helper, sys, 3);
        const auto plan = helix::ui::plan_load(sys, caps, 3, false, false);
        CHECK(plan.ams_call == helix::ui::AmsCall::Load);
        CHECK(plan.ams_arg == 3);
    }
}

// A unit whose ACE has not reported its slots yet (its USB reconnecting) keeps
// its place: the units after it keep their slot numbers when it reports.
TEST_CASE_METHOD(LVGLTestFixture, "ACE unit reporting late keeps the other units' slots",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, 1,
                [](json& status) { status["ace_instance_0"].erase("slots"); });
    {
        const auto info = helper.get_test_system_info();
        REQUIRE(info.units.size() == 2);
        CHECK_FALSE(info.units[0].connected);
        CHECK(info.units[1].first_slot_global_index == 4);
        CHECK(helper.get_slot_info(5).color_rgb == kUnit1Color);
    }

    json frame = json::object();
    frame["ace_instance_0"] = make_kobra_instance_object();
    send_frame(helper, frame);

    const auto info = helper.get_test_system_info();
    CHECK(info.units[0].connected);
    CHECK(info.units[1].first_slot_global_index == 4);
    CHECK(helper.get_slot_info(5).color_rgb == kUnit1Color);
    CHECK(helper.get_slot_info(0).color_rgb == kUnit0Slot0Color);
}

// The manager's current_index can name a tool whose unit has not reported its
// slots yet; the seat lands on that tool's slot once they arrive.
TEST_CASE_METHOD(LVGLTestFixture, "ACE seats a tool whose unit reports after the manager",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 1, 5);
    {
        const auto info = helper.get_test_system_info();
        REQUIRE(info.current_tool == 5);
        REQUIRE(info.current_slot == -1);
    }

    json frame = json::object();
    frame["ace_instance_1"] = make_kobra_second_unit_object();
    send_frame(helper, frame);

    const auto info = helper.get_test_system_info();
    CHECK(info.current_tool == 5);
    CHECK(info.current_slot == 5);
    CHECK(helper.get_slot_info(5).status == SlotStatus::LOADED);
    CHECK(helper.get_slot_info(4).status != SlotStatus::LOADED);
}

// A restart drops the session's commands with it: a load sent before it does
// not keep a later driver toolchange LOADING.
TEST_CASE_METHOD(LVGLTestFixture, "ACE restart during a load drops the load",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, 7);
    helper.set_running(true);
    REQUIRE(helper.load_filament(4).success());

    start_units(helper, client, 2, 7);
    CHECK(helper.get_test_system_info().action == AmsAction::IDLE);

    send_frame(helper, manager_delta({{"target_index", 4}, {"toolchange_in_progress", true}}));
    send_frame(helper, manager_delta({{"current_index", 4}, {"target_index", -1}}));
    CHECK(helper.get_test_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE ignores an instance number past the unit limit",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    json frame = json::object();
    frame["ace_instance_100000"] = make_kobra_second_unit_object();
    send_frame(helper, frame);

    CHECK(helper.get_test_system_info().units.size() == 2);
}

// On the ACEPRO driver a slot's tool is the driver's to name: a slot it has not named
// yet is not loaded under a guessed number, which could be another unit's tool.
TEST_CASE_METHOD(LVGLTestFixture, "ACE refuses a load the driver named no tool for",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, 1,
                [](json& status) { status["ace_instance_1"]["slots"][1].erase("tool"); });
    helper.set_running(true);

    CHECK_FALSE(helper.load_filament(5).success());
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE dryer refuses a unit the ACEPRO driver does not have",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helper.set_running(true);

    CHECK_FALSE(helper.start_drying(50.0f, 60, -1, -1).success());
    CHECK_FALSE(helper.start_drying(50.0f, 60, -1, 2).success());
    CHECK_FALSE(helper.stop_drying(2).success());
    CHECK(helper.captured_gcodes.empty());
}

// The driver retracts the seated tool before it feeds another, so a load sent
// from here with a tool seated starts as the unload it first is.
TEST_CASE_METHOD(LVGLTestFixture, "ACE load with a tool seated starts by unloading",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, 7);
    helper.set_running(true);

    REQUIRE(helper.load_filament(4).success());
    const auto info = helper.get_test_system_info();
    CHECK(info.action == AmsAction::UNLOADING);
    CHECK(info.path_active_slot() == 7);
}

// The tool map states what the per-spool T badges would add: with every tool
// on the slot of its own number they repeat the spool number and stay hidden.
TEST_CASE_METHOD(LVGLTestFixture, "ACE publishes the driver's tool map", "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);

    SECTION("tools numbered like the slots: the badges add nothing") {
        start_units(helper, client, 2, -1);
        const auto info = helper.get_test_system_info();
        REQUIRE(info.tool_to_slot_map.size() == 8);
        CHECK(info.tool_to_slot_map[5] == 5);
        CHECK_FALSE(
            helix::ui::tool_badges_add_information(info.tool_to_slot_map, info.total_slots));
    }

    SECTION("tools numbered apart from the slots: the badges name them") {
        start_units(helper, client, 1, -1, 1, [](json& status) {
            for (int i = 0; i < 4; ++i) {
                status["ace_instance_0"]["slots"][i]["tool"] = 4 + i;
            }
        });
        const auto info = helper.get_test_system_info();
        REQUIRE(info.tool_to_slot_map.size() > 6);
        CHECK(info.tool_to_slot_map[6] == 2);
        CHECK(helix::ui::tool_badges_add_information(info.tool_to_slot_map, info.total_slots));
    }
}

// The ACEPRO driver states each unit's link; a unit that is not connected shows as
// disconnected, and a frame that does not restate the link leaves it be.
TEST_CASE_METHOD(LVGLTestFixture, "ACE shows the ACEPRO driver's per-unit connection",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, 1, [](json& status) {
        status["ace_instance_1"]["connection_state"] = "initializing";
    });
    auto unit_connected = [&helper](int unit) {
        return helper.get_test_system_info().units[static_cast<size_t>(unit)].connected;
    };
    auto link = [&helper](const char* state) {
        json frame = json::object();
        frame["ace_instance_1"] = json{{"connection_state", state}};
        send_frame(helper, frame);
    };
    CHECK(unit_connected(0));
    CHECK_FALSE(unit_connected(1));

    link("connected");
    CHECK(unit_connected(1));

    for (const char* state : {"initializing", "connecting", "reconnecting", "disabled"}) {
        link("connected");
        link(state);
        INFO("connection_state " << state);
        CHECK_FALSE(unit_connected(1));
    }

    SECTION("a slots frame that does not restate the link keeps it") {
        link("reconnecting");
        json frame = json::object();
        frame["ace_instance_1"] = json{{"slots", make_kobra_second_unit_object()["slots"]}};
        send_frame(helper, frame);
        CHECK_FALSE(unit_connected(1));
    }

    SECTION("a state this backend does not know claims nothing") {
        link("connected");
        link("unknown");
        CHECK(unit_connected(1));
    }
}

// A guard the per-unit status words must keep: one unit still reporting
// "ready" does not end the other unit's "loading".
TEST_CASE_METHOD(LVGLTestFixture, "ACE idle unit does not clear another unit's action",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    json frame = json::object();
    frame["ace_instance_0"] = json{{"status", "loading"}};
    frame["ace_instance_1"] = json{{"status", "ready"}};
    send_frame(helper, frame);
    CHECK(helper.get_test_system_info().action == AmsAction::LOADING);

    SECTION("and the unit's own ready ends it") {
        json done = json::object();
        done["ace_instance_0"] = json{{"status", "ready"}};
        send_frame(helper, done);
        CHECK(helper.get_test_system_info().action == AmsAction::IDLE);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE dryer commands name the unit on the ACEPRO driver",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helper.set_running(true);

    REQUIRE(helper.start_drying(50.0f, 60, -1, 1).success());
    REQUIRE(helper.stop_drying(1).success());
    REQUIRE(helper.stop_drying(0).success());

    CHECK(helper.captured_gcodes == std::vector<std::string>{
                                        "ACE_START_DRYING INSTANCE=1 TEMP=50 DURATION=60",
                                        "ACE_STOP_DRYING INSTANCE=1",
                                        "ACE_STOP_DRYING INSTANCE=0",
                                    });
}

// The driver's tool numbers are read off the `tool` each slot publishes, not
// assumed to equal the slot: here the first unit's slots carry T4-T7, and the
// seat, the toolchange target and the load command all follow them.
TEST_CASE_METHOD(LVGLTestFixture, "ACE maps the driver's tool numbers to slots",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    helper.test_on_started();
    REQUIRE_FALSE(client.rpc_calls.empty());
    json response = make_kobra_objects_query_response(6);
    for (int i = 0; i < 4; ++i) {
        response["result"]["status"]["ace_instance_0"]["slots"][i]["tool"] = 4 + i;
    }
    client.rpc_calls.back().success_cb(response);
    helix::ui::UpdateQueue::instance().drain();

    auto info = helper.get_test_system_info();
    CHECK(info.current_tool == 6);
    CHECK(info.current_slot == 2);
    CHECK(helper.get_slot_info(2).status == SlotStatus::LOADED);

    send_frame(helper, manager_delta({{"target_index", 5}}));
    CHECK(helper.get_test_system_info().pending_target_slot == 1);
    send_frame(helper, manager_delta({{"target_index", -1}}));

    helper.set_running(true);
    REQUIRE(helper.load_filament(3).success());
    REQUIRE_FALSE(helper.captured_gcodes.empty());
    CHECK(helper.captured_gcodes.back() == "ACE_CHANGE_TOOL TOOL=7");
}

// A unit is named for the product its protocol drives, the short name, with
// a second unit of one name numbered. The driver's model string stays the
// unit's identity, never its display name, and the protocol words never reach
// the screen.
TEST_CASE_METHOD(LVGLTestFixture, "ACE units are named for their protocol",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, -1);

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 3);
    CHECK(info.units[0].display_name == "ACE Pro");
    CHECK(info.units[1].display_name == "ACE Pro 2");
    CHECK(info.units[2].display_name == "ACE Pro 2 #2");
    CHECK(info.units[0].name == "Anycubic Color Engine Pro");

    SECTION("a model stated later stays metadata") {
        json frame = json::object();
        frame["ace_instance_2"] = json{{"model", "ACE2 (Shared Bus)"}};
        send_frame(helper, frame);
        const auto after = helper.get_test_system_info();
        CHECK(after.units[2].display_name == "ACE Pro 2 #2");
        CHECK(after.units[2].name == "ACE2 (Shared Bus)");
    }
    SECTION("an unknown protocol never shows its raw value") {
        json frame = json::object();
        frame["ace_instance_2"] = json{{"protocol", "ace9_future"}};
        send_frame(helper, frame);
        CHECK(helper.get_test_system_info().units[2].display_name == "ACE Pro #2");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE Pro units of one protocol are numbered",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, /*ace_pro_units=*/2);
    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.units[0].display_name == "ACE Pro");
    CHECK(info.units[1].display_name == "ACE Pro #2");
}

// Each unit keeps the firmware it reports; the system version is the first
// unit's.
TEST_CASE_METHOD(LVGLTestFixture, "ACE keeps each unit's firmware", "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);

    auto info = helper.get_test_system_info();
    CHECK(info.units[0].firmware_version == "V1.3.856");
    CHECK(info.units[1].firmware_version == "V2.0.7");
    CHECK(info.version == "V1.3.856");

    json frame = json::object();
    frame["ace_instance_1"] = json{{"firmware", "V2.1.0"}};
    send_frame(helper, frame);
    info = helper.get_test_system_info();
    CHECK(info.units[1].firmware_version == "V2.1.0");
    CHECK(info.units[0].firmware_version == "V1.3.856");
    CHECK(info.version == "V1.3.856");
}

// The largest rig ACEPRO is daisy-chained into: two ACE Pros and four ACE 2s.
// The bootstrap asks for every instance discovery found, so the fifth and
// sixth units are there from the start, not once their spools next change.
TEST_CASE_METHOD(LVGLTestFixture, "ACE shows six daisy-chained units", "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    helper.set_discovery(ace_discovery(6));
    start_units(helper, client, 6, 17, /*ace_pro_units=*/2);

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 6);
    CHECK(info.total_slots == 24);
    CHECK(info.units[5].first_slot_global_index == 20);
    std::vector<std::string> names;
    for (const auto& unit : info.units) {
        names.push_back(unit.display_name);
    }
    CHECK(names == std::vector<std::string>{"ACE Pro", "ACE Pro #2", "ACE Pro 2", "ACE Pro 2 #2",
                                            "ACE Pro 2 #3", "ACE Pro 2 #4"});
    // T17 is the fifth unit's second slot.
    CHECK(info.current_slot == 17);
    CHECK(helper.get_slot_info(17).status == SlotStatus::LOADED);

    const ams_draw::SystemToolLayout layout = ams_draw::compute_system_tool_layout(info, &helper);
    CHECK(layout.total_physical_tools == 1);
    CHECK(layout.hub_groups.size() == 1);
}

// Endless spool matches across the chain: the second unit's four ABS spools of
// one colour are one group.
TEST_CASE_METHOD(LVGLTestFixture, "ACE endless spool groups slots on every unit",
                 "[ams][ace][multiunit][endless]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helper.set_running(true);
    json manager = make_kobra_manager_object(-1);
    manager["endless_spool_enabled"] = true;
    manager["endless_spool_match_mode"] = "exact";
    send_frame(helper, manager_delta(manager));

    std::vector<int> second_unit_group;
    for (const auto& group : helper.get_endless_spool_config().groups) {
        if (std::find(group.members.begin(), group.members.end(), 4) != group.members.end()) {
            second_unit_group = group.members;
        }
    }
    CHECK(second_unit_group == std::vector<int>{4, 5, 6, 7});
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE clears a second unit's slot override",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Blue";
    AceTestAccess::seed_override(helper, 5, ovr);
    REQUIRE(AceTestAccess::get_override(helper, 5).has_value());

    helper.clear_slot_override(5);

    CHECK_FALSE(AceTestAccess::get_override(helper, 5).has_value());
}

// A tool change names the driver's tool, which need not equal the slot.
TEST_CASE_METHOD(LVGLTestFixture, "ACE tool change finds the slot of the driver's tool",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, 1, [](json& status) {
        for (int i = 0; i < 4; ++i) {
            status["ace_instance_1"]["slots"][i]["tool"] = 10 + i;
        }
    });
    helper.set_running(true);

    REQUIRE(helper.change_tool(11).success());

    REQUIRE_FALSE(helper.captured_gcodes.empty());
    CHECK(helper.captured_gcodes.back() == "ACE_CHANGE_TOOL TOOL=11");
}

// A load sent from here holds LOADING only until its own ack or error; after
// either, a toolchange the driver starts itself ends when its target clears.
TEST_CASE_METHOD(LVGLTestFixture, "ACE load from here ends with its ack or its error",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helper.set_running(true);
    REQUIRE(helper.load_filament(4).success());

    SECTION("acked") {
        REQUIRE(helper.pending_ack != nullptr);
        helper.pending_ack();
    }
    SECTION("failed") {
        REQUIRE(helper.pending_error != nullptr);
        helper.pending_error(MoonrakerError{});
    }
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(helper.get_test_system_info().action == AmsAction::IDLE);

    send_frame(helper, manager_delta({{"target_index", 1}}));
    REQUIRE(helper.get_test_system_info().action == AmsAction::LOADING);
    send_frame(helper, manager_delta({{"target_index", -1}}));
    CHECK(helper.get_test_system_info().action == AmsAction::IDLE);
}

// A slot's `tool` sizes the published tool map, so only a number a chain of
// kMaxUnits units could carry is taken; anything else names no tool and the
// slot refuses a load.
TEST_CASE_METHOD(LVGLTestFixture, "ACE ignores a slot tool number out of range",
                 "[ams][ace][multiunit]") {
    const json bad = GENERATE(json(1000000), json(-3), json(64), json("5"), json(2.5));
    CAPTURE(bad.dump());
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 1, -1, 1,
                [&bad](json& status) { status["ace_instance_0"]["slots"][1]["tool"] = bad; });

    const auto info = helper.get_test_system_info();
    CHECK(helper.get_slot_info(1).mapped_tool == -1);
    CHECK(info.tool_to_slot_map.size() <= 4);
    helper.set_running(true);
    CHECK_FALSE(helper.load_filament(1).success());
    CHECK(helper.get_slot_info(2).mapped_tool == 2);
}

// A load sent from here stays LOADING until its own ack: the unit's status
// word reads "busy" (or "ready") while the driver feeds, and either resolves
// to IDLE.
TEST_CASE_METHOD(LVGLTestFixture, "ACE unit status does not end a load sent from here",
                 "[ams][ace][multiunit]") {
    const std::string word = GENERATE(as<std::string>{}, "busy", "ready");
    CAPTURE(word);
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    helper.set_running(true);
    REQUIRE(helper.load_filament(1).success());
    REQUIRE(helper.get_test_system_info().action == AmsAction::LOADING);

    json frame = json::object();
    frame["ace_instance_0"] = json{{"status", word}};
    send_frame(helper, frame);
    CHECK(helper.get_test_system_info().action == AmsAction::LOADING);

    REQUIRE(helper.pending_ack != nullptr);
    helper.pending_ack();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(helper.get_test_system_info().action == AmsAction::IDLE);
}

// A chain whose answer names ace_instance_0 and ace_instance_2 only: unit 1 is
// a gap holding unit 2's numbering. No instance answers to INSTANCE=1, so the
// gap takes no dryer or slot command.
TEST_CASE_METHOD(LVGLTestFixture, "ACE gap in the instance chain takes no commands",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, -1, 1, [](json& status) { status.erase("ace_instance_1"); });
    helper.set_running(true);

    auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 3);
    CHECK(info.units[1].absent);
    CHECK_FALSE(info.units[2].absent);
    CHECK(info.units[2].first_slot_global_index == 8);

    helper.captured_gcodes.clear();
    CHECK_FALSE(helper.start_drying(50.0f, 60, -1, 1).success());
    CHECK_FALSE(helper.stop_drying(1).success());
    REQUIRE(helper.stop_drying(2).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"ACE_STOP_DRYING INSTANCE=2"});

    SECTION("a slot command for the gap is refused") {
        AceTestAccess::set_seat(helper, 5, true);
        helper.captured_gcodes.clear();
        CHECK_FALSE(helper.execute_device_action("ace_manual_feed", {}).success());
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("the instance reporting fills the gap") {
        json frame = json::object();
        frame["ace_instance_1"] = make_kobra_second_unit_object();
        send_frame(helper, frame);
        CHECK_FALSE(helper.get_test_system_info().units[1].absent);
        REQUIRE(helper.stop_drying(1).success());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE with no first instance feeds the first unit present",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1, 1, [](json& status) { status.erase("ace_instance_0"); });
    REQUIRE(helper.get_test_system_info().units.at(0).absent);
    helper.captured_gcodes.clear();
    REQUIRE(helper.execute_device_action("ace_manual_feed", {}).success());
    CHECK(helper.captured_gcodes.back() == "ACE_FEED INSTANCE=1 INDEX=0 LENGTH=50 SPEED=50");
}

// The manager states its sensors only when they change, so a unit that
// reports after them takes the readings already in hand.
TEST_CASE_METHOD(LVGLTestFixture, "ACE unit reporting after the sensors shows them",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 1, -1);
    json frame = json::object();
    frame["ace_instance_1"] = make_kobra_second_unit_object();
    send_frame(helper, frame);

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.units[1].has_hub_sensor);
    CHECK(info.units[1].hub_sensor_triggered);
    CHECK(info.units[1].has_toolhead_sensor);
}

// The bootstrap answer names every unit there is: a restart onto a shorter
// chain drops the units it no longer names, with their slots and tools.
TEST_CASE_METHOD(LVGLTestFixture, "ACE restart onto fewer units drops the rest",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, -1);
    REQUIRE(helper.get_test_system_info().units.size() == 3);

    start_units(helper, client, 2, -1);
    const auto info = helper.get_test_system_info();
    CHECK(info.units.size() == 2);
    CHECK(info.total_slots == 8);
    CHECK(info.tool_to_slot_map.size() <= 8);
    CHECK_FALSE(helper.stop_drying(2).success());
}

// The ack ends only the LOADING a load from here set: an error a unit
// reported while it ran stands through the ack.
TEST_CASE_METHOD(LVGLTestFixture, "ACE load or unload ack keeps a unit's error",
                 "[ams][ace][multiunit]") {
    const bool unload = GENERATE(false, true);
    CAPTURE(unload);
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, unload ? 1 : -1);
    helper.set_running(true);
    REQUIRE((unload ? helper.unload_filament(1) : helper.load_filament(1)).success());

    json frame = json::object();
    frame["ace_instance_0"] = json{{"status", "error"}};
    send_frame(helper, frame);
    REQUIRE(helper.get_test_system_info().action == AmsAction::ERROR);

    SECTION("acked") {
        REQUIRE(helper.pending_ack != nullptr);
        helper.pending_ack();
    }
    SECTION("failed") {
        REQUIRE(helper.pending_error != nullptr);
        helper.pending_error(MoonrakerError{});
    }
    helix::ui::UpdateQueue::instance().drain();
    CHECK(helper.get_test_system_info().action == AmsAction::ERROR);
}

// A gap takes no display number: instances 0 and 2 of one model read
// "ACE Pro" and "ACE Pro #2", and the gap between them shows no name.
TEST_CASE_METHOD(LVGLTestFixture, "ACE gap does not take a display number",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, -1, /*ace_pro_units=*/3,
                [](json& status) { status.erase("ace_instance_1"); });

    const auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 3);
    REQUIRE(info.units[1].absent);
    CHECK(info.units[0].display_name == "ACE Pro");
    CHECK(info.units[1].display_name.empty());
    CHECK(info.units[2].display_name == "ACE Pro #2");
}

// A tool number belongs to the frame that names it: a slot whose frame names
// none, or a unit that a restart leaves as a gap, carries no tool, so a seat
// on that tool marks no slot.
TEST_CASE_METHOD(LVGLTestFixture, "ACE drops a tool number no frame names any more",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 3, -1);
    REQUIRE(helper.get_slot_info(5).mapped_tool == 5);

    SECTION("a restart leaves the unit carrying the seated tool a gap") {
        start_units(helper, client, 3, 5, 1, [](json& status) { status.erase("ace_instance_1"); });
        const auto info = helper.get_test_system_info();
        REQUIRE(info.units.size() == 3);
        REQUIRE(info.units[1].absent);
        CHECK(helper.get_slot_info(5).mapped_tool == -1);
        CHECK(info.current_tool == 5);
        CHECK(info.current_slot == -1);
        for (int slot = 0; slot < info.total_slots; ++slot) {
            CHECK(helper.get_slot_info(slot).status != SlotStatus::LOADED);
        }
    }
    SECTION("a slots frame that names no tool for a slot") {
        json unit1 = make_kobra_second_unit_object();
        unit1["slots"][1].erase("tool");
        json frame = json::object();
        frame["ace_instance_1"] = unit1;
        send_frame(helper, frame);
        CHECK(helper.get_slot_info(5).mapped_tool == -1);
        send_frame(helper, manager_delta({{"current_index", 5}}));
        CHECK(helper.get_test_system_info().current_slot == -1);
    }
}

// A restart that lands on the REST bridge, whether the startup answer has no
// slot-bearing object or the query fails, leaves the one unit the bridge
// serves: the old chain's other units are gone, and the bridge's slots load by
// their own numbers.
TEST_CASE_METHOD(LVGLTestFixture, "ACE restart onto the REST bridge keeps one unit",
                 "[ams][ace][multiunit]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);
    start_units(helper, client, 2, -1);
    REQUIRE(helper.get_test_system_info().units.size() == 2);

    helper.test_on_started();
    REQUIRE_FALSE(client.rpc_calls.empty());
    const auto& call = client.rpc_calls.back();
    SECTION("no instance carries slots") {
        REQUIRE(call.success_cb);
        json status = json::object();
        status["ace"] = make_kobra_manager_object(-1);
        status["ace_instance_0"] = json{{"status", "ready"}};
        call.success_cb(json{{"result", json{{"status", status}}}});
    }
    SECTION("the query fails") {
        REQUIRE(call.error_cb);
        call.error_cb(MoonrakerError{});
    }
    helix::ui::UpdateQueue::instance().drain();
    CHECK(helper.get_test_system_info().units.size() == 1);

    json slots = make_kobra_slots_array();
    for (auto& slot : slots) {
        slot.erase("tool");
    }
    helper.test_parse_slots_response(json{{"slots", slots}});
    helper.set_running(true);
    helper.captured_gcodes.clear();
    REQUIRE(helper.load_filament(1).success());
    CHECK(helper.captured_gcodes.back() == "ACE_CHANGE_TOOL TOOL=1");
}

// T<n> on ACEPRO loads the slot the driver numbers tool n, so a print with no
// remap is judged on that slot even when another slot is loaded.
TEST_CASE_METHOD(LVGLTestFixture, "ACE judges a tool on the slot the ACEPRO driver numbers it",
                 "[ams][ace][multiunit][filament_mapping][preflight]") {
    helix::test::FakeMoonrakerClient client;
    helix::test::RegisteredBackend<AmsBackendAceTestHelper> helper(&client);
    start_units(*helper, client, 1, -1, 1, [](json& status) {
        json& slots = status["ace_instance_0"]["slots"];
        slots[0]["tool"] = 2;
        slots[2]["tool"] = 0;
        slots[2]["status"] = "empty";
    });
    REQUIRE(helper->get_slot_info(0).is_present());
    REQUIRE_FALSE(helper->get_slot_info(2).is_present());
    REQUIRE(helper->firmware_default_routing().head(0) == 2);
    CHECK(helper->firmware_default_routing().head(2) == 0);

    helix::GcodeToolInfo t0{};
    t0.tool_index = 0;
    t0.color_known = false;
    auto& ams = helix::AmsState::instance();
    const auto slots = ams.collect_available_slots();
    const auto seed = ams.seed_tool_mappings({t0}, slots);
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].mapped_slot == 2);

    const auto* printing = helix::FilamentMapper::resolve_printing_slot(
        seed[0], slots, ams.collect_firmware_routing());
    REQUIRE(printing != nullptr);
    CHECK(printing->slot_index == 2);
    CHECK(helix::chip_severity(t0, printing) == helix::ChipSeverity::Block);

    const auto preflight = helix::file_preflight({t0}, seed, false);
    REQUIRE(preflight.checks.size() == 1);
    CHECK(preflight.checks[0].severity == helix::ToolCheck::Severity::EmptySlot);
    CHECK(preflight.has_block());
}
