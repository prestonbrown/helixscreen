// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 75 to 116 of test_ams_backend_snapmaker.cpp before it was split, in its order. Mostly
// [resume][load_unconfirmed], [filament_record], [insert_watcher], [tool_switcher][1784],
// [insert_watcher][1710], and more

#include "ui_batch_filament_modal.h"
#include "ui_insert_notice.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/snapmaker_test_fixtures.h"
#include "../test_helpers/step_operation_helpers.h"
#include "../test_helpers/toast_log.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "filament_insert_watcher.h"
#include "filament_op_execute.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "snapmaker_nozzle_offset.h"
#include "spoolman_types.h" // SpoolInfo + apply_spool_to_slot (the picker-side writer)
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "tool_state.h"
#include "translation_loader.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::printer;
using namespace helix;

using helix::test::answers;
using helix::test::any_script_contains;
using helix::test::blue_translucent_rgba;
using helix::test::CapturingSnapmakerBackend;
using helix::test::check_head_writes;
using helix::test::count_containing;
using helix::test::declare_head0_asa;
using helix::test::declare_heads_0_and_2;
using helix::test::edit_head0_unechoed;
using helix::test::fail_refeed_then_switch_present;
using helix::test::fed;
using helix::test::fed_head0;
using helix::test::feed_with_action;
using helix::test::feeder;
using helix::test::FilamentRecordHarness;
using helix::test::FileToolsProbe;
using helix::test::golden_extruder;
using helix::test::golden_feed;
using helix::test::golden_full_frame;
using helix::test::heads_0_and_2_cleared;
using helix::test::identity_map;
using helix::test::index_of_containing;
using helix::test::inland_pla;
using helix::test::json;
using helix::test::judge;
using helix::test::kGolden;
using helix::test::linked_to_spool_42;
using helix::test::LoadAnswers;
using helix::test::make_detect_card;
using helix::test::make_feed_status;
using helix::test::make_filament_detect_status;
using helix::test::make_head_record;
using helix::test::make_official_task;
using helix::test::make_rig_detect;
using helix::test::make_task_config;
using helix::test::make_task_filament;
using helix::test::make_task_routing;
using helix::test::make_written_entry;
using helix::test::merged;
using helix::test::pause_on_runout;
using helix::test::petg_blue;
using helix::test::petg_blue_translucent;
using helix::test::polymaker_asa;
using helix::test::resume_once;
using helix::test::ResumeRecoveryHarness;
using helix::test::seated_tag_frame;
using helix::test::slicer_print_ready;
using helix::test::SnapmakerFixture;
using helix::test::SnapmakerGoldenFixture;
using helix::test::SnapmakerInsertFixture;
using helix::test::SnapmakerLaneRig;
using helix::test::SnapmakerTmpCacheDir;
using helix::test::spool_42;
using helix::test::t0_switch_off;
using helix::test::t3_feed;
using helix::test::t3_picked;
using helix::test::t3_switch;
using helix::test::translucent_head_echoed;
using helix::test::u1_all_parked_frame;
using helix::test::untagged_frame;
using helix::test::used_heads;
using helix::test::Warnings;
using helix::test::writes_for_head;

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker prepare_for_resume authors a translated reason",
                 "[ams][snapmaker][resume][i18n]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    ps.init_subjects(false);

    // "dirty bed" is one of snapmaker_terminal_matchers()' two signals.
    json paused = {{"print_stats", {{"state", "paused"}, {"message", "detected dirty bed"}}},
                   {"virtual_sdcard", {{"is_active", false}}}};
    ps.update_from_status(paused);

    ScopedLanguage ru("ru");

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    AmsError captured{AmsResult::SUCCESS};
    bool callback_fired = false;
    backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });

    REQUIRE(callback_fired);
    REQUIRE(captured.result == AmsResult::RESUME_REQUIRES_RESTART);

    // ui_resume_dispatch interpolates user_msg into a TRANSLATED frame, so an
    // English sentence here lands inside a Russian one.
    const char* expected = lv_tr("The bed was reported dirty, so this print cannot resume.");
    REQUIRE(std::string(expected) != "The bed was reported dirty, so this print cannot resume.");
    CHECK(captured.user_msg == std::string(expected));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume drives AUTO_FEEDING load before RESUME",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();

    ResumeRecoveryHarness h;
    helix::test::ToastLog toasts;

    bool callback_fired = false;
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    h.backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });
    toasts.expect_one(ToastSeverity::INFO, "Refeeding filament — this may take a minute", 8000);
    // The feeder reports the load before the command's answer is handled.
    h.backend.fire_status(make_feed_status(0, "load_finish"));
    ResumeRecoveryHarness::drain();

    const auto& hist = h.client.gcode_script_history();

    // (a) The single firmware load call was issued: AUTO_FEEDING with LOAD=1
    // PRINTING=1 (#991) — homes, switches tool, feeds, heats, extrudes, flushes.
    REQUIRE(any_script_contains(hist, "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1"));

    // (b) None of the old hand-rolled chain pieces survive — the sensor is NOT
    // disabled (that silently neutered FEED_AUTO), config is NOT re-asserted
    // (INNER_RESUME restores it), and there is no manual M109 heat (AUTO_FEEDING
    // does the heat itself).
    REQUIRE_FALSE(any_script_contains(hist, "SET_FILAMENT_SENSOR"));
    REQUIRE_FALSE(any_script_contains(hist, "SET_PRINT_FILAMENT_CONFIG"));
    REQUIRE_FALSE(any_script_contains(hist, "M109"));

    // Recovery succeeded => on_ready(success). RESUME will be dispatched by caller.
    REQUIRE(callback_fired);
    REQUIRE(captured.success());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume: a failed load refuses the resume in authored copy",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    // The second message is firmware's own wording, naming a 0-based extruder
    // ("e0_filament") the user never sees; the failure names the feeder.
    for (const char* firmware_error :
         {"simulated AMS load failure", "Filament Sensor e0_filament: Runout Detected"}) {
        INFO(firmware_error);
        ResumeRecoveryHarness h;
        h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, firmware_error,
                                        "AUTO_FEEDING");

        std::optional<AmsError> captured;
        h.backend.prepare_for_resume(/*slot_index=*/0,
                                     [&](const AmsError& err) { captured = err; });
        ResumeRecoveryHarness::drain();

        REQUIRE(captured.has_value());
        CHECK(captured->result == AmsResult::COMMAND_FAILED);
        CHECK(captured->user_msg.find("e0_filament") == std::string::npos);
        CHECK(captured->user_msg == "Toolhead 1 (T0) didn't load. Check that its hotend is "
                                    "seated and do a test extrude before resuming.");
        CHECK(captured->suggestion.find("Feeder 1") != std::string::npos);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume after a failed load is held although the head reads present",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    fail_refeed_then_switch_present(h);

    // Resume retries the load instead of trusting the switch, and the retry's
    // OK alone, with the feeder never reaching load_finish, is no load.
    const AmsError second = resume_once(h, 0);
    CHECK_FALSE(second.success());
    CHECK(second.user_msg ==
          "Toolhead 1 (T0) didn't load. Check that its hotend is seated and do a test extrude "
          "before resuming.");
    CHECK(count_containing(h.client.gcode_script_history(),
                           "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1") == 1);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume proceeds once the feeder confirms the load",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    fail_refeed_then_switch_present(h);

    SECTION("load_finish with no error releases Resume without another load") {
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }

    SECTION("load_finish carrying a channel error holds it") {
        h.backend.fire_status(make_feed_status(0, "load_finish", true, "no_filament"));
        CHECK_FALSE(resume_once(h, 0).success());
    }

    SECTION("a retried load that reaches load_finish resumes") {
        bool ok = false;
        h.backend.prepare_for_resume(0, [&](const AmsError& err) { ok = err.success(); });
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        ResumeRecoveryHarness::drain();
        CHECK(ok);
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume on another head waits for the failed head",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    fail_refeed_then_switch_present(h);
    SnapmakerTestAccess::set_current_slot(h.backend, 2);

    SECTION("a head the print uses: refused, nothing sent") {
        // Loading head 0 now would leave it picked up when RESUME runs on
        // head 2, so the answer is a refusal naming head 0.
        h.backend.fire_status(
            json{{"print_task_config", {{"extruders_used", {true, false, true, false}}}}});
        const AmsError err = resume_once(h, 2);
        CHECK_FALSE(err.success());
        CHECK(err.user_msg ==
              "Toolhead 1 (T0) didn't load. Check that its hotend is seated and do a test "
              "extrude before resuming.");
        CHECK(h.client.gcode_script_history().empty());
    }
    SECTION("a head the print does not use holds nothing") {
        h.backend.fire_status(
            json{{"print_task_config", {{"extruders_used", {false, false, true, false}}}}});
        CHECK(resume_once(h, 2).success());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker judges a refeed on the frame that trails its OK",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h; // head 0 ran out

    std::optional<AmsError> answer;
    h.backend.prepare_for_resume(0, [&](const AmsError& err) { answer = err; });
    ResumeRecoveryHarness::drain();
    // AUTO_FEEDING answered OK before the feeder's frame named its outcome.
    REQUIRE_FALSE(answer.has_value());

    SECTION("load_finish arrives: RESUME may go") {
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK(answer->success());
    }
    SECTION("an in-progress frame keeps waiting, then load_finish") {
        h.backend.fire_status(make_feed_status(0, "load_flushing"));
        ResumeRecoveryHarness::drain();
        CHECK_FALSE(answer.has_value());
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK(answer->success());
    }
    SECTION("no frame before the wait ends: not loaded") {
        SnapmakerTestAccess::expire_refeed_wait(h.backend);
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->success());
    }
    SECTION("load_finish a frame before its error reads ok waits for that frame") {
        h.backend.fire_status(make_feed_status(0, "load_finish", true, "none"));
        ResumeRecoveryHarness::drain();
        CHECK_FALSE(answer.has_value());
        h.backend.fire_status(
            json{{"filament_feed left", {{"extruder0", {{"channel_error", "ok"}}}}}});
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK(answer->success());
    }
    SECTION("a second Resume while the refeed runs is refused, the first still answered") {
        std::optional<AmsError> second;
        h.backend.prepare_for_resume(0, [&](const AmsError& err) { second = err; });
        ResumeRecoveryHarness::drain();
        REQUIRE(second.has_value());
        CHECK(second->result == AmsResult::BUSY);
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK(answer->success());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker takes a hand load as the load a failed head needed",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    fail_refeed_then_switch_present(h);
    h.backend.fire_status(make_feed_status(0, "manual_sta_finish"));
    CHECK(resume_once(h, 0).success());
    CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resumes a head loaded by hand past its feeder",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    const json switch_on{{"filament_motion_sensor e0_filament", {{"filament_detected", true}}}};
    const json switch_off{{"filament_motion_sensor e0_filament", {{"filament_detected", false}}}};
    h.backend.fire_status(make_feed_status(0, "wait_insert", /*filament_detected=*/false));

    SECTION("pushed in after a refeed found nothing to feed") {
        fail_refeed_then_switch_present(h);
        h.backend.fire_status(switch_on);
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("ran out: Resume asks for a hand load instead of a refeed") {
        h.backend.fire_status(switch_on);
        h.backend.fire_status(switch_off);
        const AmsError err = resume_once(h, 0);
        CHECK_FALSE(err.success());
        CHECK(err.suggestion == "Push filament into the toolhead by hand, then tap Resume.");
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("a feeder spool's tail passing the port is no hand push: Resume refeeds") {
        h.backend.fire_status(make_feed_status(0, "load_finish"));
        h.backend.fire_status(switch_on);
        h.backend.fire_status(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.backend.fire_status(switch_off);
        (void)resume_once(h, 0);
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker refuses RESUME into a head its feeder is still loading",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
    const std::string state =
        GENERATE(std::string("load_heating"), std::string("manual_sta_extruding"));
    h.backend.fire_status(json{{"filament_feed left",
                                {{"extruder0",
                                  {{"filament_detected", true},
                                   {"channel_state", state},
                                   {"channel_error", "ok"},
                                   {"module_exist", true},
                                   {"disable_auto", false}}}}}});
    const AmsError err = resume_once(h, 0);
    CHECK(err.result == AmsResult::BUSY);
    CHECK(h.client.gcode_script_history().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker keeps a failed load across a reconnect",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    h.backend.fire_status(json{{"print_stats", {{"state", "paused"}}}});
    fail_refeed_then_switch_present(h);
    h.backend.before_disconnect();
    SnapmakerTestAccess::forget_print_state(h.backend);

    SECTION("the print is still paused: Resume still retries the load") {
        h.backend.fire_status(json{{"print_stats", {{"state", "paused"}}}});
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
    SECTION("the print ended meanwhile: forgotten") {
        h.backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});
        CHECK(resume_once(h, 0).success());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker reads a failed load off a feeder it just met",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
    SnapmakerTestAccess::set_current_slot(h.backend, 2);
    // A subscription's first frame names every field, the failure included.
    h.backend.fire_status(
        json{{"print_stats", {{"state", "paused"}}},
             {"print_task_config", {{"extruders_used", {true, false, true, false}}}},
             {"filament_feed left",
              {{"extruder0",
                {{"filament_detected", true},
                 {"channel_state", "load_fail"},
                 {"channel_error", "ok"},
                 {"module_exist", true},
                 {"disable_auto", false}}}}}});
    // Resuming on head 2, which reports nothing wrong: the failed head the
    // print uses holds it.
    const AmsError err = resume_once(h, 2);
    CHECK_FALSE(err.success());
    CHECK(err.user_msg.find("Toolhead 1 (T0)") == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker forgets a failed load when the print or link ends",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    h.backend.fire_status(json{{"print_stats", {{"state", "paused"}}}});
    fail_refeed_then_switch_present(h);

    SECTION("print cancelled") {
        h.backend.fire_status(json{{"print_stats", {{"state", "cancelled"}}}});
    }
    SECTION("print complete") {
        h.backend.fire_status(json{{"print_stats", {{"state", "complete"}}}});
    }

    CHECK(resume_once(h, 0).success());
    CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume with the head reading present stops on a feeder failure",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);

    SECTION("loaded: RESUME proceeds with nothing sent") {
        h.backend.fire_status(fed_head0("load_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("an empty feeder's no_filament is no fault") {
        h.backend.fire_status(fed_head0("load_finish", "no_filament"));
        CHECK(resume_once(h, 0).success());
    }
    SECTION("unloaded with filament at the feeder: no failure, RESUME proceeds") {
        h.backend.fire_status(fed_head0("unload_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("loaded by hand: no failure, RESUME proceeds") {
        h.backend.fire_status(fed_head0("manual_sta_finish"));
        CHECK(resume_once(h, 0).success());
    }
    SECTION("a failed load: the load runs first and must confirm") {
        h.backend.fire_status(fed_head0("load_fail"));
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(),
                               "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1") == 1);
    }
    SECTION("load_finish with a live feed fault loads first") {
        h.backend.fire_status(fed_head0("load_finish", "feed_timeout"));
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
    SECTION("no automatic feeder: the sensor answers") {
        h.backend.fire_status(fed_head0("preload_finish"));
        h.backend.fire_status(
            json{{"filament_feed left", {{"extruder0", {{"disable_auto", true}}}}}});
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume after a paused manual load that failed",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
    h.backend.fire_status(json{{"print_stats", {{"state", "paused"}}}});
    h.backend.fire_status(json{
        {"filament_feed left",
         {{"extruder0",
           {{"filament_detected", true},
            {"channel_state", "wait_insert"},
            {"channel_error", "ok"},
            {"module_exist", true},
            {"disable_auto", false}}}}},
        {"filament_motion_sensor e0_filament", {{"enabled", true}, {"filament_detected", true}}}});

    h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                    "extruder[0]: state: none, error: ok!", "AUTO_FEEDING");
    SnapmakerTestAccess::set_running(h.backend, true);
    const AmsError load = h.backend.load_filament(0);
    INFO(load.technical_msg << " / " << load.user_msg);
    REQUIRE(load.success());
    ResumeRecoveryHarness::drain();
    CHECK(SnapmakerTestAccess::load_unconfirmed(h.backend, 0));
    h.client.clear_gcode_script_history();

    CHECK_FALSE(resume_once(h, 0).success());
    CHECK(count_containing(h.client.gcode_script_history(),
                           "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1") == 1);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker records the routing a configured task ran with",
                 "[ams][snapmaker][reprint]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // T0 crosses over to head 2, T2 to head 0 — the file that cannot be
    // reprinted correctly today.
    std::vector<int> crossover = identity_map();
    crossover[0] = 2;
    crossover[2] = 0;

    SECTION("nothing is recorded before a task is configured") {
        // Idle: the firmware reports its default table with no task. This is the
        // reading that must NOT be mistaken for a print's routing — it is
        // indistinguishable from a job that genuinely needs no remap.
        backend.fire_status(make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping().empty());
    }

    SECTION("a configured task is recorded, and survives the task ending") {
        backend.fire_status(make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.get_tool_mapping() == crossover);
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        // Print ends: the firmware clears extruders_used AND resets its table to
        // the default. The live accessor correctly refuses to answer; the
        // snapshot is what the reprint replays.
        backend.fire_status(make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }

    SECTION("a later task replaces the record") {
        backend.fire_status(make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        std::vector<int> other = identity_map();
        other[1] = 3;
        backend.fire_status(make_task_config(other, {false, false, false, true}));
        REQUIRE(backend.last_print_tool_mapping() == other);
    }

    SECTION("a task-configured frame with no table yet records nothing") {
        // extruders_used can land before the table on an incremental update.
        // Snapshotting an empty table would publish "known: nothing", which
        // reprint_remap cannot tell from a real answer.
        json used_only = json{{"print_task_config",
                               json{{"extruders_used", json::array({true, false, false, false})}}}};
        backend.fire_status(used_only);
        REQUIRE(backend.last_print_tool_mapping().empty());

        // The table arriving in a later frame, task still configured, records it.
        backend.fire_status(make_task_config(crossover, {true, false, false, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }
}

// Resume-refusal classification

// The U1 refuses RESUME while a used extruder has no material assigned, and
// says so only on the `!!` broadcast channel: print_stats.exception keeps
// whatever paused the print (a runout), because the refusal is raised
// oneshot. Uncoded `!!` on a paused printer otherwise falls through to
// error_classify::classify(), which offers Resume — the one action the
// firmware is guaranteed to refuse again until the material is set.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker claims the not-edit-filament resume refusal",
                 "[ams][snapmaker][error-center]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    helix::ClassifyContext paused;
    paused.is_paused = true;

    SECTION("claims the refusal as a Snapmaker fault") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->source == helix::ErrorSource::SNAPMAKER);
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->sticky);
    }

    SECTION("names the slot the way the machine labels it") {
        // Firmware counts extruders from 0; every slot the user reads is 1-based
        // (ams_state.cpp display_slot). e2 is the third slot.
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->detail.find('3') != std::string::npos);
        REQUIRE(ev->detail.find("e2") == std::string::npos);
    }

    SECTION("offers no Resume — the firmware refuses it until the material is set") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE_FALSE(ev->recovery_actions.empty());
        for (const auto& action : ev->recovery_actions) {
            REQUIRE(action.gcode.find("RESUME") == std::string::npos);
        }
    }

    SECTION("an overlong extruder index is refused rather than parsed") {
        // This line arrives from the firmware, so its digit run is not ours to
        // trust: past the parse range stoi throws, and an escaping throw here
        // leaves classify_error on the main thread.
        REQUIRE_FALSE(backend.classify_error("!! e99999999999999999999 not edit filament", paused)
                          .has_value());
    }

    SECTION("marks the refusal as the answer to the user's command") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->command_refusal);
    }

    SECTION("keeps the firmware wording for cross-channel dedup") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->raw_detail == "e2 not edit filament");
    }

    SECTION("every extruder index maps to its own slot") {
        struct Case {
            const char* line;
            char slot;
        };
        for (const auto& c :
             {Case{"!! e0 not edit filament", '1'}, Case{"!! e1 not edit filament", '2'},
              Case{"!! e3 not edit filament", '4'}}) {
            auto ev = backend.classify_error(c.line, paused);
            REQUIRE(ev.has_value());
            REQUIRE(ev->detail.find(c.slot) != std::string::npos);
        }
    }

    SECTION("ignores the refusal when nothing is paused or printing") {
        helix::ClassifyContext idle;
        auto ev = backend.classify_error("!! e2 not edit filament", idle);
        REQUIRE_FALSE(ev.has_value());
    }

    SECTION("leaves unrelated errors to the generic classifier") {
        REQUIRE_FALSE(
            backend.classify_error("!! Probe triggered prior to movement", paused).has_value());
        REQUIRE_FALSE(backend.classify_error("// e2 not edit filament", paused).has_value());
    }
}

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "Snapmaker an insert asks about the stored record only when no tag vouches for it",
                 "[ams][snapmaker][1710]") {
    const json empty = make_feed_status(0, "wait_insert", false);
    const json in = make_feed_status(0, "wait_insert", true);
    const json tag = make_filament_detect_status(0, "PLA", 0xFF00AA00u, "Polymaker",
                                                 json::array({144, 32, 196, 2}));
    const json no_tag = make_filament_detect_status(0, "NONE", 0u, "NONE", json::array());
    struct Row {
        const char* name;
        std::vector<json> frames;
        size_t quiet_through; // no notice up to and including this frame
        bool asks;            // exactly one notice by the last frame
    };
    std::vector<Row> rows = {
        // The first port reading is a baseline; presence rises before the
        // reader speaks, and the read's UID vouches for the spool.
        {"a tagged insert", {empty, in, tag}, 2, false},
        // Judging the second edge on the first spool's evidence would stay
        // silent; the reader's own answer for the new spool is what asks.
        {"an untagged insert after a tagged one",
         {empty, in, tag, empty, no_tag, in, no_tag},
         5,
         true},
        // A tool change drives the channel's own unload and load: the firmware
        // moving filament, not a spool change, so no verdict is held even past
        // the pass-count fallback.
        {"a feed-port edge during a firmware feed",
         {empty, make_feed_status(0, "unload_doing", false),
          make_feed_status(0, "load_feeding", true)},
         0,
         false},
        // With no filament_detect.info ever arriving, the verdict falls back
        // after a bounded number of parses.
        {"an insert whose read never lands", {empty, in}, 1, true},
    };
    for (int i = 0; i < 12; ++i)
        rows[2].frames.push_back(make_feed_status(0, "load_finish", true));
    rows[2].quiet_through = rows[2].frames.size() - 1;
    for (int i = 0; i < 20; ++i)
        rows[3].frames.push_back(in);

    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.name) {
            for (size_t i = 0; i < row.frames.size(); ++i) {
                backend.fire_status(row.frames[i]);
                drain();
                if (i <= row.quiet_through) {
                    INFO("frame " << i);
                    CHECK(toasts.empty());
                }
            }
            if (row.asks) {
                REQUIRE(toasts.size() == 1);
                CHECK(toasts[0].first == ToastSeverity::INFO);
            } else {
                CHECK(toasts.empty());
            }
            CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
        }
    }
}

TEST_CASE_METHOD(SnapmakerInsertFixture, "Snapmaker an insert's identity waits on the tag reader",
                 "[ams][snapmaker][1710][entry_sensor]") {
    backend.fire_status(make_feed_status(0, "wait_insert", false));
    CHECK(backend.slot_insert_identity(0) == InsertIdentity::NotRead);
    backend.fire_status(make_feed_status(0, "wait_insert", true));
    REQUIRE(backend.slot_insert_identity(0) == InsertIdentity::Pending);

    SECTION("a tagged spool") {
        backend.fire_status(make_filament_detect_status(0, "PLA", 0xFF00AA00u, "Polymaker",
                                                        json::array({144, 32, 196, 2})));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::FromTag);
    }
    SECTION("an untagged spool") {
        backend.fire_status(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NoTag);
    }
    SECTION("a read that never lands") {
        for (int i = 0; i < 20; ++i)
            backend.fire_status(make_feed_status(0, "wait_insert", true));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NoTag);
    }
    SECTION("the spool leaves before any read") {
        backend.fire_status(make_feed_status(0, "wait_insert", false));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NotRead);
    }
    // Other lanes saw no insert.
    CHECK(backend.slot_insert_identity(1) == InsertIdentity::NotRead);
}

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "A backend without insert signals judges the slot status of a prep sensor lane",
                 "[ams][insert_watcher]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    AmsBackendMock mock(4);
    mock.force_slot_status(1, SlotStatus::EMPTY);
    REQUIRE(judge(watcher, mock) == -1);
    mock.force_slot_status(1, SlotStatus::AVAILABLE);
    CHECK(judge(watcher, mock) == 1);
}

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "Snapmaker an idle tool parking an empty lane is no insert",
                 "[ams][snapmaker][insert_watcher]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    backend.fire_status(make_feed_status(1, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);

    // T1 parks and the task config claims filament: both move the slot
    // status, but no spool went in.
    backend.fire_status(json{{"extruder1", json{{"park_pin", true}, {"active_pin", false}}}});
    CHECK(judge(watcher, backend) == -1);
    backend.fire_status(json{
        {"print_task_config", json{{"filament_exist", json::array({false, true, false, false})}}}});
    CHECK(backend.slot_insert_presence(1).value_or(true) == false);
    CHECK(judge(watcher, backend) == -1);
}

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "Snapmaker an untagged insert opens the editor and nothing else",
                 "[ams][snapmaker][insert_watcher][1710]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    backend.fire_status(make_feed_status(0, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);

    // The port sensor sees the spool; the reader has not answered.
    backend.fire_status(make_feed_status(0, "wait_insert", true));
    CHECK(judge(watcher, backend) == -1);

    // The reader finds no tag: the editor is this insert's one prompt.
    backend.fire_status(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
    drain();
    CHECK(judge(watcher, backend) == 0);
    CHECK(toasts.empty());
}

TEST_CASE_METHOD(SnapmakerInsertFixture, "Snapmaker a tag-identified insert opens nothing",
                 "[ams][snapmaker][insert_watcher][1710]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    backend.fire_status(make_feed_status(0, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);
    backend.fire_status(make_feed_status(0, "wait_insert", true));
    CHECK(judge(watcher, backend) == -1);
    backend.fire_status(make_filament_detect_status(0, "PLA", 0xFF00AA00u, "Polymaker",
                                                    json::array({144, 32, 196, 2})));
    drain();
    CHECK(judge(watcher, backend) == -1);
    CHECK(toasts.empty());
    CHECK_FALSE(helix::ui::insert_prompt_claimed(0));
}

TEST_CASE_METHOD(SnapmakerInsertFixture, "Snapmaker an insert prompt is claimed only by its prompt",
                 "[ams][snapmaker][insert_watcher]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    for (int lane : {0, 1})
        backend.fire_status(make_feed_status(lane, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);
    for (int lane : {0, 1})
        backend.fire_status(make_feed_status(lane, "wait_insert", true));
    REQUIRE(judge(watcher, backend) == -1);
    REQUIRE(helix::ui::insert_prompt_claimed(0));
    REQUIRE(helix::ui::insert_prompt_claimed(1));

    SECTION("a lane the editor does not open for holds no claim") {
        for (int lane : {0, 1})
            backend.fire_status(
                make_filament_detect_status(lane, "NONE", 0u, "NONE", json::array()));
        CHECK(judge(watcher, backend) == 0);
        CHECK(helix::ui::insert_prompt_claimed(0));
        CHECK_FALSE(helix::ui::insert_prompt_claimed(1));
    }
    SECTION("a held insert a guard now blocks gives its claim back") {
        backend.fire_status(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
        FilamentInsertContext ctx;
        ctx.setting_enabled = true;
        ctx.editor_open = true;
        CHECK(watcher.judge_lanes(backend, ctx) == -1);
        CHECK_FALSE(helix::ui::insert_prompt_claimed(0));
    }
    SECTION("a reconnect forgets every claim") {
        watcher.reseed();
        CHECK_FALSE(helix::ui::insert_prompt_claimed(0));
        CHECK_FALSE(helix::ui::insert_prompt_claimed(1));
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker has a toolhead sensor once a head reports its switch",
                 "[ams][snapmaker][entry_sensor]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    CHECK_FALSE(backend.get_system_info().units[0].has_toolhead_sensor);
    backend.fire_status(t3_switch(false));
    CHECK(backend.get_system_info().units[0].has_toolhead_sensor);
}

TEST_CASE_METHOD(SnapmakerGoldenFixture, "Snapmaker golden status sequence",
                 "[ams][snapmaker][golden]") {
    const std::vector<std::pair<std::string, json>> frames = {
        {"full frame", golden_full_frame()},
        // Load on head 1: the carriage picks it, the channel walks to finish.
        {"load: pick", json{{"toolhead", json{{"extruder", "extruder1"}}},
                            {"extruder", golden_extruder("PARKED", true, false)},
                            {"extruder1", golden_extruder("ACTIVE", false, true)}}},
        {"load: feeding", golden_feed(1, "load_feeding")},
        {"load: heating", golden_feed(1, "load_heating")},
        {"load: finish", golden_feed(1, "load_finish")},
        // Unload of head 1.
        {"unload: prepare", golden_feed(1, "unload_prepare")},
        {"unload: finish", golden_feed(1, "unload_finish")},
        // Head 0 loses its spool and a tagless one goes in, then a tagged one in head 3.
        {"insert: port drops", merged({golden_feed(0, "wait_insert", false), t0_switch_off})},
        {"insert: port rises", golden_feed(0, "wait_insert", true)},
        {"insert: reader answers NONE",
         json{{"filament_detect",
               json{{"info",
                     json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                  json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}}},
        {"insert: head 3 rises", golden_feed(3, "wait_insert", true)},
        {"insert: head 3 tag read",
         json{{"filament_detect",
               json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                          json{{"MAIN_TYPE", "NONE"}},
                                          json{{"MAIN_TYPE", "PETG"},
                                               {"SUB_TYPE", "Basic"},
                                               {"VENDOR", "Snapmaker"},
                                               {"ARGB_COLOR", 0xFF445566u},
                                               {"WEIGHT", 500},
                                               {"CARD_UID", json::array({9, 8, 7, 6})}}})}}}}},
        // A two-head load batch whose first head fails.
        {"batch: head 0 mid-load", golden_feed(0, "load_feeding")},
        {"batch: head 0 fails", golden_feed(0, "load_fail")},
        // Temperature-only extruder update on the active tool.
        {"extruder: temperature only", json{{"extruder1", json{{"temperature", 215.5}}}}},
        {"extruder: idle frame", json{{"extruder", json{{"temperature", 25.0}}}}},
    };

    std::vector<std::string> got;
    for (const auto& [name, frame] : frames) {
        if (name == "batch: head 0 mid-load") {
            SnapmakerTestAccess::set_batch_plan(backend, {0, 2}, /*load=*/true, "Load", "of");
        }
        got.push_back(name + "\n" + feed(frame));
    }

    std::string all;
    for (const auto& g : got) {
        all += g + "\n";
    }
    CHECK(all == kGolden);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker extruder delta frames keep the fields they omit",
                 "[ams][snapmaker][extruder][delta]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{{"extruder1", json{{"state", "PARKED"},
                                                {"park_pin", true},
                                                {"active_pin", false},
                                                {"switch_count", 7}}}});
    REQUIRE(SnapmakerTestAccess::extruder_state(backend, 1).park_pin);

    // A temperature-only update names none of the tool-changer fields.
    backend.fire_status(json{{"extruder1", json{{"temperature", 215.5}}}});
    auto held = SnapmakerTestAccess::extruder_state(backend, 1);
    CHECK(held.state == "PARKED");
    CHECK(held.park_pin);
    CHECK(held.switch_count == 7);

    // A frame that does name a field overwrites only that field.
    backend.fire_status(json{{"extruder1", json{{"state", "ACTIVE"}, {"park_pin", false}}}});
    held = SnapmakerTestAccess::extruder_state(backend, 1);
    CHECK(held.state == "ACTIVE");
    CHECK_FALSE(held.park_pin);
    CHECK(held.switch_count == 7);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker reads the mounted head from the heads' states",
                 "[ams][snapmaker][tool_switcher][1784]") {
    const json t2 = json{{"toolhead", json{{"extruder", "extruder2"}}}};
    json no_state_for_2 = u1_all_parked_frame();
    no_state_for_2.erase("extruder2");
    struct Step {
        json frame;
        int tool; // current_tool and current_slot after the frame
    };
    struct Row {
        const char* name;
        std::vector<Step> steps;
    };
    const Row rows[] = {
        {"all heads parked is an empty carriage", {{t2, 2}, {u1_all_parked_frame(), -1}}},
        {"a head with no reported state follows toolhead.extruder", {{no_state_for_2, 2}}},
        {"mounting a head after an empty carriage reads it",
         {{u1_all_parked_frame(), -1},
          {json{{"extruder1", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                {"toolhead", json{{"extruder", "extruder1"}}}},
           1}}},
        // Parking leaves toolhead.extruder alone, so the park is a delta naming
        // only the head's own state.
        {"parking the mounted head empties the carriage",
         {{t2, 2}, {json{{"extruder2", json{{"state", "PARKED"}}}}, -1}}},
        // A head can read PARKED before its pin drops, and the frame dropping
        // the pin repeats no state.
        {"a park split across two frames empties the carriage",
         {{json{{"extruder2", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                {"toolhead", json{{"extruder", "extruder2"}}}},
           2},
          {json{{"extruder2", json{{"state", "PARKED"}, {"active_pin", true}}}}, 2},
          {json{{"extruder2", json{{"active_pin", false}}}}, -1}}},
        {"parking another head leaves the mounted one",
         {{t2, 2}, {json{{"extruder1", json{{"state", "PARKED"}}}}, 2}}},
    };
    for (const auto& row : rows) {
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        for (size_t i = 0; i < row.steps.size(); ++i) {
            INFO(row.name << ", frame " << i);
            backend.fire_status(row.steps[i].frame);
            CHECK(backend.get_system_info().current_tool == row.steps[i].tool);
            CHECK(backend.get_system_info().current_slot == row.steps[i].tool);
        }
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker does not read a tool change in flight as a park",
                 "[ams][snapmaker][tool_switcher][1784]") {
    // The old head docks before the new one reports, so for a frame every head
    // reads PARKED.
    helix::test::RegisteredWired<CapturingSnapmakerBackend> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{{"extruder1", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                             {"toolhead", json{{"extruder", "extruder1"}}}});
    REQUIRE(backend.get_system_info().current_tool == 1);

    SECTION("T<n> sent, nothing has left a dock yet") {
        REQUIRE(backend.change_tool(2).success());
        backend.fire_status(json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == 1);

        backend.fire_status(json{{"extruder2", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                                 {"toolhead", json{{"extruder", "extruder2"}}}});
        CHECK(backend.get_system_info().current_tool == 2);

        // The change landed: parking the new head empties the carriage again.
        backend.fire_status(json{{"extruder2", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == -1);
    }
    SECTION("the new head is in its activating move") {
        backend.fire_status(
            json{{"extruder2", json{{"state", "PARKED"}, {"activating_move", true}}}});
        backend.fire_status(json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == 1);
    }
    SECTION("a park with no change in flight still empties the carriage") {
        backend.fire_status(json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == -1);
    }
}

TEST_CASE("Only a backend whose carriage can be empty says so",
          "[ams][snapmaker][tool_switcher][1784]") {
    AmsBackendSnapmaker snapmaker(nullptr, nullptr);
    CHECK(snapmaker.carriage_can_be_empty());

    AmsBackendAfc afc(nullptr, nullptr);
    CHECK_FALSE(afc.carriage_can_be_empty());
}

// A reconnect starts from no verdict, whether or not a store loads: a frame
// arriving while the load runs must not be judged against the last session.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker forgets channel silence as a start begins",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend;
    helix::ams::FilamentSlotOverride record;
    record.brand = "Snapmaker";
    record.material = "PLA";
    record.spool_name = "SnapSpeed";
    SnapmakerTestAccess::seed_override(*backend, 0, record);

    json info = json::array({json{{"MAIN_TYPE", "NONE"}, {"SUB_TYPE", "NONE"}}});
    backend->fire_status(json{{"filament_detect", json{{"info", info}}}});
    REQUIRE(backend->get_slot_info(0).spool_name == "SnapSpeed");

    SnapmakerTestAccess::call_on_started(*backend);
    backend->fire_status(
        json{{"print_task_config",
              json{{"filament_vendor", json::array({"NONE", "NONE", "NONE", "NONE"})},
                   {"filament_official", json::array({false, false, false, false})}}}});

    CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
}

TEST_CASE("Snapmaker filament config gcode sends a translucent filament's alpha",
          "[ams][snapmaker][filament_record][translucent]") {
    SlotInfo s;
    s.material = "PETG";
    s.color_rgb = 0xD32F2F;
    s.translucent = true;
    CHECK(AmsBackendSnapmaker::build_filament_config_gcode(
              2, s, false, AmsBackendSnapmaker::RecordLines::Colour) ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 FILAMENT_COLOR_RGBA=D32F2F80");
    s.translucent = false;
    CHECK(AmsBackendSnapmaker::build_filament_config_gcode(
              2, s, false, AmsBackendSnapmaker::RecordLines::Colour) ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 FILAMENT_COLOR_RGBA=D32F2FFF");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker print_task_config's alpha makes a lane translucent",
                 "[ams][snapmaker][translucent]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(
        json{{"print_task_config",
              json{{"filament_type", json::array({"PETG", "PLA", "PLA", "PLA"})},
                   {"filament_color_rgba",
                    json::array({"E8E8FF80", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    CHECK(backend.get_slot_info(0).color_rgb == 0xE8E8FFu);
    CHECK(backend.get_slot_info(0).translucent);
    CHECK_FALSE(backend.get_slot_info(1).translucent);
    // The lane's firmware reading states it, for every reader of the lane.
    const auto fw = helix::ams::lane_sources(backend.lane_id(0)).firmware;
    REQUIRE(fw.has_value());
    CHECK(fw->translucent == std::optional<bool>(true));

    // A record written back opaque is opaque again.
    backend.fire_status(
        json{{"print_task_config",
              json{{"filament_color_rgba",
                    json::array({"E8E8FFFF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    CHECK_FALSE(backend.get_slot_info(0).translucent);
}

TEST_CASE("Snapmaker filament config gcode maps vendor and subtype to firmware values",
          "[ams][snapmaker][filament_record]") {
    SECTION("an unknown vendor and a free-form spool name take Generic / Basic") {
        SlotInfo s;
        s.brand = "Inland";
        s.spool_name = "My Custom Spool";
        s.material = "PLA";
        s.color_rgb = 0xFF5500;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic FILAMENT_TYPE=PLA "
              "FILAMENT_SUBTYPE=Basic\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500FF");
    }
    SECTION("a Snapmaker spool keeps its vendor and a product line with a space is quoted") {
        SlotInfo s;
        s.brand = "snapmaker";
        s.spool_name = "95A HF";
        s.material = "TPU";
        s.color_rgb = 0x00000A;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(3, s) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Snapmaker FILAMENT_TYPE=TPU "
              "FILAMENT_SUBTYPE=\"95A HF\" SOFT=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 FILAMENT_COLOR_RGBA=00000AFF");
    }
    SECTION("a soft material the firmware's table cannot name is written SOFT=1") {
        // The firmware looks soft up by the exact FILAMENT_TYPE; TPU-85A has no
        // row there, so without SOFT=1 it would store the head as rigid.
        SlotInfo s;
        s.material = "TPU-85A";
        s.color_rgb = 0x1A1A1A;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s, true) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic FILAMENT_TYPE=TPU-85A "
              "FILAMENT_SUBTYPE=Basic SOFT=1 FORCE=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=1A1A1AFF FORCE=1");
        for (const char* soft : {"TPE", "PEBA", "PVA", "tpu 95a"}) {
            s.material = soft;
            CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s).find(" SOFT=1") !=
                  std::string::npos);
        }
        for (const char* rigid : {"PLA", "PETG", "PA-CF", "TPUX"}) {
            s.material = rigid;
            CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s).find("SOFT") ==
                  std::string::npos);
        }
    }
    SECTION("a material with a space is quoted; one the allowlist refuses writes nothing") {
        SlotInfo s;
        s.material = "Silk PLA";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).find(
                  "FILAMENT_TYPE=\"Silk PLA\" ") != std::string::npos);
        s.material = "PLA\"; M112";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
    }
    SECTION("force appends FORCE=1 to both lines") {
        SlotInfo s;
        s.material = "PLA";
        s.color_rgb = 0x000000;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(2, s, true) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic FILAMENT_TYPE=PLA "
              "FILAMENT_SUBTYPE=Basic FORCE=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 FILAMENT_COLOR_RGBA=000000FF FORCE=1");
    }
    SECTION("no material, or none left after cleaning, writes nothing") {
        SlotInfo s;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
        s.material = "NONE";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
        s.material = " ;# ";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a material edit on stock firmware writes the head's filament record",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_stock");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic");
    CHECK(writes[1] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500FF");
    CHECK(writes[0].find("FORCE") == std::string::npos);
    // Nothing goes to the Extended Firmware's endpoint, which would flag the
    // head official.
    CHECK(h.printer.api.rest_mock().mock_get_post_history().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a material change over a different firmware type rewrites the record",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_changed");
    h.frame(make_task_filament({"PETG", "PETG", "PETG", "PETG"}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 2, inland_pla()).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic "
                          "FILAMENT_TYPE=PLA",
                          0) == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a save sends only the record lines that differ",
                 "[ams][snapmaker][filament_record]") {
    // SET_PRINT_FILAMENT_CONFIG runs FLOW_RESET_K, so a line the record
    // already agrees with must not be sent.
    lv_init_safe();
    FilamentRecordHarness h("record_write_lines");
    json ptc = make_task_filament({"NONE", "pla", "NONE", "NONE"});
    ptc["print_task_config"]["filament_vendor"] = json::array({"NONE", "Generic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] = json::array({"NONE", "Basic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FF5500FF", "FFFFFFFF", "FFFFFFFF"});
    h.frame(ptc);
    h.printer.client.clear_gcode_script_history();

    SECTION("a re-save of what the record holds sends nothing") {
        REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().empty());
    }
    SECTION("a colour change sends the colour line alone") {
        SlotInfo edit = inland_pla();
        edit.color_rgb = 0x123456;
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=123456FF");
    }
    SECTION("a translucency change alone sends the colour line alone") {
        SlotInfo edit = inland_pla();
        edit.translucent = true;
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] ==
              fmt::format(
                  "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500{:02X}",
                  helix::filament_alpha(true)));
    }
    SECTION("a product-line change sends the identity line alone") {
        SlotInfo edit = inland_pla();
        edit.brand = "Snapmaker";
        edit.spool_name = "Matte";
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Snapmaker "
                           "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Matte");
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unseen firmware record is written only for a declared material",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_unseen");
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 0, inland_pla()).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().size() == 2);

    // The same material again, colour moved: the user declared no material.
    h.printer.client.clear_gcode_script_history();
    SlotInfo recolour = h.backend.get_slot_info(0);
    recolour.color_rgb = 0x00FF00;
    REQUIRE(helix::test::apply_edit(h.backend, 0, recolour).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

TEST_CASE("Snapmaker CARD_UID shapes decide whether a real card is present",
          "[ams][snapmaker][filament_record]") {
    const auto card = [](const json& uid) {
        return helix::snapmaker::parse_rfid_info(json{{"MAIN_TYPE", "NONE"}, {"CARD_UID", uid}})
            .has_card;
    };
    CHECK(card(0) == std::optional<bool>(false));
    CHECK(card(2953842434LL) == std::optional<bool>(true));
    CHECK(card(json::array({0, 0, 0, 0})) == std::optional<bool>(false));
    CHECK(card(json::array({144, 32, 196, 2})) == std::optional<bool>(true));
    CHECK_FALSE(helix::snapmaker::parse_rfid_info(json{{"MAIN_TYPE", "NONE"}}).has_card);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker record writes against a live U1 rig",
                 "[ams][snapmaker][filament_record]") {
    // The rig's frames: reader off, every CARD_UID a scalar 0. Head 1 is
    // official from an endpoint write; head 3 is official in print_task_config
    // with no card behind it.
    lv_init_safe();
    FilamentRecordHarness h("record_write_live_frame");
    h.frame(make_rig_detect({0, 0, 0, 0}, {false, true, false, false}));
    h.frame(make_task_filament({"NONE", "NONE", "ASA", "ASA"}, {false, true, false, true},
                               std::vector<bool>{false, false, false, false}));
    h.printer.client.clear_gcode_script_history();

    SlotInfo asa = inland_pla();
    asa.material = "ASA";
    // Head 2 already records ASA: nothing to write.
    REQUIRE(helix::test::apply_edit(h.backend, 2, asa).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    // Head 0, not official: a plain write.
    REQUIRE(helix::test::apply_edit(h.backend, 0, asa).success());
    FilamentRecordHarness::drain();
    auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic "
                       "FILAMENT_TYPE=ASA FILAMENT_SUBTYPE=Basic");

    // Head 3, official with CARD_UID 0: forced.
    h.printer.client.clear_gcode_script_history();
    REQUIRE(helix::test::apply_edit(h.backend, 3, inland_pla()).success());
    FilamentRecordHarness::drain();
    writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");

    // A real card on head 3 (a nonzero scalar UID) is the tag's to keep.
    h.frame(make_rig_detect({0, 0, 0, 2953842434LL}, {false, true, false, true}));
    h.printer.client.clear_gcode_script_history();
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(helix::test::apply_edit(h.backend, 3, petg).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an official RFID head's record is never written or forced",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_official");
    h.frame(
        make_rig_detect({0, json::array({144, 32, 196, 2}), 0, 0}, {false, true, false, false}));
    h.frame(make_task_filament({"PETG", "PETG", "PETG", "PETG"}, {false, true, false, false}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume after a runout refeeds and repairs nothing",
                 "[ams][snapmaker][filament_record][resume]") {
    // After a runout the user may have loaded a different spool, so the
    // material remembered for the old one is not recorded against it.
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_runout");
    helix::test::edit_slot_as_user(h.backend, 1, inland_pla());
    h.frame(make_task_filament({"NONE", "", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    SnapmakerTestAccess::set_sensor_present(h.backend, 1, false);
    h.printer.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError&) { fired = true; });
    FilamentRecordHarness::drain();
    SnapmakerTestAccess::expire_refeed_wait(h.backend); // no feeder frame follows here
    FilamentRecordHarness::drain();

    const auto hist = h.history();
    CHECK(index_of_containing(hist, "AUTO_FEEDING EXTRUDER=1 LOAD=1") < hist.size());
    CHECK(h.config_writes().empty());
    CHECK(fired);
}
