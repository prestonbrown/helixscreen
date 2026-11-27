// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 121 to 170 of test_ams_backend_afc.cpp before it was split, in its order. Mostly [status],
// [toolchanger], [eject][serialization], [bypass][bypass-lane-frame], [status][current_tool], and
// more

#include "ui_ams_lane_settings.h"
#include "ui_callback_helpers.h"
#include "ui_language_refresh.h"
#include "ui_panel_filament.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"
#include "ui_test_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/afc_test_fixtures.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "afc_defaults.h"
#include "ams_backend_afc.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_step_model.h"
#include "ams_tool_topology.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "error_event.h"
#include "filament_mapper.h"
#include "filament_op_execute.h"
#include "filament_op_router.h"
#include "filament_slot_override_store.h"
#include "gcode_footer_summary.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_dir_guard.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_select_detail_view_test_access.h"
#include "test_helpers/print_start_controller_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_cache_dir.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/update_queue_test_access.h"
#include "tools_used_cache.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

using helix::test::AfcBypassOn;
using helix::test::AfcEditedLaneFixture;
using helix::test::AfcFourLanes;
using helix::test::AfcFourRunningLanes;
using helix::test::AfcFourZeroBasedLanes;
using helix::test::AfcOpenDetailFixture;
using helix::test::AfcPrintStartHarness;
using helix::test::discover_afc;
using helix::test::discover_two_units;
using helix::test::feed_lane4_stands_in_for_lane2;
using helix::test::has_action;
using helix::test::kJamLine;
using helix::test::observed_lane1_v110;
using helix::test::set_map_commands;
using helix::test::setup_interleaved_toolchanger;
using helix::test::StaleFaultRig;
using helix::test::tool_of_unknown_color;
using helix::test::trace_contains;
using helix::test::two_buffer_objects;
using helix::test::two_unit_afc_state;
using helix::test::two_unit_objects;

// LANE_UNLOAD serialization
//
// Rapid-fire LANE_UNLOAD commands overlap their lane LED animations and stepper
// work on AFC's MCU, which contributes to Klippy "Timer too close" shutdowns.
// eject_lane() queues consecutive requests and dispatches them one at a time,
// firing the next only when the previous completes.

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC eject_lane serializes consecutive ejects (only first dispatches)",
                 "[ams][afc][eject][serialization]") {
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // Only the first eject should have dispatched — the rest are queued.
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
}

TEST_CASE_METHOD(AfcFourRunningLanes, "AFC eject_lane drains queue in order on each completion",
                 "[ams][afc][eject][serialization]") {
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // After each completion, the next queued eject should fire.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");

    // Queue empty — further completions do nothing.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC eject_lane deduplicates consecutive duplicate slot requests",
                 "[ams][afc][eject][serialization]") {
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    // Repeat-tap the same lane while the first is still running.
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced

    helper.complete_pending_unload();
    helper.complete_pending_unload();

    // Expected dispatches: lane1, lane2 (only one lane2, despite three taps).
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC eject_lane fires next eject after previous completes (synchronous mode)",
                 "[ams][afc][eject][serialization]") {
    // Default: defer_lane_unload_complete = false → completion fires synchronously.

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());

    // Synchronous completion means by the time eject_lane returns, the queue
    // has fully drained and both gcodes have dispatched.
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE("AFC eject_lane queue drains on failure (error path)",
          "[ams][afc][eject][serialization]") {
    // In production, dispatch_lane_unload registers success AND error callbacks
    // with api_->execute_gcode. Both call on_lane_unload_done() so the queue
    // drains either way. This test simulates the error path: the in-flight
    // gcode "fails", but on_lane_unload_done still advances to the next queued
    // entry. Without this guarantee a transient AFC error would strand the
    // queue and silently break subsequent eject taps.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());

    REQUIRE(helper.captured_gcodes.size() == 1);

    // Simulate the error callback path — completion still drains the queue.
    helper.complete_pending_unload(); // "lane1 errored" → fire lane2
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload(); // "lane2 errored" → fire lane3
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload(); // "lane3 errored" → queue empty
    REQUIRE(helper.captured_gcodes.size() == 3);
    // Subsequent eject_lane should fire immediately (in-flight flag cleared).
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");
}

TEST_CASE_METHOD(AfcFourRunningLanes, "AFC cancel drops queued ejects but lets in-flight complete",
                 "[ams][afc][eject][serialization][cancel]") {
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 1); // only lane1 dispatched

    // Cancel should drop queued lane2/lane3/lane4 but not abort the in-flight
    // lane1 — its completion callback is still pending.
    helper.cancel();

    // The in-flight callback firing now should NOT dispatch a queued next
    // (queue was cleared). It should just clear eject_in_flight_.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 1); // no new dispatches

    // A fresh eject after cancel completes should fire immediately.
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane3");
}

TEST_CASE("AFC supports_lane_eject returns true", "[ams][afc][capability]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.supports_lane_eject());
}

// Slot status mapping: AFC "Loaded" vs tool_loaded

TEST_CASE("AFC lane status: only filament at the toolhead is LOADED", "[ams][afc][status]") {
    struct Row {
        const char* what;
        bool tool_loaded;
        const char* status;
        SlotStatus expected;
    };
    // Hub-loaded filament is ready to load to the toolhead, so AVAILABLE; OpenAMS
    // uses the "Tooled" status string as its own toolhead-loaded indicator.
    const Row row =
        GENERATE(Row{"hub-loaded", false, "Loaded", SlotStatus::AVAILABLE},
                 Row{"tool_loaded", true, "Loaded", SlotStatus::LOADED},
                 Row{"Tooled without tool_loaded", false, "Tooled", SlotStatus::LOADED});
    CAPTURE(row.what);
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", row.tool_loaded},
                                      {"material", "ASA"},
                                      {"color", "#000000"},
                                      {"map", "T0"},
                                      {"status", row.status},
                                      {"weight", 570}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->status == row.expected);
    if (row.expected == SlotStatus::AVAILABLE) {
        // Not the "current" loaded slot either.
        REQUIRE(info.current_slot == -1);
        REQUIRE_FALSE(info.filament_loaded);
    }
}

TEST_CASE_METHOD(AfcFourLanes, "AFC context menu shows Eject for hub-loaded slot",
                 "[ams][afc][status]") {
    // Two lanes loaded to hub, none to toolhead
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "ASA"},
                                      {"map", "T0"},
                                      {"status", "Loaded"}});

    auto slot = helper.get_slot_info(0);
    // Slot should be present (has filament)
    REQUIRE(slot.is_present());
    // But NOT loaded to extruder
    REQUIRE(slot.status == SlotStatus::AVAILABLE);
    REQUIRE(slot.status != SlotStatus::LOADED);
}

TEST_CASE_METHOD(AfcFourLanes, "AFC slot transitions from LOADED to AVAILABLE on unload",
                 "[ams][afc][status]") {
    // First: loaded to toolhead
    helper.feed_afc_stepper("lane1", {{"tool_loaded", true},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    auto info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);

    // Then: unloaded from toolhead, still at hub
    helper.feed_afc_stepper("lane1", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::AVAILABLE);
}

// filament_loaded derived from stepper tool_loaded (no top-level AFC field)

TEST_CASE_METHOD(AfcFourLanes, "AFC filament_loaded derived from stepper tool_loaded",
                 "[ams][afc][status]") {
    // Simulate AFC version without top-level "filament_loaded" field:
    // only lane stepper data drives loaded state
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 3); // lane4 = slot index 3
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::LOADED);
}

TEST_CASE_METHOD(AfcFourLanes, "AFC filament_loaded clears when tool_loaded goes false",
                 "[ams][afc][status]") {
    // Load lane4
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);

    // Unload — tool_loaded goes false
    helper.feed_afc_stepper("lane4", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::AVAILABLE);
}

TEST_CASE_METHOD(AfcFourLanes, "AFC current_load fallback sets current_slot and filament_loaded",
                 "[ams][afc][status]") {
    // AFC state with current_load (not current_lane) and no filament_loaded field
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0); // lane1 = slot index 0
    // filament_loaded derived from current_load
    REQUIRE(info.filament_loaded);
}

TEST_CASE("AFC current_tool derived from current_load via tool mapping",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // Sets slot N → tool N mapping

    // AFC reports current_load=lane3 but no current_tool field
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2); // lane3 = slot index 2
    REQUIRE(info.current_tool == 2); // derived from slot 2 → T2 mapping
}

TEST_CASE("AFC ignores a current_tool key it never publishes",
          "[ams][afc][status][current_tool][deadfields]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // slot N → tool N

    // There is no "current_tool" in AFC.get_status() on any version (AFC.py
    // v1.2.0:2531-2564). The branch that let it override the slot→tool mapping
    // was unreachable, and honouring it would have let an invented key put the
    // UI on a tool that does not exist.
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_tool", 5},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // The slot→tool mapping is the only authority: slot 2 → T2, not the 5 fed in.
    REQUIRE(info.current_tool == 2);
}

TEST_CASE("AFC current_tool cleared on unload (current_load null)",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // First: load lane2
    helper.feed_afc_state({{"current_load", "lane2"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_tool == 1); // lane2 → slot 1 → T1
    REQUIRE(info.filament_loaded);

    // Then: unload — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE(info.current_tool == -1); // cleared
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("AFC current_tool uses default 1:1 mapping from initialize_slots",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    // initialize_test_lanes + initialize_slots_from_discovery creates default 1:1 mapping
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // Default 1:1 mapping: slot 2 → T2
    REQUIRE(info.current_tool == 2);
}

TEST_CASE_METHOD(
    AfcFourLanes,
    "AFC stepper post-scan owns filament_loaded; a stray AFC.filament_loaded is ignored",
    "[ams][afc][status][deadfields]") {
    // No AFC version publishes filament_loaded on the AFC object (AFC.py
    // v1.2.0:2531-2564), so the per-lane scan is the only authority. The old
    // code suppressed the scan whenever the key was merely PRESENT — which,
    // had AFC ever added it, would have handed a single boolean veto over
    // every lane's tool_loaded.
    nlohmann::json params;
    params["AFC"] = {{"filament_loaded", false}, {"current_state", "Unloading"}};
    params["AFC_stepper lane1"] = {
        {"tool_loaded", true}, {"status", "Tooled"}, {"prep", true}, {"load", true}, {"map", "T0"}};
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    // The lane says it is at the toolhead, so the system is loaded.
    REQUIRE(info.filament_loaded);
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);
}

TEST_CASE_METHOD(AfcFourLanes, "AFC current_load null clears filament state",
                 "[ams][afc][status]") {
    // First: loaded via current_load
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 0);

    // Then: unloaded — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.current_slot == -1);
}

// manages_active_spool() Tests
//
// AFC manages Spoolman's active spool natively (calls spoolman_set_active_spool
// on tool load/unload). HelixScreen must NOT call set_active_spool when AFC is
// the active backend, to avoid racing with AFC's own calls.

TEST_CASE("AFC backend reports manages_active_spool=true", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.manages_active_spool() == true);
}

TEST_CASE("AFC reports the manages_active_spool the shared case table claims",
          "[ams][afc][spoolman][plugin_filament]") {
    const auto rows = helix::test::ownership_rows_for("AFC");
    REQUIRE_FALSE(rows.empty());
    AmsBackendAfcTestHelper helper;
    for (const auto& row : rows) {
        INFO(row["name"].get<std::string>());
        CHECK(helper.manages_active_spool() == row["manages_active_spool"].get<bool>());
    }
}

TEST_CASE("AFC get_type returns AFC", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_type() == AmsType::AFC);
}

// auto_unloads_after_print() driven by per-printer SettingsManager toggle
//
// Unlike CFS/IFS (which hardcode auto_unloads_after_print()==true), AFC's
// post-print unload depends on the user's macros. The behaviour is controlled
// by a per-printer setting: get_unload_after_print() (default false). The
// pre-print runout warning is suppressed only when the setting is enabled.

TEST_CASE_METHOD(LVGLTestFixture, "AFC auto_unloads_after_print follows the setting",
                 "[ams][afc][capability]") {
    helix::Config::get_instance();
    helix::SettingsManager::instance().init_subjects();

    helix::SettingsManager::instance().set_unload_after_print(false);
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.auto_unloads_after_print() == false);

    helix::SettingsManager::instance().set_unload_after_print(true);
    REQUIRE(helper.auto_unloads_after_print() == true);

    // Toggling the setting back off must flip the capability back
    helix::SettingsManager::instance().set_unload_after_print(false);
    REQUIRE(helper.auto_unloads_after_print() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC Unload at end is a device action over the setting",
                 "[ams][afc][capability][device_actions]") {
    helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_unload_after_print(false);

    AmsBackendAfcTestHelper helper;
    auto find = [&]() -> std::optional<helix::printer::DeviceAction> {
        for (auto& a : helper.get_device_actions()) {
            if (a.id == "unload_at_end") {
                return a;
            }
        }
        return std::nullopt;
    };

    auto action = find();
    REQUIRE(action.has_value());
    CHECK(action->type == helix::printer::ActionType::TOGGLE);
    CHECK(action->label == "Unload at end");
    CHECK(std::any_cast<bool>(action->current_value) == false);

    REQUIRE(helper.execute_device_action("unload_at_end", std::any(true)).success());
    CHECK(settings.get_unload_after_print());
    CHECK(helper.auto_unloads_after_print());
    CHECK(std::any_cast<bool>(find()->current_value) == true);

    CHECK_FALSE(helper.execute_device_action("unload_at_end", std::any(1)).success());
    CHECK(settings.get_unload_after_print());

    REQUIRE(helper.execute_device_action("unload_at_end", std::any(false)).success());
    CHECK_FALSE(helper.auto_unloads_after_print());
}

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager unload_after_print round-trips and persists",
                 "[ams][afc][settings]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    const std::string path = config->df() + "ams/unload_after_print";

    SECTION("defaults to false") {
        settings.set_unload_after_print(false);
        REQUIRE(settings.get_unload_after_print() == false);
    }

    SECTION("set true -> get true -> persisted to per-printer config path") {
        settings.set_unload_after_print(true);
        REQUIRE(settings.get_unload_after_print() == true);
        REQUIRE(config->get<bool>(path, false) == true);

        // subject reflects the new value
        REQUIRE(lv_subject_get_int(settings.subject_unload_after_print()) == 1);
    }

    SECTION("set false after true clears the value") {
        settings.set_unload_after_print(true);
        settings.set_unload_after_print(false);
        REQUIRE(settings.get_unload_after_print() == false);
        REQUIRE(config->get<bool>(path, true) == false);
        REQUIRE(lv_subject_get_int(settings.subject_unload_after_print()) == 0);
    }
}

TEST_CASE("AFC toolchanger load and tool change follow AFC's map, not extruder order",
          "[ams][afc][toolchanger][1824]") {
    AmsBackendAfcTestHelper helper;
    setup_interleaved_toolchanger(helper);
    REQUIRE(helper.get_slot_count() == 7);
    REQUIRE(helper.get_slot_name(0) == "lane1");
    REQUIRE(helper.get_slot_name(5) == "e1");

    SECTION("load lane1 (T0, on e3) names the lane") {
        REQUIRE(helper.load_filament(0));
        REQUIRE(helper.captured_gcodes == std::vector<std::string>{"CHANGE_TOOL LANE=lane1"});
    }

    SECTION("load e1 (T3, on e1) names the lane") {
        REQUIRE(helper.load_filament(5));
        REQUIRE(helper.captured_gcodes == std::vector<std::string>{"CHANGE_TOOL LANE=e1"});
    }

    // A tool change on a toolchanger mounts the head the T number's lane feeds
    // and moves no filament; T{n} would run CHANGE_TOOL and swap lanes.
    SECTION("change_tool(0) mounts e3, the head lane1 (T0) feeds") {
        REQUIRE(helper.change_tool(0));
        REQUIRE(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e3"});
    }

    SECTION("change_tool(3) mounts e1") {
        REQUIRE(helper.change_tool(3));
        REQUIRE(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e1"});
    }

    SECTION("change_tool(5) mounts e2, past the end of the extruder list") {
        REQUIRE(helper.change_tool(5));
        REQUIRE(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e2"});
    }
}

TEST_CASE("AFC toolchanger: a T number with no lane refuses instead of guessing a head",
          "[ams][afc][toolchanger][1824]") {
    AmsBackendAfcTestHelper helper;
    setup_interleaved_toolchanger(helper);
    // Move lane4 off T6, so no lane carries T6.
    helper.feed_afc_stepper("lane4", {{"map", "T0"}, {"extruder", "e3"}});
    CHECK_FALSE(helper.change_tool(6));
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("AFC toolchanger: a head with no toolchanger_unit warns and still sends",
          "[ams][afc][toolchanger][1850]") {
    // AFC gives a head a tool lane only when its AFC_extruder sets
    // toolchanger_unit. AFC_SELECT_TOOL on any other head logs an error and
    // returns ok without moving, so the screen waits on Selecting. That is
    // warned, not refused: configfile is the only evidence.
    AmsBackendAfcTestHelper helper;
    setup_interleaved_toolchanger(helper);

    auto section = [](const char* klipper_name, bool on_toolchanger) {
        nlohmann::json s = {{"extruder_name", klipper_name}};
        if (on_toolchanger) {
            s["toolchanger_unit"] = "AFC_Toolchanger Tools";
        }
        return s;
    };
    const nlohmann::json settings = {{"afc_extruder e0", section("extruder", true)},
                                     {"afc_extruder e1", section("extruder1", true)},
                                     {"afc_extruder e2", section("extruder2", false)},
                                     {"afc_extruder e3", section("extruder3", true)}};
    AfcTestAccess::apply_configfile_topology(
        helper, AmsBackendAfc::parse_configfile_topology(
                    {{"result", {{"status", {{"configfile", {{"settings", settings}}}}}}}}));

    LogCapture log;

    SECTION("T5 feeds e2, which is not on the toolchanger") {
        REQUIRE(helper.change_tool(5));
        CHECK(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e2"});
        CHECK(log.has_line_with({"T5 feeds e2", "sets no toolchanger_unit"}));
    }

    SECTION("the warning is logged once per head, not on every tool change") {
        REQUIRE(helper.change_tool(5));
        // Let the first change finish so the second is not refused as busy.
        REQUIRE(helper.pending_macro_ack);
        helper.pending_macro_ack();
        helper.feed_afc_state({{"current_state", "Idle"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);
        REQUIRE(helper.change_tool(5));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e2", "AFC_SELECT_TOOL TOOL=e2"});
        CHECK(log.count_containing("sets no toolchanger_unit") == 1);
    }

    SECTION("a lane parsed before configfile answered still names its head by section") {
        REQUIRE(helper.change_tool(3));
        CHECK(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e1"});
        CHECK(log.count_containing("sets no toolchanger_unit") == 0);
    }

    SECTION("a lane parsed after it names the Klipper extruder") {
        helper.feed_afc_stepper("e1", {{"map", "T3"}, {"extruder", "e1"}});
        REQUIRE(helper.change_tool(3));
        CHECK(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=extruder1"});
        CHECK(log.count_containing("sets no toolchanger_unit") == 0);
    }
}

TEST_CASE_METHOD(AfcFourRunningLanes, "AFC change_tool sends T{n} in single-extruder mode",
                 "[ams][afc][toolchanger]") {
    auto result = helper.change_tool(1);

    REQUIRE(result);
    REQUIRE(helper.captured_gcodes == std::vector<std::string>{"T1"});
}

TEST_CASE("AFC change_tool takes a T number above the lane count when a lane carries it",
          "[ams][afc][toolchanger][tool_mapping]") {
    // T numbers belong to lanes, not to lane positions: lane4 mapped to T7 on
    // a four-lane unit is a legal map, and T7 is the tool that runs it.
    SECTION("single extruder sends T7") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.feed_afc_stepper("lane4", {{"map", "T7"}});
        helper.captured_gcodes.clear();
        REQUIRE(helper.change_tool(7));
        CHECK(helper.captured_gcodes == std::vector<std::string>{"T7"});
    }

    SECTION("a toolchanger mounts the head T9's lane feeds") {
        AmsBackendAfcTestHelper helper;
        setup_interleaved_toolchanger(helper);
        helper.feed_afc_stepper("lane4", {{"map", "T9"}, {"extruder", "e3"}});
        helper.captured_gcodes.clear();
        REQUIRE(helper.change_tool(9));
        CHECK(helper.captured_gcodes == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e3"});
    }

    SECTION("a T number AFC has not registered is still refused") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.feed_afc_state({{"maps", {"T0", "T1", "T2", "T3"}}});
        helper.captured_gcodes.clear();
        CHECK_FALSE(helper.change_tool(5));
        CHECK(helper.captured_gcodes.empty());
    }
}

TEST_CASE("AFC SET_MAP takes a T number above the lane count when a lane carries it",
          "[ams][afc][tool_mapping][remap]") {
    // AFC's SET_MAP accepts MAP=T<n> when some lane's map registers T<n>, and
    // refuses any other. lane4 runs T7 on a four-lane unit, so moving T7 onto
    // lane2 (a user remap, or the restore of one) is a legal command.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane4", {{"map", "T7"}});
    helper.captured_gcodes.clear();

    CHECK(helper.can_set_tool_mapping(7, 1).success());
    REQUIRE(helper.set_tool_mapping(7, 1).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"SET_MAP LANE=lane2 MAP=T7"});

    SECTION("a T number AFC has not registered is refused, as AFC refuses it") {
        helper.feed_afc_state({{"maps", {"T0", "T1", "T2", "T3", "T7"}}});
        helper.captured_gcodes.clear();
        CHECK_FALSE(helper.can_set_tool_mapping(5, 1).success());
        CHECK_FALSE(helper.set_tool_mapping(5, 1).success());
        CHECK(helper.captured_gcodes.empty());
    }
}

TEST_CASE("AFC lane settings show the tool a multi-tool lane is running",
          "[ams][afc][tool_mapping][lane_settings]") {
    // lane1 carries T1 and T5 and is running T5. Both route to it, so the first
    // match in the tool table is T1, which is not what the lane is doing.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane1", {{"map", {"T1", "T5"}}, {"current_map", "T5"}});
    helper.feed_afc_stepper("lane2", {{"map", {"T0"}}});
    REQUIRE(helper.get_tool_mapping()[1] == 0);
    REQUIRE(helper.get_tool_mapping()[5] == 0);

    CHECK(helix::ui::lane_settings::assigned_tool(helper, 0) == 5);
    CHECK(helix::ui::lane_settings::assigned_tool(helper, 1) == 0);
}

TEST_CASE("AFC: a two-step swap applies both halves", "[ams][afc][tool_mapping][remap]") {
    // A remap or restore loop exchanging two lanes sends SET_MAP for one tool
    // and then the other. AFC's SET_MAP swaps the two lanes' maps, so after the
    // first command the displaced tool sits on the other lane and is still a
    // registered T number: the second half must not be refused.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane1", {{"map", "T0"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}});
    helper.feed_afc_state({{"maps", {"T0", "T1", "T2", "T3"}}});
    helper.captured_gcodes.clear();

    REQUIRE(helper.set_tool_mapping(0, 1).success());
    CHECK(helper.get_tool_mapping() == std::vector<int>{1, 0, 2, 3});
    REQUIRE(helper.set_tool_mapping(1, 0).success());
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"SET_MAP LANE=lane2 MAP=T0", "SET_MAP LANE=lane1 MAP=T1"});
}

TEST_CASE("AFC: an extra tool on a multi-tool lane routes to that lane",
          "[ams][afc][tool_mapping][routing]") {
    // AFC virtual tools: lane1 carries T0 and T4. T4 is a registered command
    // that runs lane1, so it is a tool change, a legal SET_MAP target, and a
    // tool the printer routes to lane1, not one with no lane assigned.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.feed_afc_stepper("lane1", {{"map", {"T0", "T4"}}, {"current_map", "T0"}});
    helper.feed_afc_stepper("lane2", {{"map", {"T1"}}});
    helper.feed_afc_stepper("lane3", {{"map", {"T2"}}});
    helper.feed_afc_stepper("lane4", {{"map", {"T3"}}});
    helper.feed_afc_state({{"maps", {"T0", "T1", "T2", "T3", "T4"}}});
    helper.captured_gcodes.clear();

    CHECK(helper.firmware_default_routing().head(4) == 0);
    CHECK(helper.firmware_default_routing().head(0) == 0);
    CHECK(helper.can_set_tool_mapping(4, 2).success());
    REQUIRE(helper.change_tool(4));
    CHECK(helper.captured_gcodes == std::vector<std::string>{"T4"});
}

TEST_CASE("AFC toolchanger: Load on a seated machine plans load_filament, not change_tool",
          "[ams][afc][toolchanger][dispatch][1824]") {
    // change_tool() on a toolchanger only mounts a head. A swap Load must reach
    // load_filament() (`CHANGE_TOOL LANE=`), which unloads the seated lane itself.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(2);
    helper.set_filament_loaded(true);
    helper.set_current_slot(0);

    // Read the caps the way the Load button does, so the trait has to cross
    // read_backend_caps() to reach the planner.
    AmsSystemInfo info;
    const helix::ui::BackendCaps caps = helix::ui::read_backend_caps(&helper, info, 1);
    REQUIRE(caps.needs_unload_before_load); // the swap arm would otherwise fire
    REQUIRE(caps.load_swaps_seated_lane);

    auto plan = helix::ui::plan_load(info, caps, /*target_slot=*/1, /*macro_available=*/false,
                                     /*macro_user_configured=*/false);
    CHECK(plan.ams_call == helix::ui::AmsCall::Load);
    CHECK(plan.ams_arg == 1);
}

// AFC's CHANGE_TOOL logs "Filament loaded in bypass, not doing tool load" and
// returns without a G-code error, and a virtual bypass stays on after its spool
// is pulled out, so the refusal has to come from here, at planning, before any
// preheat or homing.
TEST_CASE_METHOD(AfcBypassOn, "AFC refuses a lane load while bypass mode is on",
                 "[ams][afc][bypass][dispatch][bypass-load-refusal]") {
    SECTION("virtual switch, toolhead empty: turn bypass off") {
        stage(/*hardware_switch=*/false, Head::Empty);
        const auto plan = plan_lane_tap(1);
        CHECK(plan.refusal == helix::ui::FilamentRefusal::BackendRefused);
        CHECK(plan.backend_refusal.result == AmsResult::WRONG_STATE);
        CHECK(plan.backend_refusal.suggestion != remove_spool_advice());
        CHECK(helper.load_filament(1).result == AmsResult::WRONG_STATE);
        CHECK(helper.change_tool(1).result == AmsResult::WRONG_STATE); // T<n> runs CHANGE_TOOL
        CHECK(helper.captured_gcodes.empty());
        REQUIRE(helper.disable_bypass());
        CHECK(helper.has_gcode("SET_FILAMENT_SENSOR SENSOR=virtual_bypass ENABLE=0"));
    }
    // An engaged bypass reads as loaded with no sensor to say otherwise, which
    // would otherwise plan BypassLoaded after the spool is already out.
    SECTION("virtual switch, no toolhead sensor: turn bypass off, not remove the spool") {
        stage(/*hardware_switch=*/false, Head::NoSensor);
        REQUIRE(helper.get_system_info().filament_loaded);
        const auto plan = plan_lane_tap(1);
        CHECK(plan.refusal == helix::ui::FilamentRefusal::BackendRefused);
        CHECK(plan.backend_refusal.suggestion != remove_spool_advice());
    }
    // The screen refuses to drive a hardware switch, so "turn bypass off" is
    // advice the user cannot follow.
    SECTION("hardware switch, toolhead empty: remove the spool") {
        stage(/*hardware_switch=*/true, Head::Empty);
        const auto plan = plan_lane_tap(1);
        CHECK(plan.refusal == helix::ui::FilamentRefusal::BackendRefused);
        CHECK(plan.backend_refusal.suggestion == remove_spool_advice());
    }
    // The toolhead reading is machine-wide and a parked head holds its own
    // filament, so a toolchanger cannot confirm a bypass strand.
    SECTION("toolchanger with a parked head loaded: turn bypass off") {
        helper.set_supports_bypass(true);
        helper.set_discovered_sensors({"filament_switch_sensor virtual_bypass"});
        AfcTestAccess::apply_configfile_topology(
            helper,
            AmsBackendAfc::parse_configfile_topology(
                {{"result",
                  {{"status",
                    {{"configfile",
                      {{"settings",
                        {{"afc_extruder extruder", {{"pin_tool_start", "sht36:PA1"}}},
                         {"afc_extruder extruder1", {{"pin_tool_start", "sht36b:PA1"}}}}}}}}}}}}));
        helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});
        helper.feed_afc_extruder("extruder", {{"tool_start_status", false}});
        helper.feed_afc_extruder("extruder1", {{"tool_start_status", true}});
        helper.feed_afc_state({{"bypass_state", true}});
        helper.captured_gcodes.clear();

        const auto plan = plan_lane_tap(1);
        CHECK(plan.refusal == helix::ui::FilamentRefusal::BackendRefused);
        CHECK(plan.backend_refusal.suggestion != remove_spool_advice());
        REQUIRE(helper.disable_bypass());
        CHECK(helper.has_gcode("SET_FILAMENT_SENSOR SENSOR=virtual_bypass ENABLE=0"));
    }
    // Turning bypass off would let the next tap feed a lane into the strand.
    SECTION("strand confirmed at the toolhead: the planner's remove-the-spool refusal") {
        const bool hardware_switch = GENERATE(false, true);
        stage(hardware_switch, Head::Full);
        CHECK(plan_lane_tap(1).refusal == helix::ui::FilamentRefusal::BypassLoaded);
        const AmsError load = helper.load_filament(1);
        CHECK(load.result == AmsResult::WRONG_STATE);
        CHECK(load.suggestion == remove_spool_advice());
        CHECK(helper.disable_bypass().result == AmsResult::WRONG_STATE);
        CHECK(helper.captured_gcodes.empty());
    }
}

TEST_CASE_METHOD(AfcFourRunningLanes, "AFC loads a lane once bypass mode is off",
                 "[ams][afc][bypass][bypass-load-refusal]") {
    helper.set_supports_bypass(true);
    helper.feed_afc_state({{"bypass_state", true}});
    helper.feed_afc_state({{"bypass_state", false}});
    helper.captured_gcodes.clear();

    REQUIRE(helper.load_filament(1));
    CHECK(helper.captured_gcodes == std::vector<std::string>{"CHANGE_TOOL LANE=lane2"});
}

TEST_CASE_METHOD(AfcFourRunningLanes, "AFC load_filament sends CHANGE_TOOL in single-extruder mode",
                 "[ams][afc][toolchanger]") {
    auto result = helper.load_filament(1);

    REQUIRE(result);
    REQUIRE(helper.captured_gcodes == std::vector<std::string>{"CHANGE_TOOL LANE=lane2"});
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC unload_filament sends bare TOOL_UNLOAD in toolchanger mode (no slot)",
                 "[ams][afc][toolchanger]") {
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(2);

    // No slot specified (slot_index = -1): unload whatever is currently loaded.
    auto result = helper.unload_active_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

// #999: selecting a non-active lane and pressing Unload must unload THAT lane,
// not whatever is loaded on the shuttle. AFC's TOOL_UNLOAD LANE=<lane> picks up
// the correct tool first, so the requested lane is honored even in toolchanger
// mode (a bare AFC_UNSELECT_TOOL would ignore the slot).
TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC unload_filament sends TOOL_UNLOAD LANE=<lane> for specific slot (#999)",
                 "[ams][afc][toolchanger]") {
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(4);

    SECTION("slot 0 unloads lane1") {
        auto result = helper.unload_filament(0);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane1"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 1 unloads lane2") {
        auto result = helper.unload_filament(1);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane2"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 3 unloads lane4") {
        auto result = helper.unload_filament(3);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }
}

// A lane frame arriving on its own while bypass is engaged still finds no lane
// loaded, but the external spool is at the toolhead: the filament panel's Unload
// reads exactly these two fields for the bypass target.
TEST_CASE_METHOD(AfcFourRunningLanes, "AFC bypass survives a lane-only status frame",
                 "[ams][afc][bypass][bypass-lane-frame]") {
    helper.set_supports_bypass(true);
    helper.feed_afc_state({{"bypass_state", true}});
    REQUIRE(helper.get_system_info().current_slot == -2);

    helper.feed_afc_stepper("lane1", {{"prep", true}, {"load", true}, {"tool_loaded", false}});

    const auto sys = helper.get_system_info();
    CHECK(sys.current_slot == -2);
    CHECK(sys.filament_loaded);
}

// bypass_state is a mode switch that stays on after the external spool is
// pulled out; a fitted toolhead sensor reading empty means nothing is loaded.
TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC bypass is loaded only as far as the toolhead sensor says",
                 "[ams][afc][bypass][bypass-lane-frame]") {
    helper.set_supports_bypass(true);
    AfcTestAccess::apply_configfile_topology(
        helper,
        AmsBackendAfc::parse_configfile_topology(
            {{"result",
              {{"status",
                {{"configfile",
                  {{"settings",
                    {{"afc_extruder extruder", {{"pin_tool_start", "sht36:PA1"}}}}}}}}}}}}));
    helper.feed_afc_extruder("extruder", {{"tool_start_status", false}});
    helper.feed_afc_state({{"bypass_state", true}});

    CHECK(helper.get_system_info().current_slot == -2);
    CHECK_FALSE(helper.get_system_info().filament_loaded);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", true}});
    CHECK(helper.get_system_info().current_slot == -2);
    CHECK(helper.get_system_info().filament_loaded);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", false}});
    CHECK_FALSE(helper.get_system_info().filament_loaded);
}

// Moonraker sends deltas, so disengaging can arrive as bypass_state alone, with
// no lane frame to recount the toolhead.
TEST_CASE_METHOD(AfcFourRunningLanes, "AFC bypass disengaged by a lone delta releases the toolhead",
                 "[ams][afc][bypass][bypass-lane-frame]") {
    helper.set_supports_bypass(true);
    helper.feed_afc_state({{"bypass_state", true}});
    REQUIRE(helper.get_system_info().current_slot == -2);

    helper.feed_afc_state({{"bypass_state", false}});

    const auto sys = helper.get_system_info();
    CHECK(sys.current_slot == -1);
    CHECK_FALSE(sys.filament_loaded);
}

// Bypass: the external spool feeds the toolhead with no lane behind it, so
// slots_.name_of(-2) resolves to "" and AFC unloads whatever is at the head via
// its own user-configured macro. The backend accepts -2; the UI decision
// layer must not treat it as "nothing resolved" alongside -1. Pinned here so a future tightening of
// the lane lookup cannot start rejecting the sentinel outright.
TEST_CASE_METHOD(AfcFourRunningLanes, "AFC unload_filament under bypass sends bare TOOL_UNLOAD",
                 "[ams][afc][bypass]") {
    helper.set_supports_bypass(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(-2); // bypass sentinel

    auto result = helper.unload_filament(-2);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    // No lane may be named: LANE=lane1 would make AFC select and unload bay 1,
    // which is empty, while the external spool stayed threaded through the head.
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC unload_filament sends bare TOOL_UNLOAD in single-extruder mode (no slot)",
                 "[ams][afc][toolchanger]") {
    helper.set_filament_loaded(true);

    auto result = helper.unload_active_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

TEST_CASE_METHOD(AfcFourRunningLanes,
                 "AFC unload_filament honors slot_index in single-extruder mode (#999)",
                 "[ams][afc][toolchanger]") {
    helper.set_filament_loaded(true);

    auto result = helper.unload_filament(3);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_UNSELECT_TOOL"));
}

// LED device actions (multi-extruder toolchanger mode)

TEST_CASE("AFC get_device_actions includes per-extruder LED toggles in toolchanger mode",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto actions = helper.get_device_actions();

    // Should have per-extruder LED actions
    bool found_t0 = false, found_t1 = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder_T0")
            found_t0 = true;
        if (a.id == "led_extruder_T1")
            found_t1 = true;
    }
    REQUIRE(found_t0);
    REQUIRE(found_t1);

    // Should NOT have the generic led_extruder action
    bool found_generic = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder")
            found_generic = true;
    }
    REQUIRE_FALSE(found_generic);
}

TEST_CASE("AFC per-tool device actions carry the tool as data, not in the label",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto find = [](const std::vector<helix::printer::DeviceAction>& actions,
                   const std::string& id) -> const helix::printer::DeviceAction* {
        for (const auto& a : actions) {
            if (a.id == id) {
                return &a;
            }
        }
        return nullptr;
    };

    const auto actions = helper.get_device_actions();
    for (int t = 0; t < 2; ++t) {
        const std::string suffix = "_T" + std::to_string(t);
        const struct {
            const char* id;
            const char* label;
        } expected[] = {{"tool_stn", "Sensor to Nozzle"},
                        {"tool_stn_unload", "Unload Distance"},
                        {"tool_sensor_after_extruder", "Post-Sensor Clear"},
                        {"led_extruder", "Turn On Toolhead LED"}};
        for (const auto& e : expected) {
            const auto* a = find(actions, e.id + suffix);
            INFO(e.id << suffix);
            REQUIRE(a != nullptr);
            CHECK(a->label == e.label);
            CHECK(a->tool_index == t);
        }
    }

    // The LED label names the action the tap performs next.
    REQUIRE(helper.execute_device_action("led_extruder_T1").success());
    const auto after = helper.get_device_actions();
    REQUIRE(find(after, "led_extruder_T1") != nullptr);
    CHECK(find(after, "led_extruder_T1")->label == "Turn Off Toolhead LED");
    CHECK(find(after, "led_extruder_T1")->icon == "light_off");
    CHECK(find(after, "led_extruder_T0")->label == "Turn On Toolhead LED");
    CHECK(find(after, "led_extruder_T0")->icon == "lightbulb_on");
}

TEST_CASE("AFC get_device_actions does not include LED toggles in single-extruder mode",
          "[ams][afc][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto actions = helper.get_device_actions();

    bool found_per_extruder = false;
    for (const auto& a : actions) {
        if (a.id.rfind("led_extruder_T", 0) == 0)
            found_per_extruder = true;
    }
    REQUIRE_FALSE(found_per_extruder);
}

// Each head's LED toggles through AFC_SET_EXTRUDER_LED on that head's extruder,
// starting on; a T number past the heads is refused.
TEST_CASE("AFC led_extruder_T<n> toggles that head's LED", "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    SECTION("T0 on, then off") {
        REQUIRE(helper.execute_device_action("led_extruder_T0"));
        CHECK(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));
        helper.clear_captured_gcodes();
        REQUIRE(helper.execute_device_action("led_extruder_T0"));
        CHECK(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=0"));
    }
    SECTION("T1") {
        REQUIRE(helper.execute_device_action("led_extruder_T1"));
        CHECK(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder1 TURN_ON=1"));
    }
    SECTION("a head that does not exist") {
        CHECK_FALSE(helper.execute_device_action("led_extruder_T5"));
        CHECK(helper.captured_gcodes.empty());
    }
}
