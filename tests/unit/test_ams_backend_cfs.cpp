// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 42 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly [calib],
// [general], [bypass], [calib][2007], [calib][k2cal], and more

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/cfs_test_fixtures.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/force_bypass_controls_guard.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toast_log.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system/telemetry_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;
using helix::printer::AmsBackendCfs;
using helix::printer::CfsMaterialDb;
using helix::test::make_cfs_status_json;
using helix::test::make_unit_box_explicit;

using helix::test::CfsAutoRefillHelper;
using helix::test::CfsCalibHelper;
using helix::test::CfsInsertRuleRig;
using helix::test::CfsK1RemapHelper;
using helix::test::CfsK2CalibHelper;
using helix::test::CfsLifecycleHelper;
using helix::test::CfsLiveAckBackend;
using helix::test::CfsOverrideRig;
using helix::test::CfsRemapHelper;
using helix::test::CfsTmpCacheDir;
using helix::test::check_not_single_spool;
using helix::test::configfile_queries;
using helix::test::drain_calib_queue;
using helix::test::feed_toolhead_filament;
using helix::test::find_action;
using helix::test::FlatFailDispatchCfs;
using helix::test::HeldAckCfs;
using helix::test::json;
using helix::test::k2_plus_startup_box;
using helix::test::k2_plus_t1_connected;
using helix::test::kEditedRemainingG;
using helix::test::link_lane_four_to_spool_137;
using helix::test::MacroCacheScope;
using helix::test::make_boot_box_all_units_down;
using helix::test::make_cfs_notification;
using helix::test::make_empty_bus_box;
using helix::test::make_flat_box;
using helix::test::make_flat_fork_box;
using helix::test::make_fork_box_with_boxes;
using helix::test::make_gapped_fork_box;
using helix::test::make_multi_unit_box;
using helix::test::make_runout_box;
using helix::test::make_runout_removed_box;
using helix::test::make_runout_seated_box;
using helix::test::make_single_unit_box;
using helix::test::make_unbranded_bay_box;
using helix::test::make_unit_up_delta;
using helix::test::no_unit_settings;
using helix::test::paused_ctx;
using helix::test::per_slot_box;
using helix::test::per_slot_unit;
using helix::test::registered_unit_settings;
using helix::test::script_has_command;
using helix::test::SyncCfs;
using helix::test::toolhead_sensor_frame;
using helix::test::UnansweredClientCfs;
using helix::test::UnhomedCfsBackend;
using json = nlohmann::json;

TEST_CASE("CFS bypass: fork dialect commands the attended load", "[ams][cfs][bypass]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    CfsTestAccess::set_flat_fork(backend, /*external_index=*/4);

    SECTION("enable dispatches T<external>") {
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        REQUIRE(backend.dispatched[0] == "T4");
    }

    SECTION("no external entry in the payload means no bypass") {
        CfsTestAccess::set_flat_fork(backend, /*external_index=*/-1);
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::NOT_SUPPORTED);
        REQUIRE(backend.dispatched.empty());
    }

    SECTION("disable while engaged dispatches BOX_UNLOAD") {
        // Engaged = firmware named the external entry in loaded_slot.
        backend.fire_status(make_cfs_notification(json::parse(
            R"({"api_version": 1, "loaded_slot": 4, "slots": [
                 {"index": 0, "external": false, "present": true},
                 {"index": 1, "external": false, "present": true},
                 {"index": 2, "external": false, "present": true},
                 {"index": 3, "external": false, "present": true},
                 {"index": 4, "external": true,  "present": true}]})")));
        REQUIRE(backend.is_bypass_active());

        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        REQUIRE(backend.dispatched[0] == "BOX_UNLOAD");
    }

    SECTION("disable when not engaged is a no-op success") {
        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.empty());
    }
}

TEST_CASE("CFS bypass: stock dialect declaration + sensor derivation", "[ams][cfs][bypass]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    // Stock is the default schema; a full box frame is what flips supports_bypass on.
    backend.fire_status(make_cfs_notification(json::parse(
        R"({"state": "connect", "filament": 0, "enable": 1,
            "filament_useup": 0,
            "map": {"T1A": "T1A"},
            "T1": {"state": "connect", "filament": "None",
                   "vender": ["none"], "remain_len": ["-1"],
                   "color_value": ["-1"], "material_type": ["-1"]}})")));

    SECTION("first full box frame flips supports_bypass") {
        auto info = backend.get_system_info();
        REQUIRE(info.supports_bypass);
        CHECK_FALSE(backend.can_cancel_operation()); // its Cancel queues behind the load script
    }

    SECTION("enable stands the CFS down and declares; sensor feed confirms; "
            "sensor clear retracts") {
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        // No motion script exists for stock — but the box's print feed is
        // stood down so it cannot drive bay filament into the tube the
        // external spool occupies.
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_CFS_PRINT ENABLE=0"});
        REQUIRE(backend.dispatched.empty());
        REQUIRE(backend.is_bypass_active()); // declaration half

        // Toolhead sensor sees hand-fed filament with no active lane.
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
        backend.fire_status(json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -2);

        // Filament pulled back out: sentinel clears. The declaration itself
        // persists until the user toggles bypass off — is_bypass_active()
        // keeps reporting the declared half, matching the sidebar toggle.
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/false);
        backend.fire_status(json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -1);
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("sensor never saw filament: no derivation") {
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/false, /*detected=*/false);
        backend.fire_status(json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -1);
    }

    SECTION("disable re-arms the CFS and clears the declaration") {
        backend.enable_bypass();
        backend.captured.clear();

        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_CFS_PRINT ENABLE=1"});
        REQUIRE_FALSE(backend.is_bypass_active());

        // Re-enable works after a disable.
        REQUIRE(backend.enable_bypass().result == AmsResult::SUCCESS);
    }

    SECTION("disable when fully off is a no-op — no spurious re-arm send") {
        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured.empty());
        REQUIRE(backend.dispatched.empty());
    }

    SECTION("the box re-arming itself does NOT drop the declaration") {
        // enable=1 does not mean "someone re-enabled the CFS through Creality's
        // own screen": the frame below is the literal frame a K2 Plus sent after
        // re-arming with no host involved — no BOX_ENABLE_CFS_PRINT in
        // klippy.log, no disable_bypass() in ours (the only thing that sends
        // ENABLE=1), and the prints in that window were plain Moonraker
        // start_print calls from a third-party client.
        //
        // Dropping the declaration on enable=1 would bite only on the first full
        // frame after a restart (partial frames omit `enable`): bypass would
        // survive the session and die on relaunch, while the external spool was
        // still feeding the nozzle and Unload greyed out.
        backend.enable_bypass();
        REQUIRE(backend.is_bypass_active());

        backend.fire_status(make_cfs_notification(json::parse(
            R"({"state":"connect","filament":0,"auto_refill":0,"enable":1,
                "map":{"T1A":"T1A"},
                "T1":{"state":"connect","filament":"None","vender":["none"],
                      "remain_len":["-1"],"color_value":["-1"],
                      "material_type":["-1"]}})")));
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("bay filament at the toolhead blocks enable") {
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true,
                                        /*current_slot=*/2);
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE(backend.dispatched.empty());
    }
}

TEST_CASE("CFS reports an unload after print only where the firmware does one",
          "[ams][cfs][unload_after_print]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    auto fork_box = [](bool unload) {
        json box = make_flat_fork_box();
        box["unload_after_print_enabled"] = unload;
        return make_cfs_notification(box);
    };

    SECTION("stock firmware states no option and always unloads") {
        backend.fire_status(make_cfs_notification(json::parse(R"({"state":"connect","filament":0,
            "auto_refill":0,"enable":1,"map":{"T1A":"T1A"},
            "T1":{"state":"connect","filament":"None","vender":["none"],"remain_len":["-1"],
                  "color_value":["-1"],"material_type":["-1"]}})")));
        CHECK(backend.auto_unloads_after_print());
    }
    SECTION("fork firmware with the option off leaves the filament in") {
        backend.fire_status(fork_box(false));
        CHECK_FALSE(backend.auto_unloads_after_print());
    }
    SECTION("fork firmware with the option on unloads") {
        backend.fire_status(fork_box(true));
        CHECK(backend.auto_unloads_after_print());
    }
}

TEST_CASE("CFS Endless Spool switch sends an explicit ENABLE", "[ams][cfs]") {
    // BOX_ENABLE_AUTO_REFILL is a setter, not a toggle: the handler reads
    // ENABLE via gcmd.get_int, and Creality's own master-server sends
    // ENABLE=1/0 explicitly on both families.
    helix::test::StartedBackend<CfsAutoRefillHelper> backend;
    backend.captured.clear();

    SECTION("switching on while the box reports off sends ENABLE=1") {
        CfsTestAccess::set_loaded_state(backend, false, -1);
        {
            // Seed endless_spool_enabled=false via a box frame (auto_refill=0).
            backend.fire_status(make_cfs_notification(json::parse(
                R"({"state":"connect","filament":0,"auto_refill":0,"enable":1,
                    "map":{"T1A":"T1A"},
                    "T1":{"state":"connect","filament":"None","vender":["none"],
                          "remain_len":["-1"],"color_value":["-1"],
                          "material_type":["-1"]}})")));
        }
        backend.captured.clear();
        auto err = backend.set_endless_spool_enabled(true);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=1"});
        drain_calib_queue();
    }

    SECTION("switching off while the box reports on sends ENABLE=0") {
        backend.fire_status(make_cfs_notification(json::parse(
            R"({"state":"connect","filament":0,"auto_refill":1,"enable":1,
                "map":{"T1A":"T1A"},
                "T1":{"state":"connect","filament":"None","vender":["none"],
                      "remain_len":["-1"],"color_value":["-1"],
                      "material_type":["-1"]}})")));
        backend.captured.clear();
        auto err = backend.set_endless_spool_enabled(false);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=0"});
        drain_calib_queue();
    }
}

TEST_CASE("CFS actions name their guided flows", "[ams][cfs][calib]") {
    CfsK1RemapHelper backend;

    const auto cutter = find_action(backend, "calibrate_cutter");
    REQUIRE(cutter.has_value());
    CHECK(cutter->guided_flow == "cfs_cutter");

    const auto chute = find_action(backend, "calibrate_purge_chute");
    REQUIRE(chute.has_value());
    CHECK(chute->guided_flow == "cfs_purge_chute");

    // An ordinary action runs straight through the backend.
    const auto rfid = find_action(backend, "refresh_rfid");
    REQUIRE(rfid.has_value());
    CHECK(rfid->guided_flow.empty());
}

TEST_CASE("CFS K1 calibration section carries the cutter and chute (#1282)", "[ams][cfs][calib]") {
    SECTION("K1 dialect exposes both calibration actions in one section") {
        CfsK1RemapHelper backend;
        const auto sections = backend.get_device_sections();
        REQUIRE(std::any_of(sections.begin(), sections.end(),
                            [](const auto& s) { return s.id == "calibration"; }));

        const auto cutter = find_action(backend, "calibrate_cutter");
        REQUIRE(cutter.has_value());
        CHECK(cutter->type == helix::printer::ActionType::BUTTON);
        CHECK(cutter->section == "calibration");
        CHECK(cutter->enabled);
        CHECK(cutter->disable_reason.empty());

        const auto chute = find_action(backend, "calibrate_purge_chute");
        REQUIRE(chute.has_value());
        CHECK(chute->section == "calibration");
        CHECK(chute->enabled);
    }

    SECTION("K2 and Fork without the K2 macros have no calibration surface") {
        for (bool fork : {false, true}) {
            CfsK2CalibHelper backend({}, fork);
            CHECK(backend.get_device_sections().size() == 1);
            // Clean Nozzle and Communication Test; stock adds Refresh RFID, which
            // the Fork has no command for.
            CHECK(backend.get_device_actions().size() == (fork ? 2u : 3u));
            CHECK(backend.tension_belts(nullptr).result == AmsResult::NOT_SUPPORTED);
            CHECK(backend.calibrate_cutter(nullptr).result == AmsResult::NOT_SUPPORTED);
            CHECK(backend.captured.empty());
        }
    }
}

TEST_CASE("CFS K2 calibration buttons follow the printer's macros", "[ams][cfs][calib][k2cal]") {
    for (bool fork : {false, true}) {
        CfsK2CalibHelper belts_only({"BELT_TENSION"}, fork);
        const auto belts = find_action(belts_only, "tension_belts");
        REQUIRE(belts.has_value());
        CHECK(belts->section == "calibration");
        CHECK(belts->moves_machine);
        CHECK(belts->guided_flow == "cfs_belt_tension");
        CHECK(belts_only.get_device_sections().back().id == "calibration");
        CHECK_FALSE(find_action(belts_only, "calibrate_cutter").has_value());
        CHECK_FALSE(find_action(belts_only, "calibrate_purge_chute").has_value());
    }
    CfsK2CalibHelper cutter_only({"CALIBRATE_CUT_POS"});
    const auto cutter = find_action(cutter_only, "calibrate_cutter");
    REQUIRE(cutter.has_value());
    CHECK(cutter->guided_flow == "cfs_cutter");
    CHECK_FALSE(find_action(cutter_only, "tension_belts").has_value());
    // Only the K1 sweep has a measured duration for the confirm to quote.
    CHECK_FALSE(cutter_only.cutter_calibration_takes_a_minute());

    // A BELT_TENSION macro without the K2's tensioner motors is someone's own macro.
    for (bool fork : {false, true}) {
        CfsK2CalibHelper no_motors({"BELT_TENSION"}, fork, /*belt_motors=*/false);
        CHECK_FALSE(find_action(no_motors, "tension_belts").has_value());
        CHECK(no_motors.tension_belts(nullptr).result == AmsResult::NOT_SUPPORTED);
        CHECK(no_motors.captured.empty());
    }

    // A K1 macro that happens to share the name is not a motorized tensioner.
    CfsK1RemapHelper k1;
    MacroCacheScope macros({"BELT_TENSION"});
    CHECK_FALSE(find_action(k1, "tension_belts").has_value());
    CHECK(k1.cutter_calibration_takes_a_minute());
}

TEST_CASE("CFS K2 calibration sends the printer's macros, one at a time",
          "[ams][cfs][calib][k2cal]") {
    CfsK2CalibHelper backend({"BELT_TENSION", "CALIBRATE_CUT_POS"});
    // Bare BELT_TENSION tensions both axes on the stock and the Kalico macro.
    backend.hold = {"BELT_TENSION"};
    bool belts_ok = false;
    REQUIRE(backend.execute_device_action("tension_belts", {}).success());
    REQUIRE(backend.tension_belts([&](bool ok, const std::string&) { belts_ok = ok; }).result ==
            AmsResult::BUSY);
    CHECK(backend.calibrate_cutter(nullptr).result == AmsResult::BUSY);
    CHECK_FALSE(find_action(backend, "calibrate_cutter")->enabled);
    CHECK(find_action(backend, "tension_belts")->disable_reason == "Calibration in progress");
    backend.release_held();
    drain_calib_queue();

    REQUIRE(backend.calibrate_cutter(nullptr).success());
    CHECK(backend.captured == std::vector<std::string>{"BELT_TENSION", "CALIBRATE_CUT_POS"});

    backend.fail = {"BELT_TENSION"};
    REQUIRE(backend.tension_belts([&](bool ok, const std::string&) { belts_ok = !ok; }).success());
    drain_calib_queue();
    CHECK(belts_ok); // the failure reached the caller
    CHECK(find_action(backend, "tension_belts")->enabled);

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::PAUSED}) {
        helix::test::set_wire_state(backend.state, state);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(backend.tension_belts(nullptr).user_msg == "Printer is busy with a print");
        CHECK(backend.calibrate_cutter(nullptr).user_msg == "Printer is busy with a print");
    }
    CHECK(backend.captured.size() == 3);
}

TEST_CASE("CFS cutter calibration sends BOX_FIND_CUT_POS through the long-send path (#1282)",
          "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    bool got_result = false;
    std::string result_line;
    auto err = backend.calibrate_cutter([&](bool ok, const std::string& line) {
        got_result = ok;
        result_line = line;
    });
    REQUIRE(err.success());
    // The macro takes no parameters; the completion callback overload is what
    // carries the long (~60s) runtime without an RPC timeout failing it.
    REQUIRE(backend.captured == std::vector<std::string>{"BOX_FIND_CUT_POS"});
    drain_calib_queue();
    CHECK(got_result);
}

TEST_CASE("CFS calibration actions move the machine and lock while one runs", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    for (const char* id : {"calibrate_cutter", "calibrate_purge_chute", "nozzle_clean"}) {
        INFO(id);
        const auto a = find_action(backend, id);
        REQUIRE(a.has_value());
        CHECK(a->moves_machine);
        CHECK(a->enabled);
    }

    backend.hold = {"BOX_FIND_CUT_POS"};
    REQUIRE(backend.calibrate_cutter(nullptr).success());
    for (const char* id : {"calibrate_cutter", "calibrate_purge_chute"}) {
        INFO(id);
        const auto a = find_action(backend, id);
        REQUIRE(a.has_value());
        CHECK_FALSE(a->enabled);
        CHECK(a->disable_reason == "Calibration in progress");
    }

    backend.release_held();
    drain_calib_queue();
    CHECK(find_action(backend, "calibrate_cutter")->enabled);
}

TEST_CASE("CFS chute calibration homes, prepares, then reports ready (#1282)",
          "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    bool ready = false;
    REQUIRE(backend.start_chute_calibration([&] { ready = true; }).success());
    drain_calib_queue();
    CHECK(backend.captured ==
          std::vector<std::string>({"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO",
                                    "BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_PREPARE"}));
    CHECK(ready);
}

TEST_CASE("CFS chute jog sends the stock screen's exact script form (#1282)", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    REQUIRE(backend.jog_chute_y(-10.0f).success());
    REQUIRE(backend.jog_chute_y(0.5f).success());
    // Verbatim stock form: state save/restore around a relative move, M400 so
    // the RPC ack means the move finished.
    const std::string jog_down = "SAVE_GCODE_STATE NAME=myMoveState\n G91\n G0 Y-10.000 F3000\n"
                                 " M400\n RESTORE_GCODE_STATE NAME=myMoveState";
    REQUIRE(backend.captured[0] == jog_down);
    REQUIRE(backend.captured[1].find("G0 Y0.500 F3000") != std::string::npos);
}

TEST_CASE("CFS chute X jog moves only X, in the stock script form", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    REQUIRE(backend.jog_chute_x(-0.1f).success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "SAVE_GCODE_STATE NAME=myMoveState\n G91\n G0 X-0.100 F3000\n"
                                 " M400\n RESTORE_GCODE_STATE NAME=myMoveState");
}

TEST_CASE("CFS chute jog refuses a print and a non-K1 dialect on both axes", "[ams][cfs][calib]") {
    SECTION("while a print owns the machine") {
        CfsCalibHelper backend;
        helix::test::set_wire_state(backend.state, helix::PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(backend.jog_chute_x(1.0f).success());
        CHECK_FALSE(backend.jog_chute_y(1.0f).success());
        CHECK(backend.captured.empty());
    }
    SECTION("on a dialect without the calibration") {
        CfsRemapHelper backend;
        CHECK(backend.jog_chute_x(1.0f).result == AmsResult::NOT_SUPPORTED);
        CHECK(backend.jog_chute_y(1.0f).result == AmsResult::NOT_SUPPORTED);
    }
}

TEST_CASE("CFS chute save reads the live position then re-parks (#1282)", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    bool saved = false;
    REQUIRE(backend.save_chute_position([&] { saved = true; }).success());
    drain_calib_queue();
    CHECK(backend.captured ==
          std::vector<std::string>({"BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_SAVE_POS",
                                    "BOX_CUSTOM_COMMAND CMD=Y_SAFE"}));
    CHECK(saved);
}

TEST_CASE("CFS chute calibration cancelled during the home stops before PREPARE (#1282)",
          "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.hold = {"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"};
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    bool ready = false;
    REQUIRE(backend.start_chute_calibration([&] { ready = true; }, nullptr, cancel).success());
    REQUIRE(backend.captured == std::vector<std::string>{"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"});

    cancel->store(true); // the user leaves while the home is still running
    backend.release_held();
    drain_calib_queue();

    // The home cannot be aborted, but nothing downstream may go out: no
    // PREPARE, so the box is never left in adjust mode at the chute.
    CHECK(backend.captured == std::vector<std::string>{"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"});
    CHECK_FALSE(ready);
}

TEST_CASE("CFS chute start failure reports the firmware message and never parks (#1282)",
          "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.fail = {"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"};
    bool ready = false;
    std::string failed_msg;
    REQUIRE(
        backend
            .start_chute_calibration([&] { ready = true; },
                                     [&](const MoonrakerError& err) { failed_msg = err.message; })
            .success());
    drain_calib_queue();
    // PREPARE never ran, so there is nothing to unwind: no further command.
    CHECK(backend.captured == std::vector<std::string>{"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"});
    CHECK_FALSE(ready);
    CHECK_FALSE(failed_msg.empty());
}

TEST_CASE("CFS chute save failure re-parks and reports the firmware message (#1282)",
          "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.fail = {"BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_SAVE_POS"};
    bool saved = false;
    std::string failed_msg;
    bool position_saved = true;
    REQUIRE(backend
                .save_chute_position([&] { saved = true; },
                                     [&](const MoonrakerError& err, bool was_saved) {
                                         failed_msg = err.message;
                                         position_saved = was_saved;
                                     })
                .success());
    drain_calib_queue();
    // The save only ever runs after PREPARE, so a failure always re-parks.
    CHECK(backend.captured ==
          std::vector<std::string>({"BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_SAVE_POS",
                                    "BOX_CUSTOM_COMMAND CMD=Y_SAFE"}));
    CHECK_FALSE(saved);
    CHECK_FALSE(failed_msg.empty());
    CHECK_FALSE(position_saved);
}

TEST_CASE("CFS chute park failure after a good save reports the position as saved (#2007)",
          "[ams][cfs][calib][2007]") {
    helix::test::FakeMoonrakerClient console;
    CfsCalibHelper backend(nullptr, &console);
    const auto handlers = console.live_handlers();
    backend.fail = {"BOX_CUSTOM_COMMAND CMD=Y_SAFE"};
    bool saved = false;
    bool failed = false;
    bool position_saved = false;
    MoonrakerErrorType type = MoonrakerErrorType::NONE;
    REQUIRE(backend
                .save_chute_position([&] { saved = true; },
                                     [&](const MoonrakerError& err, bool was_saved) {
                                         failed = true;
                                         position_saved = was_saved;
                                         type = err.type;
                                     })
                .success());
    drain_calib_queue();
    REQUIRE(failed);
    CHECK(position_saved);
    CHECK(type == MoonrakerErrorType::JSON_RPC_ERROR);
    CHECK_FALSE(saved);
    // The park itself failed, so no second park goes out after it.
    CHECK(backend.captured ==
          std::vector<std::string>({"BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_SAVE_POS",
                                    "BOX_CUSTOM_COMMAND CMD=Y_SAFE"}));
    // The save's console watch goes with the failed park.
    CHECK(console.live_handlers() == handlers);
}

TEST_CASE("CFS calibration steps complete in the queue pass after each answer",
          "[ams][cfs][calib][threading][1761]") {
    using helix::ui::UpdateQueueTestAccess;
    CfsLiveAckBackend backend;

    SECTION("cutter calibration: one answer, one pass") {
        bool got_result = false;
        REQUIRE(backend.calibrate_cutter([&](bool ok, const std::string&) { got_result = ok; })
                    .success());
        CHECK(UpdateQueueTestAccess::passes_until([&] { return got_result; }) == 1);
    }
    SECTION("chute calibration: home then prepare, one pass each") {
        bool ready = false;
        REQUIRE(backend.start_chute_calibration([&] { ready = true; }).success());
        CHECK(UpdateQueueTestAccess::passes_until([&] { return ready; }) == 2);
    }
    SECTION("chute save: save then re-park, one pass each") {
        bool saved = false;
        REQUIRE(backend.save_chute_position([&] { saved = true; }).success());
        CHECK(UpdateQueueTestAccess::passes_until([&] { return saved; }) == 2);
    }
}

TEST_CASE("CFS chute park failure keeps the pair the save wrote (#2007)",
          "[ams][cfs][calib][2007]") {
    CfsLiveAckBackend backend;
    backend.set_client_for_test(&backend.printer.client);
    backend.printer.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                                  "Must home axis first", "CMD=Y_SAFE");
    bool position_saved = false;
    REQUIRE(backend
                .save_chute_position(nullptr, [&](const MoonrakerError&,
                                                  bool was_saved) { position_saved = was_saved; })
                .success());
    backend.printer.client.dispatch_gcode_response("cmd_save_extrude_pos x=152.50 y=219.50");
    drain_calib_queue();
    REQUIRE(position_saved);
    double x = 0.0, y = 0.0;
    REQUIRE(backend.last_chute_saved_position(x, y));
    CHECK(x == Catch::Approx(152.5));
    CHECK(y == Catch::Approx(219.5));
}

TEST_CASE("CFS Endless Spool switch records the box's answer in the queue pass after it",
          "[ams][cfs][threading][1847]") {
    helix::test::StartedBackend<CfsLiveAckBackend> backend;
    backend.fire_status(make_cfs_notification(json::parse(
        R"({"state":"connect","filament":0,"auto_refill":0,"enable":1,
            "map":{"T1A":"T1A"},
            "T1":{"state":"connect","filament":"None","vender":["none"],
                  "remain_len":["-1"],"color_value":["-1"],
                  "material_type":["-1"]}})")));
    drain_calib_queue();
    REQUIRE_FALSE(backend.get_system_info().endless_spool_enabled);

    REQUIRE(backend.set_endless_spool_enabled(true).success());
    CHECK(helix::ui::UpdateQueueTestAccess::passes_until(
              [&] { return backend.get_system_info().endless_spool_enabled; }) == 1);
}

TEST_CASE("CFS refuses a second calibration while one is in flight (#1282)", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.hold = {"BOX_FIND_CUT_POS"};
    bool got_result = false;
    REQUIRE(
        backend.calibrate_cutter([&](bool, const std::string&) { got_result = true; }).success());
    REQUIRE(backend.captured == std::vector<std::string>{"BOX_FIND_CUT_POS"});

    SECTION("a second cutter start is refused as busy") {
        const auto second = backend.calibrate_cutter();
        CHECK_FALSE(second.success());
        CHECK(backend.captured == std::vector<std::string>{"BOX_FIND_CUT_POS"});
    }
    SECTION("a chute start while the cutter runs is refused as busy") {
        const auto second = backend.start_chute_calibration([] {});
        CHECK_FALSE(second.success());
        CHECK(backend.captured == std::vector<std::string>{"BOX_FIND_CUT_POS"});
    }

    backend.release_held();
    drain_calib_queue();
    CHECK(got_result);
}

TEST_CASE("CFS chute cancel path re-parks with Y_SAFE (#1282)", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    REQUIRE(backend.exit_chute_calibration().success());
    REQUIRE(backend.captured == std::vector<std::string>{"BOX_CUSTOM_COMMAND CMD=Y_SAFE"});
}

TEST_CASE("CFS chute flow sends no move once a print holds the machine", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    const std::string kHome = "BOX_CUSTOM_COMMAND CMD=XYZ_ZERO";
    const std::string kPrepare = "BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_PREPARE";
    const std::string kSave = "BOX_CUSTOM_COMMAND CMD=COORDINATES_ADJUST_SAVE_POS";
    auto print_starts = [&] {
        helix::test::set_wire_state(backend.state, helix::PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
    };
    bool ready = false;
    std::string failure;

    SECTION("the exit re-park") {
        print_starts();
        CHECK_FALSE(backend.exit_chute_calibration().success());
        CHECK(backend.captured.empty());
    }
    SECTION("a save") {
        print_starts();
        CHECK_FALSE(backend.save_chute_position().success());
        CHECK(backend.captured.empty());
    }
    SECTION("prepare after a home the print arrived during") {
        backend.hold = {kHome};
        REQUIRE(backend
                    .start_chute_calibration([&] { ready = true; },
                                             [&](const MoonrakerError& e) { failure = e.message; })
                    .success());
        print_starts();
        backend.release_held();
        drain_calib_queue();
        CHECK(backend.captured == std::vector<std::string>{kHome});
        CHECK_FALSE(ready);
        CHECK_FALSE(failure.empty());
    }
    SECTION("ready and the re-park after a prepare the print arrived during") {
        backend.hold = {kPrepare};
        REQUIRE(backend
                    .start_chute_calibration([&] { ready = true; },
                                             [&](const MoonrakerError& e) { failure = e.message; })
                    .success());
        print_starts();
        backend.release_held();
        drain_calib_queue();
        CHECK(backend.captured == std::vector<std::string>{kHome, kPrepare});
        CHECK_FALSE(ready);
        CHECK_FALSE(failure.empty());
    }
    SECTION("the re-park after a save that fails once the print has arrived") {
        backend.fail_later = {kSave};
        REQUIRE(backend
                    .save_chute_position(
                        nullptr, [&](const MoonrakerError& e, bool) { failure = e.message; })
                    .success());
        print_starts();
        backend.fail_held();
        drain_calib_queue();
        CHECK(backend.captured == std::vector<std::string>{kSave});
        CHECK_FALSE(failure.empty());
    }
    SECTION("the park after a save the print arrived during") {
        backend.hold = {kSave};
        bool saved = false;
        REQUIRE(backend.save_chute_position([&] { saved = true; }).success());
        print_starts();
        backend.release_held();
        drain_calib_queue();
        CHECK(backend.captured == std::vector<std::string>{kSave});
        CHECK(saved);
    }
}

TEST_CASE("CFS chute save refuses a toolhead that lost its homing", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    for (const char* axes : {"", "z", "xz", "yz"}) {
        lv_subject_set_string(backend.state.motion_state().get_homed_axes_subject(), axes);
        CHECK_FALSE(backend.save_chute_position().success());
        CHECK(backend.captured.empty());
    }
    lv_subject_set_string(backend.state.motion_state().get_homed_axes_subject(), "xy");
    CHECK(backend.save_chute_position().success());
    CHECK_FALSE(backend.captured.empty());
}

TEST_CASE("CFS chute save reports a park that fails after the save", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.fail = {"BOX_CUSTOM_COMMAND CMD=Y_SAFE"};
    bool saved = false;
    std::string failure;
    REQUIRE(backend
                .save_chute_position([&] { saved = true; },
                                     [&](const MoonrakerError& e, bool) { failure = e.message; })
                .success());
    drain_calib_queue();
    CHECK_FALSE(saved);
    CHECK(failure.find("Y_SAFE") != std::string::npos);
    // The failed park is reported once, not retried.
    CHECK(backend.captured.size() == 2);
}

TEST_CASE("CFS chute failure after the caller left is not reported to it", "[ams][cfs][calib]") {
    CfsCalibHelper backend;
    backend.fail = {"BOX_CUSTOM_COMMAND CMD=XYZ_ZERO"};
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    bool failed = false;
    REQUIRE(backend
                .start_chute_calibration(
                    nullptr, [&](const MoonrakerError&) { failed = true; }, cancel)
                .success());
    drain_calib_queue();
    CHECK_FALSE(failed);
}

TEST_CASE("CFS chute save line parses both firmware echo forms (#1282)", "[ams][cfs][calib]") {
    double x = 0.0, y = 0.0;
    REQUIRE(AmsBackendCfs::parse_chute_save_line("cmd_save_extrude_pos x=184.50 y=304.00", x, y));
    CHECK(x == Catch::Approx(184.5));
    CHECK(y == Catch::Approx(304.0));

    REQUIRE(AmsBackendCfs::parse_chute_save_line(
        "SAVE_BOX_CFG ok: extrude_pos_x=184.5,extrude_pos_y=304.0", x, y));
    CHECK(x == Catch::Approx(184.5));
    CHECK(y == Catch::Approx(304.0));

    CHECK_FALSE(AmsBackendCfs::parse_chute_save_line("SAVE_BOX_CFG ok: cut_pos_y=223.5", x, y));
    CHECK_FALSE(AmsBackendCfs::parse_chute_save_line("", x, y));
}

TEST_CASE("CFS cutter found line parses axis and value (#1282)", "[ams][cfs][calib]") {
    char axis = '\0';
    double value = 0.0;
    REQUIRE(AmsBackendCfs::parse_cut_found_line("Found cut position y: 304.0", axis, value));
    CHECK(axis == 'y');
    CHECK(value == Catch::Approx(304.0));

    CHECK_FALSE(
        AmsBackendCfs::parse_cut_found_line("SAVE_BOX_CFG ok: cut_pos_y=223.5", axis, value));
    CHECK_FALSE(AmsBackendCfs::parse_cut_found_line("", axis, value));
}

// The UI greys these through moves_machine; the backend refuses whatever gets
// past. A pause blocks too: both flows home the toolhead by design, which a
// paused print cannot tolerate.
TEST_CASE("CFS calibration refuses while a print owns the machine (#1282)", "[ams][cfs][calib]") {
    CfsCalibHelper backend;

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::PAUSED}) {
        helix::test::set_wire_state(backend.state, state);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(backend.calibrate_cutter(nullptr).user_msg == "Printer is busy with a print");
        CHECK(backend.start_chute_calibration(nullptr).user_msg == "Printer is busy with a print");
        CHECK(backend.captured.empty());
    }

    helix::test::set_wire_state(backend.state, helix::PrintJobState::STANDBY);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(backend.calibrate_cutter(nullptr).success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "BOX_FIND_CUT_POS");
    // Flush the completion defer before the backend goes out of scope.
    drain_calib_queue();
}

TEST_CASE("CFS type enum", "[ams][cfs]") {
    REQUIRE(std::string(ams_type_to_string(AmsType::CFS)) == "CFS");
}

TEST_CASE("CFS names each lane by its box number and bay letter", "[ams][cfs][lane_name]") {
    const helix::printer::AmsBackendCfs backend(nullptr, nullptr);
    CHECK(backend.lane_name(0) == "1A");
    CHECK(backend.lane_name(3) == "1D");
    CHECK(backend.lane_name(5) == "2B");
    CHECK(backend.lane_name(15) == "4D");
    // No bay: the bypass sentinel and indexes past the fourth box have no name,
    // so the caller falls back to the numbered label.
    CHECK(backend.lane_name(-2).empty());
    CHECK(backend.lane_name(16).empty());
}

TEST_CASE("CFS material database", "[ams][cfs]") {
    // Material resolution goes through FilamentCatalog's transient "cfs"-coded
    // slice.
    const auto catalog = FilamentCatalog::load_codes("cfs");

    SECTION("known material lookup") {
        const auto* info = catalog.resolve_code("cfs", "01001");
        REQUIRE(info != nullptr);
        REQUIRE(info->brand == "Creality");
        REQUIRE(info->name == "Hyper PLA");
        REQUIRE(info->type == "PLA");
        REQUIRE(info->nozzle_min == 190);
        REQUIRE(info->nozzle_max == 240);
    }

    SECTION("unknown material returns nullptr") {
        const auto* info = catalog.resolve_code("cfs", "99999");
        REQUIRE(info == nullptr);
    }

    SECTION("code stripping: 101001 to 01001 (K2, Creality prefix)") {
        auto id = CfsMaterialDb::strip_code("101001");
        REQUIRE(id == "01001");
    }

    SECTION("code stripping: K1 Generic prefix (#968)") {
        // K1C reporter tn_data.json: 000001 = PLA, 000003 = PETG — the
        // catalog ids are 00001 / 00003, so the leading '0' is the same
        // brand-prefix digit the K2 carries as '1'.
        REQUIRE(CfsMaterialDb::strip_code("000001") == "00001");
        REQUIRE(CfsMaterialDb::strip_code("000003") == "00003");
        REQUIRE(FilamentCatalog::load_codes("cfs").resolve_code("cfs", "00003") != nullptr);
    }

    SECTION("short code returned as-is") {
        auto id = CfsMaterialDb::strip_code("01001");
        REQUIRE(id == "01001");
    }

    SECTION("sentinel -1 returns empty") {
        auto id = CfsMaterialDb::strip_code("-1");
        REQUIRE(id.empty());
    }
}

TEST_CASE("CFS color parsing", "[ams][cfs]") {
    SECTION("parse 0RRGGBB format") {
        REQUIRE(CfsMaterialDb::parse_color("0C12E1F") == 0xC12E1F);
        REQUIRE(CfsMaterialDb::parse_color("0FFFFFF") == 0xFFFFFF);
        REQUIRE(CfsMaterialDb::parse_color("0000000") == 0x000000);
    }

    SECTION("sentinel values return default") {
        REQUIRE(CfsMaterialDb::parse_color("-1") == 0x808080);
        REQUIRE(CfsMaterialDb::parse_color("None") == 0x808080);
    }
}

TEST_CASE("CFS error decoding", "[ams][cfs]") {
    SECTION("known error code decodes to message and hint") {
        auto alert = CfsErrorDecoder::decode("key845", 0, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->message == "Nozzle clog detected");
        REQUIRE_FALSE(alert->hint.empty());
        REQUIRE(alert->level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert->error_code == "CFS-845");
    }

    SECTION("slot-level error includes slot index") {
        auto alert = CfsErrorDecoder::decode("key843", 0, 2);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::SLOT);
        REQUIRE(alert->slot_index == 2);
    }

    SECTION("unit-level error includes unit index") {
        auto alert = CfsErrorDecoder::decode("key853", 1, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::UNIT);
        REQUIRE(alert->unit_index == 1);
    }

    SECTION("unknown error code returns nullopt") {
        auto alert = CfsErrorDecoder::decode("key999", 0, -1);
        REQUIRE_FALSE(alert.has_value());
    }
}

TEST_CASE("CFS error message+values decoding", "[ams][cfs]") {
    using nlohmann::json;

    SECTION("key849 splices unit/slot locator into message") {
        // Real telemetry shape observed 2026-05-05.
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Retract failed") != std::string::npos);
        REQUIRE(out->first.find("unit 1 slot B") != std::string::npos);
        // Hint passes through unchanged.
        REQUIRE(out->second.find("Manually pull") != std::string::npos);
    }

    SECTION("key851 (slot retract didn't reach) also gets unit/slot locator") {
        json values = json::array({2, "C"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key851", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit 2 slot C") != std::string::npos);
    }

    SECTION("key840 (unit-level) gets unit-only locator") {
        json values = json::array({3});
        auto out = CfsErrorDecoder::lookup_message_with_values("key840", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("on unit 3") != std::string::npos);
    }

    SECTION("key832/key833 decode the generic retract and feed jams") {
        json values = json::array({1, "A"});
        auto retract = CfsErrorDecoder::lookup_message_with_values("key832", values);
        REQUIRE(retract.has_value());
        REQUIRE(retract->first.find("Retract failed") != std::string::npos);
        auto feed = CfsErrorDecoder::lookup_message_with_values("key833", values);
        REQUIRE(feed.has_value());
        REQUIRE(feed->first.find("Feed failed") != std::string::npos);
    }

    SECTION("key838 points at the CFS hub rather than the hotend") {
        json values = json::array({1, "A"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key838", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("CFS hub") != std::string::npos);
        REQUIRE(out->second.find("hotend") == std::string::npos);
    }

    SECTION("key111 (cold extruder) surfaces pre-heat guidance") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key111", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Pre-heat") != std::string::npos);
        REQUIRE(out->second.find("220") != std::string::npos);
        REQUIRE(out->second.find("PETG") != std::string::npos);
    }

    SECTION("key298 (system, no formatter) returns message untouched") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key298", values);
        REQUIRE(out.has_value());
        // No "unit" or "slot" appended.
        REQUIRE(out->first.find("unit") == std::string::npos);
        REQUIRE(out->first.find("slot") == std::string::npos);
    }

    SECTION("malformed values fall back to no locator (no regression)") {
        // Wrong shape — array but slot not a string.
        json values = json::array({1, 2});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit") == std::string::npos);
    }

    SECTION("unknown code returns nullopt") {
        json values = json::array({1, "A"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key999", values);
        REQUIRE_FALSE(out.has_value());
    }

    SECTION("key843 prefers the firmware's own msg when present") {
        // key843's causes vary (a busy box, an unreadable tag) and the
        // firmware's msg names which one fired (#1387). The table text would
        // assert one cause for all of them.
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key843", values,
                                                               "read rfid failed, box is busy");
        REQUIRE(out.has_value());
        CHECK(out->first.find("box is busy") != std::string::npos);
        CHECK(out->first.find("Can't read filament RFID tag") == std::string::npos);
        // The unit/slot locator is still spliced onto the firmware's wording.
        CHECK(out->first.find("unit 1 slot B") != std::string::npos);
    }

    SECTION("key843 without a firmware msg keeps the table text") {
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key843", values);
        REQUIRE(out.has_value());
        CHECK(out->first.find("Can't read filament RFID tag") != std::string::npos);
    }

    SECTION("non-preferring codes ignore the firmware msg") {
        // key849's table text is right for every cause; the firmware wording
        // ("retrude error...") must not displace it.
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values, "retrude error");
        REQUIRE(out.has_value());
        CHECK(out->first.find("Retract failed") != std::string::npos);
    }
}

TEST_CASE("CFS slot addressing", "[ams][cfs]") {
    SECTION("global index to TNN name") {
        REQUIRE(CfsMaterialDb::slot_to_tnn(0) == "T1A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(1) == "T1B");
        REQUIRE(CfsMaterialDb::slot_to_tnn(3) == "T1D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(4) == "T2A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(7) == "T2D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(15) == "T4D");
    }

    SECTION("TNN name to global index") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1A") == 0);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1D") == 3);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T2A") == 4);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T4D") == 15);
    }

    SECTION("invalid TNN returns -1") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("invalid") == -1);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T5A") == -1);
    }
}

TEST_CASE("CFS tag reading carries the Spoolman spool its record names", "[ams][cfs][tag_link]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json box = make_multi_unit_box(1);
    box["T1"]["vender"] = json::array({"AB1240276A21010010FFFFFF0165000042000000",
                                       "BB5250276A21010010FFFFFF0165000001000000", "none", "none"});
    box["T1"]["material_type"] = json::array({"101001", "101001", "-1", "-1"});
    box["T1"]["color_value"] = json::array({"0FFFFFF", "0FFFFFF", "-1", "-1"});
    backend.fire_status(make_cfs_notification(box));

    CHECK(backend.tags_name_spoolman_spools());
    const auto tagged = backend.tag_reading(0);
    REQUIRE(tagged);
    CHECK(tagged->spoolman_id == 42);
    const auto stock = backend.tag_reading(1);
    REQUIRE(stock);
    CHECK_FALSE(stock->spoolman_id);

    SECTION("a record naming no spool takes the id away") {
        box["T1"]["vender"][0] = "BB5250276A21010010FFFFFF0165000001000000";
        backend.fire_status(make_cfs_notification(box));
        const auto after = backend.tag_reading(0);
        REQUIRE(after);
        CHECK_FALSE(after->spoolman_id);
    }
}

// Klipper sends deltas, so one frame's presence gates read the frame while the
// values they guard can come from every frame before it.
TEST_CASE("CFS reads a stock delta against the frames before it", "[ams][cfs]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json box = make_multi_unit_box(1);
    box["filament_useup"] = 1;
    box["T1"]["vender"] =
        json::array({"AB1240276A21010010FFFFFF0165000042000000", "none", "none", "none"});
    auto poll = [&backend](const json& frame) {
        backend.fire_status(make_cfs_notification(frame));
        helix::ui::UpdateQueue::instance().drain();
    };
    poll(box);
    REQUIRE(backend.get_system_info().filament_runout);
    REQUIRE(backend.get_system_info().endless_spool_enabled);
    backend.captured.clear();

    SECTION("a unit delta that omits a field keeps what it said") {
        poll(json{{"T1", {{"remain_len", json::array({"40", "-1", "-1", "-1"})}}}});
        const auto tagged = backend.tag_reading(0);
        REQUIRE(tagged);
        CHECK(tagged->spoolman_id == 42);
        CHECK(backend.get_system_info().filament_runout);
        CHECK(backend.get_system_info().endless_spool_enabled);
        CHECK(backend.captured.empty());
    }

    SECTION("a delta that states a field replaces it") {
        poll(json{{"filament_useup", 0}, {"auto_refill", 0}, {"filament", 0}});
        CHECK_FALSE(backend.get_system_info().filament_runout);
        CHECK_FALSE(backend.get_system_info().endless_spool_enabled);
    }

    SECTION("a vender delta alone is an insert edge") {
        poll(json{{"filament_useup", 0}, {"filament", 0}}); // a latched runout defers the probe
        poll(json{{"T1",
                   {{"vender", json::array({"AB1240276A21010010FFFFFF0165000042000000", "unknown",
                                            "none", "none"})}}}});
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");
    }
}

// Every CFS box feeds the printer's one extruder, so the overview draws one
// toolhead and routes each box on the bus into it; an absent box routes nowhere.
TEST_CASE("CFS boxes converge on one toolhead in the system path layout",
          "[ams][cfs][tool_layout][ams_draw]") {
    auto [boxes, gap] = GENERATE(std::pair{1, 0}, std::pair{2, 0}, std::pair{4, 0}, std::pair{2, 1},
                                 std::pair{4, 2}, std::pair{4, 3});
    INFO(boxes << " boxes, absent address " << gap);
    json box = make_multi_unit_box(boxes);
    if (gap > 0) {
        box["T" + std::to_string(gap)]["state"] = "None";
    }
    const auto info = AmsBackendCfs::parse_box_status(box);
    const auto layout = ams_draw::compute_system_tool_layout(info, nullptr);

    CHECK(layout.total_physical_tools == 1);
    REQUIRE(layout.units.size() == info.units.size());
    int routed = 0;
    for (size_t u = 0; u < info.units.size(); ++u) {
        INFO("unit " << u);
        CHECK(layout.units[u].tool_count == (info.units[u].absent ? 0 : 1));
        if (!info.units[u].absent) {
            CHECK(layout.units[u].first_physical_tool == 0);
            ++routed;
        }
    }
    CHECK(routed == boxes - (gap > 0 ? 1 : 0));
    // The active-route highlight resolves every bay's tool to that toolhead.
    for (const auto& unit : info.units) {
        for (const auto& slot : unit.slots) {
            if (slot.mapped_tool >= 0) {
                CHECK(layout.virtual_to_physical.at(slot.mapped_tool) == 0);
            }
        }
    }
}

// Cards run in box-address order, so a box off the bus keeps its physical place.
TEST_CASE("CFS overview cards stay in address order around an absent box",
          "[ams][cfs][tool_layout][ams_draw]") {
    json box = make_multi_unit_box(4);
    box["T2"]["state"] = "None";
    const auto info = AmsBackendCfs::parse_box_status(box);
    const auto order = ams_draw::compute_system_tool_layout(info, nullptr).display_order;

    REQUIRE(order.size() == 4);
    std::vector<int> addresses;
    for (int i : order) {
        addresses.push_back(info.units[static_cast<size_t>(i)].unit_index + 1);
    }
    CHECK(addresses == std::vector<int>{1, 2, 3, 4});
    CHECK(info.units[static_cast<size_t>(order[1])].absent);
}

TEST_CASE("CFS echo lines parse by scanf's rules, all or nothing (#2165)", "[ams][cfs][calib]") {
    double x = -1.0, y = -2.0;
    SECTION("negative values, loose spacing, trailing text") {
        REQUIRE(
            AmsBackendCfs::parse_chute_save_line("cmd_save_extrude_pos x=-1.5   y= 2e1 ok", x, y));
        CHECK(x == Catch::Approx(-1.5));
        CHECK(y == Catch::Approx(20.0));
    }
    SECTION("the SAVE_BOX_CFG spelling yields both coordinates") {
        REQUIRE(AmsBackendCfs::parse_chute_save_line(
            "SAVE_BOX_CFG ok: extrude_pos_x=184.5,extrude_pos_y=-304.0", x, y));
        CHECK(x == Catch::Approx(184.5));
        CHECK(y == Catch::Approx(-304.0));
    }
    SECTION("a line missing y leaves both outputs alone") {
        CHECK_FALSE(AmsBackendCfs::parse_chute_save_line("cmd_save_extrude_pos x=184.5 y=", x, y));
        CHECK_FALSE(AmsBackendCfs::parse_chute_save_line("cmd_save_extrude_pos x=184.5", x, y));
        CHECK_FALSE(AmsBackendCfs::parse_chute_save_line(
            "SAVE_BOX_CFG ok: extrude_pos_x=184.5;extrude_pos_y=3", x, y));
        CHECK_FALSE(AmsBackendCfs::parse_chute_save_line("cmd_save_extrude_pos x=abc y=1", x, y));
        CHECK(x == -1.0);
        CHECK(y == -2.0);
    }
    SECTION("the cut line takes any single character as the axis") {
        char axis = '\0';
        double v = 0.0;
        REQUIRE(AmsBackendCfs::parse_cut_found_line("Found cut position   x:-12.5", axis, v));
        CHECK(axis == 'x');
        CHECK(v == Catch::Approx(-12.5));
        axis = '\0';
        v = 7.0;
        CHECK_FALSE(AmsBackendCfs::parse_cut_found_line("Found cut position y 304.0", axis, v));
        CHECK_FALSE(AmsBackendCfs::parse_cut_found_line("Found cut position y: ", axis, v));
        CHECK_FALSE(AmsBackendCfs::parse_cut_found_line("Found cut position ", axis, v));
        CHECK(axis == '\0');
        CHECK(v == 7.0);
    }
}
