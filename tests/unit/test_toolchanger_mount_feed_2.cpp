// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 54 to 92 of test_toolchanger_mount_feed.cpp before it was split, in its order. Mostly
// [medusahc], [tool-number], [ams], [general], [medusahc][batch], and more

#include "ui_ams_device_operations_overlay.h"
#include "ui_batch_filament_modal.h"

#include "../busy_guard_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/step_operation_helpers.h"
#include "../test_helpers/toolchanger_mount_feed_fixtures.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../test_helpers/zero_lane_numbering.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cleanup_hold.h"
#include "config.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "filament_op_slot_resolver.h"
#include "macro_param_cache.h"
#include "mount_feed_sequence.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "toolchanger_addon.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace mf = helix::mount_feed;
using helix::test::ToolChangerHelper;
using json = nlohmann::json;

using helix::test::AckingChanger;
using helix::test::ConnectionRecordingBackend;
using helix::test::gcodes;
using helix::test::irbis_medusahc_discovery;
using helix::test::LiveApiChanger;
using helix::test::load_request;
using helix::test::multi_load_request;
using helix::test::pin_watch_frame;
using helix::test::pin_watch_medusahc_discovery;
using helix::test::PrintableChanger;
using helix::test::RestorePurgeSetting;
using helix::test::ScopedGcodeToolStyle;
using helix::test::start_load_to_mount;
using helix::test::start_printing;

TEST_CASE("mount-to-feed: an abort that cannot trust the carriage leaves it alone",
          "[toolchanger][mount_feed]") {
    mf::Run run(load_request());
    run.stage_done(); // the prompt is up
    run.operator_reply(mf::Reply::Continue);
    REQUIRE(run.current_stage() != nullptr);
    CHECK(run.current_stage()->swaps); // the mount
    run.abort(/*restore_carriage=*/false);
    CHECK(run.current_gcode() ==
          std::optional<std::string>("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0"));
    REQUIRE(run.current_stage() != nullptr);
    CHECK_FALSE(run.current_stage()->swaps);

    mf::Run restoring(load_request());
    restoring.stage_done();
    restoring.operator_reply(mf::Reply::Continue);
    restoring.abort();
    REQUIRE(restoring.current_stage() != nullptr);
    CHECK(restoring.current_stage()->swaps); // its cleanup puts tool 0 back
}

TEST_CASE("A load prompt numbers tools as the screen does when slots count from 0",
          "[toolchanger][mount_feed][zero-numbering]") {
    ScopedZeroLaneNumbering zero;
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({0, 2}).success());
    CHECK(tc.sent_.back().find("Insert filament into Tool 0 as far as it goes, and into the hot "
                               "end of Tool 2 while it is docked") != std::string::npos);
}

TEST_CASE("MedusaHC words a mixed load's prompt for the mounted tool and the docked ones",
          "[toolchanger][mount_feed][medusahc][batch]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({0, 2}).success());
    CHECK(tc.sent_.back().find("action:prompt_text Insert filament into Tool 1 as far as it "
                               "goes, and into the hot end of Tool 3 while it is docked, then "
                               "tap Continue") != std::string::npos);
}

// ============================================================================
// A carriage nothing can vouch for is never restored
// ============================================================================

TEST_CASE("MedusaHC: a Mount stage that fails sends no restore swap",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(-2)); // the swap stopped partway
    tc.fail();                    // the machine's T1 reported `!!`
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
          tc.sent_.end());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load stopped. The carriage was not put back: check which tool is mounted.");
}

TEST_CASE("MedusaHC: a Cancel mid-swap waits for the swap, then stops without a restore",
          "[toolchanger][mount_feed][medusahc]") {
    const std::string mount = mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"";
    const std::string toast = "Load stopped. The tool change under way finished, and Tool 1 "
                              "was not put back on the carriage.";
    SECTION("acked, waiting on the frame that confirms it") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        tc.ack();
        REQUIRE(tc.cancel().success());
        // The run stays busy, with nothing sent, while the carriage still moves.
        CHECK(tc.sent_.back() == mount);
        CHECK(tc.get_current_action() == AmsAction::SELECTING); // still the Mount
        CHECK_FALSE(tc.can_cancel_operation());
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());

        tc.feed(pin_watch_frame(1)); // the swap settled
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
              tc.sent_.end());
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == toast);
        CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) ==
              helix::NoticeSeverity::WARNING);
        tc.ack();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
    }
    SECTION("the swap's own ack still to come") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        REQUIRE(tc.cancel().success());
        CHECK(tc.sent_.back() == mount);
        tc.ack();
        CHECK(tc.sent_.back() == mount);
        tc.feed(pin_watch_frame(1));
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) == toast);
    }
    SECTION("a swap that never settles stops on the sensors' fault") {
        AckingChanger tc(4);
        start_load_to_mount(tc);
        tc.ack();
        REQUIRE(tc.cancel().success());
        ToolChangerTestAccess::feed_swap_check_elapsed(tc);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Load stopped. Dock sensors cannot tell which tool is mounted.");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "MedusaHC's step rows name each tool as the screen numbers it",
                 "[toolchanger][mount_feed][medusahc][batch][tool_style]") {
    ScopedGcodeToolStyle gcode;
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    REQUIRE(tc.display_tool_number_style() == helix::ui::ToolNumberStyle::Gcode);

    const auto model = tc.get_operation_step_model(StepOperationType::LOAD_FRESH);
    REQUIRE(model.steps.size() == 8);
    // Head 1 owns the first four rows and head 3 the last four. Each names its
    // tool the way the screen numbers tools, beside its place in the run.
    for (size_t row = 0; row < 4; ++row) {
        CHECK(model.steps[row].label.rfind("T1 (1/2): ", 0) == 0);
        CHECK(model.steps[row + 4].label.rfind("T3 (2/2): ", 0) == 0);
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "MedusaHC's toast for a swap that finished names the tool as the screen numbers it",
    "[toolchanger][mount_feed][medusahc][tool_style]") {
    ScopedGcodeToolStyle gcode;
    AckingChanger tc(4);
    start_load_to_mount(tc);
    REQUIRE(tc.display_tool_number_style() == helix::ui::ToolNumberStyle::Gcode);
    REQUIRE(tc.cancel().success());
    tc.ack();
    tc.feed(pin_watch_frame(1)); // the swap settled with tool 1 on the carriage

    // Tool 0 is the one the run owed the carriage.
    const auto toast = ToolChangerTestAccess::feed_last_toast(tc);
    CHECK(toast.find("and T0 was not put back on the carriage.") != std::string::npos);
    CHECK(toast.find("Tool 1") == std::string::npos);
}

TEST_CASE("MedusaHC: losing the connection mid-Mount sends no restore",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    ToolChangerTestAccess::feed_disconnect_stopping(tc);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0");
    CHECK(std::find(tc.sent_.begin(), tc.sent_.end(), "SELECT_TOOL TOOL=\"tool T0\"") ==
          tc.sent_.end());
}

TEST_CASE("MedusaHC: a Cancel after a confirmed Mount still restores the carriage",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    start_load_to_mount(tc);
    tc.feed(pin_watch_frame(1));
    tc.ack(); // mounted and confirmed; heating
    REQUIRE(tc.cancel().success());
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SELECT_TOOL TOOL=\"tool T0\"");
    // The carriage goes back, and the operator's own Cancel needs no toast.
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("MedusaHC: a dock sensor jostled while the operator inserts is no fault",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    const size_t sent_before = tc.sent_.size();
    tc.feed(pin_watch_frame(-2));
    tc.feed(pin_watch_frame(0));
    CHECK(tc.sent_.size() == sent_before);
    CHECK(tc.can_cancel_operation());
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
}

TEST_CASE("MedusaHC: a latch left over from before a restart does not fault a later run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    // The first full frame after a restart: the flag still latched, T2 seated.
    tc.feed(
        json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}, {"sensor_error", true}}}});
    REQUIRE(tc.get_system_info().action != AmsAction::ERROR);

    // T2 is already mounted, so the run sends no MHC operation that would
    // clear the flag: nothing but the heat and the retract.
    REQUIRE(tc.unload_filament(2).success());
    tc.ack();
    tc.ack();
    REQUIRE(tc.get_current_action() != AmsAction::UNLOADING);
    CHECK(tc.get_system_info().action != AmsAction::ERROR);
    CHECK(tc.unload_blocked_reason(2).empty());
}

TEST_CASE("MedusaHC: a fault the controller reports at the prompt still stops the run",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensor_error", false}}}});
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    tc.feed(json{{"medusahc", {{"sensor_error", true}}}});
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
}

TEST_CASE("MedusaHC: a sensor fault left over from before a run does not stop the next one",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    ToolChangerTestAccess::set_feed_sensor_fault(tc);
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    tc.feed(json{{"medusahc", {{"operation", "idle"}}}});
    CHECK(tc.can_cancel_operation());
    CHECK(tc.sent_.back().find("action:prompt_show") != std::string::npos);
}

TEST_CASE_METHOD(BusyGuardFixture,
                 "MedusaHC's run is answered while busy even when each stage homes first",
                 "[toolchanger][mount_feed][medusahc]") {
    // A printer that lost homing to idle_timeout sends G28 ahead of every stage,
    // and the stage itself goes out from the G28's answer. The busy-gate bypass
    // has to survive that hop, or a heater-only cleanup is dropped again.
    LiveApiChanger tc(api.get(), &mock_client);
    tc.homed = false;
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    begin_blocking_episode();
    const auto drain = []() {
        for (int i = 0; i < 6; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };

    REQUIRE(tc.load_filament(2).success()); // already mounted: nothing to restore
    drain();                                // homed, then the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.cancel().success()); // stopped while homing for the heat
    drain();                        // homed, then the heater-only cleanup
    const auto& history = mock_client.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    REQUIRE(history.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0");
    CHECK(std::count(history.begin(), history.end(), "G28") >= 3);
    // The cleanup's answer ended the run.
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(tc.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("MedusaHC: a frame silent about the carriage leaves a standing sensor fault",
          "[toolchanger][medusahc]") {
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
    tc.feed(json{{"medusahc", {{"current_tool", -2}}}}); // no swap running: a fault
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);

    // Moonraker republishes only what changed: a docks-only frame is not the
    // sensors agreeing again.
    tc.feed(json{{"medusahc", {{"sensors", {{"t0", 1}, {"t2", 1}}}}}});
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
    CHECK(tc.get_system_info().operation_detail == "Sensor Error");
    CHECK_FALSE(tc.unload_blocked_reason(1).empty());

    tc.feed(json{{"medusahc", {{"current_tool", 1}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_system_info().operation_detail.empty());
    CHECK(tc.unload_blocked_reason(1).empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "MedusaHC reads the saved and clamped purge length when it discovers the printer",
                 "[toolchanger][mount_feed][medusahc][purge]") {
    RestorePurgeSetting restore;
    Config* config = Config::get_instance();
    const std::string key = config->df() + helix::wizard::kMountFeedPurgeMm;
    // What a restart does: SettingsManager rebuilt from the saved config.
    const auto reload = [&](int stored) {
        config->set<int>(key, stored);
        helix::SettingsManager::instance().deinit_subjects();
        helix::SettingsManager::instance().init_subjects();
        return helix::SettingsManager::instance().get_mount_feed_purge_mm();
    };
    CHECK(reload(500) == mf::kMaxPurgeMm);
    CHECK(reload(-5) == 0);
    REQUIRE(reload(120) == 120);

    AckingChanger tc(4);
    tc.set_discovery(irbis_medusahc_discovery());
    REQUIRE(tc.max_heads_per_filament_op() == 4); // set_discovery kept mount-to-feed
    const auto actions = tc.get_device_actions();
    const auto slider = std::find_if(actions.begin(), actions.end(),
                                     [](const auto& a) { return a.id == "purge_length"; });
    REQUIRE(slider != actions.end());
    const auto* value = std::any_cast<float>(&slider->current_value);
    REQUIRE(value != nullptr);
    // The slider shows the value the next load sends: the same member feeds
    // both (the setting case above follows it into the gcode).
    CHECK(*value == 120.0f);
}

TEST_CASE("MedusaHC tells a failed swap stage once, not again when the frame repeats it",
          "[toolchanger][mount_feed][medusahc][last_error]") {
    const std::string failure = "MHC_SET: sensors did not confirm T1";
    AckingChanger tc(4);
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(irbis_medusahc_discovery()));
    tc.feed(json{{"medusahc", {{"current_tool", 0}, {"sensor_error", false}}}});
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.get_current_action() == AmsAction::SELECTING); // the Mount stage

    // The controller refuses the pick; its command_error reaches us first.
    tc.fail(failure);
    const std::string run_toast = ToolChangerTestAccess::feed_last_toast(tc);
    REQUIRE_FALSE(run_toast.empty());
    for (int i = 0; i < 4 && tc.get_current_action() != AmsAction::IDLE; ++i) {
        tc.ack(); // the cleanup
    }
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);

    // Then the status frame latching the same failure, the pick never having
    // left the dock.
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", 0},
                   {"sensor_error", true},
                   {"last_error", failure}}}});
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) == run_toast);
}

// ============================================================================
// The run machinery: the wait's clock, a lost printer, a print, a missing
// [respond], the step bar's labels
// ============================================================================

TEST_CASE("MedusaHC's prompt waits its whole ceiling under a watchdog ticking each second",
          "[toolchanger][mount_feed][medusahc]") {
    using namespace std::chrono_literals;
    AckingChanger tc(4);
    std::chrono::steady_clock::time_point now{};
    ToolChangerTestAccess::set_feed_clock(tc, [&now]() { return now; });
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack(); // the prompt is up; the watchdog starts counting here

    for (int i = 0; i < 30; ++i) {
        now += 1s;
        ToolChangerTestAccess::feed_wait_tick(tc);
    }
    CHECK(tc.get_system_info().is_busy());
    CHECK(tc.sent_.size() == 1);

    // Still waiting one second short of the ceiling.
    const auto ceiling = std::chrono::milliseconds(mf::kPromptTimeoutMs);
    while (now < std::chrono::steady_clock::time_point{} + ceiling - 1s) {
        now += 1s;
        ToolChangerTestAccess::feed_wait_tick(tc);
    }
    REQUIRE(tc.sent_.size() == 1);
    now += 1s;
    ToolChangerTestAccess::feed_wait_tick(tc);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
}

TEST_CASE("MedusaHC ends a run on a Klipper restart and turns its heaters off once ready",
          "[toolchanger][mount_feed][medusahc]") {
    SECTION("mid-heat, the restart reported only by klippy_state") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; the heat is in flight
        const size_t before = tc.sent_.size();

        tc.on_klipper_restarted();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(tc.can_cancel_operation());
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
        CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
              "Load stopped. The connection to Klipper was lost. The carriage was not put back: "
              "check which tool is mounted.");

        // A frame later the run stays over: nothing republishes its action.
        tc.feed_status("ready", 0);
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(tc.sent_.size() == before + 1);
    }
    SECTION("the stage failed with Klippy's disconnect first") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; the heat is in flight
        const size_t before = tc.sent_.size();
        tc.lose();
        CHECK(tc.sent_.size() == before); // nothing goes out to a Klippy that is gone
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        tc.on_klipper_restarted();
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    }
    SECTION("at the prompt nothing heated, so nothing is owed") {
        AckingChanger tc(4);
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack(); // the prompt is up
        tc.on_klipper_restarted();
        CHECK(tc.get_current_action() == AmsAction::IDLE);
        CHECK(tc.sent_.size() == 1);
    }
}

TEST_CASE("MedusaHC: a dropped connection ends the run and turns its heaters off on return",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament_batch({1, 3}).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // tool 1 mounted, tool 3 preheating; the heat is in flight
    const size_t before = tc.sent_.size();

    tc.lose(); // the socket closed under the stage
    CHECK(tc.sent_.size() == before);
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Load all stopped. The connection to Klipper was lost. The carriage was not put back: "
          "check which tool is mounted.");
    tc.on_connection_lost(); // the connection edge arrives after the failed stage
    CHECK(tc.sent_.size() == before);

    tc.feed_status("ready", 1); // reconnected, but no word yet on a print
    CHECK(tc.sent_.size() == before);
    tc.feed(json{{"print_stats", {{"state", "standby"}}}}); // the reconnect's snapshot
    REQUIRE(tc.sent_.size() == before + 1);
    CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0\n"
                             "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    tc.feed_status("ready", 1);
    CHECK(tc.sent_.size() == before + 1);
}

TEST_CASE("MedusaHC: a heaters-off dropped by the connection again stays owed",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.feed_status("ready", 0);
    const std::string off = "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0";
    REQUIRE(tc.sent_.back() == off);
    tc.lose(); // that send dropped too
    tc.feed_status("ready", 0);
    CHECK(std::count(tc.sent_.begin(), tc.sent_.end(), off) == 2);
}

TEST_CASE("MedusaHC: a print that takes over owns the heaters a lost run owed",
          "[toolchanger][mount_feed][medusahc]") {
    PrintableChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    const size_t before = tc.sent_.size();

    tc.printing = true; // the reconnect finds a print running
    tc.feed_status("ready", 0);
    CHECK(tc.sent_.size() == before);

    CHECK(ToolChangerTestAccess::feed_last_toast(tc).find("4") != std::string::npos);

    tc.printing = false; // the print ended: the debt went with it
    tc.feed_status("ready", 0);
    CHECK(tc.sent_.size() == before);
}

TEST_CASE("MedusaHC: a reconnect sends owed heaters-off only once the print state is known",
          "[toolchanger][mount_feed][medusahc]") {
    const auto lose_heating_run = [](PrintableChanger& tc) {
        tc.feed_status("ready", 0);
        REQUIRE(tc.load_filament(3).success());
        tc.ack();
        ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
        tc.ack(); // mounted; heating
        tc.lose();
        tc.on_connection_lost();
    };
    SECTION("the snapshot names a print PrinterState has not applied yet") {
        PrintableChanger tc(4);
        lose_heating_run(tc);
        const size_t before = tc.sent_.size();
        tc.feed_status("ready", 0); // the toolchanger's frame lands first
        CHECK(tc.sent_.size() == before);
        // PrinterState still reads idle (printing == false); the frame says otherwise.
        tc.feed(json{{"print_stats", {{"state", "printing"}}}});
        CHECK(tc.sent_.size() == before);
        CHECK(ToolChangerTestAccess::feed_last_toast(tc).find("4") != std::string::npos);
        tc.feed(json{{"print_stats", {{"state", "complete"}}}});
        CHECK(tc.sent_.size() == before); // the debt went with the print
    }
    SECTION("the snapshot says no print") {
        PrintableChanger tc(4);
        lose_heating_run(tc);
        const size_t before = tc.sent_.size();
        tc.feed(json{{"print_stats", {{"state", "standby"}}}});
        REQUIRE(tc.sent_.size() == before + 1);
        CHECK(tc.sent_.back() == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    }
}

TEST_CASE("MedusaHC: a heaters-off dropped late keeps what a later run came to owe",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.feed_status("ready", 0); // the owed heaters-off goes out
    const size_t first_off = tc.sent_.size() - 1;
    REQUIRE(tc.sent_[first_off] == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");

    // A second run starts and is lost before the first heaters-off answers.
    REQUIRE(tc.load_filament(1).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    tc.lose();
    tc.lose_at(first_off); // the first heaters-off dropped with that connection too

    tc.feed_status("ready", 0);
    const std::string& off = tc.sent_.back();
    CHECK(off.find("HEATER=extruder1 TARGET=0") != std::string::npos);
    CHECK(off.find("HEATER=extruder3 TARGET=0") != std::string::npos);
}

TEST_CASE("MedusaHC: a cleanup that only closes the prompt never claims the heaters failed",
          "[toolchanger][mount_feed][medusahc]") {
    // A printer without [respond] refuses every RESPOND line: the prompt
    // stage fails, and so does the cleanup closing it.
    struct NoRespondChanger : AckingChanger {
        explicit NoRespondChanger(int tools) : AckingChanger(tools) {}
        AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                               helix::SharedCallback<void(const MoonrakerError&)> on_error,
                               bool silent) override {
            const bool refused = gcode.find("RESPOND") != std::string::npos;
            auto error = on_error;
            AmsError sent = AckingChanger::execute_gcode(gcode, std::move(on_complete),
                                                         std::move(on_error), silent);
            if (refused && error) {
                MoonrakerError err;
                err.message = "Unknown command:\"RESPOND\"";
                error(err); // deferred: the test drains it
            }
            return sent;
        }
    };
    NoRespondChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::feed_last_toast(tc).empty());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc).find("eater") == std::string::npos);
    CHECK(tc.sent_.back() == mf::prompt_end_gcode());
}

TEST_CASE_METHOD(BusyGuardFixture, "MedusaHC: a print starting under a run ends it, unsent",
                 "[toolchanger][mount_feed][medusahc]") {
    LiveApiChanger tc(api.get(), &mock_client);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});
    const auto drain = []() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    };
    REQUIRE(tc.unload_filament_batch({1, 2, 3}).success());
    const auto& history = mock_client.gcode_script_history();
    REQUIRE_FALSE(history.empty());
    // The first stage, the Mount with the next tool's preheat, went out.
    REQUIRE(history.back() == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=220");
    const size_t sent = history.size();

    start_printing(*this);
    drain(); // the Mount's ack arrives with the print running
    CHECK(history.size() == sent);
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK(ToolChangerTestAccess::feed_last_toast(tc) ==
          "Unload all stopped. A print started. The carriage was not put back: check which "
          "tool is mounted. Tool 3's nozzle is still heating.");
}

TEST_CASE_METHOD(BusyGuardFixture,
                 "MedusaHC: a run stopped under a print still names the nozzle it left heating",
                 "[toolchanger][mount_feed][medusahc]") {
    // The cleanup cannot go out under a print, so nothing turns the run's
    // heaters off. A run that had already stopped (here by Cancel) gets no
    // "print started" accounting of its own; what is new is the heat.
    LiveApiChanger tc(api.get(), &mock_client);
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});
    const auto drain = []() { helix::ui::UpdateQueue::instance().drain(); };
    const auto sent_text = [this]() {
        std::string all;
        for (const auto& script : mock_client.gcode_script_history()) {
            all += script + "\n";
        }
        return all;
    };

    REQUIRE(tc.unload_filament_batch({1, 2}).success());
    // Tool 2's preheat rides tool 1's Mount stage. Once that stage is answered
    // the heater is the run's to turn off, and tool 1's own heat stage is in
    // flight.
    bool tool1_heating_sent = false;
    for (int pass = 0; pass < 12 && !tool1_heating_sent; ++pass) {
        drain();
        tool1_heating_sent = sent_text().find("HEATER=extruder1 TARGET=") != std::string::npos;
    }
    REQUIRE(tool1_heating_sent);
    REQUIRE(sent_text().find("HEATER=extruder2 TARGET=") != std::string::npos);
    REQUIRE(sent_text().find("HEATER=extruder2 TARGET=0") == std::string::npos);
    REQUIRE(tc.can_cancel_operation());

    start_printing(*this);
    const size_t sent = mock_client.gcode_script_history().size();
    REQUIRE(tc.cancel().success());

    // The Cancel's own accounting is replaced by the one thing that is new:
    // extruder2 was left at its target, and nothing is going to turn it off.
    const auto toast = ToolChangerTestAccess::feed_last_toast(tc);
    CHECK(toast.find("Tool 3's nozzle is still heating") != std::string::npos);
    CHECK(ToolChangerTestAccess::feed_last_toast_severity(tc) == helix::NoticeSeverity::WARNING);
    // Nothing went out to turn the heater off or to put the carriage back.
    CHECK(mock_client.gcode_script_history().size() == sent);
    CHECK_FALSE(tc.can_cancel_operation());

    // The stage in flight still answers; the run it belonged to is over, so
    // its answer sends nothing.
    drain();
    drain();
    CHECK(mock_client.gcode_script_history().size() == sent);
}

TEST_CASE_METHOD(LVGLTestFixture, "Every backend hears the connection drop, once per drop",
                 "[toolchanger][mount_feed][ams]") {
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);
    const auto set_connection = [](ConnectionState state) {
        get_printer_state().set_printer_connection_state(static_cast<int>(state), "");
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    set_connection(ConnectionState::CONNECTED);
    auto owned = std::make_unique<ConnectionRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::DISCONNECTED); // still down: no second drop
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::CONNECTED);
    CHECK(backend->lost == 1);
    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 2);

    AmsState::instance().set_backend(nullptr);
    set_connection(ConnectionState::CONNECTED);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A print start waits only for a filament operation this screen runs",
                 "[toolchanger][mount_feed][ams]") {
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<RunningOpBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    // Another client's swap: the printer queues a print behind it itself.
    CHECK_FALSE(AmsState::instance().any_operation_running_here());
    backend->running = StepOperationType::LOAD_FRESH;
    backend->action = AmsAction::HEATING;
    CHECK(AmsState::instance().any_operation_running_here());

    AmsState::instance().set_backend(nullptr);
}

TEST_CASE("MedusaHC offers Cancel exactly while cancel() can act",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK_FALSE(tc.can_cancel_operation());
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack(); // mounted; heating
    CHECK(tc.can_cancel_operation());
    REQUIRE(tc.cancel().success());
    // The cleanup is in flight: a second Cancel has nothing left to stop.
    CHECK_FALSE(tc.can_cancel_operation());
    CHECK_FALSE(tc.cancel().success());
}

TEST_CASE_METHOD(LVGLTestFixture, "Abort says Aborting before the backend reports its own result",
                 "[toolchanger][mount_feed][ams]") {
    struct ReportingBackend : RunningOpBackend {
        std::vector<std::string>* infos = nullptr;
        size_t infos_at_cancel = 0;
        AmsError cancel() override {
            infos_at_cancel = infos->size();
            return AmsErrorHelper::success();
        }
    };
    std::vector<std::string> infos;
    helix::ui::set_test_notification_info_hook(
        [&infos](const std::string& m) { infos.push_back(m); });
    AmsState::instance().init_subjects(true);
    auto owned = std::make_unique<ReportingBackend>();
    owned->infos = &infos;
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    helix::ui::abort_ams_operation();
    CHECK(backend->infos_at_cancel == 1);
    REQUIRE(infos.size() == 1);
    CHECK(infos.front() == "Aborting...");

    AmsState::instance().set_backend(nullptr);
    helix::ui::set_test_notification_info_hook(nullptr);
}

TEST_CASE("MedusaHC remembers which docked hot ends a completed load fed",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    CHECK_FALSE(tc.head_has_filament(2).has_value()); // nothing senses it
    REQUIRE(tc.load_filament(2).success());
    tc.ack(); // the prompt
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack();                                         // mounted
    tc.ack();                                         // heated
    tc.ack();                                         // purged
    CHECK_FALSE(tc.head_has_filament(2).has_value()); // not done until restored
    tc.ack();                                         // tool 0 back on the carriage
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);
    CHECK(tc.head_has_filament(2) == std::optional<bool>(true));
    CHECK_FALSE(tc.head_has_filament(1).has_value());
    // The Unload picker reads the docked tool as loaded, not ready to load.
    CHECK(helix::ui::BatchFilamentModal::row_at_toolhead(tc, 2, 2));
    CHECK_FALSE(helix::ui::BatchFilamentModal::row_at_toolhead(tc, 1, 1));

    REQUIRE(tc.unload_filament(2).success());
    tc.ack(); // mounted
    tc.ack(); // heated
    tc.ack(); // retracted
    tc.ack(); // restored
    REQUIRE(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.head_has_filament(2).has_value());
}

TEST_CASE("MedusaHC: a new run turns an earlier run's owed heaters off before its own",
          "[toolchanger][mount_feed][medusahc]") {
    AckingChanger tc(4);
    tc.feed_status("ready", 0);
    REQUIRE(tc.load_filament(3).success());
    tc.ack();
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    tc.ack();  // mounted; heating
    tc.lose(); // owed, and no frame has arrived since
    const size_t before = tc.sent_.size();
    REQUIRE(tc.load_filament(1).success());
    REQUIRE(tc.sent_.size() == before + 2);
    CHECK(tc.sent_[before] == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(tc.sent_[before + 1].find("action:prompt_begin") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A backend hears the connection drop after PrinterState's subjects are rebuilt",
                 "[toolchanger][mount_feed][ams]") {
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);
    // A soft restart rebuilds PrinterState's subjects; AmsState re-inits on top.
    get_printer_state().deinit_subjects();
    get_printer_state().init_subjects(false);
    AmsState::instance().init_subjects(true);

    const auto set_connection = [](ConnectionState state) {
        get_printer_state().set_printer_connection_state(static_cast<int>(state), "");
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    set_connection(ConnectionState::CONNECTED);
    auto owned = std::make_unique<ConnectionRecordingBackend>();
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    set_connection(ConnectionState::RECONNECTING);
    CHECK(backend->lost == 1);

    AmsState::instance().set_backend(nullptr);
    set_connection(ConnectionState::CONNECTED);
}

TEST_CASE("mount-to-feed: a tool number that skips seats the tool Klipper numbers it",
          "[toolchanger][mount_feed][tool-number]") {
    // Tools T0, T2 and T3: Klipper's tool 2 is the second head, slot 1.
    AckingChanger tc(3);
    tc.set_discovered_tools({"T0", "T2", "T3"});
    tc.feed(json{{"toolchanger",
                  {{"status", "ready"},
                   {"tool_numbers", json::array({0, 2, 3})},
                   {"tool_names", json::array({"tool T0", "tool T2", "tool T3"})},
                   {"tool_number", 2}}}});

    REQUIRE(tc.get_system_info().current_slot == 1);

    // Unloading the mounted head works T2's head, never T3's.
    REQUIRE(tc.unload_filament(-1).success());
    for (int i = 0; i < 4; ++i) {
        const size_t sent = tc.sent_.size();
        tc.ack();
        if (tc.sent_.size() == sent)
            break;
    }
    INFO("sent: " << [&] {
        std::string all;
        for (const auto& g : tc.sent_)
            all += "[" + g + "] ";
        return all;
    }());
    const auto names = [&](const std::string& needle) {
        return std::any_of(tc.sent_.begin(), tc.sent_.end(), [&](const std::string& g) {
            return g.find(needle) != std::string::npos;
        });
    };
    CHECK(names("HEATER=extruder1 "));
    CHECK_FALSE(names("extruder2"));
    CHECK_FALSE(names("tool T3"));

    SECTION("a number no tool carries seats none") {
        tc.feed(json{{"toolchanger", {{"tool_number", 1}}}});
        CHECK(tc.get_system_info().current_slot == -1);
    }
}

TEST_CASE("toolchanger: a tool listing sent in separate frames re-seats the carriage tool",
          "[toolchanger][tool-number]") {
    // Moonraker sends only changed fields: tool_numbers, tool_names each alone.
    helix::test::ToolChangerHelper tc(3);
    tc.set_discovered_tools({"T0", "T2", "T3"});
    tc.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});
    tc.feed(json{{"toolchanger", {{"tool_numbers", json::array({0, 2, 3})}}}});
    tc.feed(
        json{{"toolchanger", {{"tool_names", json::array({"tool T0", "tool T2", "tool T3"})}}}});
    CHECK(tc.get_system_info().current_slot == 1);
    CHECK(tc.get_system_info().current_tool == 2);

    // ASSIGN_TOOL moving number 2 onto T3 arrives as a listing alone.
    tc.feed(json{{"toolchanger", {{"tool_numbers", json::array({0, 3, 2})}}}});
    CHECK(tc.get_system_info().current_slot == 2);

    // Swapping the two numbers back renames instead: the delta carries tool_names alone.
    tc.feed(
        json{{"toolchanger", {{"tool_names", json::array({"tool T0", "tool T3", "tool T2"})}}}});
    CHECK(tc.get_system_info().current_slot == 1);

    // A remap's listing and tool_number in one frame: the listing answers first.
    tc.feed(json{{"toolchanger",
                  {{"tool_numbers", json::array({0, 1})},
                   {"tool_names", json::array({"tool T3", "tool T0"})},
                   {"tool_number", 0}}}});
    CHECK(tc.get_system_info().current_slot == 2);
}

TEST_CASE("toolchanger: mounting a lane on gapped tool numbers mounts that lane's tool",
          "[toolchanger][tool-number]") {
    helix::test::ToolChangerHelper tc(3);
    tc.set_discovered_tools({"T0", "T2", "T3"});
    tc.feed(json{{"toolchanger",
                  {{"status", "ready"},
                   {"tool_numbers", json::array({0, 2, 3})},
                   {"tool_names", json::array({"tool T0", "tool T2", "tool T3"})},
                   {"tool_number", 0}}}});

    REQUIRE(tc.change_tool(2).success());
    REQUIRE_FALSE(tc.sent_.empty());
    CHECK(tc.sent_.back() == "SELECT_TOOL TOOL=\"tool T3\"");
}

TEST_CASE("toolchanger: tool numbers come from the listing for badges, remaps and resets",
          "[toolchanger][tool-number]") {
    helix::test::ToolChangerHelper tc(3);
    tc.set_discovered_tools({"T0", "T2", "T3"});
    const auto listing = [&](json numbers, json names) {
        tc.feed(json{{"toolchanger", {{"tool_numbers", numbers}, {"tool_names", names}}}});
    };
    const json in_order = {"tool T0", "tool T2", "tool T3"};
    const json rotated = {"tool T3", "tool T0", "tool T2"};
    const std::vector<std::string> homes{"ASSIGN_TOOL TOOL=\"tool T0\" N=0",
                                         "ASSIGN_TOOL TOOL=\"tool T2\" N=2",
                                         "ASSIGN_TOOL TOOL=\"tool T3\" N=3"};

    SECTION("each head is badged with its tool's number, and a remap moves it") {
        listing({0, 2, 3}, in_order);
        CHECK(tc.get_slot_info(1).mapped_tool == 2);
        CHECK(tc.get_tool_mapping() == std::vector<int>{0, -1, 1, 2});
        listing({0, 2, 3}, rotated);
        CHECK(tc.get_slot_info(2).mapped_tool == 0);
    }
    SECTION("a listing that arrived before the tool names still numbers them") {
        helix::test::ToolChangerHelper late(3);
        late.feed(json{{"toolchanger", {{"tool_numbers", {0, 2, 3}}, {"tool_names", in_order}}}});
        late.set_discovered_tools({"T0", "T2", "T3"});
        CHECK(late.get_tool_mapping() == std::vector<int>{0, -1, 1, 2});
    }
    SECTION("a listed number past the tool count can be remapped") {
        listing({0, 2, 3}, in_order);
        REQUIRE(tc.set_tool_mapping(3, 0).success());
        CHECK(tc.sent_.back() == "ASSIGN_TOOL TOOL=\"tool T0\" N=3");
        CHECK_FALSE(tc.can_set_tool_mapping(7, 0).success());
    }
    SECTION("a frame that carries no listing leaves a remap where the user put it") {
        listing({0, 2, 3}, in_order);
        REQUIRE(tc.set_tool_mapping(3, 0).success());
        REQUIRE(tc.get_slot_info(0).mapped_tool == 3);

        // A status delta: nothing in it about the tools' numbers.
        tc.feed(json{{"toolchanger", {{"status", "ready"}}}});

        CHECK(tc.get_slot_info(0).mapped_tool == 3);
        CHECK(tc.get_slot_info(2).mapped_tool == -1);
    }
    SECTION("a reset returns each tool to its own number, gaps included") {
        listing({0, 2, 3}, in_order);
        listing({0, 2, 3}, rotated);
        REQUIRE(tc.reset_tool_mappings().success());
        CHECK(tc.sent_ == homes);
    }
    SECTION("set_discovery's configured numbers outrank a first listing already remapped") {
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T2", "tool T3"}));
        hw.parse_configured_tool_numbers({{"tool t0", {{"tool_number", 0}}},
                                          {"tool t2", {{"tool_number", 2}}},
                                          {"tool t3", {{"tool_number", 3}}},
                                          {"extruder", {{"tool_number", 7}}}});
        CHECK(hw.configured_tool_numbers().size() == 3);
        tc.set_discovery(hw);
        listing({0, 2, 3}, rotated);
        REQUIRE(tc.reset_tool_mappings().success());
        CHECK(tc.sent_ == homes);
    }
    SECTION("a Klipper restart forgets the old config's home numbers") {
        listing({0, 2, 3}, in_order);
        tc.on_klipper_restarted();
        listing({0, 1, 2}, in_order);
        REQUIRE(tc.reset_tool_mappings().success());
        CHECK(tc.sent_.empty());
    }
    SECTION("a lost connection forgets the old listing's home numbers") {
        listing({0, 2, 3}, in_order);
        REQUIRE(tc.can_set_tool_mapping(3, 0).success());

        // The reconnect finds the tools numbered from scratch, without a
        // restart in between for the listing to be cleared by.
        tc.on_connection_lost();
        CHECK_FALSE(tc.can_set_tool_mapping(3, 0).success());
        listing({0, 1, 2}, in_order);
        REQUIRE(tc.reset_tool_mappings().success());
        CHECK(tc.sent_.empty());
    }
    SECTION("a reset leaves a tool the listing gives no number without a badge") {
        // T2 is listed with no number; the other two swap theirs.
        listing({2, -1, 0}, in_order);
        listing({0, -1, 2}, in_order);
        REQUIRE(tc.get_slot_info(1).mapped_tool == -1);

        REQUIRE(tc.reset_tool_mappings().success());
        CHECK(tc.sent_ == std::vector<std::string>{"ASSIGN_TOOL TOOL=\"tool T0\" N=2",
                                                   "ASSIGN_TOOL TOOL=\"tool T3\" N=0"});
        // Klipper's next listing is what badges the tools again: no number is
        // invented for T2 in the meantime.
        CHECK(tc.get_slot_info(1).mapped_tool == -1);
    }
    SECTION("a reset on a machine with no ASSIGN_TOOL fails and keeps the listed badges") {
        tc.set_tool_commands(toolchanger_addon::ToolCommands{true, "test", "T", "", false});
        listing({0, 2, 3}, in_order);
        listing({0, 2}, json{"tool T2", "tool T0"});
        REQUIRE(tc.get_slot_info(2).mapped_tool == -1);
        CHECK_FALSE(tc.reset_tool_mappings().success());
        CHECK(tc.get_slot_info(2).mapped_tool == -1);
        CHECK(tc.sent_.empty());
    }
}

TEST_CASE("toolchanger: a listed reset of a contiguous swap sends exactly two ASSIGN_TOOLs",
          "[toolchanger][tool-number]") {
    helix::test::ToolChangerHelper tc(3);
    tc.feed(
        json{{"toolchanger",
              {{"tool_numbers", {0, 1, 2}}, {"tool_names", {"tool T0", "tool T1", "tool T2"}}}}});
    tc.feed(json{{"toolchanger", {{"tool_names", {"tool T1", "tool T0", "tool T2"}}}}});
    REQUIRE(tc.reset_tool_mappings().success());
    CHECK(tc.sent_ == std::vector<std::string>{"ASSIGN_TOOL TOOL=\"tool T0\" N=0",
                                               "ASSIGN_TOOL TOOL=\"tool T1\" N=1"});
}
