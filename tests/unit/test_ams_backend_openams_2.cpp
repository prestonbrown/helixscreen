// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 28 to 55 of test_ams_backend_openams.cpp before it was split, in its order. Mostly
// [commands], [dryer], [openams_spoolman], [fault], [openams_spoolman][review], and more

#include "ui_ams_detail.h"
#include "ui_test_utils.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/ams_backend_openams_fixtures.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "ams_backend_openams.h"
#include "ams_error.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "core/update_queue.h"
#include "filament_chip_model.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/openams_status_json.h"
#include "test_helpers/openams_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsBackendOpenAms;
using helix::AmsResult;
using helix::AmsType;
using helix::PathTopology;
using helix::SlotStatus;
using json = nlohmann::json;
using namespace openams_json;

using helix::test::backup_pair_manager;
using helix::test::component_status;
using helix::test::ComponentRig;
using helix::test::kClearHt;
using helix::test::OpenAmsHarness;
using helix::test::TmpCacheDir;
using helix::test::tool_group;

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS unload names the lane, for either command map",
                 "[ams][openams][commands]") {
    struct Map {
        json commands;
        std::string unload;
    };
    for (const Map& map : {Map{all_commands(), "OPENAMS_UNLOAD FPS=fps"},
                           Map{toolhead_commands(), "OAMSM_UNLOAD_FROM_TOOLHEAD FPS=fps"}}) {
        CAPTURE(map.unload);
        OpenAmsHarness backend;
        backend.feed(shared_manager(3, map.commands));
        REQUIRE(backend.unload_filament(3).success());
        CHECK(backend.operations == std::vector<std::string>{map.unload});
    }

    SECTION("a slot that is not loaded falls back to the loaded one") {
        OpenAmsHarness backend;
        backend.feed(shared_manager(3, toolhead_commands()));
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.operations == std::vector<std::string>{"OAMSM_UNLOAD_FROM_TOOLHEAD FPS=fps"});
    }

    SECTION("nothing loaded sends nothing") {
        OpenAmsHarness backend;
        backend.feed(shared_manager(-1));
        CHECK_FALSE(backend.unload_filament(2).success());
        CHECK(backend.operations.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS load into an empty lane is one command, for either map",
                 "[ams][openams][commands]") {
    struct Map {
        json commands;
        std::string load;
    };
    for (const Map& map : {Map{all_commands(), "OPENAMS_LOAD GROUP=T2 SLOT=2"},
                           Map{toolhead_commands(), "OAMSM_LOAD_TO_TOOLHEAD GROUP=T2 SLOT=2"}}) {
        CAPTURE(map.load);
        OpenAmsHarness backend;
        backend.feed(shared_manager(-1, map.commands));
        REQUIRE(backend.load_filament(2).success());
        CHECK(backend.operations == std::vector<std::string>{map.load});
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "OpenAMS load over another slot on the lane unloads it first, as one operation",
                 "[ams][openams][commands]") {
    struct Map {
        json commands;
        std::string script;
    };
    for (const Map& map :
         {Map{all_commands(), "OPENAMS_UNLOAD FPS=fps\nOPENAMS_LOAD GROUP=T2 SLOT=2"},
          Map{toolhead_commands(),
              "OAMSM_UNLOAD_FROM_TOOLHEAD FPS=fps\nOAMSM_LOAD_TO_TOOLHEAD GROUP=T2 SLOT=2"}}) {
        CAPTURE(map.script);
        {
            OpenAmsHarness backend;
            backend.feed(shared_manager(0, map.commands));
            REQUIRE(backend.load_filament(2).success());
            CHECK(backend.operations == std::vector<std::string>{map.script});
            CHECK(backend.get_current_action() == AmsAction::LOADING);
            CHECK(backend.needs_unload_before_load(backend.get_system_info(), 2));
        }
        {
            OpenAmsHarness backend; // a tool change to T2 swaps the same way
            backend.feed(shared_manager(0, map.commands));
            REQUIRE(backend.change_tool(2).success());
            CHECK(backend.operations == std::vector<std::string>{map.script});
        }
    }

    SECTION("loading the slot already loaded does not unload it") {
        OpenAmsHarness backend;
        backend.feed(shared_manager(2, toolhead_commands()));
        REQUIRE(backend.load_filament(2).success());
        CHECK(backend.operations ==
              std::vector<std::string>{"OAMSM_LOAD_TO_TOOLHEAD GROUP=T2 SLOT=2"});
        CHECK_FALSE(backend.needs_unload_before_load(backend.get_system_info(), 2));
    }

    SECTION("a manager with no unload command cannot swap") {
        json commands = toolhead_commands();
        commands.erase("unload");
        OpenAmsHarness backend;
        backend.feed(shared_manager(0, commands));
        CHECK(backend.load_filament(2).result == AmsResult::NOT_SUPPORTED);
        CHECK(backend.operations.empty());
    }

    SECTION("a slot on another lane is loaded without touching the first") {
        OpenAmsHarness backend;
        json m = shared_manager(0, toolhead_commands());
        m["units"][1]["lane"] = "fps1";
        json second = lane("unloaded");
        second["id"] = "fps1";
        m["lanes"].push_back(second);
        backend.feed(m);
        REQUIRE(backend.load_filament(2).success());
        CHECK(backend.operations ==
              std::vector<std::string>{"OAMSM_LOAD_TO_TOOLHEAD GROUP=T2 SLOT=2"});
        CHECK_FALSE(backend.needs_unload_before_load(backend.get_system_info(), 2));
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS names no macro in its missing-command errors",
                 "[ams][openams][commands]") {
    json commands = toolhead_commands();
    commands.erase("load");
    commands.erase("unload");
    OpenAmsHarness backend;
    backend.feed(shared_manager(0, commands));
    for (const auto& result : {backend.load_filament(2), backend.unload_filament(0)}) {
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
        CHECK(result.technical_msg.find("oams_macros") == std::string::npos);
        CHECK(result.technical_msg.find("OPENAMS_") == std::string::npos);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "with openams_spoolman a bay shows the component's status, not lane_data",
                 "[ams][openams][openams_spoolman][late_links]") {
    MockPrinter mock_printer;
    mock_printer.client.set_openams_spoolman(true);
    mock_printer.client.set_openams_spoolman_status(component_status(41, "PETG", "1F3A93"));
    // Its lane_data is per tool: this record is tool 2's, not slot 2's.
    mock_printer.client.mock_db_set("lane_data", "lane3",
                                    json{{"lane", "2"}, {"material", "ABS"}, {"spool_id", 7}});
    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&mock_printer.api,
                                                               &mock_printer.client);
    OpenAmsHarness& backend = *backend_reg;
    backend.feed(shared_manager(0));
    helix::OpenAmsTestAccess::start(backend);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    auto info = backend.get_slot_info(2);
    CHECK(info.spoolman_id == 41);
    CHECK(info.material == "PETG");
    CHECK(info.color_rgb == 0x1F3A93u);
    CHECK(info.brand == "Sunlu");
    CHECK(info.remaining_weight_g == Catch::Approx(500.0f));
    CHECK_FALSE(helix::ams::lane_sources(backend.lane_id(2)).record.has_value());
    // The poll's read of lane_data files nothing onto the bays either.
    helix::OpenAmsTestAccess::refresh_lane_records(backend);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(helix::ams::lane_sources(backend.lane_id(2)).record.has_value());

    // A link made after start arrives with the component's notification.
    mock_printer.client.dispatch_method_callback(
        "notify_openams_spoolman_status",
        json{{"method", "notify_openams_spoolman_status"},
             {"params", json::array({component_status(42, "PLA", "FFFFFF")})}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    info = backend.get_slot_info(2);
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a per-tool record is not filed on a bay before the component answers",
                 "[ams][openams][openams_spoolman][review]") {
    MockPrinter mock_printer;
    mock_printer.client.set_openams_spoolman(true);
    mock_printer.client.set_openams_spoolman_status(component_status(41, "PETG", "1F3A93"));
    mock_printer.client.mock_db_set("lane_data", "lane3",
                                    json{{"lane", "2"}, {"material", "ABS"}, {"spool_id", 7}});
    mock_printer.client.defer_next("server.openams_spoolman.status");
    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&mock_printer.api,
                                                               &mock_printer.client);
    OpenAmsHarness& backend = *backend_reg;
    backend.feed(shared_manager(0));
    helix::OpenAmsTestAccess::start(backend);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // The lane_data read is in; what it describes is not known yet.
    CHECK_FALSE(helix::ams::lane_sources(backend.lane_id(2)).record.has_value());
    CHECK(backend.get_slot_info(2).material != "ABS");
    CHECK_FALSE(backend.lane_records_ready());

    mock_printer.client.fire_deferred("server.openams_spoolman.status");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(backend.lane_records_ready());
    CHECK(backend.get_slot_info(2).material == "PETG");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "without the component the records read before the probe are filed after it",
                 "[ams][openams][openams_spoolman][review]") {
    MockPrinter mock_printer;
    mock_printer.client.set_openams_spoolman(false);
    mock_printer.client.mock_db_set("lane_data", "lane3",
                                    json{{"lane", "2"}, {"material", "ABS"}, {"spool_id", 7}});
    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&mock_printer.api,
                                                               &mock_printer.client);
    OpenAmsHarness& backend = *backend_reg;
    backend.feed(shared_manager(0));
    helix::OpenAmsTestAccess::start(backend);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(backend.lane_records_ready());
    CHECK(backend.get_slot_info(2).material == "ABS");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Clear Spool is the component's on an OpenAMS it manages",
                 "[ams][openams][openams_spoolman]") {
    MockPrinter mock_printer;
    const bool component = GENERATE(false, true);
    mock_printer.client.set_openams_spoolman(component);
    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&mock_printer.api,
                                                               &mock_printer.client);
    OpenAmsHarness& backend = *backend_reg;
    backend.feed(shared_manager(0));
    helix::OpenAmsTestAccess::start(backend);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    CHECK(backend.clear_spool_refusal(2).empty() == !component);
    const int deletes = mock_printer.client.call_count("server.database.delete_item");
    backend.clear_slot_override(2);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK((mock_printer.client.call_count("server.database.delete_item") > deletes) == !component);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "with openams_spoolman an OpenAMS edit goes through its endpoint, not lane_data",
                 "[ams][openams][openams_spoolman]") {
    ComponentRig rig;
    const std::uint64_t before = helix::ams::pending_filings(rig.reg.lane(2));
    REQUIRE(rig.edit(2,
                     [](SlotInfo& s) {
                         s.material = "PETG";
                         s.color_rgb = 0x1E5AA8;
                         s.remaining_weight_g = 640.0f;
                     })
                .success());
    helix::ui::UpdateQueue::instance().drain_until_empty();

    const auto& edits = rig.printer.client.openams_spoolman_edits();
    REQUIRE(edits.size() == 1);
    CHECK(edits[0]["unit"] == "ams2");
    CHECK(edits[0]["bay"] == 1);
    CHECK(edits[0]["material"] == "PETG");
    CHECK(edits[0]["color_hex"] == "1e5aa8");
    CHECK(edits[0]["remaining_weight"] == 640.0);
    CHECK_FALSE(edits[0].contains("vendor"));
    // lane_data is the component's: nothing written there.
    CHECK(rig.printer.client.mock_db_get("lane_data", "lane3").is_null());
    // The component's answer is the acknowledgement.
    std::optional<helix::ams::PendingOutcome> heard;
    helix::ams::await_pending(rig.reg.lane(2), before,
                              [&](helix::ams::PendingOutcome o, const std::string&) { heard = o; });
    CHECK(heard == helix::ams::PendingOutcome::Echoed);
}

TEST_CASE_METHOD(LVGLUITestFixture, "openams_spoolman's refusal reaches the edit's waiter",
                 "[ams][openams][openams_spoolman]") {
    ComponentRig rig;
    MoonrakerError offline;
    offline.code = 503;
    offline.message = "Spoolman is offline";
    rig.printer.client.fail_next("server.openams_spoolman.edit", offline);
    const std::uint64_t before = helix::ams::pending_filings(rig.reg.lane(2));
    (void)rig.edit(2, [](SlotInfo& s) { s.material = "PETG"; });
    helix::ui::UpdateQueue::instance().drain_until_empty();

    std::optional<helix::ams::PendingOutcome> heard;
    std::string why;
    helix::ams::await_pending(rig.reg.lane(2), before,
                              [&](helix::ams::PendingOutcome o, const std::string& r) {
                                  heard = o;
                                  why = r;
                              });
    CHECK(heard == helix::ams::PendingOutcome::Failed);
    CHECK(why == "Spoolman is offline");
    CHECK_FALSE(helix::ams::lane_sources(rig.reg.lane(2)).pending.has_value());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "with openams_spoolman a field it cannot hold is refused before anything is sent",
                 "[ams][openams][openams_spoolman]") {
    ComponentRig rig;
    const int which = GENERATE(0, 1);
    const helix::AmsError err = which == 0
                                    ? rig.edit(2, [](SlotInfo& s) { s.spool_name = "PolyTerra"; })
                                    : rig.edit(2, [](SlotInfo& s) { s.spoolman_id = 9; });
    CHECK_FALSE(err.success());
    CHECK_FALSE(err.partially_applied);
    CHECK(rig.printer.client.openams_spoolman_edits().empty());
    CHECK_FALSE(helix::ams::lane_sources(rig.reg.lane(2)).pending.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS surfaces a latched unit fault in plain words",
                 "[ams][openams][fault]") {
    OpenAmsHarness backend;
    backend.feed(faulted_manager(
        json::array({fault_entry("motor_drive_fault", "motor_drive_fault", {"clear_fault"})})));

    const auto info = backend.get_system_info();
    CHECK(info.action == AmsAction::ERROR);
    CHECK(info.operation_detail == "ams_ht: Motor drive fault");
    REQUIRE(info.units[0].slots[0].error.has_value());
    CHECK(info.units[0].slots[0].error->message == "Motor drive fault");
    CHECK(info.units[0].has_any_error());
    CHECK_FALSE(info.units[1].has_any_error());

    const auto event = backend.current_error();
    REQUIRE(event.has_value());
    CHECK(event->detail == "ams_ht: Motor drive fault");
    REQUIRE(event->recovery_actions.size() == 1);
    CHECK(event->recovery_actions[0].gcode == kClearHt);

    SECTION("motion_timeout reads in words, an unknown code reads as the unit's text") {
        backend.feed(faulted_manager(json::array(
            {fault_entry("motion_timeout", "x", {"clear_fault"}),
             fault_entry("odd_code", "Spool jammed at the gate", json::array(), "pause")})));
        const auto detail = backend.get_system_info().operation_detail;
        CHECK(detail.find("Motion timed out") != std::string::npos);
        CHECK(detail.find("Spool jammed at the gate") != std::string::npos);
    }

    SECTION("a language switch re-words the standing fault with no new frame") {
        helix::ScopedLanguage fr("fr");
        backend.refresh_translated_texts();
        const auto fr_info = backend.get_system_info();
        CHECK(fr_info.operation_detail == "ams_ht: Défaut du pilote moteur");
        CHECK(fr_info.units[0].slots[0].error->message == "Défaut du pilote moteur");
    }

    SECTION("a bay fault marks only that bay") {
        backend.feed(
            faulted_manager(json::array({fault_entry("unload failed", "Spool unloading failed",
                                                     json::array({"clear_errors"}), "pause", 0)})));
        CHECK(backend.get_system_info().units[0].slots[0].error.has_value());
        CHECK_FALSE(backend.get_system_info().units[1].has_any_error());
    }

    SECTION("clearing the fault retires the error") {
        backend.feed(faulted_manager(json::array()));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK_FALSE(backend.current_error().has_value());
        CHECK_FALSE(backend.get_system_info().units[0].has_any_error());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS clears each faulted unit, then resets the manager",
                 "[ams][openams][fault]") {
    SECTION("openams shape: unit clears first, in unit order, then commands.reset") {
        OpenAmsHarness backend;
        backend.feed(
            faulted_manager(json::array({fault_entry("motor_drive_fault", "m", {"clear_fault"})}),
                            json::array({fault_entry("motion_timeout", "t", {"clear_fault"})})));
        REQUIRE(backend.reset().success());
        CHECK(backend.commands ==
              std::vector<std::string>{"OAMS_CLEAR_FAULT OAMS=1\nOAMS_CLEAR_FAULT OAMS=2\n"
                                       "OAMSM_CLEAR_ERRORS"});
    }

    SECTION("only the faulted unit is cleared") {
        OpenAmsHarness backend;
        backend.feed(faulted_manager(
            json::array(), json::array({fault_entry("motor_drive_fault", "m", {"clear_fault"})})));
        REQUIRE(backend.recover().success());
        CHECK(backend.commands ==
              std::vector<std::string>{"OAMS_CLEAR_FAULT OAMS=2\nOAMSM_CLEAR_ERRORS"});
    }

    SECTION("a fault the unit offers no clear_fault for resets the manager only") {
        OpenAmsHarness backend;
        backend.feed(
            faulted_manager(json::array({fault_entry("unload failed", "Spool unloading failed",
                                                     json::array({"clear_errors"}), "pause", 0)})));
        REQUIRE(backend.reset().success());
        CHECK(backend.commands == std::vector<std::string>{"OAMSM_CLEAR_ERRORS"});
        CHECK(backend.current_error().has_value());
    }

    SECTION("clear_fault clears the latched unit fault, and sends nothing without one") {
        OpenAmsHarness backend;
        backend.feed(
            faulted_manager(json::array({fault_entry("motor_drive_fault", "m", {"clear_fault"})})));
        REQUIRE(backend.clear_fault(-1).success());
        CHECK(backend.commands == std::vector<std::string>{kClearHt});

        OpenAmsHarness quiet;
        quiet.feed(shared_manager(-1, toolhead_commands()));
        REQUIRE(quiet.clear_fault(-1).success());
        CHECK(quiet.commands.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS shows no fault and resets as before without devices",
                 "[ams][openams][fault]") {
    // klipper_openams publishes no `devices`.
    OpenAmsHarness backend;
    backend.feed(shared_manager(-1, all_commands(), false));

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(backend.current_error().has_value());
    REQUIRE(backend.reset().success());
    CHECK(backend.commands == std::vector<std::string>{"OAMSM_CLEAR_ERRORS"});
}

// Environment and dryer

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS publishes no environment or dryer without devices",
                 "[ams][openams][environment][dryer]") {
    // klipper_openams publishes no `devices`.
    OpenAmsHarness backend;
    backend.feed(shared_manager(-1, all_commands(), false));

    CHECK_FALSE(backend.get_dryer_info(0).supported);
    CHECK_FALSE(backend.traits().has_environment_sensors);
    CHECK_FALSE(backend.get_system_info().units[0].environment.has_value());
    CHECK_FALSE(backend.start_drying(55.0f, 60, -1, 0).success());
    CHECK_FALSE(backend.stop_drying(0).success());
    CHECK(backend.commands.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS starts and stops a unit's dryer, clamped to its range",
                 "[ams][openams][dryer]") {
    OpenAmsHarness backend;
    backend.feed(dryer_manager());
    CHECK(backend.traits().has_environment_sensors);

    REQUIRE(backend.start_drying(55.0f, 120, -1, 0).success());
    CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=1 TARGET=55 DURATION=7200");

    SECTION("the AMS 2 Pro addresses its own index and ceiling") {
        REQUIRE(backend.start_drying(70.0f, 30, -1, 1).success());
        CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=2 TARGET=65 DURATION=1800");
    }
    SECTION("a target below the floor rises to it") {
        REQUIRE(backend.start_drying(30.0f, 60, -1, 0).success());
        CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=1 TARGET=45 DURATION=3600");
    }
    SECTION("a duration is held to 1 s .. 7 days") {
        REQUIRE(backend.start_drying(55.0f, 999999, -1, 0).success());
        CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=1 TARGET=55 DURATION=604800");
        REQUIRE(backend.start_drying(55.0f, 0, -1, 0).success());
        CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=1 TARGET=55 DURATION=60");
    }
    SECTION("stop names the unit") {
        REQUIRE(backend.stop_drying(1).success());
        CHECK(backend.commands.back() == "OAMS_DRYER_STOP OAMS=2");
    }
    SECTION("an unknown unit is refused") {
        const auto before = backend.commands.size();
        CHECK_FALSE(backend.start_drying(55.0f, 60, -1, 7).success());
        CHECK_FALSE(backend.stop_drying(-1).success());
        CHECK(backend.commands.size() == before);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS has no progress for a dry it did not see begin",
                 "[ams][openams][dryer]") {
    OpenAmsHarness backend;
    backend.feed(dryer_manager(-1, dryer_device(true, "holding", 55.0, 50 * 60)));
    CHECK(backend.get_dryer_info(0).get_progress_pct() == -1);
    backend.feed(dryer_manager(-1, dryer_device(true, "holding", 55.0, 30 * 60)));
    CHECK(backend.get_dryer_info(0).remaining_min == 30);
    CHECK(backend.get_dryer_info(0).get_progress_pct() == -1);

    SECTION("a dry that begins while watched measures from its longest reading") {
        backend.feed(dryer_manager());
        CHECK_FALSE(backend.get_dryer_info(0).active);
        backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 50 * 60)));
        CHECK(backend.get_dryer_info(0).get_progress_pct() == 0);
        backend.feed(dryer_manager(-1, dryer_device(true, "holding", 55.0, 30 * 60)));
        CHECK(backend.get_dryer_info(0).duration_min == 50);
        CHECK(backend.get_dryer_info(0).get_progress_pct() == 40);
    }
    SECTION("a requested duration longer than anything seen still wins") {
        backend.feed(dryer_manager());
        REQUIRE(backend.start_drying(55.0f, 120, -1, 0).success());
        backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 90 * 60)));
        CHECK(backend.get_dryer_info(0).duration_min == 120);
        CHECK(backend.get_dryer_info(0).get_progress_pct() == 25);
    }
    SECTION("a stop, or the API going away, forgets the cycle") {
        backend.feed(dryer_manager());
        backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 50 * 60)));
        REQUIRE(backend.get_dryer_info(0).duration_min == 50);
        SECTION("a stop") {
            REQUIRE(backend.stop_drying(0).success());
        }
        SECTION("the API going away") {
            json gone = dryer_manager();
            gone["api_version"] = 2;
            backend.feed(gone);
        }
        backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 20 * 60)));
        CHECK(backend.get_dryer_info(0).get_progress_pct() == -1);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS measures a dry it starts from that dry's own readings",
                 "[ams][openams][dryer]") {
    OpenAmsHarness backend;
    backend.feed(dryer_manager());
    backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 120 * 60)));
    REQUIRE(backend.get_dryer_info(0).duration_min == 120);

    // supported_actions and dryer.state are separate deltas: the unit can
    // advertise dryer_start again while its state still reads the last cycle
    // running, and a shorter dry is started in that window.
    json advertises_start = dryer_device(true, "heating", 55.0, 120 * 60);
    advertises_start["supported_actions"] = json{"load", "unload", "dryer_start", "dryer_stop"};
    backend.feed(dryer_manager(-1, advertises_start));
    REQUIRE(backend.start_drying(55.0f, 60, -1, 0).success());

    backend.feed(dryer_manager(-1, dryer_device(true, "heating", 55.0, 60 * 60)));
    CHECK(backend.get_dryer_info(0).duration_min == 60);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS an edit shows on its lane before any frame echoes it",
                 "[ams][openams][edit]") {
    OpenAmsHarness backend;
    backend.feed(manager());

    SlotInfo info = backend.get_slot_info(1);
    info.material = "PETG";
    info.color_rgb = 0x00FF00u;
    REQUIRE(helix::test::apply_edit(backend, 1, info).success());

    CHECK(backend.get_slot_info(1).material == "PETG");
    CHECK(backend.get_slot_info(1).color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS refuses to dry a unit that must be unloaded first",
                 "[ams][openams][dryer]") {
    OpenAmsHarness backend;
    // Slot 2 belongs to the AMS 2 Pro; the AMS HT dryer has no such condition.
    backend.feed(dryer_manager(2));

    const auto refused = backend.start_drying(55.0f, 60, -1, 1);
    CHECK_FALSE(refused.success());
    CHECK(refused.result == AmsResult::WRONG_STATE);
    CHECK(backend.commands.empty());

    REQUIRE(backend.start_drying(55.0f, 60, -1, 0).success());
    CHECK(backend.commands.size() == 1);

    backend.feed(dryer_manager(-1));
    REQUIRE(backend.start_drying(55.0f, 60, -1, 1).success());
    CHECK(backend.commands.back() == "OAMS_DRYER_START OAMS=2 TARGET=55 DURATION=3600");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "OpenAMS keeps a running dryer stoppable when only dryer_stop is advertised",
                 "[ams][openams][dryer]") {
    OpenAmsHarness backend;
    // dryer_start is withdrawn for as long as the cycle runs.
    backend.feed(
        dryer_manager(-1, dryer_device(true), dryer_device(false, "timed_dry", 45.0, 3000)));

    const DryerInfo running = backend.get_dryer_info(1);
    CHECK(running.supported);
    CHECK(running.active);
    CHECK(backend.get_dryer_info(0).supported);
    CHECK_FALSE(backend.get_dryer_info(0).active);

    REQUIRE(backend.stop_drying(1).success());
    CHECK(backend.commands.back() == "OAMS_DRYER_STOP OAMS=2");

    const auto before = backend.commands.size();
    const auto refused = backend.start_drying(55.0f, 60, -1, 1);
    CHECK_FALSE(refused.success());
    CHECK(refused.result == AmsResult::WRONG_STATE);
    CHECK(backend.commands.size() == before);

    SECTION("an idle unit with both actions still starts and stops") {
        REQUIRE(backend.start_drying(55.0f, 60, -1, 0).success());
        REQUIRE(backend.stop_drying(0).success());
    }
    SECTION("a unit offering only dryer_start cannot be stopped") {
        json start_only = dryer_device(true);
        start_only["supported_actions"] = json::array({"dryer_start"});
        backend.feed(dryer_manager(-1, start_only));
        CHECK(backend.get_dryer_info(0).supported);
        CHECK_FALSE(backend.stop_drying(0).success());
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "OpenAMS names each lane's extruder from the plugin's lanes_by_fps",
                 "[ams][openams][shared_hub]") {
    OpenAmsHarness backend;
    json m = shared_manager(0, all_commands(), true);
    m["units"][1]["lane"] = "fps2";
    for (auto& g : m["groups"]) {
        if (g["name"] != "T0") {
            g["lane"] = "fps2";
        }
    }
    json second = lane("unloaded");
    second["id"] = "fps2";
    m["lanes"].push_back(second);
    m["lanes_by_fps"]["fps2"] = {{"op", "idle"}, {"extruder", "extruder1"}};
    backend.feed(m);

    const auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.units[0].slots[0].extruder_name == "extruder");
    for (const auto& slot : info.units[1].slots) {
        CHECK(slot.extruder_name == "extruder1");
    }

    // Two lanes are two toolheads, each badged by its extruder.
    const auto layout = ams_draw::compute_system_tool_layout(info, &backend);
    CHECK(layout.total_physical_tools == 2);
    CHECK(layout.physical_to_extruder_name == std::vector<std::string>{"extruder", "extruder1"});

    SECTION("a manager without lanes_by_fps names none") {
        OpenAmsHarness legacy;
        legacy.feed(shared_manager(0));
        CHECK(legacy.get_system_info().units[0].slots[0].extruder_name.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS fleet mock publishes twelve units on two hubs",
                 "[ams][openams][mock][fleet]") {
    helix::ScopedEnv ams("HELIX_MOCK_AMS", "openams");
    helix::ScopedEnv units("HELIX_MOCK_OPENAMS_UNITS", "fleet");
    helix::ScopedEnv api("HELIX_MOCK_OPENAMS_API", nullptr);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);

    OpenAmsHarness backend;
    backend.feed(client.openams_status_json());

    const auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 12);
    CHECK(info.total_slots == 45);
    CHECK(info.units[0].slot_count == 1);
    for (size_t u = 1; u < info.units.size(); ++u) {
        CHECK(info.units[u].slot_count == 4);
    }

    std::set<std::string> hubs;
    for (size_t u = 0; u < info.units.size(); ++u) {
        hubs.insert(info.units[u].hub_id);
        CHECK(info.units[u].hub_id == (u < 10 ? "fps" : "fps2"));
        REQUIRE(info.units[u].environment.has_value());
    }
    CHECK(hubs.size() == 2);

    // Each hub's lane feeds its own extruder.
    for (size_t u = 0; u < info.units.size(); ++u) {
        for (const auto& slot : info.units[u].slots) {
            CHECK(slot.extruder_name == (u < 10 ? "extruder" : "extruder1"));
        }
    }

    // Readings differ unit to unit, so a page showing the wrong unit is visible.
    CHECK(info.units[1].environment->humidity_pct != info.units[2].environment->humidity_pct);

    // One bay loaded, on the third unit; one unit mid-dryer-cycle.
    CHECK(info.current_slot == 5);
    CHECK(info.units[2].slots[0].status == SlotStatus::LOADED);
    for (size_t u = 0; u < info.units.size(); ++u) {
        INFO("unit " << u);
        CHECK(backend.get_dryer_info(static_cast<int>(u)).active == (u == 5));
    }
    CHECK(backend.get_dryer_info(5).remaining_min > 0);

    // A group per bay: T0..T44.
    CHECK(backend.get_tool_mapping().size() == 45);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS maps a tool to the one slot serving it",
                 "[ams][openams][mapping]") {
    OpenAmsHarness backend;

    SECTION("the first ready member serves; its backup carries no tool") {
        backend.feed(backup_pair_manager());
        const auto info = backend.get_system_info();
        CHECK(info.units[0].slots[1].mapped_tool == 0);
        CHECK(info.units[0].slots[2].mapped_tool == -1);
        CHECK(backend.get_tool_mapping()[0] == 1);
    }

    SECTION("when the serving slot is empty, the backup takes the tool") {
        json m = backup_pair_manager();
        m["units"][0]["slots"][1]["ready"] = false;
        backend.feed(m);
        const auto info = backend.get_system_info();
        CHECK(info.units[0].slots[1].mapped_tool == -1);
        CHECK(info.units[0].slots[2].mapped_tool == 0);
        CHECK(backend.get_tool_mapping()[0] == 2);
    }

    SECTION("a loaded backup serves the tool over a ready member") {
        json m = backup_pair_manager();
        m["lanes"] = json::array({lane("loaded", "T0", 2)});
        m["units"][0]["slots"][2]["loaded"] = true;
        backend.feed(m);
        const auto info = backend.get_system_info();
        CHECK(info.units[0].slots[2].mapped_tool == 0);
        CHECK(info.units[0].slots[1].mapped_tool == -1);
        CHECK(backend.get_tool_mapping()[0] == 2);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS reports a runout-backup group as endless spool",
                 "[ams][openams][endless]") {
    OpenAmsHarness backend;

    SECTION("nothing is reported before a ready manager") {
        const auto caps = backend.get_endless_spool_capabilities();
        CHECK_FALSE(caps.available());
        CHECK(backend.get_endless_spool_config().empty());
    }

    SECTION("a two-slot group is one read-only, enabled group") {
        backend.feed(backup_pair_manager());
        const auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.available());
        CHECK_FALSE(caps.editable());
        CHECK(caps.enabled == helix::printer::EndlessSpoolEnabled::On);
        CHECK(caps.restriction == helix::printer::EndlessSpoolRestriction::FirmwareManaged);

        const auto config = backend.get_endless_spool_config();
        REQUIRE(config.groups.size() == 1);
        CHECK(config.groups[0].members == std::vector<int>{1, 2});
    }

    SECTION("single-slot groups back nothing up, so the setting is off") {
        json m = manager();
        m["groups"] =
            json::array({tool_group("T0", json::array({0})), tool_group("T1", json::array({2})),
                         tool_group("T2", json::array({3}))});
        backend.feed(m);
        const auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.available());
        CHECK(caps.enabled == helix::printer::EndlessSpoolEnabled::Off);
        CHECK(backend.get_endless_spool_config().empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS dispatches a backup slot as its own load",
                 "[ams][openams][dispatch]") {
    // Slot 0 is loaded on lane fps and slot 2 is its runout backup in T0.
    json m = backup_pair_manager();
    m["lanes"] = json::array({lane("loaded", "T0", 0)});
    m["units"][0]["slots"][0]["ready"] = true;
    m["units"][0]["slots"][0]["loaded"] = true;
    m["groups"] =
        json::array({tool_group("T0", json::array({0, 1, 2})), tool_group("T1", json::array({3}))});
    OpenAmsHarness backend;
    backend.feed(m);

    helix::AmsSystemInfo info;
    const helix::ui::BackendCaps caps = helix::ui::read_backend_caps(&backend, info, 2);
    REQUIRE(info.units[0].slots[2].mapped_tool == -1);

    const helix::ui::FilamentOpPlan plan = helix::ui::plan_load(info, caps, 2, false, false);
    CHECK(plan.tier == helix::ui::FilamentTier::AmsBackend);
    CHECK(plan.ams_call == helix::ui::AmsCall::Load);
    CHECK(plan.ams_arg == 2);
    CHECK(backend.needs_unload_before_load(info, 2));
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS tool change loads the slot serving the tool",
                 "[ams][openams][commands]") {
    SECTION("the ready serving slot, not the backup behind it") {
        OpenAmsHarness backend;
        backend.feed(backup_pair_manager());
        REQUIRE(backend.change_tool(0).success());
        CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=1"});
    }

    SECTION("the backup when the serving slot is empty") {
        OpenAmsHarness backend;
        json m = backup_pair_manager();
        m["units"][0]["slots"][1]["ready"] = false;
        backend.feed(m);
        REQUIRE(backend.change_tool(0).success());
        CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=2"});
    }

    SECTION("a loaded backup keeps serving the tool") {
        OpenAmsHarness backend;
        json m = backup_pair_manager();
        m["lanes"] = json::array({lane("loaded", "T0", 2)});
        m["units"][0]["slots"][2]["loaded"] = true;
        backend.feed(m);
        REQUIRE(backend.change_tool(0).success());
        CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=2"});
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS judges a tool on the slot serving its group",
                 "[ams][openams][mapping][filament_mapping][preflight]") {
    // manager(): T0 owns bays 0 and 1, bay 0 empty, so bay 1 serves T0.
    helix::test::RegisteredBackend<OpenAmsHarness> backend;
    backend->feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(backend->get_remap_strategy() == helix::AmsBackend::RemapStrategy::None);
    REQUIRE(backend->firmware_default_routing().head(0) == 1);

    helix::GcodeToolInfo t0{};
    t0.tool_index = 0;
    t0.color_known = false;
    auto& ams = helix::AmsState::instance();
    const auto slots = ams.collect_available_slots();
    const auto seed = ams.seed_tool_mappings({t0}, slots);
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].mapped_slot == 1);

    const auto* printing = helix::FilamentMapper::resolve_printing_slot(
        seed[0], slots, ams.collect_firmware_routing());
    REQUIRE(printing != nullptr);
    CHECK(printing->slot_index == 1);
    CHECK(helix::chip_severity(t0, printing) != helix::ChipSeverity::Block);

    const auto preflight = helix::file_preflight({t0}, seed, false);
    REQUIRE(preflight.checks.size() == 1);
    CHECK(preflight.checks[0].severity != helix::ToolCheck::Severity::EmptySlot);
    CHECK_FALSE(preflight.has_block());
}
