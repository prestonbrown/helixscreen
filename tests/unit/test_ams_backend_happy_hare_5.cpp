// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 136 to 170 of test_ams_backend_happy_hare.cpp before it was split, in its order. Mostly
// [hh_v4], [1199], [dry_gates], [hh_overrides], [gate_weights], and more

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../test_helpers/happy_hare_test_fixtures.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_mapper.h"
#include "hh_defaults.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "spoolman_types.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/device_action_controls.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_fixture.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <any>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams_action_from_string;
using helix::AmsAction;
using helix::AmsBackendHappyHareTestHelper;
using helix::AmsError;
using helix::AmsResult;
using helix::AmsType;
using helix::BufferMotion;
using helix::EncoderClogInfo;
using helix::path_segment_from_happy_hare_pos;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::spoolman_mode_from_string;
using helix::spoolman_mode_to_string;
using helix::SpoolmanMode;
using helix::SpoolMotorAction;
using helix::StepOperationType;

using helix::test::action_enabled;
using helix::test::connect_with_fixture;
using helix::test::fan_entry;
using helix::test::feed_params;
using helix::test::find_action;
using helix::test::funnel_cleared_copy;
using helix::test::golden_hh_full_frame;
using helix::test::happy_hare_fixture_at;
using helix::test::HappyHareGoldenFixture;
using helix::test::has_recovery;
using helix::test::hh_gates;
using helix::test::HhFourGates;
using helix::test::HhFourGatesOf;
using helix::test::HhFourGatesWithClient;
using helix::test::HhHelper;
using helix::test::HhHelperWithClient;
using helix::test::join_ints;
using helix::test::kHappyHareGolden;
using helix::test::kObjectsQuery;
using helix::test::PrintLifecycleFixture;
using helix::test::recover_req;
using helix::test::ScopedConfig;
using helix::test::TuningRig;
using helix::test::unit_scoped_commands;
using helix::test::v3_fixture;
using helix::test::v3_linear_fixture;
using helix::test::v4_control_rows;

TEST_CASE("Happy Hare tuning lives in Moonraker and drops what the config has outgrown",
          "[ams][happy_hare][hh_v4][hh_overrides]") {
    // One saved against the built-in 60 while the printer's own default could
    // not be read (the printer's real default is 80); one saved against a real
    // default the printer has since changed, which is stale.
    TuningRig rig({{"gear_from_spool_speed", {{"value", 95.0}, {"config_default", 60.0}}},
                   {"gear_unload_speed", {{"value", 99.0}, {"config_default", 111.0}}},
                   {"gear_from_buffer_speed", {{"value", 120.0}, {"config_default", 150.0}}},
                   {"toolhead_ooze_reduction", {{"value", 1.5}, {"config_default", 2.0}}}});
    rig.connect();

    const auto& o = HappyHareTestAccess::user_overrides(*rig.reg);
    REQUIRE(o.gear_from_spool_speed);
    CHECK(*o.gear_from_spool_speed == Catch::Approx(95.0f));
    CHECK_FALSE(o.gear_unload_speed);
    CHECK(o.gear_from_buffer_speed == std::optional<float>(120.0f));
    CHECK(o.toolhead_ooze_reduction == std::optional<float>(1.5f));
    // The built-in-default record is re-pointed at the printer's, in Moonraker.
    CHECK(rig.stored()["gear_from_spool_speed"]["config_default"] == Catch::Approx(80.0));
    CHECK(rig.reg->has_gcode_containing("GEAR_LOAD_SPEED=95"));
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare endless spool groups follow the gate map with auto groups on",
                 "[ams][happy_hare][endless_spool][auto_groups]") {
    helper.initialize_test_gates(4);
    nlohmann::json fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    SECTION("auto groups on") {
        fx["configfile_settings"]["mmu_parameters"]["endless_spool_auto_groups"] = 1;
        connect_with_fixture(helper, client, fx);

        const auto caps = helper.get_endless_spool_capabilities();
        CHECK_FALSE(caps.editable());
        CHECK(caps.restriction == helix::printer::EndlessSpoolRestriction::GroupsFollowSlots);
        CHECK_FALSE(helix::printer::endless_spool_restriction_text(caps.restriction).empty());
        // ENABLE= is still honored.
        CHECK(caps.switchable);
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper.set_endless_spool_backup(0, 1).success());
        CHECK_FALSE(helper.has_gcode_containing("GROUPS="));
    }
    SECTION("auto groups off, or a Happy Hare without the option") {
        if (GENERATE(true, false)) {
            fx["configfile_settings"]["mmu_parameters"]["endless_spool_auto_groups"] = 0;
        }
        connect_with_fixture(helper, client, fx);
        CHECK(helper.get_endless_spool_capabilities().editability ==
              helix::printer::EndlessSpoolEditability::Group);
    }
}

TEST_CASE("a Happy Hare remaining weight is kept beside the gate map and echoes",
          "[ams][happy_hare][review]") {
    TuningRig rig;
    rig.connect(v3_linear_fixture());
    const SlotInfo original = rig.reg->get_slot_info(0);
    SlotInfo edited = original;
    edited.remaining_weight_g = 500.0f;
    REQUIRE(rig.reg->commit_user_edit(0, original, edited).success());
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    const nlohmann::json kept =
        rig.printer.client.mock_db_get("helix-screen-hh-overrides", "lane1");
    REQUIRE(kept.is_object());
    CHECK(kept["remaining_weight_g"] == Catch::Approx(500.0));
    CHECK_FALSE(helix::ams::lane_sources(rig.reg->lane_id(0)).pending.has_value());
}

TEST_CASE("a Happy Hare 4.2 weight edit stays out of the extras record",
          "[ams][happy_hare][gate_weights]") {
    TuningRig rig;
    rig.connect(happy_hare_fixture_at("4.2.0"));
    const SlotInfo original = rig.reg->get_slot_info(0);
    SlotInfo edited = original;
    edited.remaining_weight_g = 500.4f;
    edited.total_weight_g = 1000.0f;
    REQUIRE(rig.reg->commit_user_edit(0, original, edited).success());
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(rig.reg->has_gcode("MMU_GATE_MAP GATE=0 WEIGHT=500 INITIAL_WEIGHT=1000 TEMP=0"));
    nlohmann::json kept = rig.printer.client.mock_db_get("helix-screen-hh-overrides", "lane1");
    if (kept.is_object()) {
        CHECK((!kept.contains("remaining_weight_g") || kept["remaining_weight_g"].is_null()));
        CHECK((!kept.contains("total_weight_g") || kept["total_weight_g"].is_null()));
    }
}

TEST_CASE("Happy Hare 4.2 takes over the weights a 4.1 record kept, once",
          "[ams][happy_hare][gate_weights]") {
    TuningRig rig;
    for (const auto& [key, lane, grams] :
         {std::tuple{"lane1", "0", 640}, {"lane2", "1", 300}, {"lane3", "2", 210}}) {
        rig.printer.client.mock_db_set("helix-screen-hh-overrides", key,
                                       {{"lane", lane},
                                        {"writer", "helixscreen"},
                                        {"remaining_weight_g", grams},
                                        {"total_weight_g", 1000}});
    }
    rig.connect(happy_hare_fixture_at("4.2.0"));
    // Gate 1's gate map already holds a weight, which is never overwritten. Gate
    // 2's spool is Spoolman's, and its old weight stays off the gate after unlink.
    nlohmann::json frame{{"gate_remaining_weight", {-1, 450, -1, -1}},
                         {"gate_initial_weight", {-1, -1, -1, -1}},
                         {"gate_spool_id", {-1, -1, 7, -1}}};
    for (int i = 0; i < 2; ++i) {
        rig.reg->feed_mmu_status(frame);
        helix::ui::UpdateQueue::instance().drain();
        frame["gate_spool_id"][2] = -1;
    }
    CHECK_FALSE(rig.reg->has_gcode_containing("GATE=2 "));
    CHECK(rig.printer.client.mock_db_get("helix-screen-hh-overrides", "lane3")["total_weight_g"]
              .is_null());
    const auto& sent = rig.reg->captured_gcodes;
    CHECK(std::count(sent.begin(), sent.end(),
                     "MMU_GATE_MAP GATE=0 WEIGHT=640 INITIAL_WEIGHT=1000 TEMP=0 QUIET=1") == 1);
    CHECK(rig.reg->has_gcode("MMU_GATE_MAP GATE=1 INITIAL_WEIGHT=1000 TEMP=0 QUIET=1"));
    CHECK_FALSE(rig.reg->has_gcode_containing("GATE=1 WEIGHT"));
    CHECK(rig.printer.client.mock_db_get("helix-screen-hh-overrides", "lane1")["remaining_weight_g"]
              .is_null());
}

TEST_CASE("a gate name another client clears shows as cleared", "[ams][happy_hare][review]") {
    TuningRig rig;
    rig.printer.client.mock_db_set(
        "helix-screen-hh-overrides", "lane1",
        {{"lane", "0"}, {"writer", "helixscreen"}, {"name", "PolyTerra Charcoal"}});
    rig.connect(v3_linear_fixture());
    // The extras record is read and filed on the lane.
    REQUIRE(helix::ams::lane_sources(rig.reg->lane_id(0)).record.has_value());
    rig.reg->feed_mmu_status(
        {{"gate_name", nlohmann::json::array({"PolyTerra Charcoal", "", "", ""})}});
    REQUIRE(rig.reg->get_slot_info(0).spool_name == "PolyTerra Charcoal");

    // Mainsail's MMU_GATE_MAP GATE=0 NAME= empties the gate map's name.
    rig.reg->feed_mmu_status({{"gate_name", nlohmann::json::array({"", "", "", ""})}});
    CHECK(rig.reg->get_slot_info(0).spool_name.empty());
}

TEST_CASE("a Happy Hare tuning change is written to Moonraker, not the local config",
          "[ams][happy_hare][hh_overrides]") {
    ScopedConfig config;
    TuningRig rig;
    rig.connect(v3_linear_fixture());
    REQUIRE(rig.reg->execute_device_action("gear_from_spool_speed", std::any(70.0)).success());
    CHECK(rig.stored()["gear_from_spool_speed"]["value"] == Catch::Approx(70.0));
    // Clog detection is the MMU's to report, so it is never stored.
    REQUIRE(
        rig.reg->execute_device_action("clog_detection", std::any(std::string("Auto"))).success());
    CHECK_FALSE(rig.stored().contains("clog_detection"));
    CHECK_FALSE(config->exists("/hh_overrides"));
    CHECK_FALSE(config->exists(config->df() + "hh_overrides"));
}

TEST_CASE("a Happy Hare tuning change before the document is read keeps what Moonraker holds",
          "[ams][happy_hare][hh_overrides]") {
    ScopedConfig config;
    TuningRig rig({{"gear_unload_speed", {{"value", 90.0}, {"config_default", 80.0}}}});
    // The tuning read, which follows the configfile answer, fails.
    HappyHareTestAccess::on_started(*rig.reg);
    helix::ui::UpdateQueue::instance().drain();
    rig.printer.client.fail_next("server.database.get_item");
    REQUIRE(rig.client.reply_to_last(
        kObjectsQuery,
        nlohmann::json{
            {"result",
             {{"status",
               {{"configfile", {{"settings", v3_linear_fixture()["configfile_settings"]}}}}}}}}));
    for (int i = 0; i < 3; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE_FALSE(HappyHareTestAccess::user_overrides(*rig.reg).gear_unload_speed);
    REQUIRE(rig.stored().contains("gear_unload_speed"));

    REQUIRE(rig.reg->execute_device_action("gear_from_spool_speed", std::any(70.0)).success());
    for (int i = 0; i < 3; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }

    // The change is read into the stored document, not written over it.
    const nlohmann::json stored = rig.stored();
    REQUIRE(stored.contains("gear_unload_speed"));
    REQUIRE(stored.contains("gear_from_spool_speed"));
    CHECK(stored["gear_unload_speed"]["value"] == Catch::Approx(90.0));
    CHECK(stored["gear_from_spool_speed"]["value"] == Catch::Approx(70.0));
    const auto& o = HappyHareTestAccess::user_overrides(*rig.reg);
    REQUIRE(o.gear_from_spool_speed);
    CHECK(*o.gear_from_spool_speed == Catch::Approx(70.0f));
}

TEST_CASE("a local Happy Hare tuning record moves to Moonraker once",
          "[ams][happy_hare][hh_overrides]") {
    ScopedConfig config;
    config->set<float>("/hh_overrides/gear_from_spool_speed/value", 95.0f);
    config->set<float>("/hh_overrides/gear_from_spool_speed/config_default", 80.0f);

    SECTION("Moonraker holds none: it moves") {
        TuningRig rig;
        rig.connect();
        CHECK_FALSE(config->exists("/hh_overrides"));
        CHECK(rig.stored()["gear_from_spool_speed"]["value"] == Catch::Approx(95.0));
        const auto& o = HappyHareTestAccess::user_overrides(*rig.reg);
        REQUIRE(o.gear_from_spool_speed);
        CHECK(*o.gear_from_spool_speed == Catch::Approx(95.0f));
    }
    SECTION("Moonraker already holds a document: it wins") {
        TuningRig rig({{"gear_unload_speed", {{"value", 90.0}, {"config_default", 80.0}}}});
        rig.connect();
        CHECK_FALSE(rig.stored().contains("gear_from_spool_speed"));
        CHECK_FALSE(HappyHareTestAccess::user_overrides(*rig.reg).gear_from_spool_speed);
    }
}

TEST_CASE("Happy Hare selector speed override survives a restart under either key",
          "[ams][happy_hare][hh_v4]") {
    nlohmann::json tuning;
    SECTION("saved by the slider") {
        TuningRig first;
        first.connect(v3_linear_fixture());
        REQUIRE(first.reg->execute_device_action("selector_speed", std::any(180.0)).success());
        tuning = first.stored();
        CHECK(tuning.contains("selector_move_speed"));
    }
    SECTION("an older record under the action id") {
        tuning = {{"selector_speed", {{"value", 180.0}, {"config_default", 200.0}}}};
    }
    TuningRig rig(tuning);
    rig.connect(v3_linear_fixture());
    const auto& o = HappyHareTestAccess::user_overrides(*rig.reg);
    REQUIRE(o.selector_move_speed);
    CHECK(*o.selector_move_speed == Catch::Approx(180.0f));
}

TEST_CASE_METHOD(HhHelper,
                 "Happy Hare v3 eSpooler list covering only some gates is not read per gate",
                 "[ams][happy_hare][hh_v4]") {
    // v3 lists only the gates with an eSpooler fitted: here two of four.
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}},
                                 {"gate", 1},
                                 {"espooler", {"rewind", "assist"}},
                                 {"espooler_active", "print"}});
    CHECK(helper.get_system_info().espooler_state == "print");
    // One entry per gate lines up even on v3.
    helper.test_parse_mmu_state({{"espooler", {"off", "rewind", "off", "off"}}});
    CHECK(helper.get_system_info().espooler_state == "rewind");
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare v4 shows no bypass before its units are known",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    // The first v4 frame says has_bypass: true for every install.
    auto frame = fx["mmu_status"];
    frame["tangle_prevention"] = nullptr;
    helper.test_parse_mmu_state(frame);
    CHECK_FALSE(helper.get_system_info().supports_bypass);
    connect_with_fixture(helper, client, fx);
    CHECK(helper.get_system_info().supports_bypass); // the ERCF has one
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare clog detection is named the way each version takes it",
                 "[ams][happy_hare][hh_v4][clog]") {
    SECTION("before 3.42: ENABLE_CLOG_DETECTION and the calibrated clog length") {
        connect_with_fixture(helper, client, v3_fixture(3.40));
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=12.0"));
        helper.execute_device_action("clog_detection", std::string("Auto"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=2"});
        CHECK_FALSE(helper.clog_detection_length_setting());
        CHECK(HappyHareTestAccess::config_defaults(helper).clog_detection == 1);
    }
    SECTION("3.42: the FlowGuard encoder parameters, no UNIT") {
        connect_with_fixture(helper, client, v3_fixture(3.42));
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0"));
        helper.execute_device_action("clog_detection", std::string("Off"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=0"});
        CHECK(helper.clog_detection_length_setting() == std::optional<float>(18.0f));
    }
    SECTION("v4 multi-unit: the encoder unit's own parameters") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(helper.clog_detection_length_setting() == std::optional<float>(25.0f));
        CHECK(helper.clog_detection_mode_gcode(2, 0.0f) ==
              std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
    }
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare reapplies each override alone, under its version's name",
                 "[ams][happy_hare][hh_v4]") {
    auto set_overrides = [&] {
        auto& o = HappyHareTestAccess::user_overrides(helper);
        o = {};
        o.gear_from_spool_speed = 70.0f;
        o.gear_unload_speed = 90.0f;
        o.selector_move_speed = 180.0f;
        helper.clear_captured_gcodes();
        helper.test_reapply_overrides();
        return helper.captured_gcodes;
    };

    SECTION("3.01: no gear unload speed yet") {
        connect_with_fixture(helper, client, v3_fixture(3.01));
        CHECK(set_overrides() ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG SELECTOR_MOVE_SPEED=180"});
    }
    SECTION("3.42 on a selector with no moving carriage") {
        connect_with_fixture(helper, client, v3_fixture(3.42, "VirtualSelector"));
        CHECK(set_overrides() ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG GEAR_UNLOAD_SPEED=90"});
    }
}

TEST_CASE_METHOD(HhHelper,
                 "Happy Hare v4 frame names v4 parameters before the config query answers",
                 "[ams][happy_hare][hh_v4]") {
    helper.set_config_defaults_for_test();
    auto frame =
        helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json")["mmu_status"];
    frame["tangle_prevention"] = nullptr;
    helper.test_parse_mmu_state(frame);
    helper.clear_captured_gcodes();
    helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=70"});
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 treats a disabled toolhead sensor as not fitted",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    helper.clear_captured_gcodes();
    REQUIRE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(30.0)).success());
    // v4 publishes a disabled sensor as null, and refuses its distance.
    helper.test_parse_mmu_state({{"sensors", {{"toolhead", nullptr}, {"extruder", false}}}});
    CHECK_FALSE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(31.0)));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG TOOLHEAD_SENSOR_TO_NOZZLE=30.0 UNIT=1"});
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 unit encoder flag comes from the unit's configured encoder",
                 "[ams][happy_hare][hh_v4]") {
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    // A Type A unit configured without an encoder.
    fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].topology == PathTopology::LINEAR);
    CHECK_FALSE(info.units[0].has_encoder);
    CHECK_FALSE(action_enabled(helper, "calibrate_encoder"));
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare connect with an unchanged split does not rebuild the units",
                 "[ams][happy_hare][hh_multi_unit]") {
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}}, {"gate", 1}});
    helper.mark_unit(0, "kept");
    connect_with_fixture(helper, client, v3_fixture(3.42));
    CHECK(helper.unit_marker(0) == "kept");
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare whole-machine drying stops the units it started when one refuses",
                 "[ams][happy_hare][hh_v4]") {
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    helper.clear_captured_gcodes();
    helper.fail_gcode = "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9";

    CHECK_FALSE(helper.start_drying(50.0f, 60, -1, -1));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=0 GATES=0,1,2,3,4,5",
                                   "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                   "MMU_HEATER STOP=1 UNIT=0 GATES=0,1,2,3,4,5"});
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare v4 encoder flag holds when the split is unchanged",
                 "[ams][happy_hare][hh_v4]") {
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    // No display_name: the unit keeps the name "MMU", so the split is unchanged
    // and only the topologies are refreshed.
    fx["mmu_machine"]["unit_0"].erase("display_name");
    fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    helper.mark_unit(0, "kept");
    connect_with_fixture(helper, client, fx);
    REQUIRE(helper.unit_marker(0) == "kept");
    const auto info = helper.get_system_info();
    CHECK(info.units[0].topology == PathTopology::LINEAR);
    CHECK_FALSE(info.units[0].has_encoder); // no [mmu_unit] encoder configured
}

TEST_CASE("Happy Hare gate_vendor is the lane brand unless the user declared one",
          "[ams][happy_hare][hh_v4]") {
    using nlohmann::json;
    struct Row {
        const char* name;
        const char* declared; // override brand, nullptr = none
        std::vector<json> frames;
        const char* brand0;
    };
    const Row rows[] = {
        {"a vendor lands", nullptr, {{{"gate_vendor", json::array({"QIDI", ""})}}}, "QIDI"},
        {"an empty vendor clears it",
         nullptr,
         {{{"gate_vendor", json::array({"QIDI", ""})}}, {{"gate_vendor", json::array({"", ""})}}},
         ""},
        {"a frame without gate_vendor leaves it alone",
         nullptr,
         {{{"gate_material", json::array({"PLA", "PETG"})}}},
         ""},
        {"an empty vendor keeps a declared brand",
         "Polymaker",
         {{{"gate_vendor", json::array({"QIDI", ""})}}, {{"gate_vendor", json::array({"", ""})}}},
         "Polymaker"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        if (row.declared) {
            helix::ams::FilamentSlotOverride o;
            o.brand = row.declared;
            helper.set_gate_override(0, o);
        }
        for (const auto& f : row.frames)
            helper.feed_mmu_status(f);
        CHECK(helper.get_slot_info(0).brand == row.brand0);
        CHECK(helper.get_slot_info(1).brand.empty());
    }
}

TEST_CASE("Happy Hare writes VENDOR to the gate map on v4 only", "[ams][happy_hare][hh_v4]") {
    for (const bool v4 : {true, false}) {
        INFO((v4 ? "v4" : "v3"));
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        if (v4)
            helper.feed_mmu_status({{"tangle_prevention", {{"enabled", false}}}});

        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.brand = "Prusament";
        helix::test::apply_edit(helper, 0, info);

        if (v4) {
            CHECK(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=FF0000 VENDOR=Prusament TEMP=0"));
        } else {
            CHECK(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=FF0000 TEMP=0"));
            CHECK_FALSE(helper.has_gcode_containing("VENDOR"));
        }
    }
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare v4 takes a drying humidity target, v3 does not",
                 "[ams][happy_hare][hh_v4][dryer_humidity]") {
    SECTION("v3: no capability, and the temp + time command is unchanged") {
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK_FALSE(helper.get_dryer_info(0).supports_humidity_target);
        // A default left over in the dryer state is not reported without the capability.
        HappyHareTestAccess::dryer_info(helper).default_humidity_target = 25.0f;
        CHECK_FALSE(helper.get_dryer_info(0).default_humidity_target);

        helper.clear_captured_gcodes();
        const auto refused = helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0);
        CHECK(refused.result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());

        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60"});
    }
    SECTION("v4 single unit") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK(helper.get_dryer_info(0).supports_humidity_target);
        // heater_default_dry_humidity from the unit's parameters section.
        REQUIRE(helper.get_dryer_info(0).default_humidity_target);
        CHECK(*helper.get_dryer_info(0).default_humidity_target == 25.0f);

        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60"});

        CHECK(helper.stop_drying(0).success());
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15"});
    }
    SECTION("v4: a duration change restarts the cycle with the same humidity target") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0).success());
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 50.0f;

        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(-1.0f, 90, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=90 HUMIDITY=15"});

        // A plain start afterwards carries no target forward.
        CHECK(helper.stop_drying(0).success());
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.update_drying(-1.0f, 90, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER STOP=1",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=90"});
    }
    SECTION("v4 two units: the target sits before the unit and gate suffix") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        HappyHareTestAccess::dryer_info(helper).supported = true;

        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare v4 sync feedback and fan controls; v3 and absent hardware show none",
                 "[ams][happy_hare][hh_v4][hh_v4_controls]") {
    helper.set_running(true);
    const nlohmann::json both = {{"sync_feedback_enabled", true},
                                 {"fans", {fan_entry("unit0", 0, true, {2})}}};

    SECTION("v3: no rows, and a control is refused unsent") {
        feed_params(helper, {{"mmu", both}});
        CHECK(v4_control_rows(helper).empty());
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper.execute_device_action("sync_feedback", std::any(false)).success());
        CHECK_FALSE(helper.execute_device_action("mmu_fan_mode:unit0", std::any(std::string("On")))
                        .success());
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("v4: autotune starts from the config, and a reload forgets what was sent") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        fx["configfile_settings"]["mmu_unit_parameters unit0"]["autotune_rotation_distance"] = 1;
        const auto autotune = [this] {
            return *std::any_cast<bool>(
                &v4_control_rows(helper)["sync_feedback_autotune"].current_value);
        };
        connect_with_fixture(helper, client, fx);
        feed_params(helper, {{"mmu", both}});
        CHECK(autotune());
        REQUIRE(helper.execute_device_action("sync_feedback_autotune", std::any(false)).success());
        CHECK_FALSE(autotune());
        connect_with_fixture(helper, client, fx);
        CHECK(autotune());
    }
    SECTION("v4 single unit: each control's command, no UNIT=") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        connect_with_fixture(helper, client, fx);
        // The fixture's unit has no buffer and no managed fans.
        feed_params(helper, {{"mmu", fx["mmu_status"]}});
        CHECK(v4_control_rows(helper).empty());

        feed_params(helper, {{"mmu", both}});
        auto rows = v4_control_rows(helper);
        REQUIRE(rows.size() == 6);
        CHECK(*std::any_cast<bool>(&rows["sync_feedback"].current_value));
        CHECK_FALSE(*std::any_cast<bool>(&rows["sync_feedback_autotune"].current_value));
        CHECK(rows["sync_feedback_reset"].enabled);
        CHECK(rows["sync_feedback_adjust_tension"].moves_machine);
        CHECK(*std::any_cast<std::string>(&rows["mmu_fan_mode:unit0"].current_value) == "Auto");

        helper.clear_captured_gcodes();
        CHECK(helper.execute_device_action("sync_feedback", std::any(false)).success());
        CHECK(helper.execute_device_action("sync_feedback_autotune", std::any(true)).success());
        CHECK(helper.execute_device_action("sync_feedback_reset", {}).success());
        CHECK(helper.execute_device_action("sync_feedback_adjust_tension", {}).success());
        CHECK(helper.execute_device_action("mmu_fan_control:unit0", std::any(false)).success());
        CHECK(helper.execute_device_action("mmu_fan_mode:unit0", std::any(std::string("On")))
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_SYNC_FEEDBACK ENABLE=0", "MMU_SYNC_FEEDBACK AUTOTUNE=1",
                                       "MMU_SYNC_FEEDBACK RESET=1",
                                       "MMU_SYNC_FEEDBACK ADJUST_TENSION=1", "MMU_FAN ENABLE=0",
                                       "MMU_FAN FAN_FORCED=1"});
        // Happy Hare does not report AUTOTUNE, so the row shows what was sent.
        CHECK(
            *std::any_cast<bool>(&v4_control_rows(helper)["sync_feedback_autotune"].current_value));

        // Off: Reset does nothing in Happy Hare, so it is refused here.
        feed_params(helper, {{"mmu", {{"sync_feedback_enabled", false}}}});
        CHECK_FALSE(v4_control_rows(helper)["sync_feedback_reset"].enabled);
        // Null: the selected unit has no buffer, so its rows go.
        feed_params(helper, {{"mmu", {{"sync_feedback_enabled", nullptr}}}});
        CHECK(v4_control_rows(helper).count("sync_feedback") == 0);
        CHECK(v4_control_rows(helper).count("mmu_fan_mode:unit0") == 1);
    }
    SECTION("v4 two units: the command names its unit") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        connect_with_fixture(helper, client, fx);
        fx["mmu_status"]["sync_feedback_enabled"] = true;
        fx["mmu_status"]["fans"] = {fan_entry("ercf", 0, true, {2}),
                                    fan_entry("bt", 6, false, {0, 1, nullptr, 0})};
        feed_params(helper, {{"mmu", fx["mmu_status"]}});
        auto rows = v4_control_rows(helper);
        // Per-gate fans that disagree have no one mode to show.
        CHECK(std::any_cast<std::string>(&rows["mmu_fan_mode:bt"].current_value)->empty());
        CHECK(rows["mmu_fan_mode:bt"].description == "Box Turtle");
        // Nothing is loaded in this fixture.
        CHECK_FALSE(rows["sync_feedback_adjust_tension"].enabled);

        helper.clear_captured_gcodes();
        CHECK(helper.execute_device_action("sync_feedback", std::any(false)).success());
        CHECK(helper.execute_device_action("mmu_fan_mode:bt", std::any(std::string("Auto")))
                  .success());
        CHECK_FALSE(helper.execute_device_action("sync_feedback_adjust_tension", {}).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_SYNC_FEEDBACK ENABLE=0 UNIT=1",
                                       "MMU_FAN FAN_FORCED=2 UNIT=bt"});
    }
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare 4.2 rotates spools on a shared heater without GATES",
                 "[ams][happy_hare][hh_v4][dry_gates][shared_rotation]") {
    // Unit 0 (ERCF) shares one heater; unit 1 (Box Turtle) has one per gate.
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
    const std::string version = GENERATE(std::string("4.2.0"), std::string("4.1.0"));
    INFO("Happy Hare " << version);
    fx["mmu_machine"]["happy_hare_version"] = version;
    fx["configfile_settings"]["mmu_machine"]["happy_hare_version"] = version;
    const bool v42 = version == "4.2.0";
    helper.test_parse_mmu_state(fx["mmu_status"]);

    SECTION("a gear that turns its spool counts") {
        // No eSpooler anywhere; the ERCF unit's gear turns its spools.
        fx["configfile_settings"]["mmu_unit ercf"]["gear_rotates_spool"] = 1;
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state({{"drying_state", {{"active", false}}}});
        helper.test_parse_mmu_state(
            {{"espooler", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        CHECK_FALSE(helper.get_dryer_info(0).supports_gate_selection);
        CHECK(helper.get_dryer_info(0).supports_rotation == v42);

        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 0};
        req.rotate = true;
        helper.clear_captured_gcodes();
        const AmsError err = helper.start_drying_request(req);
        if (v42) {
            REQUIRE(err.success());
            CHECK(helper.captured_gcodes ==
                  std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 ROTATE=1 UNIT=0"});
        } else {
            CHECK(err.result == AmsResult::NOT_SUPPORTED);
            CHECK(helper.captured_gcodes.empty());
        }
    }
    SECTION("a unit whose gear grips but does not turn the spool") {
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state({{"drying_state", {{"active", false}}}});
        helper.test_parse_mmu_state(
            {{"espooler", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        CHECK_FALSE(helper.get_dryer_info(0).supports_rotation);
    }
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 drives a chosen set of gates on per-gate heaters",
                 "[ams][happy_hare][hh_v4][dry_gates]") {
    // Unit 0 (ERCF) shares one heater across its gates; unit 1 (Box Turtle) has one per
    // gate and an eSpooler.
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    HappyHareTestAccess::dryer_info(helper).supported = true;

    SECTION("only the per-gate unit offers a selection, and rotation where spools turn") {
        CHECK(helper.get_dryer_info(1).supports_gate_selection);
        CHECK(helper.get_dryer_info(1).supports_rotation);
        CHECK_FALSE(helper.get_dryer_info(0).supports_gate_selection);
        CHECK_FALSE(helper.get_dryer_info(0).supports_rotation);
    }
    SECTION("no eSpooler, no rotation") {
        helper.test_parse_mmu_state(
            {{"espooler", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        CHECK(helper.get_dryer_info(1).supports_gate_selection);
        CHECK_FALSE(helper.get_dryer_info(1).supports_rotation);
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7}};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("a selection names its gates") {
        helper.clear_captured_gcodes();
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {8, 7, 8}})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=7,8"});
    }
    SECTION("humidity and rotation ride the same command") {
        DryingRequest req{
            .temp_c = 50.0f, .duration_min = 60, .humidity_pct = 15.0f, .unit = 1, .gates = {6, 9}};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 ROTATE=1 UNIT=1 GATES=6,9"});
    }
    SECTION("rotation with no selection names every gate on the unit") {
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 ROTATE=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("no selection sends exactly the whole-unit start") {
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .unit = 1})
                  .success());
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .humidity_pct = 15.0f, .unit = 1})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a shared-heater unit refuses a selection") {
        helper.clear_captured_gcodes();
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 0, .gates = {1}})
                  .result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.stop_drying_gates(0, {1}).result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("gates on no unit are dropped, and none left sends nothing") {
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper
                        .start_drying_request(
                            {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {42, -1}})
                        .success());
        CHECK(helper.captured_gcodes.empty());
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {42, 9}})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=9"});
    }
    SECTION("stopping a selection cancels only those gates") {
        helper.clear_captured_gcodes();
        CHECK(helper.stop_drying_gates(1, {8}).success());
        CHECK(helper.stop_drying_gates(1, {}).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=8",
                                       "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a stopped gate leaves the selection a temperature change reaches") {
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}})
                  .success());
        REQUIRE(helper.stop_drying_gates(1, {8}).success());
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=7"});
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 55.0f;
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(-1.0f, 90, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER DRY=1 TEMP=55 TIMER=90 UNIT=1 GATES=7"});
    }
    SECTION("a cycle the printer finishes forgets its gates") {
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}})
                  .success());
        // A frame from before the start is not the end of the cycle.
        helper.test_parse_mmu_state(
            {{"drying_state", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=7,8"});

        helper.test_parse_mmu_state(
            {{"drying_state",
              nlohmann::json::array({"", "", "", "", "", "", "", "active", "queued", ""})}});
        helper.test_parse_mmu_state(
            {{"drying_state",
              nlohmann::json::array({"", "", "", "", "", "", "", "complete", "cancelled", ""})}});
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a restart and a retarget keep the selected gates") {
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}};
        req.rotate = true;
        CHECK(helper.start_drying_request(req).success());
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 50.0f;

        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.update_drying(-1.0f, 90, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER TEMP=55 UNIT=1 GATES=7,8", "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=90 ROTATE=1 UNIT=1 GATES=7,8"});

        // A stop ends the selection with the cycle, and a whole-unit cycle carries none.
        CHECK(helper.stop_drying(1).success());
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.start_drying(50.0f, 60, -1, 1).success());
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v3 never offers a gate selection",
                 "[ams][happy_hare][dry_gates]") {
    helper.initialize_test_gates(2);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1"}}}});
    HappyHareTestAccess::dryer_info(helper).supported = true;

    CHECK_FALSE(helper.get_dryer_info(0).supports_gate_selection);
    helper.clear_captured_gcodes();
    CHECK(helper.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .gates = {1}}).result ==
          AmsResult::NOT_SUPPORTED);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Each slot carries its gate's drying state", "[ams][happy_hare][dry_gates]") {
    using helix::printer::ZoneDryingState;
    helix::test::FakeMoonrakerClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);

    auto state_of = [&helper](int gate) {
        const auto info = helper.get_system_info();
        const SlotInfo* slot = info.get_slot_global(gate);
        REQUIRE(slot != nullptr);
        return slot->drying_state;
    };

    // A short list: gates past its end read Idle rather than shifting or throwing.
    helper.test_parse_mmu_state(
        {{"drying_state",
          nlohmann::json::array({"", "", "", "", "", "", "active", "queued", "complete"})}});
    CHECK(state_of(0) == ZoneDryingState::Idle);
    CHECK(state_of(6) == ZoneDryingState::Active);
    CHECK(state_of(7) == ZoneDryingState::Queued);
    CHECK(state_of(8) == ZoneDryingState::Complete);
    CHECK(state_of(9) == ZoneDryingState::Idle);

    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", "", "", "", "",
                                                                         "", "", "cancelled"})}});
    CHECK(state_of(6) == ZoneDryingState::Idle);
    CHECK(state_of(9) == ZoneDryingState::Cancelled);
}

TEST_CASE("A drying_state-only frame reaches the UI", "[ams][happy_hare][dry_gates]") {
    using helix::printer::ZoneDryingState;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    int state_events = 0;
    helper.set_event_callback([&](const std::string& name, const std::string&) {
        state_events += name == helix::AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
    });

    helper.feed_mmu_status({{"drying_state", nlohmann::json::array({"", "active", "", ""})}});

    // EVENT_STATE_CHANGED is what makes AmsState resync every slot's subjects.
    CHECK(state_events == 1);
    CHECK(helper.get_system_info().get_slot_global(1)->drying_state == ZoneDryingState::Active);
}

TEST_CASE_METHOD(HhHelperWithClient, "Per-gate zones each carry their own unit's dryer",
                 "[ams][happy_hare][dry_gates]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    HappyHareTestAccess::dryer_info(helper).supported = true;

    const auto zones = helper.get_environment_zones(1);
    REQUIRE_FALSE(zones.empty());
    for (const auto& z : zones) {
        CHECK(z.dryer.supports_gate_selection);
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare TTG remap is the routing an unremapped print runs with",
                 "[ams][happy_hare][tool_mapping][routing]") {
    // Gate 1 out of service, gate 3 standing in for it via MMU_TTG_MAP. A print
    // using T1 must seed gate 3, so print start sends no MMU_TTG_MAP undoing it.
    nlohmann::json mmu_data = {{"gate", -1},
                               {"tool", -1},
                               {"filament", "Unloaded"},
                               {"action", "Idle"},
                               {"gate_status", {1, 0, 1, 1}},
                               {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
                               {"gate_material", {"PLA", "PLA", "PLA", "PLA"}},
                               {"ttg_map", {0, 3, 2, 1}}};
    helper.test_parse_mmu_state(mmu_data);
    helix::ui::UpdateQueue::instance().drain();

    const auto live = helper.get_tool_mapping();
    REQUIRE(live.size() == 4);
    REQUIRE(live[1] == 3);
    CHECK(helper.firmware_default_routing().head(1) == 3);
    CHECK(helper.firmware_default_routing().head(3) == 1);

    helix::GcodeToolInfo t1{};
    t1.tool_index = 1;
    t1.color_rgb = 0x808080;
    t1.color_known = false;
    const std::vector<helix::GcodeToolInfo> tools{t1};
    auto& ams = helix::AmsState::instance();
    const auto slots = ams.collect_available_slots();
    const auto seed = ams.seed_tool_mappings(tools, slots, false);
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].mapped_slot == live[1]);
    CHECK_FALSE(helix::PreflightValidator::validate(tools, slots, seed,
                                                    ams.collect_firmware_routing(), false, false)
                    .has_block());
}

TEST_CASE("Happy Hare leaves an unloaded from_buffer gate as FROM_BUFFER",
          "[ams][happy_hare][1199]") {
    // The upgrade must be about the loaded gate, not about from_buffer gates in
    // general — otherwise every buffered gate would read as seated.
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({1, 1, 2, 1}, 0, "Loaded"));

    CHECK(hh.get_slot_info(0).status == SlotStatus::LOADED);
    CHECK(hh.get_slot_info(2).status == SlotStatus::FROM_BUFFER);
}

// Hole 2: gate / filament arrive without gate_status

TEST_CASE("Happy Hare moves the LOADED stamp on a gate-only delta", "[ams][happy_hare][1199]") {
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, 0, "Loaded"));
    REQUIRE(hh.get_slot_info(0).status == SlotStatus::LOADED);

    SECTION("a toolchange frame carrying only gate follows the firmware") {
        // The frame a real toolchange produces: gate_status did not change, so
        // Moonraker does not resend it. The stamp still has to move.
        hh.feed_mmu_status(nlohmann::json{{"gate", 3}});

        REQUIRE(hh.get_system_info().current_slot == 3);
        CHECK(hh.get_slot_info(3).status == SlotStatus::LOADED);
        CHECK(hh.get_slot_info(0).status == SlotStatus::AVAILABLE);
    }

    SECTION("an unload frame carrying only filament clears it") {
        hh.feed_mmu_status(nlohmann::json{{"filament", "Unloaded"}});

        REQUIRE_FALSE(hh.is_filament_loaded());
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }

    SECTION("a later gate_status delta does not resurrect the old gate") {
        hh.feed_mmu_status(nlohmann::json{{"gate", 3}});
        hh.feed_mmu_status(nlohmann::json{{"gate_status", nlohmann::json::array({1, 0, 1, 1})}});

        CHECK(hh.get_slot_info(3).status == SlotStatus::LOADED);
        CHECK(hh.get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK(hh.get_slot_info(1).status == SlotStatus::EMPTY);
    }
}

// Where the aggregate and the per-gate status legitimately disagree

TEST_CASE("Happy Hare does not paint an empty gate as loaded", "[ams][happy_hare][1199]") {
    // gate_status 0 after a runout is real information: the gate has nothing
    // left to feed, and load_filament()'s "slot not available" refusal keys on
    // it. Happy Hare can still report that gate as the loaded one, because the
    // filament it already fed is at the toolhead — the same shape as the AD5X
    // #995 runout. This disagreement is precisely why Happy Hare keeps the
    // aggregate rule: switching to the per-gate status would blank the
    // active-lane highlight here.
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({0, 1, 1, 1}, 0, "Loaded"));

    CHECK(hh.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK(hh.slot_is_actively_loaded(0));
    CHECK_FALSE(hh.slot_is_actively_loaded(1));
}

TEST_CASE("Happy Hare bypass and no-gate states stamp nothing", "[ams][happy_hare][1199]") {
    helix::AmsBackendHappyHareTestHelper hh;

    SECTION("bypass (gate -2) is not a gate index") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, -2, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
            CHECK_FALSE(hh.slot_is_actively_loaded(i));
        }
    }

    SECTION("no gate selected (-1) is not a gate index") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, -1, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }

    SECTION("a gate index past the end is ignored, not a crash") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, 9, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }
}
