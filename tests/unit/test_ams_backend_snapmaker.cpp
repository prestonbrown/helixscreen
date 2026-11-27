// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 47 of test_ams_backend_snapmaker.cpp before it was split, in its order. Mostly
// [unload][action_state], [general], [channel_state], [action_state],
// [channel_state][unload][parked], and more

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

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker type enum", "[ams][snapmaker]") {
    REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker detection via filament_detect", "[ams][snapmaker]") {
    PrinterDiscovery discovery;

    SECTION("filament_detect triggers SNAPMAKER detection") {
        nlohmann::json objects = nlohmann::json::array(
            {"extruder", "extruder1", "extruder2", "extruder3", "toolchanger", "filament_detect",
             "toolhead", "heater_bed", "print_task_config"});
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_snapmaker());
        REQUIRE(discovery.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("empty toolchanger without filament_detect is not SNAPMAKER") {
        nlohmann::json objects =
            nlohmann::json::array({"extruder", "toolchanger", "tool T0", "tool T1", "toolhead"});
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_snapmaker());
        REQUIRE(discovery.has_tool_changer());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "AmsBackendSnapmaker construction", "[ams][snapmaker]") {
    SECTION("type returns SNAPMAKER") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == AmsType::SNAPMAKER);
    }

    SECTION("topology is PARALLEL") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("name is Snapmaker SnapSwap") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.type_name == "Snapmaker SnapSwap");
    }

    SECTION("has 4 slots in 1 unit") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.total_slots == 4);
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("tool_to_slot_map is 1:1 identity (gates 2D toolpath colors)") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }

    SECTION("tip_method is NONE (U1 has no cutter; unload is heat + retract)") {
        // Drives the unload/swap stepper to omit the "Cut & retract" tip step.
        // Default TipMethod is CUT; the backend must override it to NONE.
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.tip_method == TipMethod::NONE);
    }

    SECTION("no bypass for the override to reach: each head has its own path") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        CHECK_FALSE(info.supports_bypass);
        CHECK_FALSE(helix::bypass_override_offered(info));
    }
}

// The U1's own sequence homes and heats
//
// FEED_AUTO walks load_homing -> load_feeding -> load_heating -> ... and
// unload_homing -> unload_picking -> unload_heating -> ..., setting its own
// nozzle target from the lane's material. A UI preheat or G28 in front of it
// is a second wait the firmware repeats anyway.

TEST_CASE_METHOD(SnapmakerFixture, "The U1 homes and heats for its own loads",
                 "[ams][snapmaker][homing][preheat]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    helix::ui::FilamentOpPlan load;
    load.tier = helix::ui::FilamentTier::AmsBackend;
    load.ams_call = helix::ui::AmsCall::Load;
    load.ams_arg = 1;
    CHECK(helix::ui::preheat_skip_reason(load, StandardMacroSlot::LoadFilament, &backend) ==
          helix::ui::PreheatSkip::BackendSelfHeats);
    CHECK_FALSE(helix::ui::needs_prerequisite_home(load, StandardMacroSlot::LoadFilament, &backend,
                                                   /*toolhead_homed=*/false));

    helix::ui::FilamentOpPlan unload = load;
    unload.ams_call = helix::ui::AmsCall::Unload;
    CHECK_FALSE(helix::ui::needs_prerequisite_home(unload, StandardMacroSlot::UnloadFilament,
                                                   &backend, /*toolhead_homed=*/false));
}

// Feeder / Toolhead noun tests

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker ToolInfo::display_label names the toolhead, not the feeder",
                 "[ams][snapmaker][numbering]") {
    // Registering the real backend (not a mock) is the point: active_tool_noun()
    // reads it through AmsState, exactly as production init_tools() does, so
    // this exercises the actual lookup path rather than asserting the enum
    // value in isolation.
    AmsState::instance().set_backend(std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr));

    helix::PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3", "toolchanger",
                               "filament_detect", "toolhead", "heater_bed", "print_task_config"}));
    REQUIRE(hw.has_snapmaker());

    helix::ToolState::instance().deinit_subjects();
    helix::ToolState::instance().init_subjects(/*register_xml=*/false);
    helix::ToolState::instance().init_tools(hw);

    REQUIRE(helix::ToolState::instance().tool_count() == 4);
    const auto& tools = helix::ToolState::instance().tools();
    CHECK(tools[0].display_label == "Toolhead 1");
    CHECK(tools[0].display_label != "Feeder 1");
    CHECK(tools[3].display_label == "Toolhead 4");

    helix::ToolState::instance().deinit_subjects();
    AmsState::instance().clear_backends();
}

// Operation Step Model Tests

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_operation_step_model is the per-direction firmware sequence",
                 "[ams][snapmaker][stepmodel]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SECTION("LOAD is a 5-step model ending in Purge with a live-temp Heat step") {
        auto model = operation_step_model(backend, StepOperationType::LOAD_FRESH);
        // filament_feed.py sets load_homing, load_picking, load_feeding,
        // load_heating, load_extruding, load_flushing in that order.
        const char* const labels[] = {"Home",        "Select",  "Feed filament",
                                      "Heat nozzle", "Extrude", "Purge"};
        REQUIRE(model.steps.size() == 6);
        for (size_t i = 0; i < 6; ++i) {
            CHECK(model.steps[i].label == labels[i]);
            // phase_id mirrors the firmware operation_phase index the classifier emits.
            CHECK(model.steps[i].phase_id == static_cast<int>(i));
            // Only the Heat step shows a live nozzle temperature.
            CHECK(model.steps[i].live_temp == (i == 3));
        }
    }

    SECTION("LOAD_SWAP also produces the 5-step load-direction model") {
        auto model = backend.get_operation_step_model(StepOperationType::LOAD_SWAP);
        REQUIRE(model.steps.size() == 6);
        CHECK(std::string(model.steps[2].label) == "Feed filament");
        CHECK(std::string(model.steps[5].label) == "Purge");
    }

    SECTION("UNLOAD is a 4-step model ending in Retract") {
        auto model = backend.get_operation_step_model(StepOperationType::UNLOAD);
        REQUIRE(step_labels(model) ==
                std::vector<std::string>{"Home", "Select", "Heat nozzle", "Retract"});
        CHECK(model.steps[0].phase_id == 0);
        CHECK(model.steps[3].phase_id == 3);
        CHECK(model.steps[2].live_temp);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_operation_step_index_subject is the firmware phase subject",
                 "[ams][snapmaker][stepmodel]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    // The U1 drives the current step from the firmware Home/Select/Heat/Move
    // phase, surfaced via AmsState's operation_phase subject (static singleton).
    for (auto op :
         {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP, StepOperationType::UNLOAD}) {
        CHECK(operation_step_index_subject(backend, op) ==
              AmsState::instance().get_ams_operation_phase_subject());
    }
}

// Filament Operation Command Tests

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load routes through AUTO_FEEDING LOAD=1",
                 "[ams][snapmaker][load]") {
    // The mirror of the unload contract below: pinned so a UI-layer change cannot
    // silently swap this command for `T{n}`.
    //
    // FEED_AUTO silent-returns when passed no LOAD/UNLOAD parameter, and `T{n}`
    // only seats the carriage without feeding, so both wrong answers look like
    // "the button did nothing". See load_filament()'s trail-of-bad-guesses note.
    SECTION("explicit slot emits AUTO_FEEDING EXTRUDER=n LOAD=1") {
        helix::test::StartedBackend<CapturingSnapmakerBackend> backend;
        auto err = backend.load_filament(2);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 LOAD=1");
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker never routes a load through a tool change",
                 "[ams][snapmaker][load][dispatch]") {
    // PARALLEL topology: four toolheads, each with its own extruder, sharing no
    // path to a nozzle. The base needs_unload_before_load() rule is SERIAL —
    // "clear the shared path first" — and is true here essentially always,
    // because current_slot is assigned from toolhead.extruder and a tool is
    // always picked up.
    //
    // Answering true routes a load through change_tool() -> `T{n}`, which seats
    // the carriage and feeds nothing. That is what AmsOperationSidebar has been
    // dispatching, and what plan_load() would make the filament panel dispatch
    // too. AUTO_FEEDING already targets an arbitrary extruder directly.
    helix::test::StartedBackend<CapturingSnapmakerBackend> backend;

    AmsSystemInfo seated = backend.get_system_info();
    seated.filament_loaded = true;
    seated.current_slot = 0;
    CHECK_FALSE(backend.needs_unload_before_load(seated, /*target_slot=*/1));

    // And the command that would have been sent instead is the known-bad one,
    // so this stays a real distinction rather than two spellings of one thing.
    backend.captured_gcodes.clear();
    REQUIRE(backend.change_tool(1).success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    CHECK(backend.captured_gcodes[0] == "T1");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unload routes through AUTO_FEEDING UNLOAD=1",
                 "[ams][snapmaker][unload]") {
    // INNER_FILAMENT_UNLOAD is the leaf macro the firmware's feed state machine
    // calls internally; emitting it directly skips the state transitions that
    // aftermarket feeders (e.g. the U1-Ace ACE Pro mod) hook to retract their
    // spool. Unload must mirror load and go through AUTO_FEEDING so the channel
    // reaches "unload_finish". See prestonbrown/helixscreen#974.

    SECTION("explicit slot emits AUTO_FEEDING EXTRUDER=n UNLOAD=1") {
        helix::test::StartedBackend<CapturingSnapmakerBackend> backend;
        auto err = backend.unload_filament(2);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 UNLOAD=1");
    }

    SECTION("unload_active_filament dispatches the loaded slot's extruder") {
        // Contract: unload_active_filament() reads current_slot from system_info_
        // (single source of truth in the base class) and forwards it. This is
        // the path the Filament panel's Unload button takes.
        helix::test::StartedBackend<CapturingSnapmakerBackend> backend;
        backend.set_loaded_slot(1);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1");
    }

    SECTION("unload_active_filament with T3 loaded dispatches EXTRUDER=3 (U1 field bug)") {
        // With T3 loaded, Unload on the Filament panel must send EXTRUDER=3, not
        // EXTRUDER=0 from a stale current_slot (the firmware would visit T0
        // first). unload_active_filament() resolves current_slot ONCE in the base
        // class — the same snapshot the UI's "is anything loaded?" guard uses —
        // so the two cannot diverge.
        helix::test::StartedBackend<CapturingSnapmakerBackend> backend;
        backend.set_loaded_slot(3);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
        // Explicit guard against the bug returning: must NOT touch T0.
        REQUIRE(backend.captured_gcodes[0].find("EXTRUDER=0") == std::string::npos);
    }

    SECTION("unload_active_filament with no active slot falls back to bare INNER_FILAMENT_UNLOAD") {
        // current_slot = -1 (no tool picked up). The base helper forwards -1 to
        // the backend override, which keeps its "trust the firmware" behavior
        // (bare leaf macro, no AUTO_FEEDING state machine).
        helix::test::StartedBackend<CapturingSnapmakerBackend>
            backend; // current_slot defaults to -1
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "INNER_FILAMENT_UNLOAD");
    }
}

// can_unload_from_toolhead — the U1 is a 4-toolhead machine; every tool that
// holds filament must offer per-slot Unload, not just the single active one.
// The base AmsBackend gates Unload on SlotStatus::LOADED (one active tool),
// which hid the per-slot Unload action for toolheads 2/3/4 and forced users
// onto the sidebar's active-slot Unload button — which always unloads toolhead
// 0. (U1 multi-toolhead field report.)
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker can_unload_from_toolhead offers unload for every loaded toolhead",
                 "[ams][snapmaker][unload]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Slot 0 active (LOADED), slots 1 & 3 hold filament (AVAILABLE), slot 2 empty.
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}}, // active tool = slot 0
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, true})}}}};
    backend.fire_status(status);

    // Loaded state is keyed off channel_state now: slots 0/1/3 report load_finish
    // (filament at the toolhead), slot 2 reports wait_insert (empty channel).
    json feed = json{
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}},
              {"extruder1", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}},
        {"filament_feed right",
         json{{"extruder2", json{{"filament_detected", false}, {"channel_state", "wait_insert"}}},
              {"extruder3", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}}};
    backend.fire_status(feed);

    // Sanity: the status drove the slot states the gate keys on.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.get_slot_info(1).status == SlotStatus::AVAILABLE);
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(3).status == SlotStatus::AVAILABLE);

    SECTION("active (LOADED) toolhead is unloadable") {
        CHECK(backend.can_unload_from_toolhead(0));
    }
    SECTION("non-active toolheads holding filament are unloadable (the fix)") {
        CHECK(backend.can_unload_from_toolhead(1));
        CHECK(backend.can_unload_from_toolhead(3));
    }
    SECTION("empty toolhead offers no unload") {
        CHECK_FALSE(backend.can_unload_from_toolhead(2));
    }
}

// can_unload_from_toolhead answers "loaded to the nozzle", not merely present in
// the buffer. After an unload filament_exist stays true (slot AVAILABLE) and the
// toolhead switch e{N}_filament stays true too, since the retracted tip is still
// in the toolhead, but channel_state reports unload_finish. The channel_state
// latch answers it; the switch answers slot_filament_parked_in_toolhead().
TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker can_unload_from_toolhead requires filament at the toolhead, not just buffer",
    "[ams][snapmaker][unload]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // All four tools loaded to their toolhead (channel_state load_finish).
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    backend.fire_status(status);
    for (int e = 0; e < 4; ++e) {
        backend.fire_status(make_feed_status(e, "load_finish"));
    }

    // Every loaded tool is unloadable.
    REQUIRE(backend.can_unload_from_toolhead(2));

    SECTION("a tool unloaded to the buffer (channel_state unload_finish) is NOT unloadable") {
        // Post-unload: the port still reads filament (detected=true) and
        // channel_state reports unload_finish, so the loaded latch clears.
        backend.fire_status(make_feed_status(2, "unload_finish", true));
        CHECK_FALSE(backend.can_unload_from_toolhead(2));
        // Tools still loaded at their toolhead remain unloadable.
        CHECK(backend.can_unload_from_toolhead(1));
        CHECK(backend.can_unload_from_toolhead(3));
    }
}

// Lane presence gates every presence-keyed affordance (Unload, the batch
// picker's pre-tick). filament_detect.state is the entrance/tag reader: with
// all four lanes loaded it reads [0,1,0,1] — 0 for exactly the heads whose
// filament has been fed THROUGH to the toolhead — while the port sensor
// (filament_feed .extruderN.filament_detected) reads true on all four and
// channel_state alternates load_finish/preload_finish. Presence must therefore
// resolve from the port sensor OR the loaded-at-toolhead latch, never from
// the tag reading.
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker lane presence comes from the port sensor and load latch, not "
                 "filament_detect.state",
                 "[ams][snapmaker]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Heads 0/2 fed to the toolhead (load_finish); heads 1/3 preloaded short
    // of the gear (preload_finish, latch clear). Port sensor sees filament on
    // 0-2; head 3 reads port=false so its presence rides the latch alone.
    json feed = json{
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}},
              {"extruder1",
               json{{"filament_detected", true}, {"channel_state", "preload_finish"}}}}},
        {"filament_feed right",
         json{
             {"extruder2", json{{"filament_detected", true}, {"channel_state", "load_finish"}}},
             {"extruder3", json{{"filament_detected", false}, {"channel_state", "load_finish"}}}}}};
    backend.fire_status(feed);

    // The entrance/tag reading: 0 for the two heads fed through it.
    backend.fire_status(json{{"filament_detect", json{{"state", json::array({0, 1, 0, 1})}}}});

    SECTION("a head fed through to the toolhead (tag reading 0) still holds filament") {
        CHECK(backend.get_slot_info(0).is_present());
        CHECK(backend.get_slot_info(0).status != SlotStatus::EMPTY);
        CHECK(backend.get_slot_info(2).is_present());
        // The user-visible consequence: Unload is offered for a loaded head.
        CHECK(backend.can_unload_from_toolhead(0));
        CHECK(backend.can_unload_from_toolhead(3));
    }

    SECTION("a preloaded head (latch clear) rides the port sensor") {
        CHECK(backend.get_slot_info(1).is_present());
        CHECK(backend.get_slot_info(1).status != SlotStatus::EMPTY);
    }

    SECTION("a lane with neither signal present reads EMPTY") {
        backend.fire_status(make_feed_status(1, "wait_insert", false));
        CHECK_FALSE(backend.get_slot_info(1).is_present());
        CHECK(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }

    SECTION("a delta frame carrying no presence fields leaves the last reading standing") {
        backend.fire_status(json{{"toolhead", json{{"extruder", "extruder"}}}});
        CHECK(backend.get_slot_info(0).is_present());
        CHECK(backend.get_slot_info(0).status != SlotStatus::EMPTY);
    }
}

// The firmware reports channel_error "no_filament" for every lane sitting empty,
// including a lane left unloaded on purpose for a multi-colour print. That idle
// empty lane must not latch the backend into Error. A lane mid-load, or the
// active lane, still raises it.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker channel_error raises Error only on a lane in use",
                 "[ams][snapmaker]") {
    const auto lane = [](bool detected, const char* state, const char* error) {
        return json{
            {"filament_detected", detected}, {"channel_state", state}, {"channel_error", error}};
    };
    struct Row {
        const char* name;
        int active; // current slot (-1 = none)
        json feed;
        bool error;
        const char* detail; // exact; "" = empty; nullptr = contains "Feeder 2: No filament"
    };
    const Row rows[] = {
        {"an idle empty lane between two loaded ones", -1,
         json{{"extruder0", lane(true, "inited", "ok")},
              {"extruder1", lane(false, "wait_insert", "no_filament")},
              {"extruder2", lane(true, "inited", "ok")}},
         false, ""},
        // Mapped to authored copy spelled with lane_label() ("Feeder N").
        {"a lane mid-load", -1, json{{"extruder1", lane(false, "load_heating", "no_filament")}},
         true, nullptr},
        // An unknown token passes through so a novel error is never hidden.
        {"the active lane while idle", 0, json{{"extruder0", lane(false, "inited", "some_fault")}},
         true, "some_fault"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        if (row.active >= 0)
            SnapmakerTestAccess::set_current_slot(backend, row.active);
        backend.fire_status(json{{"filament_feed left", row.feed}});

        const auto info = backend.get_system_info();
        CHECK((info.action == AmsAction::ERROR) == row.error);
        if (row.detail == nullptr)
            CHECK(info.operation_detail.find("Feeder 2: No filament") != std::string::npos);
        else
            CHECK(info.operation_detail == row.detail);
        if (row.detail == nullptr) {
            // A standing fault re-words on a language switch with no new frame.
            ScopedLanguage fr("fr");
            backend.refresh_translated_texts();
            CHECK(backend.get_system_info().operation_detail.find("Pas de filament") !=
                  std::string::npos);
        }
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker steps project Heat and Purge by kind, positioning by direction",
                 "[ams][snapmaker][coarse]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    const auto load = backend.get_operation_step_model(StepOperationType::LOAD_FRESH);
    for (int i = 0; i < 6; ++i) {
        CHECK(load.action_at(i) == (i == 3   ? AmsAction::HEATING
                                    : i == 5 ? AmsAction::PURGING
                                             : AmsAction::LOADING));
    }
    CHECK_FALSE(load.action_at(-1).has_value());
    CHECK_FALSE(load.action_at(6).has_value());

    const auto unload = backend.get_operation_step_model(StepOperationType::UNLOAD);
    CHECK(unload.action_at(0) == AmsAction::UNLOADING);
    CHECK(unload.action_at(1) == AmsAction::UNLOADING);
    CHECK(unload.action_at(2) == AmsAction::HEATING);
    CHECK(unload.action_at(3) == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker publishes the action its current step projects",
                 "[ams][snapmaker][coarse]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    auto frame = [&](const char* state) {
        backend.fire_status(
            json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                                {"channel_state", state},
                                                                {"channel_error", "ok"}}}}}});
        return backend.get_system_info();
    };

    SECTION("load") {
        CHECK(frame("load_homing").action == AmsAction::LOADING);
        CHECK(frame("load_feeding").action == AmsAction::LOADING);
        CHECK(frame("load_heating").action == AmsAction::HEATING);
        CHECK(frame("load_extruding").action == AmsAction::LOADING);
        CHECK(frame("load_flushing").action == AmsAction::PURGING);
        CHECK(frame("load_finish").action == AmsAction::IDLE);
    }
    SECTION("unload") {
        CHECK(frame("unload_picking").action == AmsAction::UNLOADING);
        CHECK(frame("unload_heating").action == AmsAction::HEATING);
        CHECK(frame("unload_doing").action == AmsAction::UNLOADING);
        CHECK(frame("unload_finish").action == AmsAction::IDLE);
    }
    SECTION("a fault on the Heat step reads ERROR") {
        REQUIRE(frame("load_heating").action == AmsAction::HEATING);
        backend.fire_status(json{
            {"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                           {"channel_state", "load_heating"},
                                                           {"channel_error", "heat_timeout"}}}}}});
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unload_finish resolves UNLOADING to IDLE",
                 "[ams][snapmaker][unload]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Mid-unload: action becomes UNLOADING.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_doing"},
                                                            {"channel_error", "ok"}}}}}};
    backend.fire_status(doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    // unload_finish is the TRUE end of the operation → IDLE.
    json finish =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    backend.fire_status(finish);
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker preload_finish is NOT matched by the load_ prefix branch",
                 "[ams][snapmaker][unload]") {
    // preload_finish starts with "preload_", NOT "load_" — it must reach the
    // existing preload_finish handler (which leaves the action alone) rather than
    // being misread as a granular load sub-state.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Start mid-unload so there is an action to (incorrectly) clobber.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_doing"},
                                                            {"channel_error", "ok"}}}}}};
    backend.fire_status(doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    json preload =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "preload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    backend.fire_status(preload);
    // preload_finish leaves the in-progress action alone (does NOT become LOADING
    // and does NOT resolve to IDLE — only unload_finish/load_finish/idle do).
    CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker unload that settles in one frame ends on channel_action_state",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("spool out: wait_insert") {
        backend.fire_status(
            feed_with_action(3, "wait_insert", "unload_finish", /*detected=*/false));
    }
    SECTION("spool in: preload_finish") {
        backend.fire_status(feed_with_action(3, "preload_finish", "unload_finish"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
    CHECK(AmsState::instance().was_slot_recently_unloaded(3));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed unload that settles in one frame raises the error",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.fire_status(feed_with_action(3, "preload_finish", "unload_fail"));
    CHECK(backend.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unload mid-heat at preload_finish keeps UNLOADING until it ends",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(feed_with_action(3, "unload_prepare", "unload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    for (const char* step : {"unload_heating", "unload_heat_finish", "unload_doing"}) {
        backend.fire_status(feed_with_action(3, "preload_finish", step));
        INFO("channel_action_state=" << step);
        CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
    }

    backend.fire_status(feed_with_action(3, "preload_finish", "unload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed op from before startup raises nothing on the first frame",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    backend.fire_status(feed_with_action(3, "preload_finish", "load_fail"));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a terminal channel_state on the first frame fires no unload event",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    SECTION("unload_finish left over from before startup") {
        backend.fire_status(feed_with_action(3, "unload_finish", "unload_finish"));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

        SECTION("a full frame repeating it is still not an unload") {
            backend.fire_status(feed_with_action(3, "unload_finish", "unload_finish"));
            CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
        }
        SECTION("a real unload afterwards still marks the slot") {
            backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
            REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
            backend.fire_status(feed_with_action(3, "unload_finish", "unload_finish"));
            CHECK(backend.get_system_info().action == AmsAction::IDLE);
            CHECK(AmsState::instance().was_slot_recently_unloaded(3));
        }
    }
    SECTION("load_finish left over from before startup") {
        backend.fire_status(feed_with_action(3, "load_finish", "load_finish"));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Snapmaker a stale terminal on another lane does not end the op",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    auto& cooldown = PostOpCooldownManager::instance();
    cooldown.init();
    cooldown.cancel();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE_FALSE(cooldown.has_pending_timer());

    // One frame, two lanes: lane 1 mid-load, lane 4 resting at a terminal it
    // reached before this backend started watching.
    json frame = feed_with_action(0, "load_feeding", "load_feeding");
    frame.merge_patch(feed_with_action(3, "unload_finish", "unload_finish"));
    backend.fire_status(frame);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
    CHECK_FALSE(cooldown.has_pending_timer());

    SECTION("the load's own terminal still ends it and arms the cooldown") {
        backend.fire_status(feed_with_action(0, "load_finish", "load_finish"));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(cooldown.has_pending_timer());
    }
    SECTION("a real unload on lane 4 still ends and arms the cooldown") {
        backend.fire_status(feed_with_action(0, "load_finish", "load_finish"));
        cooldown.cancel();
        backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        backend.fire_status(feed_with_action(3, "unload_finish", "unload_finish"));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(cooldown.has_pending_timer());
    }
    cooldown.cancel();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a spool-insert preload ends at preload_finish",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(make_feed_status(1, "preload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SECTION("every step reported") {
        backend.fire_status(make_feed_status(1, "preload_feeding"));
        REQUIRE(backend.get_system_info().action == AmsAction::LOADING);
    }
    SECTION("preload_finish arrives straight from preload_prepare") {}

    backend.fire_status(make_feed_status(1, "preload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker the first frame after a Klipper restart ends no operation",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    // The backend object outlives a Klipper restart, so what it remembers of
    // each channel describes firmware that no longer exists. The subscription's
    // first full frame must be read as a fresh start, not compared against it.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.on_klipper_restarted();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    SECTION("a finished unload's action_state") {
        backend.fire_status(feed_with_action(3, "preload_finish", "unload_finish"));
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failed unload's action_state") {
        backend.fire_status(feed_with_action(3, "preload_finish", "unload_fail"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a Klipper restart leaves an ERROR in place",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    helix::test::StartedBackend<CapturingSnapmakerBackend> backend;

    backend.fire_status(make_feed_status(1, "load_feeding"));
    backend.fire_status(make_feed_status(1, "load_fail"));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    backend.on_klipper_restarted();

    CHECK(backend.get_current_action() == AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a previous op's channel_action_state never ends a new one",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // At rest after an earlier unload: the firmware still reports its terminal.
    backend.fire_status(feed_with_action(3, "preload_finish", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    // A new unload starts; this frame omits the unchanged channel_action_state.
    backend.fire_status(make_feed_status(3, "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SECTION("a delta omitting channel_action_state") {
        backend.fire_status(make_feed_status(3, "preload_finish"));
    }
    SECTION("a full frame repeating the held channel_action_state") {
        backend.fire_status(feed_with_action(3, "preload_finish", "unload_finish"));
    }

    CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an outcome that arrives before the channel settles ends the op",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("a finish") {
        backend.fire_status(feed_with_action(3, "unload_doing", "unload_finish"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        // The settling frame omits the unchanged channel_action_state.
        backend.fire_status(make_feed_status(3, "wait_insert", /*detected=*/false));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failure") {
        backend.fire_status(feed_with_action(3, "unload_doing", "unload_fail"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        backend.fire_status(make_feed_status(3, "wait_insert"));
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a channel that settles before its outcome arrives ends on the outcome",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.fire_status(make_feed_status(3, "wait_insert", false));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.fire_status(feed_with_action(3, "wait_insert", "unload_finish", false));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a held outcome is dropped when a new op starts",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(feed_with_action(3, "unload_doing", "unload_doing"));
    backend.fire_status(feed_with_action(3, "unload_doing", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.fire_status(feed_with_action(3, "load_prepare", "load_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    backend.fire_status(make_feed_status(3, "wait_insert"));
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
}

// get_slot_filament_segment — on the U1's PARALLEL multi-toolhead topology
// every tool loaded to its own dedicated nozzle renders all the way into the
// toolhead (NOZZLE); multiple tools can be loaded at once, each to its own
// nozzle. "Loaded to the nozzle" is the channel_state latch (load_finish), NOT
// the motion sensor. A tool whose feeder port sensor alone sees filament
// renders PREP (the tube down to its entry sensor); an empty lane renders
// nothing.
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_slot_filament_segment renders NOZZLE for every loaded tool",
                 "[ams][snapmaker][path]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    SECTION("active loaded tool renders into the toolhead") {
        backend.fire_status(make_feed_status(0, "load_finish"));
        CHECK(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }
    SECTION("multiple non-active loaded tools each render into their own toolhead") {
        backend.fire_status(make_feed_status(1, "load_finish"));
        backend.fire_status(make_feed_status(3, "load_finish"));
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NOZZLE);
        CHECK(backend.get_slot_filament_segment(3) == PathSegment::NOZZLE);
    }
    SECTION("empty tool renders no filament line") {
        SnapmakerTestAccess::set_sensor_present(backend, 2, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, false);
        backend.fire_status(make_feed_status(2, "wait_insert",
                                             /*filament_detected=*/false));
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }
    SECTION("a tool with neither sensor present (real runout) renders no line") {
        // Not loaded (latch defaults false), and both the toolhead motion sensor
        // AND the buffer/port sensor read empty → genuine runout / no filament.
        SnapmakerTestAccess::set_sensor_present(backend, 1, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 1, false);
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NONE);
    }
    SECTION("filament at the feeder (not loaded, port present) renders to the entry sensor") {
        // U1 post-unload state: filament retracted out of the toolhead but left
        // in the feed tube. Latch clear (unload_finish), toolhead motion sensor
        // reads empty, the feeder's port sensor still present → the lane reads
        // at its entry sensor (PREP), not nothing.
        SnapmakerTestAccess::set_sensor_present(backend, 1, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 1, true);
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::PREP);
    }
}

// The per-tool motion sensor does not drop after an unload on current firmware,
// so channel_state is the load signal: load_finish loads, and unload_finish,
// wait_insert and preload_finish (staged in the buffer) unload.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker channel_state sets the loaded latch",
                 "[ams][snapmaker][channel_state]") {
    struct Row {
        const char* name;
        bool start_loaded;
        const char* state;
        bool detected;
        bool loaded;
        std::optional<SlotStatus> status;
    };
    const Row rows[] = {
        {"load_finish loads", false, "load_finish", true, true, std::nullopt},
        // Captured on a U1 (20260608): the toolhead switch still reads present.
        {"unload_finish unloads with the switch still present", true, "unload_finish", true, false,
         SlotStatus::AVAILABLE},
        {"wait_insert means the channel is empty", true, "wait_insert", false, false, std::nullopt},
        {"preload_finish is present but not loaded", true, "preload_finish", true, false,
         SlotStatus::AVAILABLE},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        if (row.start_loaded) {
            backend.fire_status(make_feed_status(2, "load_finish"));
            REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
            SnapmakerTestAccess::set_sensor_present(backend, 2, true);
        }
        backend.fire_status(make_feed_status(2, row.state, row.detected));

        CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 2) == row.loaded);
        CHECK(backend.slot_has_filament_at_toolhead(2) == row.loaded);
        CHECK(backend.can_unload_from_toolhead(2) == row.loaded);
        if (row.status)
            CHECK(backend.get_slot_info(2).status == *row.status);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker unload_finish preserves current_slot for the picked-up tool",
                 "[ams][snapmaker][channel_state][unload]") {
    // Bart's U1 field report (Discord 2026-07-20): T3 picked up on the carriage,
    // channel_state=unload_finish (TPU loaded directly into the toolhead, bypassing
    // the feeder state machine). With current_slot=-1 reset on every unload_finish,
    // the Filament panel's Unload routed through the bare INNER_FILAMENT_UNLOAD
    // leaf macro (no tool specifier), and the firmware defaulted to T0.
    //
    // current_slot / current_tool track which toolhead is PICKED UP (authority:
    // toolhead.extruder, parsed at the top of handle_status). They are
    // independent of whether feeder filament is at the nozzle. Only
    // filament_loaded should flip on unload_finish.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Simulate T2 picked up (toolhead.extruder = "extruder2").
    json picked_up = json{{"toolhead", json{{"extruder", "extruder2"}}}};
    backend.fire_status(picked_up);
    REQUIRE(backend.get_system_info().current_slot == 2);
    REQUIRE(backend.get_system_info().current_tool == 2);

    // T2's feeder channel reaches unload_finish (filament retracted from nozzle,
    // but toolhead still mounted). filament_loaded must clear; current_slot must NOT.
    backend.fire_status(make_feed_status(2, "unload_finish",
                                         /*filament_detected=*/true));

    CHECK(backend.get_system_info().current_slot == 2);
    CHECK(backend.get_system_info().current_tool == 2);
    CHECK_FALSE(backend.get_system_info().filament_loaded);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker filament parked in the toolhead offers Unload and Load",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(merged({t3_picked(), t3_feed(), t3_switch(true)}));

    CHECK(answers(backend, 3) == LoadAnswers{/*filament_loaded=*/true, SlotStatus::AVAILABLE,
                                             /*actively=*/false,
                                             /*at_nozzle=*/false, /*parked=*/true});
    CHECK_FALSE(backend.can_unload_from_toolhead(3));

    // The single-slot executors resolve the unload target through this.
    CHECK(helix::ui::read_unload_target_loaded(&backend, backend.get_system_info(), 3));

    // Head picker: both directions offer head 3.
    const auto view = helix::ui::read_filament_head_view(&backend, get_printer_state());
    REQUIRE(view.heads.size() == 4);
    CHECK(helix::ui::head_can_unload(view.heads[3]));
    CHECK(helix::ui::head_can_load(view.heads[3]));
}

// A load that fails partway leaves filament fed into the head, sometimes a
// broken stub only an unload clears; the toolhead switch need not see it.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker head whose load failed with filament offers Unload",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    SECTION("load_fail with the lane holding filament") {
        backend.fire_status(make_feed_status(3, "load_fail", /*filament_detected=*/true,
                                             /*channel_error=*/"move_extrude"));

        CHECK(backend.slot_filament_parked_in_toolhead(3));
        CHECK_FALSE(backend.slot_has_filament_at_toolhead(3));
        CHECK(helix::ui::read_unload_target_loaded(&backend, backend.get_system_info(), 3));
        const auto view = helix::ui::read_filament_head_view(&backend, get_printer_state());
        REQUIRE(view.heads.size() == 4);
        CHECK(helix::ui::head_can_unload(view.heads[3]));
    }
    SECTION("load_fail on an empty lane leaves nothing to unload") {
        backend.fire_status(make_feed_status(3, "load_fail", /*filament_detected=*/false));
        CHECK_FALSE(backend.slot_filament_parked_in_toolhead(3));
    }
    SECTION("a later settled state clears it") {
        backend.fire_status(make_feed_status(3, "load_fail", true, "move_extrude"));
        backend.fire_status(make_feed_status(3, "unload_finish", true));
        CHECK_FALSE(backend.slot_filament_parked_in_toolhead(3));
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker loaded answers do not depend on frame order",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    const LoadAnswers parked{true, SlotStatus::AVAILABLE, false, false, true};

    SECTION("parked head, however the frames are split") {
        CHECK(fed({merged({t3_picked(), t3_feed(), t3_switch(true)})}) == parked);
        CHECK(fed({t3_picked(), t3_feed(), t3_switch(true)}) == parked);
        CHECK(fed({t3_feed(), t3_switch(true), t3_picked()}) == parked);
        CHECK(fed({t3_switch(true), t3_picked(), t3_feed()}) == parked);
    }
    SECTION("a pick with no switch reading says nothing is loaded") {
        const LoadAnswers empty{false, SlotStatus::AVAILABLE, false, false, false};
        CHECK(fed({t3_feed(), t3_picked()}) == empty);
        CHECK(fed({t3_picked(), t3_feed()}) == empty);
    }
    SECTION("load_finish on the picked head is loaded to the nozzle") {
        const auto loaded = make_feed_status(3, "load_finish", true);
        const LoadAnswers at_nozzle{true, SlotStatus::LOADED, true, true, false};
        CHECK(fed({t3_picked(), loaded, t3_switch(true)}) == at_nozzle);
        CHECK(fed({loaded, t3_switch(true), t3_picked()}) == at_nozzle);
    }
    SECTION("a reported runout on a loaded head drops the loaded readout") {
        const auto loaded = make_feed_status(3, "load_finish", true);
        const LoadAnswers runout{false, SlotStatus::AVAILABLE, false, true, false};
        CHECK(fed({t3_picked(), loaded, t3_switch(false)}) == runout);
        CHECK(fed({t3_switch(false), loaded, t3_picked()}) == runout);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker parked head draws its per-slot path to the nozzle",
                 "[ams][snapmaker][channel_state][parked]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(merged({t3_picked(), t3_feed(), t3_switch(true)}));
    REQUIRE(backend.get_system_info().filament_loaded);
    CHECK(backend.get_filament_segment() == PathSegment::NOZZLE);
    CHECK(backend.get_slot_filament_segment(3) == PathSegment::NOZZLE);

    backend.fire_status(t3_switch(false));
    CHECK(backend.get_slot_filament_segment(3) == PathSegment::PREP);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker feeder port sensor is each lane's entry sensor",
                 "[ams][snapmaker][path][entry_sensor]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    for (int i = 0; i < 4; i++)
        CHECK(backend.slot_has_prep_sensor(i));
    CHECK_FALSE(backend.slot_has_prep_sensor(4));
    CHECK_FALSE(backend.slot_has_prep_sensor(-1));

    SECTION("with no tool on the carriage the system path reads nothing") {
        CHECK(backend.get_filament_segment() == PathSegment::NONE);
    }
    SECTION("the mounted tool reports its own lane") {
        backend.fire_status(merged({t3_picked(), t3_feed(), t3_switch(false)}));
        CHECK(backend.get_filament_segment() == PathSegment::PREP);
        backend.fire_status(t3_switch(true));
        CHECK(backend.get_filament_segment() == PathSegment::NOZZLE);
    }
    SECTION("an empty mounted lane reads nothing") {
        backend.fire_status(
            merged({t3_picked(), make_feed_status(3, "wait_insert", false), t3_switch(false)}));
        SnapmakerTestAccess::set_port_sensor_present(backend, 3, false);
        CHECK(backend.get_slot_filament_segment(3) == PathSegment::NONE);
        CHECK(backend.get_filament_segment() == PathSegment::NONE);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a head whose feeder ran empty reads empty",
                 "[ams][snapmaker][empty_port]") {
    // The mock U1's full frame after head 2's spool left its port, as the real
    // U1 reports it: filament_detected false at wait_insert, filament_exist false.
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    helix::ScopedEnv u1{"HELIX_MOCK_PRINTER", "snapmaker_u1"};
    helix::ScopedEnv empty{"HELIX_MOCK_U1_EMPTY_PORTS", "2"};
    MoonrakerClientMock u1_mock(MoonrakerClientMock::PrinterType::SNAPMAKER_U1, 100.0);
    const json status = u1_mock.u1_snapswap_status_json();
    REQUIRE(status["filament_feed right"]["extruder2"]["channel_state"] == "wait_insert");
    REQUIRE(status["print_task_config"]["filament_exist"][2] == false);

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(status);
    CHECK(backend.get_slot_info(2).status == SlotStatus::EMPTY);
    CHECK(backend.get_slot_info(1).status != SlotStatus::EMPTY);
    // The head leaves the carriage and parks again (a tool change past it):
    // the port has not refilled, so the lane is still empty.
    backend.fire_status(json{{"extruder2", json{{"active_pin", true}, {"park_pin", false}}}});
    backend.fire_status(json{{"extruder2", json{{"active_pin", false}, {"park_pin", true}}}});
    CHECK(backend.get_slot_info(2).status == SlotStatus::EMPTY);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker active pin alone does not mark a tool loaded",
                 "[ams][snapmaker][channel_state][parked]") {
    // The frame's active_pin names tool 1 while toolhead.extruder names tool 3,
    // and tool 1's latch is clear: tool 1 is not loaded.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(
        merged({json{{"extruder1", json{{"active_pin", true}, {"park_pin", false}}}}, t3_picked(),
                make_feed_status(1, "preload_finish", true), t3_feed()}));
    REQUIRE(backend.get_system_info().current_tool == 3);
    CHECK(backend.get_slot_info(1).status != SlotStatus::LOADED);
    CHECK_FALSE(backend.slot_is_actively_loaded(1));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unloaded head with a clear switch offers only Load",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(
        merged({t3_picked(), make_feed_status(3, "unload_finish", true), t3_switch(false)}));

    CHECK(answers(backend, 3) == LoadAnswers{false, SlotStatus::AVAILABLE, false, false, false});
    const auto view = helix::ui::read_filament_head_view(&backend, get_printer_state());
    REQUIRE(view.heads.size() == 4);
    CHECK_FALSE(helix::ui::head_can_unload(view.heads[3]));
    CHECK(helix::ui::head_can_load(view.heads[3]));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker only a finished unload clears the latch, not its steps or a failure",
                 "[ams][snapmaker][channel_state]") {
    // A failed unload leaves the filament at the toolhead, and every step of a
    // running unload keeps "loaded" until unload_finish lands.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(make_feed_status(3, "load_finish"));
    REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));

    for (const char* state : {"unload_fail", "unload_prepare", "unload_homing", "unload_picking",
                              "unload_heating", "unload_heat_finish", "unload_doing"}) {
        backend.fire_status(make_feed_status(3, state));
        INFO("state=" << state);
        CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));
    }

    backend.fire_status(make_feed_status(3, "unload_finish"));
    CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker step bar never runs backwards through a real operation",
                 "[ams][snapmaker][channel_state][phase]") {
    // Sequences recorded on a U1 (filament_feed channel_state, one entry per
    // change). A load whose head is already picked skips load_picking.
    const std::vector<std::vector<const char*>> sequences = {
        {"load_homing", "load_feeding", "load_heating", "load_extruding", "load_flushing"},
        {"load_homing", "load_picking", "load_feeding", "load_heating", "load_extruding",
         "load_flushing"},
        {"unload_homing", "unload_picking", "unload_heating", "unload_doing"},
        {"manual_sta_prepare", "manual_sta_homing", "manual_sta_picking",
         "manual_sta_prepare_finish", "manual_sta_heating", "manual_sta_extruding",
         "manual_sta_extrude_finish", "manual_sta_flushing", "manual_sta_flush_finish"},
        {"preload_prepare", "preload_feeding"},
    };
    for (const auto& seq : sequences) {
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        int prev = -1;
        for (const char* state : seq) {
            backend.fire_status(make_feed_status(2, state));
            const int phase = backend.get_system_info().operation_phase;
            INFO("state=" << state << " phase=" << phase << " prev=" << prev);
            CHECK(phase >= prev);
            prev = phase;
        }
    }
}

// Full 39-state channel_state coverage (action + operation_phase)
// Every firmware channel_state (filament_feed.py:34-72, firmware 20260608) maps
// to the correct AmsAction and step index. Driven off the single
// classify_channel_state() table in the backend.

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker every channel_state maps to the right action and phase",
                 "[ams][snapmaker][channel_state]") {
    struct Case {
        const char* state;
        AmsAction action;
        int phase;
    };
    const Case cases[] = {
        // idle / init
        {"none", AmsAction::IDLE, -1},
        {"inited", AmsAction::IDLE, -1},
        {"wait_insert", AmsAction::IDLE, -1},
        {"test", AmsAction::IDLE, -1},
        // preload
        {"preload_prepare", AmsAction::LOADING, 0},
        {"preload_feeding", AmsAction::LOADING, 2},
        {"preload_finish", AmsAction::IDLE, -1},
        {"preload_fail", AmsAction::ERROR, -1},
        // load
        {"load_prepare", AmsAction::LOADING, 0},
        {"load_homing", AmsAction::LOADING, 0},
        {"load_picking", AmsAction::LOADING, 1},
        {"load_feeding", AmsAction::LOADING, 2},
        {"load_heating", AmsAction::HEATING, 3},
        {"load_extruding", AmsAction::LOADING, 4},
        {"load_flushing", AmsAction::PURGING, 5},
        {"load_finish", AmsAction::IDLE, -1},
        {"load_fail", AmsAction::ERROR, -1},
        // unload
        {"unload_prepare", AmsAction::UNLOADING, 0},
        {"unload_homing", AmsAction::UNLOADING, 0},
        {"unload_picking", AmsAction::UNLOADING, 1},
        {"unload_heating", AmsAction::HEATING, 2},
        {"unload_heat_finish", AmsAction::HEATING, 2},
        {"unload_doing", AmsAction::UNLOADING, 3},
        {"unload_finish", AmsAction::IDLE, -1},
        {"unload_fail", AmsAction::ERROR, -1},
        // manual feed
        {"manual_sta_prepare", AmsAction::LOADING, 0},
        {"manual_sta_homing", AmsAction::LOADING, 0},
        {"manual_sta_picking", AmsAction::LOADING, 1},
        {"manual_sta_prepare_finish", AmsAction::LOADING, 1},
        {"manual_sta_prepare_fail", AmsAction::ERROR, -1},
        {"manual_sta_heating", AmsAction::HEATING, 3},
        {"manual_sta_extruding", AmsAction::LOADING, 4},
        {"manual_sta_extrude_finish", AmsAction::LOADING, 4},
        {"manual_sta_extrude_fail", AmsAction::ERROR, -1},
        {"manual_sta_flushing", AmsAction::PURGING, 5},
        {"manual_sta_flush_finish", AmsAction::PURGING, 5},
        {"manual_sta_flush_fail", AmsAction::ERROR, -1},
        {"manual_sta_finish", AmsAction::IDLE, -1},
        {"manual_sta_fail", AmsAction::ERROR, -1},
    };

    for (const auto& c : cases) {
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        // Make extruder 0 the active lane so *_fail states surface as ERROR
        // (the false-alarm guard only suppresses errors on idle empty NON-active
        // lanes) and feed present filament.
        SnapmakerTestAccess::set_current_slot(backend, 0);
        SnapmakerTestAccess::set_current_tool(backend, 0);
        backend.fire_status(make_feed_status(0, c.state));
        INFO("channel_state=" << c.state);
        CHECK(backend.get_system_info().action == c.action);
        CHECK(backend.get_system_info().operation_phase == c.phase);

        // The same frame on lane 2 with no lane active: a present lane reads alike.
        helix::test::RegisteredWired<AmsBackendSnapmaker> idle_reg;
        idle_reg->fire_status(make_feed_status(2, c.state));
        INFO("on lane 2, none active");
        CHECK(idle_reg->get_system_info().action == c.action);
        CHECK(idle_reg->get_system_info().operation_phase == c.phase);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail channel_state raises ERROR with detail",
                 "[ams][snapmaker][channel_state]") {
    // A *_fail channel_state alone (channel_error still "ok") must surface an
    // error — Change 2 adds this on top of the existing channel_error handling.
    for (const char* fail : {"load_fail", "unload_fail", "preload_fail", "manual_sta_fail"}) {
        helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
        auto& backend = *backend_reg;
        SnapmakerTestAccess::set_current_slot(backend, 1);
        backend.fire_status(make_feed_status(1, fail));
        INFO("fail state=" << fail);
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
        CHECK_FALSE(backend.get_system_info().operation_detail.empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail on an idle empty non-active lane stays quiet",
                 "[ams][snapmaker][channel_state]") {
    // The multi-color false-alarm guard must still hold: a fail on a lane that
    // is empty, idle, and not active should not pop a spurious error modal.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 0); // active lane is 0, not 2
    backend.fire_status(make_feed_status(2, "preload_fail",
                                         /*filament_detected=*/false));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}
