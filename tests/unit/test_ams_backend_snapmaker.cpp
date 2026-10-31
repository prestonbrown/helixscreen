// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_batch_filament_modal.h"
#include "ui_insert_notice.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_insert_watcher.h"
#include "filament_op_execute.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "snapmaker_nozzle_offset.h"
#include "spoolman_types.h" // SpoolInfo + apply_spool_to_slot (the picker-side writer)
#include "test_helpers/registered_backend.h"
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

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

/// This file had no fixture at all, so nothing ever drained the UpdateQueue.
/// handle_status() publishes first-gate port presence through
/// AmsState::set_active_tool_port_present, which marshals to the main thread via
/// AsyncLifetimeGuard::defer — the single largest named producer in the
/// cross-test leak report (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects and the test's
/// backend are still alive, before HelixTestFixture's own teardown.
struct SnapmakerFixture : public HelixTestFixture {
    ~SnapmakerFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

// Build a filament_feed status notification for a single channel/extruder with
// a given channel_state (+ optional filament_detected / channel_error). Routes
// extruders 0,1 to "filament_feed left" and 2,3 to "filament_feed right" to
// mirror the U1 feed topology, though the backend reads either object for any
// extruder key. Shared by the channel_state coverage tests below.
static json make_feed_status(int extruder_idx, const std::string& channel_state,
                             bool filament_detected = true,
                             const std::string& channel_error = "ok") {
    const char* feed_key = (extruder_idx <= 1) ? "filament_feed left" : "filament_feed right";
    std::string ext_key = "extruder" + std::to_string(extruder_idx);
    return json{{feed_key, json{{ext_key, json{{"filament_detected", filament_detected},
                                               {"channel_state", channel_state},
                                               {"channel_error", channel_error}}}}}};
}

namespace {
// Per-test tmp cache dir — same idiom as test_ams_backend_ad5x_ifs.cpp.
struct SnapmakerTmpCacheDir {
    std::filesystem::path path;
    explicit SnapmakerTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("snapmaker_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~SnapmakerTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Build a filament_detect status notification for a single slot with
// configurable material, color, brand, and CARD_UID. Slot index is 0-based.
// The other channels carry no entry, so the frame says nothing about them;
// @p others_none reports them as readers naming no tag instead.
json make_filament_detect_status(int slot_index, const std::string& main_type, uint32_t argb_color,
                                 const std::string& manufacturer, const json& card_uid,
                                 bool others_none = false) {
    json info_arr = json::array();
    for (int i = 0; i < 4; ++i) {
        if (i == slot_index) {
            info_arr.push_back(json{{"MAIN_TYPE", main_type},
                                    {"SUB_TYPE", "Basic"},
                                    {"MANUFACTURER", manufacturer},
                                    {"VENDOR", "Snapmaker"},
                                    {"ARGB_COLOR", argb_color},
                                    {"HOTEND_MIN_TEMP", 190},
                                    {"HOTEND_MAX_TEMP", 220},
                                    {"BED_TEMP", 60},
                                    {"WEIGHT", 1000},
                                    {"CARD_UID", card_uid}});
        } else if (others_none) {
            info_arr.push_back(json{{"MAIN_TYPE", "NONE"}});
        } else {
            info_arr.push_back(nullptr);
        }
    }
    return json{{"filament_detect", json{{"info", info_arr}}}};
}

// Captures execute_gcode() so the load/unload command path can be asserted
// without a live Moonraker connection — same idiom as test_ams_backend_afc.cpp.
class CapturingSnapmakerBackend : public AmsBackendSnapmaker {
  public:
    // running_ has to be set: the filament ops gate on check_preconditions(),
    // which answers not_connected on a backend that never started. api_ stays
    // null, so the print-active half of that gate passes (unknown print state is
    // not blocked) and these tests keep asserting the command, not the gate —
    // the gate itself is covered in test_ams_paused_filament_ops.cpp.
    CapturingSnapmakerBackend() : AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                           helix::SharedCallback<void(const MoonrakerError&)>, bool) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // current_slot is protected on the subscription base; expose a setter so
    // tests can stand in for "filament currently loaded in slot N".
    void set_loaded_slot(int slot) {
        system_info_.current_slot = slot;
    }
};
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker type enum", "[ams][snapmaker]") {
    SECTION("SNAPMAKER is a valid AmsType") {
        auto t = AmsType::SNAPMAKER;
        REQUIRE(t != AmsType::NONE);
        REQUIRE(static_cast<int>(t) == 7);
    }

    SECTION("SNAPMAKER is both a tool changer and filament system") {
        REQUIRE(is_tool_changer(AmsType::SNAPMAKER));
        REQUIRE(is_filament_system(AmsType::SNAPMAKER));
    }

    SECTION("ams_type_to_string returns Snapmaker") {
        REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
    }

    SECTION("ams_type_from_string parses Snapmaker variants") {
        REQUIRE(ams_type_from_string("snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("Snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("snapswap") == AmsType::SNAPMAKER);
    }
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

// ============================================================================
// Backend Construction Tests
// ============================================================================

#include "ams_backend_snapmaker.h"

#include "hv/json.hpp"

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
}

// ============================================================================
// The U1's own sequence homes and heats
//
// FEED_AUTO walks load_homing -> load_feeding -> load_heating -> ... and
// unload_homing -> unload_picking -> unload_heating -> ..., setting its own
// nozzle target from the lane's material. A UI preheat or G28 in front of it
// is a second wait the firmware repeats anyway.
// ============================================================================

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

// ============================================================================
// Feeder / Toolhead noun tests
//
// The U1's own firmware UI binary spells filament entry "Feeder 1".."Feeder 4"
// and the printing end "Toolhead 1".."Toolhead 4" (156 and 566 capitalised
// hits respectively; zero for Slot, Lane, or capitalised Channel) - two
// different words for the same 1:1 physical position.
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker lane_noun and tool_noun name two different things",
                 "[ams][snapmaker][numbering]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    CHECK(backend.lane_noun() == helix::ui::LaneNoun::Feeder);
    CHECK(backend.tool_noun() == helix::ui::LaneNoun::Toolhead);
    CHECK(backend.lane_noun() != backend.tool_noun());
}

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

// ============================================================================
// Operation Step Model Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_operation_step_model is the per-direction firmware sequence",
                 "[ams][snapmaker][stepmodel]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SECTION("LOAD is a 5-step model ending in Purge with a live-temp Heat step") {
        auto model = backend.get_operation_step_model(StepOperationType::LOAD_FRESH);
        // filament_feed.py sets load_homing, load_picking, load_feeding,
        // load_heating, load_extruding, load_flushing in that order.
        REQUIRE(model.steps.size() == 6);
        CHECK(std::string(model.steps[0].label) == "Home");
        CHECK(std::string(model.steps[1].label) == "Select");
        CHECK(std::string(model.steps[2].label) == "Feed filament");
        CHECK(std::string(model.steps[3].label) == "Heat nozzle");
        CHECK(std::string(model.steps[4].label) == "Extrude");
        CHECK(std::string(model.steps[5].label) == "Purge");
        // phase_id mirrors the firmware operation_phase index the classifier emits.
        for (int i = 0; i < 6; ++i) {
            CHECK(model.steps[static_cast<size_t>(i)].phase_id == i);
        }
        // Only the Heat step shows a live nozzle temperature.
        CHECK(model.steps[3].live_temp);
        CHECK_FALSE(model.steps[0].live_temp);
        CHECK_FALSE(model.steps[2].live_temp);
        CHECK_FALSE(model.steps[4].live_temp);
    }

    SECTION("LOAD_SWAP also produces the 5-step load-direction model") {
        auto model = backend.get_operation_step_model(StepOperationType::LOAD_SWAP);
        REQUIRE(model.steps.size() == 6);
        CHECK(std::string(model.steps[2].label) == "Feed filament");
        CHECK(std::string(model.steps[5].label) == "Purge");
    }

    SECTION("UNLOAD is a 4-step model ending in Retract") {
        auto model = backend.get_operation_step_model(StepOperationType::UNLOAD);
        REQUIRE(model.steps.size() == 4);
        CHECK(std::string(model.steps[0].label) == "Home");
        CHECK(std::string(model.steps[1].label) == "Select");
        CHECK(std::string(model.steps[2].label) == "Heat nozzle");
        CHECK(std::string(model.steps[3].label) == "Retract");
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
        CHECK(backend.get_operation_step_index_subject(op) ==
              AmsState::instance().get_ams_operation_phase_subject());
    }
}

// ============================================================================
// Filament Operation Command Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load routes through AUTO_FEEDING LOAD=1",
                 "[ams][snapmaker][load]") {
    // The mirror of the unload contract below, and previously unasserted — the
    // gap that let a UI-layer change silently swap this command for `T{n}`.
    //
    // FEED_AUTO silent-returns when passed no LOAD/UNLOAD parameter, and `T{n}`
    // only seats the carriage without feeding, so both wrong answers look like
    // "the button did nothing". See load_filament()'s trail-of-bad-guesses note.
    SECTION("explicit slot emits AUTO_FEEDING EXTRUDER=n LOAD=1") {
        CapturingSnapmakerBackend backend;
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
    CapturingSnapmakerBackend backend;

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
        CapturingSnapmakerBackend backend;
        auto err = backend.unload_filament(2);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 UNLOAD=1");
    }

    SECTION("unload_active_filament dispatches the loaded slot's extruder") {
        // Contract: unload_active_filament() reads current_slot from system_info_
        // (single source of truth in the base class) and forwards it. This is
        // the path the Filament panel's Unload button takes.
        CapturingSnapmakerBackend backend;
        backend.set_loaded_slot(1);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1");
    }

    SECTION("unload_active_filament with T3 loaded dispatches EXTRUDER=3 (U1 field bug)") {
        // Regression for the Snapmaker U1 Filament-panel-unload wrong-tool bug
        // (Discord report from Bart, 2026-07-20): with T3 loaded, hitting Unload
        // on the Filament panel sent EXTRUDER=0 (stale current_slot) and the
        // firmware dutifully visited T0 first. The fix routes through
        // unload_active_filament(), which resolves current_slot ONCE in the base
        // class — same snapshot the UI's "is anything loaded?" guard used — so
        // the two can never diverge.
        CapturingSnapmakerBackend backend;
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
        CapturingSnapmakerBackend backend; // current_slot defaults to -1
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // Slot 0 active (LOADED), slots 1 & 3 hold filament (AVAILABLE), slot 2 empty.
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}}, // active tool = slot 0
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, true})}}}};
    SnapmakerTestAccess::handle_status(backend, status);

    // Loaded state is keyed off channel_state now: slots 0/1/3 report load_finish
    // (filament at the toolhead), slot 2 reports wait_insert (empty channel).
    json feed = json{
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}},
              {"extruder1", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}},
        {"filament_feed right",
         json{{"extruder2", json{{"filament_detected", false}, {"channel_state", "wait_insert"}}},
              {"extruder3", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}}};
    SnapmakerTestAccess::handle_status(backend, feed);

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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // All four tools loaded to their toolhead (channel_state load_finish).
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    SnapmakerTestAccess::handle_status(backend, status);
    for (int e = 0; e < 4; ++e) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(e, "load_finish"));
    }

    // Every loaded tool is unloadable.
    REQUIRE(backend.can_unload_from_toolhead(2));

    SECTION("a tool unloaded to the buffer (channel_state unload_finish) is NOT unloadable") {
        // Post-unload: the port still reads filament (detected=true) and
        // channel_state reports unload_finish, so the loaded latch clears.
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish", true));
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

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
    SnapmakerTestAccess::handle_status(backend, feed);

    // The entrance/tag reading: 0 for the two heads fed through it.
    SnapmakerTestAccess::handle_status(
        backend, json{{"filament_detect", json{{"state", json::array({0, 1, 0, 1})}}}});

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
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "wait_insert", false));
        CHECK_FALSE(backend.get_slot_info(1).is_present());
        CHECK(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }

    SECTION("a delta frame carrying no presence fields leaves the last reading standing") {
        SnapmakerTestAccess::handle_status(backend,
                                           json{{"toolhead", json{{"extruder", "extruder"}}}});
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
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        if (row.active >= 0)
            SnapmakerTestAccess::set_current_slot(backend, row.active);
        SnapmakerTestAccess::handle_status(backend, json{{"filament_feed left", row.feed}});

        const auto info = backend.get_system_info();
        CHECK((info.action == AmsAction::ERROR) == row.error);
        if (row.detail == nullptr)
            CHECK(info.operation_detail.find("Feeder 2: No filament") != std::string::npos);
        else
            CHECK(info.operation_detail == row.detail);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker steps project Heat and Purge by kind, positioning by direction",
                 "[ams][snapmaker][coarse]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    const auto load = backend.get_operation_step_model(StepOperationType::LOAD_FRESH);
    CHECK(load.action_at(0) == AmsAction::LOADING);
    CHECK(load.action_at(1) == AmsAction::LOADING);
    CHECK(load.action_at(2) == AmsAction::LOADING);
    CHECK(load.action_at(3) == AmsAction::HEATING);
    CHECK(load.action_at(4) == AmsAction::LOADING);
    CHECK(load.action_at(5) == AmsAction::PURGING);
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto frame = [&](const char* state) {
        SnapmakerTestAccess::handle_status(
            backend,
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
        SnapmakerTestAccess::handle_status(
            backend, json{{"filament_feed left",
                           json{{"extruder2", json{{"filament_detected", true},
                                                   {"channel_state", "load_heating"},
                                                   {"channel_error", "heat_timeout"}}}}}});
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unload_finish resolves UNLOADING to IDLE",
                 "[ams][snapmaker][unload]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // Mid-unload: action becomes UNLOADING.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_doing"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    // unload_finish is the TRUE end of the operation → IDLE.
    json finish =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, finish);
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker preload_finish is NOT matched by the load_ prefix branch",
                 "[ams][snapmaker][unload]") {
    // preload_finish starts with "preload_", NOT "load_" — it must reach the
    // existing preload_finish handler (which leaves the action alone) rather than
    // being misread as a granular load sub-state.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // Start mid-unload so there is an action to (incorrectly) clobber.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_doing"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    json preload =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "preload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, preload);
    // preload_finish leaves the in-progress action alone (does NOT become LOADING
    // and does NOT resolve to IDLE — only unload_finish/load_finish/idle do).
    CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
}

// The firmware sets an op's terminal and then, in the same reactor tick, the
// channel's resting state (wait_insert with the spool out, preload_finish with
// it in). Moonraker publishes only the last channel_state, so the terminal
// survives only in channel_action_state, which resting states leave alone.
namespace {
json feed_with_action(int extruder_idx, const std::string& channel_state,
                      const std::string& action_state, bool filament_detected = true) {
    json frame = make_feed_status(extruder_idx, channel_state, filament_detected);
    frame.begin().value().begin().value()["channel_action_state"] = action_state;
    return frame;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker unload that settles in one frame ends on channel_action_state",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("spool out: wait_insert") {
        SnapmakerTestAccess::handle_status(
            backend, feed_with_action(3, "wait_insert", "unload_finish", /*detected=*/false));
    }
    SECTION("spool in: preload_finish") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
    CHECK(AmsState::instance().was_slot_recently_unloaded(3));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed unload that settles in one frame raises the error",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_fail"));
    CHECK(backend.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unload mid-heat at preload_finish keeps UNLOADING until it ends",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_prepare", "unload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    for (const char* step : {"unload_heating", "unload_heat_finish", "unload_doing"}) {
        SnapmakerTestAccess::handle_status(backend, feed_with_action(3, "preload_finish", step));
        INFO("channel_action_state=" << step);
        CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
    }

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed op from before startup raises nothing on the first frame",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend, feed_with_action(3, "preload_finish", "load_fail"));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a terminal channel_state on the first frame fires no unload event",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SECTION("unload_finish left over from before startup") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_finish", "unload_finish"));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

        SECTION("a full frame repeating it is still not an unload") {
            SnapmakerTestAccess::handle_status(
                backend, feed_with_action(3, "unload_finish", "unload_finish"));
            CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
        }
        SECTION("a real unload afterwards still marks the slot") {
            SnapmakerTestAccess::handle_status(backend,
                                               feed_with_action(3, "unload_doing", "unload_doing"));
            REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
            SnapmakerTestAccess::handle_status(
                backend, feed_with_action(3, "unload_finish", "unload_finish"));
            CHECK(backend.get_system_info().action == AmsAction::IDLE);
            CHECK(AmsState::instance().was_slot_recently_unloaded(3));
        }
    }
    SECTION("load_finish left over from before startup") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "load_finish", "load_finish"));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Snapmaker a stale terminal on another lane does not end the op",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
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
    SnapmakerTestAccess::handle_status(backend, frame);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
    CHECK_FALSE(cooldown.has_pending_timer());

    SECTION("the load's own terminal still ends it and arms the cooldown") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(0, "load_finish", "load_finish"));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(cooldown.has_pending_timer());
    }
    SECTION("a real unload on lane 4 still ends and arms the cooldown") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(0, "load_finish", "load_finish"));
        cooldown.cancel();
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_doing", "unload_doing"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_finish", "unload_finish"));
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SECTION("every step reported") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_feeding"));
        REQUIRE(backend.get_system_info().action == AmsAction::LOADING);
    }
    SECTION("preload_finish arrives straight from preload_prepare") {}

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker the first frame after a Klipper restart ends no operation",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    // The backend object outlives a Klipper restart, so what it remembers of
    // each channel describes firmware that no longer exists. The subscription's
    // first full frame must be read as a fresh start, not compared against it.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.on_klipper_restarted();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    SECTION("a finished unload's action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failed unload's action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_fail"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a Klipper restart leaves an ERROR in place",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    CapturingSnapmakerBackend backend;

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_feeding"));
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_fail"));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    backend.on_klipper_restarted();

    CHECK(backend.get_current_action() == AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a previous op's channel_action_state never ends a new one",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // At rest after an earlier unload: the firmware still reports its terminal.
    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    // A new unload starts; this frame omits the unchanged channel_action_state.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SECTION("a delta omitting channel_action_state") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "preload_finish"));
    }
    SECTION("a full frame repeating the held channel_action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
    }

    CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an outcome that arrives before the channel settles ends the op",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("a finish") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_doing", "unload_finish"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        // The settling frame omits the unchanged channel_action_state.
        SnapmakerTestAccess::handle_status(backend,
                                           make_feed_status(3, "wait_insert", /*detected=*/false));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failure") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_doing", "unload_fail"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert"));
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a channel that settles before its outcome arrives ends on the outcome",
                 "[ams][snapmaker][unload][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert", false));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "wait_insert", "unload_finish", false));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a held outcome is dropped when a new op starts",
                 "[ams][snapmaker][action_state]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "load_prepare", "load_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert"));
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SECTION("active loaded tool renders into the toolhead") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "load_finish"));
        CHECK(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }
    SECTION("multiple non-active loaded tools each render into their own toolhead") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_finish"));
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "load_finish"));
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NOZZLE);
        CHECK(backend.get_slot_filament_segment(3) == PathSegment::NOZZLE);
    }
    SECTION("empty tool renders no filament line") {
        SnapmakerTestAccess::set_sensor_present(backend, 2, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, false);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "wait_insert",
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
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        if (row.start_loaded) {
            SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
            REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
            SnapmakerTestAccess::set_sensor_present(backend, 2, true);
        }
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, row.state, row.detected));

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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // Simulate T2 picked up (toolhead.extruder = "extruder2").
    json picked_up = json{{"toolhead", json{{"extruder", "extruder2"}}}};
    SnapmakerTestAccess::handle_status(backend, picked_up);
    REQUIRE(backend.get_system_info().current_slot == 2);
    REQUIRE(backend.get_system_info().current_tool == 2);

    // T2's feeder channel reaches unload_finish (filament retracted from nozzle,
    // but toolhead still mounted). filament_loaded must clear; current_slot must NOT.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish",
                                                                 /*filament_detected=*/true));

    CHECK(backend.get_system_info().current_slot == 2);
    CHECK(backend.get_system_info().current_tool == 2);
    CHECK_FALSE(backend.get_system_info().filament_loaded);
}

// The U1 toolhead switch (filament_motion_sensor e{N}_filament) reads its pin
// directly outside a print. After a firmware unload, and after filament is fed
// to the nozzle by plain extrusion, it reads filament while channel_state says
// preload_finish: filament parked in the toolhead. Unload and Load are both
// offered for it, and no answer depends on which frame carried which field.
namespace {
json t3_picked() {
    return json{{"toolhead", json{{"extruder", "extruder3"}}}};
}
json t3_feed() {
    return make_feed_status(3, "preload_finish", /*filament_detected=*/true);
}
json t3_switch(bool detected) {
    return json{{"filament_motion_sensor e3_filament",
                 json{{"filament_detected", detected}, {"enabled", true}}}};
}
json merged(std::initializer_list<json> frames) {
    json out = json::object();
    for (const auto& f : frames) {
        out.update(f);
    }
    return out;
}

struct LoadAnswers {
    bool filament_loaded;
    SlotStatus status;
    bool actively_loaded;
    bool at_nozzle;
    bool parked;
    bool operator==(const LoadAnswers& o) const {
        return filament_loaded == o.filament_loaded && status == o.status &&
               actively_loaded == o.actively_loaded && at_nozzle == o.at_nozzle &&
               parked == o.parked;
    }
};
} // namespace

template <> struct Catch::StringMaker<LoadAnswers> {
    static std::string convert(const LoadAnswers& a) {
        return "{filament_loaded=" + std::to_string(a.filament_loaded) +
               " status=" + std::to_string(static_cast<int>(a.status)) +
               " actively=" + std::to_string(a.actively_loaded) +
               " at_nozzle=" + std::to_string(a.at_nozzle) + " parked=" + std::to_string(a.parked) +
               "}";
    }
};

namespace {
LoadAnswers answers(const AmsBackendSnapmaker& b, int slot) {
    return {b.get_system_info().filament_loaded, b.get_slot_info(slot).status,
            b.slot_is_actively_loaded(slot), b.slot_has_filament_at_toolhead(slot),
            b.slot_filament_parked_in_toolhead(slot)};
}
LoadAnswers fed(std::initializer_list<json> frames) {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> reg(nullptr, nullptr);
    for (const auto& f : frames) {
        SnapmakerTestAccess::handle_status(*reg, f);
    }
    return answers(*reg, 3);
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker filament parked in the toolhead offers Unload and Load",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::handle_status(backend, merged({t3_picked(), t3_feed(), t3_switch(true)}));

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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::handle_status(backend, merged({t3_picked(), t3_feed(), t3_switch(true)}));
    REQUIRE(backend.get_system_info().filament_loaded);
    CHECK(backend.get_filament_segment() == PathSegment::NOZZLE);
    CHECK(backend.get_slot_filament_segment(3) == PathSegment::NOZZLE);

    SnapmakerTestAccess::handle_status(backend, t3_switch(false));
    CHECK(backend.get_slot_filament_segment(3) == PathSegment::PREP);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker feeder port sensor is each lane's entry sensor",
                 "[ams][snapmaker][path][entry_sensor]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    for (int i = 0; i < 4; i++)
        CHECK(backend.slot_has_prep_sensor(i));
    CHECK_FALSE(backend.slot_has_prep_sensor(4));
    CHECK_FALSE(backend.slot_has_prep_sensor(-1));

    SECTION("with no tool on the carriage the system path reads nothing") {
        CHECK(backend.get_filament_segment() == PathSegment::NONE);
    }
    SECTION("the mounted tool reports its own lane") {
        SnapmakerTestAccess::handle_status(backend,
                                           merged({t3_picked(), t3_feed(), t3_switch(false)}));
        CHECK(backend.get_filament_segment() == PathSegment::PREP);
        SnapmakerTestAccess::handle_status(backend, t3_switch(true));
        CHECK(backend.get_filament_segment() == PathSegment::NOZZLE);
    }
    SECTION("an empty mounted lane reads nothing") {
        SnapmakerTestAccess::handle_status(
            backend,
            merged({t3_picked(), make_feed_status(3, "wait_insert", false), t3_switch(false)}));
        SnapmakerTestAccess::set_port_sensor_present(backend, 3, false);
        CHECK(backend.get_slot_filament_segment(3) == PathSegment::NONE);
        CHECK(backend.get_filament_segment() == PathSegment::NONE);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker active pin alone does not mark a tool loaded",
                 "[ams][snapmaker][channel_state][parked]") {
    // The frame's active_pin names tool 1 while toolhead.extruder names tool 3,
    // and tool 1's latch is clear: tool 1 is not loaded.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::handle_status(
        backend, merged({json{{"extruder1", json{{"active_pin", true}, {"park_pin", false}}}},
                         t3_picked(), make_feed_status(1, "preload_finish", true), t3_feed()}));
    REQUIRE(backend.get_system_info().current_tool == 3);
    CHECK(backend.get_slot_info(1).status != SlotStatus::LOADED);
    CHECK_FALSE(backend.slot_is_actively_loaded(1));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unloaded head with a clear switch offers only Load",
                 "[ams][snapmaker][channel_state][unload][parked]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::handle_status(
        backend,
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "load_finish"));
    REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));

    for (const char* state : {"unload_fail", "unload_prepare", "unload_homing", "unload_picking",
                              "unload_heating", "unload_heat_finish", "unload_doing"}) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, state));
        INFO("state=" << state);
        CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));
    }

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "unload_finish"));
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
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        int prev = -1;
        for (const char* state : seq) {
            SnapmakerTestAccess::handle_status(backend, make_feed_status(2, state));
            const int phase = backend.get_system_info().operation_phase;
            INFO("state=" << state << " phase=" << phase << " prev=" << prev);
            CHECK(phase >= prev);
            prev = phase;
        }
    }
}

// ============================================================================
// Full 39-state channel_state coverage (action + operation_phase)
// ============================================================================
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
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        // Make extruder 0 the active lane so *_fail states surface as ERROR
        // (the false-alarm guard only suppresses errors on idle empty NON-active
        // lanes) and feed present filament.
        SnapmakerTestAccess::set_current_slot(backend, 0);
        SnapmakerTestAccess::set_current_tool(backend, 0);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(0, c.state));
        INFO("channel_state=" << c.state);
        CHECK(backend.get_system_info().action == c.action);
        CHECK(backend.get_system_info().operation_phase == c.phase);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail channel_state raises ERROR with detail",
                 "[ams][snapmaker][channel_state]") {
    // A *_fail channel_state alone (channel_error still "ok") must surface an
    // error — Change 2 adds this on top of the existing channel_error handling.
    for (const char* fail : {"load_fail", "unload_fail", "preload_fail", "manual_sta_fail"}) {
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        SnapmakerTestAccess::set_current_slot(backend, 1);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, fail));
        INFO("fail state=" << fail);
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
        CHECK_FALSE(backend.get_system_info().operation_detail.empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail on an idle empty non-active lane stays quiet",
                 "[ams][snapmaker][channel_state]") {
    // The multi-color false-alarm guard must still hold: a fail on a lane that
    // is empty, idle, and not active should not pop a spurious error modal.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    SnapmakerTestAccess::set_current_slot(backend, 0); // active lane is 0, not 2
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "preload_fail",
                                                                 /*filament_detected=*/false));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker motion-sensor runout path is independent of the loaded latch",
                 "[ams][snapmaker][channel_state][runout]") {
    // No regression: mid-print runout is still driven by the per-tool motion
    // sensor (e{N}_filament), a different question from "is the lane loaded".
    // The loaded latch must NOT be touched by a motion-sensor runout — filament
    // that stops moving is still loaded at the toolhead, it just ran out upstream.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // Active tool 0 loaded to its nozzle, with filament present everywhere.
    json loaded = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    SnapmakerTestAccess::handle_status(backend, loaded);
    REQUIRE(backend.get_system_info().filament_loaded);

    // Active lane's motion sensor drops during extrusion → runout.
    json runout = json{{"filament_motion_sensor e0_filament", json{{"filament_detected", false}}}};
    SnapmakerTestAccess::handle_status(backend, runout);

    // Runout path fired: the global loaded flag reflects the motion sensor.
    CHECK_FALSE(backend.get_system_info().filament_loaded);
    // But the channel_state loaded latch is untouched by the motion sensor.
    CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 0));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker filament path segment follows the loaded latch, not the motion sensor",
                 "[ams][snapmaker][channel_state]") {
    // The canvas draws NOZZLE (filament threaded to the hotend) only when the
    // lane is actually loaded. The live bug: an unloaded lane kept its motion
    // sensor present, so the segment rendered NOZZLE and the lane looked fully
    // loaded on screen even though its Unload button was correctly disabled.
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SECTION("unloaded lane with motion sensor still present draws PREP, not NOZZLE") {
        // Exact live condition: unload_finish but the toolhead motion sensor and
        // buffer port sensor both still read present.
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
        SnapmakerTestAccess::set_sensor_present(backend, 2, true);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, true);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish",
                                                                     /*filament_detected=*/true));
        CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
        // Filament is at the feeder but NOT at the nozzle.
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::PREP);
    }
}

// ============================================================================
// Extruder State Parser Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker extruder state parsing", "[ams][snapmaker]") {
    SECTION("parses parked extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "park_pin": true,
            "active_pin": false,
            "grab_valid_pin": false,
            "activating_move": false,
            "extruder_offset": [0.073, -0.037, 0.0],
            "switch_count": 86,
            "retry_count": 0,
            "error_count": 1
        })");
        ExtruderToolState state;
        state.apply(snapmaker::parse_extruder_delta(j));
        REQUIRE(state.state == "PARKED");
        REQUIRE(state.park_pin == true);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.073f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(-0.037f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
        REQUIRE(state.switch_count == 86);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 1);
    }

    SECTION("parses active extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVE",
            "park_pin": false,
            "active_pin": true,
            "activating_move": false,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 12,
            "retry_count": 2,
            "error_count": 0
        })");
        ExtruderToolState state;
        state.apply(snapmaker::parse_extruder_delta(j));
        REQUIRE(state.state == "ACTIVE");
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == true);
        REQUIRE(state.switch_count == 12);
        REQUIRE(state.retry_count == 2);
        REQUIRE(state.error_count == 0);
    }

    SECTION("parses activating move in progress") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVATING",
            "park_pin": false,
            "active_pin": false,
            "activating_move": true,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 5,
            "retry_count": 0,
            "error_count": 0
        })");
        ExtruderToolState state;
        state.apply(snapmaker::parse_extruder_delta(j));
        REQUIRE(state.state == "ACTIVATING");
        REQUIRE(state.activating_move == true);
    }

    SECTION("handles missing fields gracefully") {
        auto j = nlohmann::json::parse("{}");
        ExtruderToolState state;
        state.apply(snapmaker::parse_extruder_delta(j));
        REQUIRE(state.state.empty());
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.switch_count == 0);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 0);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }

    SECTION("handles partial extruder_offset array") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "extruder_offset": [1.5]
        })");
        ExtruderToolState state;
        state.apply(snapmaker::parse_extruder_delta(j));
        REQUIRE(state.extruder_offset[0] == Catch::Approx(1.5f));
        // Missing indices stay at default
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }
}

// ============================================================================
// RFID Info Parser Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker RFID info parsing", "[ams][snapmaker]") {
    SECTION("parses full RFID tag data") {
        // ARGB 0xFF080A0D -> RGB 0x080A0D
        auto j = nlohmann::json::parse(R"({
            "VERSION": 1,
            "VENDOR": "Snapmaker",
            "MANUFACTURER": "Polymaker",
            "MAIN_TYPE": "PLA",
            "SUB_TYPE": "SnapSpeed",
            "ARGB_COLOR": 4278716941,
            "DIAMETER": 175,
            "WEIGHT": 500,
            "HOTEND_MAX_TEMP": 230,
            "HOTEND_MIN_TEMP": 190,
            "BED_TEMP": 60,
            "OFFICIAL": true
        })");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PLA");
        REQUIRE(info.sub_type == "SnapSpeed");
        REQUIRE(info.manufacturer == "Polymaker");
        REQUIRE(info.vendor == "Snapmaker");
        REQUIRE(info.hotend_min_temp == 190);
        REQUIRE(info.hotend_max_temp == 230);
        REQUIRE(info.bed_temp == 60);
        REQUIRE(info.weight_g == 500);
        // ARGB 4278716941 = 0xFF080A0D → mask off alpha → 0x080A0D
        REQUIRE(info.color_rgb == 0x080A0Du);
    }

    SECTION("ARGB alpha byte is masked to produce RGB") {
        // 0xFF0000FF (opaque blue) -> 0x0000FF
        auto j = nlohmann::json::parse(R"({"ARGB_COLOR": 4278190335})");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.color_rgb == 0x0000FFu);
    }

    SECTION("stores both MANUFACTURER and VENDOR independently") {
        auto j = nlohmann::json::parse(R"({
            "VENDOR": "Generic",
            "MANUFACTURER": "",
            "MAIN_TYPE": "PETG"
        })");
        auto info = snapmaker::parse_rfid_info(j);
        // Parser stores fields as-is; brand fallback logic is in handle_status
        REQUIRE(info.vendor == "Generic");
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.main_type == "PETG");
    }

    SECTION("handles missing RFID fields with safe defaults") {
        auto j = nlohmann::json::parse("{}");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type.empty());
        REQUIRE(info.sub_type.empty());
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.vendor.empty());
        REQUIRE(info.hotend_min_temp == 0);
        REQUIRE(info.hotend_max_temp == 0);
        REQUIRE(info.bed_temp == 0);
        REQUIRE(info.weight_g == 0);
        // Default color is 0x808080 (grey)
        REQUIRE(info.color_rgb == 0x808080u);
    }

    SECTION("parses PETG with different temperatures") {
        auto j = nlohmann::json::parse(R"({
            "MANUFACTURER": "Generic3D",
            "MAIN_TYPE": "PETG",
            "SUB_TYPE": "Basic",
            "HOTEND_MIN_TEMP": 220,
            "HOTEND_MAX_TEMP": 250,
            "BED_TEMP": 80,
            "WEIGHT": 1000
        })");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PETG");
        REQUIRE(info.sub_type == "Basic");
        REQUIRE(info.manufacturer == "Generic3D");
        REQUIRE(info.hotend_min_temp == 220);
        REQUIRE(info.hotend_max_temp == 250);
        REQUIRE(info.bed_temp == 80);
        REQUIRE(info.weight_g == 1000);
    }

    SECTION("parses CARD_UID array as comma-joined string") {
        auto j = json::parse(R"({"CARD_UID": [144, 32, 196, 2]})");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.uid == "144,32,196,2");
    }

    SECTION("missing CARD_UID leaves uid empty") {
        auto j = json::parse(R"({"MAIN_TYPE": "PLA"})");
        auto info = snapmaker::parse_rfid_info(j);
        REQUIRE(info.uid.empty());
    }
}

// ============================================================================
// Task 12: filament slot override integration
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker override loaded at init is applied over firmware data",
                 "[ams][snapmaker][filament_slot_override]") {
    // Seed lane_data in the mock Moonraker DB with a slot 0 override.
    // Inject the pre-loaded override into the backend directly (skipping
    // on_started() since the backend is built with api=nullptr for simplicity —
    // on_started's load_blocking path is covered by store tests elsewhere).
    // Then push a firmware status update whose values differ from the
    // override and verify the override wins for override-eligible fields
    // while firmware wins for hardware-truth fields (color is present on
    // both, but the override's color_rgb is non-zero and wins per policy).
    SnapmakerTmpCacheDir tmp("task12_override_applied");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware pushes a different color (blue) and a different material.
    // 0xFF0000FF = opaque red in ARGB.
    json status =
        make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    // Override-eligible fields won.
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker clear_slot_override drops the Spoolman handles",
                 "[ams][snapmaker][filament_slot_override][1625]") {
    SnapmakerTmpCacheDir tmp("clear_spoolman_link");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // A filament_detect parse populates the units a clear walks.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::seed_live_spoolman_link(backend, 0, 42, 77, 3));
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 77);

    backend.clear_slot_override(0);

    // All three Spoolman handles die together. The linked spool's name goes
    // with them, and the name left standing is the product line the tag
    // itself states (the helper's tag says "Basic"): the clear neither keeps a
    // name the record supplied nor blanks one the tag still owns.
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.spoolman_filament_id == 0);
    CHECK(info.spool_name == "Basic");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker product line beside another vendor's brand names no spool",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("foreign_product_line");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // "Basic" is the placeholder SUB_TYPE firmware keeps beside any vendor.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Generic", json::array({1, 2, 3, 4})));
    CHECK(backend.get_slot_info(0).spool_name.empty());

    backend.clear_slot_override(0);
    auto info = backend.get_slot_info(0);
    CHECK(info.spool_name.empty());
    CHECK(info.material == "PLA");
    CHECK(info.brand == "Generic");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker tag reader naming no tag leaves nothing of the old tag after a clear",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("clear_after_untagged_reading");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(1, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(1).material == "PLA");
    REQUIRE(backend.get_slot_info(1).spool_name == "Basic");

    // An untagged spool: the firmware's info reads NONE throughout.
    const json untagged{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"},
                                         {"SUB_TYPE", "NONE"},
                                         {"VENDOR", "NONE"},
                                         {"MANUFACTURER", "NONE"},
                                         {"CARD_UID", json::array({0, 0, 0, 0})}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};
    SnapmakerTestAccess::handle_status(backend, untagged);
    CHECK_FALSE(helix::ams::lane_sources(backend_reg.lane(1)).vendor_cache.has_value());

    backend.clear_slot_override(1);
    auto info = backend.get_slot_info(1);
    CHECK(info.material.empty());
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.product_name.empty());
    CHECK(info.color_rgb == kAmsDefaultSlotColor);

    // The same silence on the next frame has nothing left to drop.
    int events = 0;
    backend.set_event_callback([&](const std::string& ev, const std::string&) {
        if (ev == AmsBackend::EVENT_STATE_CHANGED)
            ++events;
    });
    SnapmakerTestAccess::handle_status(backend, untagged);
    CHECK(events == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker print_task_config type NONE is no material",
                 "[ams][snapmaker][print_task_config]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_type", json::array({"PLA", "NONE", "PETG", ""})}}}});
    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(1).material.empty());
    CHECK(backend.get_slot_info(2).material == "PETG");
    CHECK(backend.get_slot_info(3).material.empty());

    // The firmware stops a print on a head whose record names no type.
    CHECK_FALSE(backend.get_slot_info(0).material_required);
    CHECK(backend.get_slot_info(1).material_required);
    CHECK_FALSE(backend.get_slot_info(2).material_required);
    CHECK(backend.get_slot_info(3).material_required);

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_type", json::array({"PLA", "PLA", "PETG", ""})}}}});
    CHECK_FALSE(backend.get_slot_info(1).material_required);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "a U1 head with no material set blocks a print that calibrates it",
                 "[ams][snapmaker][print_task_config][preflight]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_type", json::array({"ASA", "NONE", "PC", "NONE"})},
                            {"filament_exist", json::array({true, true, true, false})}}}});
    helix::ui::UpdateQueue::instance().drain();

    const auto slots = AmsState::instance().collect_available_slots();
    const auto head1 = std::find_if(slots.begin(), slots.end(),
                                    [](const AvailableSlot& s) { return s.slot_index == 1; });
    REQUIRE(head1 != slots.end());
    REQUIRE_FALSE(head1->is_empty);
    CHECK(head1->material_required);

    const std::vector<GcodeToolInfo> tools = {{0, 0xFFFFFF, "PLA"}};
    const std::vector<ToolMapping> mapping = {
        {0, 1, 0, false, false, ToolMapping::MatchReason::AUTO}};
    const auto r = PreflightValidator::validate(tools, slots, mapping,
                                                helix::FirmwareRouting::identity(), false,
                                                /*material_needed=*/true);
    REQUIRE(r.checks.size() == 1);
    CHECK(r.checks[0].severity == ToolCheck::Severity::UnknownMaterial);
    CHECK(r.has_block());

    // With no option that needs the material, the head prints as it is.
    CHECK_FALSE(PreflightValidator::validate(tools, slots, mapping,
                                             helix::FirmwareRouting::identity(), false,
                                             /*material_needed=*/false)
                    .has_block());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker tag reader naming no tag takes back the tag's temps and weight",
                 "[ams][snapmaker][rfid]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // The helper's tag states 190-220 nozzle, 60 bed, 1000 g.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(2, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(2).nozzle_temp_max == 220);
    REQUIRE(backend.get_slot_info(2).total_weight_g == 1000.0f);

    SnapmakerTestAccess::handle_status(
        backend,
        json{{"filament_detect",
              json{{"info",
                    json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                 json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}});
    const SlotInfo blank;
    auto info = backend.get_slot_info(2);
    CHECK(info.nozzle_temp_min == blank.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == blank.nozzle_temp_max);
    CHECK(info.bed_temp == blank.bed_temp);
    CHECK(info.total_weight_g == blank.total_weight_g);
}

namespace {
json untagged_frame() {
    return json{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker user's edit stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("edit_over_untagged");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // The helper's tag: PLA, Snapmaker, 190-220 nozzle, 60 bed.
    const json tag =
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    SlotInfo edit = backend.get_slot_info(0);
    SECTION("an edit the tag then repeats") {
        // An edit only declares what it changes, so it is made before the
        // tag states the same values.
        edit.material = "PLA";
        edit.brand = "Snapmaker";
        edit.nozzle_temp_min = 190;
        edit.nozzle_temp_max = 220;
        edit.bed_temp = 60;
        helix::test::edit_slot_as_user(backend, 0, edit);
        SnapmakerTestAccess::handle_status(backend, tag);
    }
    SECTION("an edit differing from the tag") {
        SnapmakerTestAccess::handle_status(backend, tag);
        edit = backend.get_slot_info(0);
        edit.material = "PETG";
        edit.brand = "eSUN";
        edit.nozzle_temp_min = 230;
        edit.nozzle_temp_max = 250;
        edit.bed_temp = 80;
        helix::test::edit_slot_as_user(backend, 0, edit);
    }
    REQUIRE(backend.get_slot_info(0).material == edit.material);

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.material == edit.material);
    CHECK(info.brand == edit.brand);
    CHECK(info.nozzle_temp_min == edit.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == edit.nozzle_temp_max);
    CHECK(info.bed_temp == edit.bed_temp);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker bound spool stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("spool_over_untagged");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));

    SpoolInfo spool;
    spool.id = 42;
    SECTION("a spool equal to the tag") {
        spool.vendor = "Snapmaker";
        spool.material = "PLA";
    }
    SECTION("a spool differing from the tag") {
        spool.vendor = "Polymaker";
        spool.material = "PETG";
    }
    spool.filament_name = "Bound";
    SlotInfo edit = backend.get_slot_info(0);
    edit.spoolman_id = 42;
    helix::test::edit_slot_as_user(backend, 0, edit);
    helix::test::spool_states(backend, 0, spool);

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == spool.material);
    CHECK(info.brand == spool.vendor);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker restart forgets the previous session's tag and task values",
                 "[ams][snapmaker][rfid]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // print_task_config names one colour, then the tag reads another, so the
    // slot shows the tag's while the task colour waits behind it.
    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_color_rgba",
                             json::array({"AA0000FF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).color_rgb == 0x202020u);
    REQUIRE(backend.get_slot_info(0).nozzle_temp_max == 220);

    SnapmakerTestAccess::call_on_started(backend);
    SnapmakerTestAccess::handle_status(backend, untagged_frame());

    // Neither value the previous session recorded is judged against: the
    // temperatures are not taken back as that session's tag's, and the colour
    // does not fall back to that session's task colour.
    auto info = backend.get_slot_info(0);
    CHECK(info.nozzle_temp_min == 190);
    CHECK(info.nozzle_temp_max == 220);
    CHECK(info.bed_temp == 60);
    CHECK(info.color_rgb != 0xAA0000u);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker apply_user_edit writes override and survives status update",
                 "[ams][snapmaker][filament_slot_override]") {
    // The next firmware status update wipes an edit kept only in memory, so the
    // override has to survive subsequent parses.
    SnapmakerTmpCacheDir tmp("task12_persist_survives");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit = backend.get_slot_info(0);
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.spoolman_filament_id = 55;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    // The user links spool 42 and the server says what spool 42 is. A commit
    // that moves spoolman_id states the binding and nothing else, so the brand,
    // name, material and colour that ride in with a link need the source that
    // actually owns them.
    helix::test::edit_slot_as_user(backend, 0, edit);
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 55);
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PLA Orange";
    spool.material = "PLA";
    spool.color_hex = "FF5500";
    helix::test::spool_states(backend, 0, spool);

    // Override is staged in-memory AND written to the Moonraker DB.
    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Simulate a subsequent Klipper status update with conflicting firmware
    // data. Pre-Task-12 this wiped the user's edit; the fix is that
    // apply_overrides re-layers the saved override over the parse output.
    json status =
        make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");                // survived
    CHECK(info.spool_name == "PolyLite PLA Orange"); // survived
    CHECK(info.spoolman_id == 42);                   // survived
    CHECK(info.spoolman_filament_id == 55);          // survived
    CHECK(info.material == "PLA");                   // override material wins
    CHECK(info.color_rgb == 0xFF5500u);              // override color wins
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker sync_external_identity does NOT write store",
                 "[ams][snapmaker][filament_slot_override]") {
    SnapmakerTmpCacheDir tmp("task12_no_persist");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.sync_external_identity(0, edit);
    REQUIRE(err.success());

    // No override staged, no DB write.
    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Preview edit is still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker RFID UID change clears override (hardware swap detected)",
                 "[ams][snapmaker][filament_slot_override]") {
    SnapmakerTmpCacheDir tmp("task12_uid_swap_clears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Seed an override AND the corresponding DB entry so we can verify
    // clear_async deletes it on swap.
    mock_printer.client.mock_db_set(
        "lane_data", "T0",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: CARD_UID=[1,2,3,4] establishes the baseline. No clear.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));

    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    REQUIRE(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Second parse: DIFFERENT CARD_UID — physical swap detected. Override
    // must be cleared in-memory AND the Moonraker DB entry deleted.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", json::array({5, 6, 7, 8})));

    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    // Baseline advanced to the new UID.
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "5,6,7,8");

    // Override-exclusive fields reset on the live slot; firmware's new
    // reading flows through. Its SUB_TYPE (the helper hard-codes "Basic")
    // names no spool beside a Generic brand, so the spool name is blank.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Generic");
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.material == "PETG"); // firmware's new material
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker first RFID UID observation does NOT clear override",
                 "[ams][snapmaker][filament_slot_override]") {
    // Even when the override was saved against a different (now-stale) UID,
    // the very first observation is a BASELINE and must never fire a clear.
    SnapmakerTmpCacheDir tmp("task12_first_uid_baseline");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a UID on the FIRST observation — no prior baseline,
    // so this must NOT trigger a clear. Override survives.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                             json::array({99, 99, 99, 99})));

    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // A second parse of the SAME UID stays the baseline — no clear, no
    // "weird state" that fires on unchanged polls.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                             json::array({99, 99, 99, 99})));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker restart compares against the fingerprint the record carried",
                 "[ams][snapmaker][filament_slot_override]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    const json uid_f1 = json::array({1, 2, 3, 4});
    const json uid_f2 = json::array({5, 6, 7, 8});

    // A record as a session running older code left it: a user override with
    // no fingerprint. Session 1 boots on it, reads the tag, and the
    // observation must persist into the record: two backends over one mock
    // DB stand in for two app lifetimes.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    saved.color_set = true;
    mock_printer.client.mock_db_set("lane_data", "T0", helix::ams::to_lane_data_record(0, saved));
    {
        helix::test::RegisteredBackend<AmsBackendSnapmaker> session1(&api, nullptr);
        SnapmakerTestAccess::call_on_started(*session1);
        // The record on the wire loaded as this session's override.
        REQUIRE(SnapmakerTestAccess::get_override(*session1, 0).has_value());
        SnapmakerTestAccess::handle_status(
            *session1, make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", uid_f1));
        auto stored = mock_printer.client.mock_db_get("lane_data", "T0");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "1,2,3,4");
    }

    SECTION("a swap made while the app was down clears the override") {
        helix::test::RegisteredBackend<AmsBackendSnapmaker> session2(&api, nullptr);
        SnapmakerTestAccess::call_on_started(*session2);
        // The loaded record's fingerprint is the baseline, so the first frame
        // after restart is a comparison, not a first observation.
        REQUIRE(SnapmakerTestAccess::get_override(*session2, 0).has_value());
        SnapmakerTestAccess::handle_status(
            *session2, make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", uid_f2));
        CHECK(SnapmakerTestAccess::last_rfid_uid(*session2, 0) == "5,6,7,8");
        CHECK_FALSE(SnapmakerTestAccess::get_override(*session2, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }

    SECTION("the same spool across a restart keeps the override") {
        helix::test::RegisteredBackend<AmsBackendSnapmaker> session2(&api, nullptr);
        SnapmakerTestAccess::call_on_started(*session2);
        SnapmakerTestAccess::handle_status(
            *session2, make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", uid_f1));
        auto ovr = SnapmakerTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }

    SECTION("a record without a stored fingerprint stays a first-observation baseline") {
        // The shape every record written before the field existed has.
        mock_printer.client.mock_db_set("lane_data", "T0",
                                        helix::ams::to_lane_data_record(0, saved));
        helix::test::RegisteredBackend<AmsBackendSnapmaker> session2(&api, nullptr);
        SnapmakerTestAccess::call_on_started(*session2);
        SnapmakerTestAccess::handle_status(
            *session2, make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", uid_f2));
        // Baseline, not Changed: the tag differs from the record's data, but
        // with no stored fingerprint there is nothing to compare against.
        auto ovr = SnapmakerTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        // The baseline observation itself heals the fingerprint in, so the
        // NEXT restart compares against this spool.
        auto stored = mock_printer.client.mock_db_get("lane_data", "T0");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "5,6,7,8");
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unreadable UID keeps the record through the glitch",
                 "[ams][snapmaker][filament_slot_override]") {
    // A frame that decodes the tag but not its CARD_UID is still a reading:
    // material and colour stand as evidence, and the baseline takes them. The
    // protection the spool-swap check needs lives in the verdict instead: with
    // no UID on the new side, nothing contradicts the spool the baseline
    // names, so a transient read glitch clears nothing.
    SnapmakerTmpCacheDir tmp("task12_empty_uid_noop");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid UID "1,2,3,4" — baseline established.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // Second parse: the tag decoded (PLA / FF5500) but its CARD_UID came back
    // empty. The frame answered, so the baseline takes the material-and-colour
    // reading - and the record stands, because with no UID on the new side
    // there is nothing to call a different spool over.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PLA|FF5500");
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Third parse: the same spool's UID decodes again. The reading changed
    // spelling, but material and colour match what the unreadable pass saw,
    // so the verdict is same spool and the override survives the glitch.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a NONE entry with no UID is no signal; the record stands",
                 "[ams][snapmaker][filament_slot_override][1710]") {
    // MAIN_TYPE NONE with no CARD_UID is three indistinguishable states: the
    // RFID reader disabled (its default on Extended Firmware), an untagged
    // spool, an empty channel. None of them is a reading of the channel, so
    // the entry files no evidence at all: the fingerprint is empty, the
    // tracker reports no signal, no baseline moves and nothing clears.
    // Unloading a tagged spool therefore leaves the record describing it.
    SnapmakerTmpCacheDir tmp("1710_untagged_clears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // The reader answers NONE with no UID: no evidence either way.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // A channel the reader cannot answer stays unanswered across reads: the
    // baseline never moves, nothing clears, nothing crashes.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
}

// The insert rule needs a presence edge to fire on; for a channel whose RFID
// side says nothing (reader disabled, untagged spool) that edge is the feed
// port's false -> true. The edge holds a pending verdict and the reader's next
// filament_detect.info entry judges it; a NONE entry is the rule's NoEvidence
// verdict (#1710).
TEST_CASE_METHOD(LVGLUITestFixture, "Snapmaker an untagged channel insert offers the notice",
                 "[ams][snapmaker][1710]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // The stored record the notice asks about: user-entered details on lane 0.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.color_rgb = 0x1188FF;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // The port flag's first reading is the baseline, not an edge.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", false));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // Filament goes in; the reader's next entry carries no tag behind it: the
    // record stands and the notice asks.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "load_finish", true));
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());

    // A channel whose tag was read never asks: the reader's entry after the
    // edge vouches for the insert, and the UID rule owns the swap verdict.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "wait_insert", false));
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_finish", true));
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(1, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.size() == 1);

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker a tag appearing after only NONE entries is a first baseline, not a swap",
    "[ams][snapmaker][filament_slot_override][1710]") {
    // NONE entries are no signal, so a channel that has only ever answered
    // NONE holds no reading an arriving tag could differ from: the first UID
    // the reader decodes is a baseline, and the record stands. A swap is only
    // decidable when both sides read a tag (UID, or material and colour off a
    // tag whose UID never decoded).
    SnapmakerTmpCacheDir tmp("1710_tag_appears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Baseline attempt: NONE answers carry no reading, so none is taken.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));
    REQUIRE_FALSE(SnapmakerTestAccess::last_rfid_uid(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());

    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker two tags whose UIDs never decoded are judged by material and colour",
                 "[ams][snapmaker][filament_slot_override][1710]") {
    // With no UID on either side, material and colour are the only evidence,
    // so they decide: a PETG tag following a PLA one is a different spool,
    // and the same reading repeated is not an event at all.
    SnapmakerTmpCacheDir tmp("1710_broken_uid_material");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PLA|FF5500");

    SECTION("a different material is a different spool") {
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "PETG", 0x00FF00u, "Polymaker", json::array()));

        CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());
        CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PETG|00FF00");
    }

    SECTION("the same reading repeated is not an event") {
        SnapmakerTestAccess::handle_status(
            backend,
            make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));

        CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
        CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }
}

// ============================================================================
// Auto-mirror
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker auto-mirror uses OverwriteAlways policy after firmware writeback",
                 "[ams][snapmaker][firmware_writeback]") {
    // User edits round-trip to firmware through SET_PRINT_FILAMENT_CONFIG,
    // and firmware-truth converges with user-truth on the next status update. The auto-mirror tail
    // in handle_status therefore overwrites lane_data unconditionally — picking up external edits
    // (CHANGE_ZCOLOR from a print, slicer, etc) AND keeping user edits in sync. This test exercises
    // that overwrite by seeding lane_data with a stale color, then firing a status update with a
    // different firmware color, and verifying lane_data was overwritten.
    SnapmakerTmpCacheDir tmp("firmware_writeback_overwrite");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(&api, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Pre-seed lane_data AND the in-memory override map with a stale color
    // (e.g. set externally before this backend started). With FillUnsetOnly
    // the stale color would survive; with OverwriteAlways the next firmware
    // status overwrites it. We seed the in-memory override too because the
    // mirror helper compares against `overrides_[slot]` (creating a default
    // empty entry otherwise) and writes only when the value differs.
    helix::ams::FilamentSlotOverride stale;
    stale.color_rgb = 0xABCDEF;
    stale.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, stale);
    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"color", "#ABCDEF"}, {"material", "PLA"}});

    // Build a status update that ALSO sets filament_detect.state[0]=1 so the
    // slot resolves to AVAILABLE — the mirror helper short-circuits when the
    // slot has no filament loaded ("no signal" contract). Stack the existing
    // info builder with an explicit state array.
    json status =
        make_filament_detect_status(0, "PETG", 0xFF112233u, "Polymaker", json::array({1, 2, 3, 4}));
    status["filament_detect"]["state"] = json::array({1, 0, 0, 0});
    SnapmakerTestAccess::handle_status(backend, status);

    auto lane = mock_printer.client.mock_db_get("lane_data", "T0");
    REQUIRE(!lane.is_null());
    REQUIRE(lane.contains("color"));
    // Firmware-truth color (0x112233) wins over stale lane_data (#ABCDEF).
    auto color_str = lane["color"].get<std::string>();
    // Color is stored as "#RRGGBB"; case may vary, so compare lowercase.
    std::string lower = color_str;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(c));
    CHECK(lower == "#112233");
}

// ============================================================================
// The auto-mirror and the lane's declaring sources (prestonbrown/helixscreen#1654)
// ============================================================================
//
// OverwriteAlways hands a colour or material to firmware because a user's edit
// reaches firmware too, so the two converge. A Spoolman record never reaches
// firmware, and neither does a value the user's record holds without a lock,
// so the mirror leaves a field a declaring source holds on the lane, and the
// stored record keeps agreeing with what the lane shows.

namespace {

/// A registered Snapmaker backend with a lane_data store behind it, which is
/// what a case asserting on both the override and the lane records needs.
struct SnapmakerLaneRig {
    explicit SnapmakerLaneRig(const std::string& name)
        : tmp(name), api(client, state), backend_reg(&api, nullptr) {
        state.init_subjects(false);
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        SnapmakerTestAccess::inject_override_store(*backend_reg, std::move(store));
    }

    SnapmakerTmpCacheDir tmp;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg;
};

/// Slot 0 seated, its tag reading @p argb and @p main_type. The port sensor
/// is what makes the slot present and AVAILABLE, the only status the mirror
/// runs on, and the CARD_UID stays the same across frames so no case reads as
/// a spool swap. The state array rides along as the entrance/tag reading,
/// which backs no presence claim.
json seated_tag_frame(uint32_t argb, const std::string& main_type) {
    json status =
        make_filament_detect_status(0, main_type, argb, "OtherBrand", json::array({1, 2, 3, 4}));
    status["filament_detect"]["state"] = json::array({1, 0, 0, 0});
    status["filament_feed left"] = json{{"extruder0", json{{"filament_detected", true}}}};
    return status;
}

/// Slot 0's stored record as a link to spool 42 leaves it: the spool's colour
/// and material rode in with the binding, so neither is declared.
helix::ams::FilamentSlotOverride linked_to_spool_42() {
    helix::ams::FilamentSlotOverride ovr;
    ovr.spoolman_id = 42;
    ovr.brand = "Polymaker";
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    return ovr;
}

SpoolInfo spool_42() {
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.material = "PLA";
    spool.color_hex = "FF5500";
    return spool;
}

} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker firmware restating a linked lane's colour leaves the stored record on "
                 "the spool's",
                 "[ams][snapmaker][lane][1654]") {
    SnapmakerLaneRig rig("mirror_keeps_linked");
    AmsBackendSnapmaker& backend = *rig.backend_reg;

    SnapmakerTestAccess::seed_override(backend, 0, linked_to_spool_42());
    {
        const auto loaded = helix::ams::lane_sources(rig.backend_reg.lane(0));
        REQUIRE(loaded.spoolman.has_value());
        REQUIRE(loaded.spoolman->color_rgb == 0xFF5500u);
        REQUIRE(loaded.spoolman->material == "PLA");
    }

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFF112233u, "PETG"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    const SlotInfo info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");

    const auto stored = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(stored.has_value());
    CHECK(stored->color_rgb == 0xFF5500u);
    CHECK(stored->material == "PLA");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a frame repeating a linked lane's values writes nothing",
                 "[ams][snapmaker][lane][1654]") {
    SnapmakerLaneRig rig("mirror_idle_frame");
    AmsBackendSnapmaker& backend = *rig.backend_reg;

    SnapmakerTestAccess::seed_override(backend, 0, linked_to_spool_42());
    const int posts_before = rig.client.call_count("server.database.post_item");

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFFFF5500u, "PLA"));

    const auto sources = helix::ams::lane_sources(rig.backend_reg.lane(0));
    // The frame was parsed, and the slot is one the mirror runs on.
    REQUIRE(sources.sensed.has_value());
    REQUIRE(sources.sensed->present == true);
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    CHECK(rig.client.call_count("server.database.post_item") == posts_before);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->color_rgb == 0xFF5500u);
    CHECK(sources.spoolman->material == "PLA");
    CHECK(sources.spoolman->spoolman_id == 42);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a linked lane holds the spool's colour across frames and a Spoolman "
                 "poll",
                 "[ams][snapmaker][lane][1654]") {
    // Firmware and the Spoolman poll both keep stating their own value. Neither
    // the lane nor the stored record may follow whichever spoke last.
    SnapmakerLaneRig rig("mirror_no_flip_flop");
    AmsBackendSnapmaker& backend = *rig.backend_reg;

    SnapmakerTestAccess::seed_override(backend, 0, linked_to_spool_42());

    const auto holds_spool_colour = [&](const char* step) {
        INFO(step);
        const SlotInfo info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0xFF5500u);
        CHECK(info.material == "PLA");
        const auto stored = SnapmakerTestAccess::get_override(backend, 0);
        REQUIRE(stored.has_value());
        CHECK(stored->color_rgb == 0xFF5500u);
        CHECK(stored->material == "PLA");
    };

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFF112233u, "PETG"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
    holds_spool_colour("after the first frame");

    SpoolmanManager::file_spool_on_lane(rig.backend_reg.lane(0), spool_42());
    holds_spool_colour("after the poll re-files the spool");

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFF112233u, "PETG"));
    holds_spool_colour("after the second frame");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unlinked lane with no declaration still follows firmware",
                 "[ams][snapmaker][lane][1654]") {
    SnapmakerLaneRig rig("mirror_unlinked_follows");
    AmsBackendSnapmaker& backend = *rig.backend_reg;

    // Undeclared and unlinked: what the store remembers.
    helix::ams::FilamentSlotOverride remembered;
    remembered.color_rgb = 0xABCDEF;
    remembered.color_set = true;
    remembered.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, remembered);
    {
        const auto loaded = helix::ams::lane_sources(rig.backend_reg.lane(0));
        REQUIRE_FALSE(loaded.spoolman.has_value());
        REQUIRE_FALSE(loaded.local_user.has_value());
    }

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFF112233u, "PETG"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    const auto stored = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(stored.has_value());
    CHECK(stored->color_rgb == 0x112233u);
    CHECK(stored->material == "PETG");
    CHECK(backend.get_slot_info(0).color_rgb == 0x112233u);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker firmware restating a colour the user's record holds leaves the stored "
                 "record on it",
                 "[ams][snapmaker][lane][1654]") {
    // The stored record carries the colour undeclared, while the user's
    // record on the lane declares it. Both are filed directly: the override
    // through the load path, the declaration through the user's own funnel.
    SnapmakerLaneRig rig("mirror_keeps_user_value");
    AmsBackendSnapmaker& backend = *rig.backend_reg;

    helix::ams::FilamentSlotOverride unlocked;
    unlocked.color_rgb = 0xFFFFFF;
    unlocked.color_set = true;
    unlocked.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, unlocked);

    helix::ams::Observation pick(helix::ams::ObservationSource::LocalUser);
    pick.color_rgb = 0xFFFFFF;
    helix::ams::commit_slot_edit(rig.backend_reg.lane(0), pick);
    {
        const auto seeded = SnapmakerTestAccess::get_override(backend, 0);
        REQUIRE(seeded.has_value());
        REQUIRE_FALSE(helix::ams::declares_color(*seeded));
        const auto before = helix::ams::lane_sources(rig.backend_reg.lane(0));
        REQUIRE(before.local_user.has_value());
        REQUIRE(before.local_user->color_rgb == 0xFFFFFFu);
        REQUIRE_FALSE(before.spoolman.has_value());
    }

    SnapmakerTestAccess::handle_status(backend, seated_tag_frame(0xFF112233u, "PLA"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    const auto stored = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(stored.has_value());
    CHECK(stored->color_rgb == 0xFFFFFFu);
    CHECK(backend.get_slot_info(0).color_rgb == 0xFFFFFFu);
}

// ============================================================================
// prepare_for_resume: classify_pause() decides whether a pause can resume.
// An inactive virtual_sdcard alone is not terminal; the terminal matchers
// (dirty bed, defect exception 532 code 1) answer RESUME_REQUIRES_RESTART up
// front. A recoverable pause whose head reads filament present skips the
// recovery load; one reading absent needs the API to load it.
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker prepare_for_resume classifies the pause",
                 "[ams][snapmaker][resume]") {
    const auto paused = [](bool sd_active, json exception = nullptr) {
        json stats = {{"state", "paused"}};
        if (!exception.is_null())
            stats["exception"] = exception;
        return json{{"print_stats", stats}, {"virtual_sdcard", {{"is_active", sd_active}}}};
    };
    struct Row {
        const char* name;
        json status;
        int slot;
        bool present; // slot 0's toolhead sensor
        AmsResult expected;
    };
    const Row rows[] = {
        {"an inactive sdcard with no matcher is recoverable", paused(false), 0, true,
         AmsResult::SUCCESS},
        // The message avoids "dirty bed" so only the id+code matcher can match.
        {"defect 532 code 1 is terminal",
         paused(false, {{"id", 532}, {"code", 1}, {"message", "unlabeled defect"}}), 0, true,
         AmsResult::RESUME_REQUIRES_RESTART},
        // The firmware pauses, not cancels, for every defect_detection code.
        {"defect 532 code 2 (noodle) is recoverable",
         paused(false, {{"id", 532}, {"code", 2}, {"message", "detected noodle"}}), 0, true,
         AmsResult::SUCCESS},
        {"an active sdcard with no active tool", paused(true), -1, true, AmsResult::SUCCESS},
        // A null API proves the present head never reaches the gcode chain.
        {"filament present skips the recovery load", paused(true), 0, true, AmsResult::SUCCESS},
        {"filament absent with no API is not connected", paused(true), 0, false,
         AmsResult::NOT_CONNECTED},
    };
    lv_init_safe();
    for (const auto& row : rows) {
        INFO(row.name);
        PrinterState& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.update_from_status(row.status);
        if (row.status["print_stats"].contains("exception"))
            REQUIRE(ps.print_state().get_print_exception_id() == 532);

        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        SnapmakerTestAccess::set_sensor_present(backend, 0, row.present);

        std::optional<AmsError> captured;
        backend.prepare_for_resume(row.slot, [&](const AmsError& err) { captured = err; });
        REQUIRE(captured.has_value());
        CHECK(captured->result == row.expected);
    }
}

namespace {

// LVGL has no pack-unregister API, so selecting a language nothing has loaded
// makes every subsequent lookup miss again - the restore idiom
// test_translation_loader.cpp uses.
struct ScopedLanguage {
    ScopedLanguage() = default;
    ~ScopedLanguage() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;
};

} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker prepare_for_resume authors a translated reason",
                 "[ams][snapmaker][resume][i18n]") {
    lv_init_safe();
    ScopedLanguage restore_lang;
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // "dirty bed" is one of snapmaker_terminal_matchers()' two signals.
    json paused = {{"print_stats", {{"state", "paused"}, {"message", "detected dirty bed"}}},
                   {"virtual_sdcard", {{"is_active", false}}}};
    ps.update_from_status(paused);

    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");

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

// ---------------------------------------------------------------------------
// #991 FIX 2 — the filament-config re-assert is sent as its OWN gcode and is
// best-effort: a config rejection must NOT abort the heat/feed/extrude chain.
// FIX 3 — when the AMS load fails, prepare_for_resume reports COMMAND_FAILED so
// the caller never dispatches RESUME.
// ---------------------------------------------------------------------------

namespace {
// Wire up a full mock stack + Snapmaker backend with slot 0 populated from a
// known sub-type ("Basic"), the runout sensor latched absent, and slot 0 as the
// active tool — i.e. the post-runout recovery branch of prepare_for_resume.
struct ResumeRecoveryHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState api_state;
    MoonrakerAPIMock api;
    AmsBackendSnapmaker backend;

    ResumeRecoveryHarness() : api(client, api_state), backend(&api, nullptr) {
        api_state.init_subjects(false);
        // The MoonrakerAPIMock's execute_gcode halt gate reads ITS OWN state
        // (api_state). Mark Klipper ready there so recovery gcode isn't refused.
        api_state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});

        // Global printer state (read by prepare_for_resume's classify_pause):
        // paused, SD inactive (no-op-shaped), no terminal exception
        // (empty message + id -1 => classify Unknown => recoverable).
        PrinterState& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                                   {"print_stats", {{"state", "paused"}}},
                                   {"virtual_sdcard", {{"is_active", false}}}});

        // Populate slot 0: MAIN_TYPE=PLA, MANUFACTURER=Polymaker, SUB_TYPE=Basic
        // (a recognized product line, so it round-trips into the config gcode).
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "PLA", 0xFF112233u, "Polymaker",
                                                 json::array({1, 2, 3, 4})));

        SnapmakerTestAccess::set_sensor_present(backend, 0, false); // runout latched
        SnapmakerTestAccess::set_current_slot(backend, 0);
        client.clear_gcode_script_history();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

// True if any recorded gcode script contains `needle`.
bool any_script_contains(const std::vector<std::string>& history, const std::string& needle) {
    for (const auto& s : history) {
        if (s.find(needle) != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume drives AUTO_FEEDING load before RESUME",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();

    ResumeRecoveryHarness h;

    bool callback_fired = false;
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    h.backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });
    // The feeder reports the load before the command's answer is handled.
    SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
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

// ---------------------------------------------------------------------------
// A head whose load failed holds Resume until its feeder reports load_finish
// with no error. The head presence switch reads "present" for filament that
// reached the toolhead and stopped short of the nozzle, so it vouches for
// nothing.
// ---------------------------------------------------------------------------

namespace {
/// prepare_for_resume once, drained; the answer it gave.
AmsError resume_once(ResumeRecoveryHarness& h, int slot) {
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    bool fired = false;
    h.backend.prepare_for_resume(slot, [&](const AmsError& err) {
        fired = true;
        captured = err;
    });
    ResumeRecoveryHarness::drain();
    if (!fired) {
        // A refeed's OK waits for its head's frame; none is coming here.
        SnapmakerTestAccess::expire_refeed_wait(h.backend);
        ResumeRecoveryHarness::drain();
    }
    REQUIRE(fired);
    return captured;
}

size_t count_containing(const std::vector<std::string>& history, const std::string& needle) {
    size_t n = 0;
    for (const auto& s : history) {
        if (s.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

/// The runout refeed on head 0 fails, then the head switch reads present.
void fail_refeed_then_switch_present(ResumeRecoveryHarness& h) {
    h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                    "extruder[0]: state: none, error: ok!", "AUTO_FEEDING");
    REQUIRE_FALSE(resume_once(h, 0).success());
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
    h.client.clear_gcode_script_history();
}
} // namespace

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
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }

    SECTION("load_finish carrying a channel error holds it") {
        SnapmakerTestAccess::handle_status(h.backend,
                                           make_feed_status(0, "load_finish", true, "no_filament"));
        CHECK_FALSE(resume_once(h, 0).success());
    }

    SECTION("a retried load that reaches load_finish resumes") {
        bool ok = false;
        h.backend.prepare_for_resume(0, [&](const AmsError& err) { ok = err.success(); });
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
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
        SnapmakerTestAccess::handle_status(
            h.backend,
            json{{"print_task_config", {{"extruders_used", {true, false, true, false}}}}});
        const AmsError err = resume_once(h, 2);
        CHECK_FALSE(err.success());
        CHECK(err.user_msg ==
              "Toolhead 1 (T0) didn't load. Check that its hotend is seated and do a test "
              "extrude before resuming.");
        CHECK(h.client.gcode_script_history().empty());
    }
    SECTION("a head the print does not use holds nothing") {
        SnapmakerTestAccess::handle_status(
            h.backend,
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
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
        ResumeRecoveryHarness::drain();
        REQUIRE(answer.has_value());
        CHECK(answer->success());
    }
    SECTION("an in-progress frame keeps waiting, then load_finish") {
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_flushing"));
        ResumeRecoveryHarness::drain();
        CHECK_FALSE(answer.has_value());
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
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
        SnapmakerTestAccess::handle_status(h.backend,
                                           make_feed_status(0, "load_finish", true, "none"));
        ResumeRecoveryHarness::drain();
        CHECK_FALSE(answer.has_value());
        SnapmakerTestAccess::handle_status(
            h.backend, json{{"filament_feed left", {{"extruder0", {{"channel_error", "ok"}}}}}});
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
        SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "load_finish"));
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
    SnapmakerTestAccess::handle_status(h.backend, make_feed_status(0, "manual_sta_finish"));
    CHECK(resume_once(h, 0).success());
    CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
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
    SnapmakerTestAccess::handle_status(h.backend, json{{"filament_feed left",
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
    SnapmakerTestAccess::handle_status(h.backend, json{{"print_stats", {{"state", "paused"}}}});
    fail_refeed_then_switch_present(h);
    h.backend.before_disconnect();
    SnapmakerTestAccess::forget_print_state(h.backend);

    SECTION("the print is still paused: Resume still retries the load") {
        SnapmakerTestAccess::handle_status(h.backend, json{{"print_stats", {{"state", "paused"}}}});
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
    SECTION("the print ended meanwhile: forgotten") {
        SnapmakerTestAccess::handle_status(h.backend,
                                           json{{"print_stats", {{"state", "standby"}}}});
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
    SnapmakerTestAccess::handle_status(
        h.backend, json{{"print_stats", {{"state", "paused"}}},
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
    SnapmakerTestAccess::handle_status(h.backend, json{{"print_stats", {{"state", "paused"}}}});
    fail_refeed_then_switch_present(h);

    SECTION("print cancelled") {
        SnapmakerTestAccess::handle_status(h.backend,
                                           json{{"print_stats", {{"state", "cancelled"}}}});
    }
    SECTION("print complete") {
        SnapmakerTestAccess::handle_status(h.backend,
                                           json{{"print_stats", {{"state", "complete"}}}});
    }

    CHECK(resume_once(h, 0).success());
    CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
}

namespace {
/// Head 0 served by an automatic feeder, reading @p state / @p error.
json fed_head0(const std::string& state, const std::string& error = "ok") {
    return json{{"filament_feed left",
                 {{"extruder0",
                   {{"filament_detected", true},
                    {"channel_state", state},
                    {"channel_error", error},
                    {"module_exist", true},
                    {"disable_auto", false}}}}}};
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume with the head reading present stops on a feeder failure",
                 "[ams][snapmaker][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    ResumeRecoveryHarness h;
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);

    SECTION("loaded: RESUME proceeds with nothing sent") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("load_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("an empty feeder's no_filament is no fault") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("load_finish", "no_filament"));
        CHECK(resume_once(h, 0).success());
    }
    SECTION("unloaded with filament at the feeder: no failure, RESUME proceeds") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("unload_finish"));
        CHECK(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 0);
    }
    SECTION("loaded by hand: no failure, RESUME proceeds") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("manual_sta_finish"));
        CHECK(resume_once(h, 0).success());
    }
    SECTION("a failed load: the load runs first and must confirm") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("load_fail"));
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(),
                               "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1") == 1);
    }
    SECTION("load_finish with a live feed fault loads first") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("load_finish", "feed_timeout"));
        CHECK_FALSE(resume_once(h, 0).success());
        CHECK(count_containing(h.client.gcode_script_history(), "AUTO_FEEDING") == 1);
    }
    SECTION("no automatic feeder: the sensor answers") {
        SnapmakerTestAccess::handle_status(h.backend, fed_head0("preload_finish"));
        SnapmakerTestAccess::handle_status(
            h.backend, json{{"filament_feed left", {{"extruder0", {{"disable_auto", true}}}}}});
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
    SnapmakerTestAccess::handle_status(h.backend, json{{"print_stats", {{"state", "paused"}}}});
    SnapmakerTestAccess::handle_status(h.backend,
                                       json{{"filament_feed left",
                                             {{"extruder0",
                                               {{"filament_detected", true},
                                                {"channel_state", "wait_insert"},
                                                {"channel_error", "ok"},
                                                {"module_exist", true},
                                                {"disable_auto", false}}}}},
                                            {"filament_motion_sensor e0_filament",
                                             {{"enabled", true}, {"filament_detected", true}}}});

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

// ============================================================================
// last_print_tool_mapping — the routing a reprint has to replay
// ============================================================================
//
// get_tool_mapping() answers "what routing is live right now" and deliberately
// goes empty the moment extruders_used clears, which is exactly when a reprint
// needs the answer. Nothing else on the reprint path can reconstruct it: there
// is no detail view, no swatch card and no picker, so a remapped file could not
// be reprinted at all — the empty remap resolved every used tool to its
// firmware-default head and wrote that over the crossover.
//
// The snapshot is taken while the task is still configured, so it survives both
// the gate closing and the firmware resetting its own table.

namespace {
/// print_task_config frame carrying a routing table and the used-heads flags.
json make_task_config(const std::vector<int>& extruder_map, const std::vector<bool>& used) {
    json map_arr = json::array();
    for (int head : extruder_map) {
        map_arr.push_back(head);
    }
    json used_arr = json::array();
    for (bool b : used) {
        used_arr.push_back(b);
    }
    return json{
        {"print_task_config", json{{"extruder_map_table", map_arr}, {"extruders_used", used_arr}}}};
}

/// Firmware default: logical tools 0-3 on their own head, 4-31 on head 0.
std::vector<int> identity_map() {
    std::vector<int> m(32, 0);
    for (int i = 0; i < 4; ++i) {
        m[static_cast<size_t>(i)] = i;
    }
    return m;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker records the routing a configured task ran with",
                 "[ams][snapmaker][reprint]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    // T0 crosses over to head 2, T2 to head 0 — the file that cannot be
    // reprinted correctly today.
    std::vector<int> crossover = identity_map();
    crossover[0] = 2;
    crossover[2] = 0;

    SECTION("nothing is recorded before a task is configured") {
        // Idle: the firmware reports its default table with no task. This is the
        // reading that must NOT be mistaken for a print's routing — it is
        // indistinguishable from a job that genuinely needs no remap.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping().empty());
    }

    SECTION("a configured task is recorded, and survives the task ending") {
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.get_tool_mapping() == crossover);
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        // Print ends: the firmware clears extruders_used AND resets its table to
        // the default. The live accessor correctly refuses to answer; the
        // snapshot is what the reprint replays.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }

    SECTION("a later task replaces the record") {
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        std::vector<int> other = identity_map();
        other[1] = 3;
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(other, {false, false, false, true}));
        REQUIRE(backend.last_print_tool_mapping() == other);
    }

    SECTION("a task-configured frame with no table yet records nothing") {
        // extruders_used can land before the table on an incremental update.
        // Snapshotting an empty table would publish "known: nothing", which
        // reprint_remap cannot tell from a real answer.
        json used_only = json{{"print_task_config",
                               json{{"extruders_used", json::array({true, false, false, false})}}}};
        SnapmakerTestAccess::handle_status(backend, used_only);
        REQUIRE(backend.last_print_tool_mapping().empty());

        // The table arriving in a later frame, task still configured, records it.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(crossover, {true, false, false, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }
}

// ============================================================================
// Resume-refusal classification
// ============================================================================

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

// ============================================================================
// Task 13: the insert rule on the feed-port edge (#1710)
// ============================================================================
// The port flag's false -> true edge is the insert; the tag reader's answer
// for the new spool lands a frame or two later in filament_detect.info. These
// tests pin the pending-verdict contract: the edge holds its judgement until
// the next info entry for the channel speaks, with a bounded fallback when no
// entry ever comes.

namespace {

struct SnapmakerInsertFixture : public LVGLUITestFixture {
    SnapmakerInsertFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);

        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));

        helix::ams::FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        ovr.spool_name = "PolyLite Orange";
        ovr.material = "PLA";
        ovr.color_rgb = 0xFF5500;
        SnapmakerTestAccess::seed_override(backend, 0, ovr);

        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& msg,
                                              uint32_t) { toasts.emplace_back(severity, msg); });
    }

    ~SnapmakerInsertFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    SnapmakerTmpCacheDir tmp{"task1710_pending_insert"};
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg{&api, nullptr};
    AmsBackendSnapmaker& backend = *backend_reg;
    std::vector<std::pair<ToastSeverity, std::string>> toasts;
};

} // namespace

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
                SnapmakerTestAccess::handle_status(backend, row.frames[i]);
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
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", false));
    CHECK(backend.slot_insert_identity(0) == InsertIdentity::NotRead);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", true));
    REQUIRE(backend.slot_insert_identity(0) == InsertIdentity::Pending);

    SECTION("a tagged spool") {
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "PLA", 0xFF00AA00u, "Polymaker",
                                                 json::array({144, 32, 196, 2})));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::FromTag);
    }
    SECTION("an untagged spool") {
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NoTag);
    }
    SECTION("a read that never lands") {
        for (int i = 0; i < 20; ++i)
            SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", true));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NoTag);
    }
    SECTION("the spool leaves before any read") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", false));
        CHECK(backend.slot_insert_identity(0) == InsertIdentity::NotRead);
    }
    // Other lanes saw no insert.
    CHECK(backend.slot_insert_identity(1) == InsertIdentity::NotRead);
}

namespace {
/// One pass of the insert watcher over the backend, as an AMS change runs it.
int judge(FilamentInsertWatcher& watcher, const AmsBackend& backend) {
    FilamentInsertContext ctx;
    ctx.setting_enabled = true;
    return watcher.judge_lanes(backend, ctx);
}
} // namespace

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "Snapmaker an idle tool parking an empty lane is no insert",
                 "[ams][snapmaker][insert_watcher]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);

    // T1 parks and the task config claims filament: both move the slot
    // status, but no spool went in.
    SnapmakerTestAccess::handle_status(
        backend, json{{"extruder1", json{{"park_pin", true}, {"active_pin", false}}}});
    CHECK(judge(watcher, backend) == -1);
    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_exist", json::array({false, true, false, false})}}}});
    CHECK(backend.slot_insert_presence(1).value_or(true) == false);
    CHECK(judge(watcher, backend) == -1);
}

TEST_CASE_METHOD(SnapmakerInsertFixture,
                 "Snapmaker an untagged insert opens the editor and nothing else",
                 "[ams][snapmaker][insert_watcher][1710]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);

    // The port sensor sees the spool; the reader has not answered.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", true));
    CHECK(judge(watcher, backend) == -1);

    // The reader finds no tag: the editor is this insert's one prompt.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
    drain();
    CHECK(judge(watcher, backend) == 0);
    CHECK(toasts.empty());
}

TEST_CASE_METHOD(SnapmakerInsertFixture, "Snapmaker a tag-identified insert opens nothing",
                 "[ams][snapmaker][insert_watcher][1710]") {
    helix::ui::reset_insert_offers_for_test();
    FilamentInsertWatcher watcher(nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "wait_insert", true));
    CHECK(judge(watcher, backend) == -1);
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF00AA00u, "Polymaker",
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
        SnapmakerTestAccess::handle_status(backend, make_feed_status(lane, "wait_insert", false));
    REQUIRE(judge(watcher, backend) == -1);
    for (int lane : {0, 1})
        SnapmakerTestAccess::handle_status(backend, make_feed_status(lane, "wait_insert", true));
    REQUIRE(judge(watcher, backend) == -1);
    REQUIRE(helix::ui::insert_prompt_claimed(0));
    REQUIRE(helix::ui::insert_prompt_claimed(1));

    SECTION("a lane the editor does not open for holds no claim") {
        for (int lane : {0, 1})
            SnapmakerTestAccess::handle_status(
                backend, make_filament_detect_status(lane, "NONE", 0u, "NONE", json::array()));
        CHECK(judge(watcher, backend) == 0);
        CHECK(helix::ui::insert_prompt_claimed(0));
        CHECK_FALSE(helix::ui::insert_prompt_claimed(1));
    }
    SECTION("a held insert a guard now blocks gives its claim back") {
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;
    CHECK_FALSE(backend.get_system_info().units[0].has_toolhead_sensor);
    SnapmakerTestAccess::handle_status(backend, t3_switch(false));
    CHECK(backend.get_system_info().units[0].has_toolhead_sensor);
}

// ============================================================================
// Golden sequence: one full status frame, then the deltas a U1 sends for a
// load, an unload, an RFID insert, a failed batch head and a temperature-only
// extruder update. Each frame is pinned as a text snapshot of what the parse
// leaves behind (slots, action, working slot, latch, pending-insert state)
// plus the side effects it dispatches after releasing the lock. Any reordering
// of the parse sections shows up here as a changed line.
// ============================================================================

namespace {

struct SnapmakerGoldenFixture : public LVGLUITestFixture {
    SnapmakerGoldenFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);

        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));

        helix::ams::FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        ovr.spool_name = "PolyLite Orange";
        ovr.material = "PLA";
        ovr.color_rgb = 0xFF5500;
        SnapmakerTestAccess::seed_override(backend, 0, ovr);

        SnapmakerTestAccess::set_use_batch_macro(backend, true);
        SnapmakerTestAccess::set_batch_macro_object(backend, "gcode_macro AUTO_FEEDING_BATCH");
        backend.set_event_callback(
            [this](const std::string& name, const std::string&) { events.push_back(name); });
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string&, uint32_t) { ++toasts; });
    }

    ~SnapmakerGoldenFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Feeds one frame and returns the snapshot line block for it.
    std::string feed(const json& frame) {
        events.clear();
        backend.captured_gcodes.clear();
        toasts = 0;
        SnapmakerTestAccess::handle_status(backend, frame);
        helix::ui::UpdateQueue::instance().drain();
        return snapshot();
    }

    std::string snapshot() {
        const AmsSystemInfo info = backend.get_system_info();
        std::string out =
            fmt::format("action={} phase={} detail='{}' working={} tool={} slot={} loaded={}\n",
                        ams_action_to_string(info.action), info.operation_phase,
                        info.operation_detail, info.operation_working_slot, info.current_tool,
                        info.current_slot, info.filament_loaded);
        for (int i = 0; i < 4; ++i) {
            const SlotInfo slot = backend.get_slot_info(i);
            const auto snap = backend.channel_snapshot(i);
            out += fmt::format(
                "  s{} {} {}/{}/{} #{:06X} n{}-{} b{} w{:.0f} latch={} pend={} state={} "
                "detected={}\n",
                i, slot_status_to_string(slot.status), slot.material, slot.brand, slot.spool_name,
                slot.color_rgb, slot.nozzle_temp_min, slot.nozzle_temp_max, slot.bed_temp,
                slot.total_weight_g, SnapmakerTestAccess::loaded_at_toolhead(backend, i),
                SnapmakerTestAccess::pending_insert_passes(backend, i), snap.state,
                snap.filament_detected);
        }
        std::string gcodes;
        for (const auto& g : backend.captured_gcodes) {
            gcodes += (gcodes.empty() ? "" : "|") + g;
        }
        std::string unloaded;
        for (int i = 0; i < 4; ++i) {
            if (AmsState::instance().was_slot_recently_unloaded(i)) {
                unloaded += std::to_string(i);
            }
        }
        out += fmt::format(
            "  batch_active={} port_present={} events={} gcodes={} toasts={} unloaded={}",
            backend.batch_plan().active, SnapmakerTestAccess::last_published_port_present(backend),
            events.size(), gcodes, toasts, unloaded);
        return out;
    }

    SnapmakerTmpCacheDir tmp{"golden_sequence"};
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredBackend<CapturingSnapmakerBackend> backend_reg{};
    CapturingSnapmakerBackend& backend = *backend_reg;
    std::vector<std::string> events;
    int toasts = 0;
};

json golden_extruder(const char* state, bool park, bool active) {
    return json{{"state", state},
                {"park_pin", park},
                {"active_pin", active},
                {"activating_move", false},
                {"extruder_offset", json::array({0.0, 0.0, 0.0})},
                {"switch_count", 3},
                {"retry_count", 0},
                {"error_count", 0}};
}

json golden_full_frame() {
    json info = json::array();
    info.push_back(json{{"MAIN_TYPE", "PLA"},
                        {"SUB_TYPE", "SnapSpeed"},
                        {"MANUFACTURER", "Snapmaker"},
                        {"VENDOR", "Snapmaker"},
                        {"ARGB_COLOR", 0xFF112233u},
                        {"HOTEND_MIN_TEMP", 190},
                        {"HOTEND_MAX_TEMP", 230},
                        {"BED_TEMP", 60},
                        {"WEIGHT", 1000},
                        {"CARD_UID", json::array({1, 2, 3, 4})}});
    for (int i = 1; i < 4; ++i) {
        info.push_back(json{{"MAIN_TYPE", "NONE"}});
    }
    return json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"extruder", golden_extruder("ACTIVE", false, true)},
        {"extruder1", golden_extruder("PARKED", true, false)},
        {"extruder2", golden_extruder("PARKED", true, false)},
        {"extruder3", golden_extruder("PARKED", true, false)},
        {"filament_detect", json{{"info", info}, {"state", json::array({1, 1, 1, 0})}}},
        {"filament_feed left", json{{"extruder0", json{{"filament_detected", true},
                                                       {"channel_state", "load_finish"},
                                                       {"channel_error", "ok"},
                                                       {"module_exist", true}}},
                                    {"extruder1", json{{"filament_detected", true},
                                                       {"channel_state", "preload_finish"}}}}},
        {"filament_feed right",
         json{{"extruder2", json{{"filament_detected", true}, {"channel_state", "preload_finish"}}},
              {"extruder3", json{{"filament_detected", false}, {"channel_state", "wait_insert"}}}}},
        {"print_task_config",
         json{
             {"filament_exist", json::array({true, true, true, false})},
             {"filament_type", json::array({"PLA", "PETG", "ABS", ""})},
             {"filament_vendor", json::array({"Snapmaker", "Generic", "Generic", ""})},
             {"filament_color_rgba", json::array({"112233FF", "AA0000FF", "00BB00FF", "FFFFFFFF"})},
             {"extruder_map_table", json::array({0, 1, 2, 3})},
             {"extruders_used", json::array({true, false, false, false})},
             {"auto_replenish_filament", true},
             {"filament_entangle_sen", "medium"},
             {"end_unload_filament", json::array({false, true})}}},
        {"filament_motion_sensor e0_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e1_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e2_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e3_filament",
         json{{"enabled", true}, {"filament_detected", false}}},
        {"gcode_macro AUTO_FEEDING_BATCH", json{{"doing", false}}}};
}

json golden_feed(int ext, const char* state, std::optional<bool> detected = std::nullopt) {
    json ch{{"channel_state", state}};
    if (detected) {
        ch["filament_detected"] = *detected;
    }
    return json{{ext <= 1 ? "filament_feed left" : "filament_feed right",
                 json{{"extruder" + std::to_string(ext), ch}}}};
}

} // namespace

namespace {
const std::string kGolden = R"GOLD(full frame
action=Idle phase=-1 detail='' working=-1 tool=0 slot=0 loaded=true
  s0 Loaded PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: pick
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: feeding
action=Loading phase=2 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=load_feeding detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: heating
action=Heating phase=3 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=load_heating detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: finish
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Loaded PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=true pend=0 state=load_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
unload: prepare
action=Unloading phase=0 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Loaded PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=true pend=0 state=unload_prepare detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
unload: finish
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: port drops
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Empty PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=false pend=0 state=wait_insert detected=false
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: port rises
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=false pend=2 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: reader answers NONE
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=1 unloaded=1
insert: head 3 rises
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available // #FFFFFF n0-0 b0 w-1 latch=false pend=2 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: head 3 tag read
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
batch: head 0 mid-load
action=Loading phase=2 detail='' working=0 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_feeding detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=true port_present=1 events=1 gcodes= toasts=0 unloaded=1
batch: head 0 fails
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes=AUTO_FEEDING_BATCH ACTION=END toasts=0 unloaded=1
extruder: temperature only
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=0 gcodes= toasts=0 unloaded=1
extruder: idle frame
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=0 gcodes= toasts=0 unloaded=1
)GOLD";
} // namespace

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
        {"insert: port drops", golden_feed(0, "wait_insert", false)},
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
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
    AmsBackendSnapmaker& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(backend, json{{"extruder1", json{{"state", "PARKED"},
                                                                        {"park_pin", true},
                                                                        {"active_pin", false},
                                                                        {"switch_count", 7}}}});
    REQUIRE(SnapmakerTestAccess::extruder_state(backend, 1).park_pin);

    // A temperature-only update names none of the tool-changer fields.
    SnapmakerTestAccess::handle_status(backend, json{{"extruder1", json{{"temperature", 215.5}}}});
    auto held = SnapmakerTestAccess::extruder_state(backend, 1);
    CHECK(held.state == "PARKED");
    CHECK(held.park_pin);
    CHECK(held.switch_count == 7);

    // A frame that does name a field overwrites only that field.
    SnapmakerTestAccess::handle_status(
        backend, json{{"extruder1", json{{"state", "ACTIVE"}, {"park_pin", false}}}});
    held = SnapmakerTestAccess::extruder_state(backend, 1);
    CHECK(held.state == "ACTIVE");
    CHECK_FALSE(held.park_pin);
    CHECK(held.switch_count == 7);
}

// ============================================================================
// Empty carriage: toolhead.extruder names the last head used, not a mounted
// one, so a head reporting PARKED is never the active tool
// (prestonbrown/helixscreen#1784).
// ============================================================================

namespace {
// The frame read from the U1 idle after a print: every head parked while
// toolhead.extruder still names extruder2.
json u1_all_parked_frame() {
    return json{{"extruder", json{{"state", "PARKED"}}},
                {"extruder1", json{{"state", "PARKED"}}},
                {"extruder2", json{{"state", "PARKED"}}},
                {"extruder3", json{{"state", "PARKED"}}},
                {"toolhead", json{{"extruder", "extruder2"}}}};
}
} // namespace

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
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend_reg(nullptr, nullptr);
        AmsBackendSnapmaker& backend = *backend_reg;
        for (size_t i = 0; i < row.steps.size(); ++i) {
            INFO(row.name << ", frame " << i);
            SnapmakerTestAccess::handle_status(backend, row.steps[i].frame);
            CHECK(backend.get_system_info().current_tool == row.steps[i].tool);
            CHECK(backend.get_system_info().current_slot == row.steps[i].tool);
        }
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker does not read a tool change in flight as a park",
                 "[ams][snapmaker][tool_switcher][1784]") {
    // The old head docks before the new one reports, so for a frame every head
    // reads PARKED.
    helix::test::RegisteredBackend<CapturingSnapmakerBackend> backend_reg;
    CapturingSnapmakerBackend& backend = *backend_reg;

    SnapmakerTestAccess::handle_status(
        backend, json{{"extruder1", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                      {"toolhead", json{{"extruder", "extruder1"}}}});
    REQUIRE(backend.get_system_info().current_tool == 1);

    SECTION("T<n> sent, nothing has left a dock yet") {
        REQUIRE(backend.change_tool(2).success());
        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == 1);

        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder2", json{{"state", "ACTIVE"}, {"active_pin", true}}},
                          {"toolhead", json{{"extruder", "extruder2"}}}});
        CHECK(backend.get_system_info().current_tool == 2);

        // The change landed: parking the new head empties the carriage again.
        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder2", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == -1);
    }
    SECTION("the new head is in its activating move") {
        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder2", json{{"state", "PARKED"}, {"activating_move", true}}}});
        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
        CHECK(backend.get_system_info().current_tool == 1);
    }
    SECTION("a park with no change in flight still empties the carriage") {
        SnapmakerTestAccess::handle_status(
            backend, json{{"extruder1", json{{"state", "PARKED"}, {"active_pin", false}}}});
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

// Four records in a real U1's lane_data shape: T0 and T1 are Snapmaker PLA with
// an undeclared SnapSpeed name, T2 is a Generic ASA catalog pick with no name,
// and T3 is a Generic ASA catalog pick with an undeclared SnapSpeed name. Load
// drops the product line only where the brand cannot carry it (T3), on disk as
// well as in memory; silent firmware then retracts the rest.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load drops a product line its brand cannot carry",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    const json snapmaker_pla = {{"bed_temp", 60},
                                {"color", "#080A0D"},
                                {"helix_declared", json::array({"color_rgb", "material"})},
                                {"helix_locked_color", true},
                                {"helix_locked_material", true},
                                {"helix_material", "PLA"},
                                {"material", "PLA"},
                                {"name", "SnapSpeed"},
                                {"nozzle_temp", 205},
                                {"remaining_weight_g", 350.0},
                                {"scan_time", "2026-10-16T05:13:06Z"},
                                {"spool_name", "SnapSpeed"},
                                {"total_weight_g", 500.0},
                                {"vendor", "Snapmaker"},
                                {"vendor_name", "Snapmaker"}};
    json t0 = snapmaker_pla;
    t0["lane"] = "0";
    json t1 = snapmaker_pla;
    t1["lane"] = "1";
    t1["color"] = "#E2DEDB";
    const json t2 = {{"bed_temp", 100},
                     {"color", "#1A1A1A"},
                     {"color_name", "Dark Gray"},
                     {"helix_catalog_id", "generic-asa"},
                     {"helix_locked_color", true},
                     {"helix_locked_material", true},
                     {"helix_material", "ASA"},
                     {"helix_product_name", "ASA"},
                     {"lane", "2"},
                     {"material", "ASA"},
                     {"nozzle_temp", 260},
                     {"scan_time", "2026-09-17T01:09:22Z"},
                     {"vendor", "Generic"},
                     {"vendor_name", "Generic"}};
    const json t3 = {{"bed_temp", 100},
                     {"color", "#FFFFFF"},
                     {"color_name", "White"},
                     {"helix_catalog_id", "generic-asa"},
                     {"helix_declared", json::array({"color_rgb", "material"})},
                     {"helix_fingerprint", "M|ASA|FFFFFF"},
                     {"helix_locked_color", true},
                     {"helix_locked_material", true},
                     {"helix_material", "ASA"},
                     {"helix_product_name", "ASA"},
                     {"lane", "3"},
                     {"material", "ASA"},
                     {"name", "SnapSpeed"},
                     {"nozzle_temp", 260},
                     {"scan_time", "2026-10-07T00:37:29Z"},
                     {"spool_name", "SnapSpeed"},
                     {"total_weight_g", 0.0},
                     {"vendor", "Generic"},
                     {"vendor_name", "Generic"}};
    mock_printer.client.mock_db_set("lane_data", "T0", t0);
    mock_printer.client.mock_db_set("lane_data", "T1", t1);
    mock_printer.client.mock_db_set("lane_data", "T2", t2);
    mock_printer.client.mock_db_set("lane_data", "T3", t3);

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);

    for (int i : {0, 1}) {
        auto ovr = SnapmakerTestAccess::get_override(*backend, i);
        REQUIRE(ovr.has_value());
        CHECK(ovr->spool_name == "SnapSpeed");
        CHECK(mock_printer.client.mock_db_get("lane_data", "T" + std::to_string(i))
                  .value("spool_name", "") == "SnapSpeed");
    }
    auto generic = SnapmakerTestAccess::get_override(*backend, 3);
    REQUIRE(generic.has_value());
    CHECK(generic->spool_name.empty());
    CHECK(generic->brand == "Generic");
    CHECK(generic->color_rgb == 0xFFFFFFu);
    const json stored = mock_printer.client.mock_db_get("lane_data", "T3");
    REQUIRE(!stored.is_null());
    CHECK_FALSE(stored.contains("spool_name"));
    CHECK_FALSE(stored.contains("name"));
    CHECK(stored.value("vendor", "") == "Generic");

    // The lanes paint from what the load filed: a tag reading nothing, with
    // print_task_config not yet heard from, leaves the stored identity standing.
    json none_info = json::array();
    for (int i = 0; i < 4; ++i) {
        none_info.push_back(json{{"VENDOR", "NONE"},
                                 {"MANUFACTURER", "NONE"},
                                 {"MAIN_TYPE", "NONE"},
                                 {"SUB_TYPE", "NONE"},
                                 {"CARD_UID", 0}});
    }
    SnapmakerTestAccess::handle_status(*backend,
                                       json{{"filament_detect", json{{"info", none_info}}}});
    CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
    CHECK(backend->get_slot_info(1).spool_name == "SnapSpeed");
    CHECK(backend->get_slot_info(2).spool_name.empty());
    CHECK(backend->get_slot_info(3).spool_name.empty());

    // The device's print_task_config: T2 names Generic ASA, the other three
    // channels name nothing and none is official. On those channels nothing in
    // the firmware stands behind the brand and product line the records only
    // remembered, so they retract, in memory and on disk. Colour and material
    // were declared and stay. T3's Generic rides its catalog pick and stays.
    SnapmakerTestAccess::handle_status(
        *backend,
        json{{"print_task_config",
              json{{"filament_vendor", json::array({"NONE", "NONE", "Generic", "NONE"})},
                   {"filament_type", json::array({"NONE", "NONE", "ASA", "NONE"})},
                   {"filament_sub_type", json::array({"NONE", "NONE", "Generic", "NONE"})},
                   {"filament_official", json::array({false, false, false, false})}}}});

    for (int i : {0, 1}) {
        INFO("slot " << i);
        const SlotInfo info = backend->get_slot_info(i);
        CHECK(info.spool_name.empty());
        CHECK(info.brand.empty());
        CHECK(info.material == "PLA");
        auto ovr = SnapmakerTestAccess::get_override(*backend, i);
        REQUIRE(ovr.has_value());
        CHECK(ovr->spool_name.empty());
        CHECK(ovr->brand.empty());
        CHECK(ovr->material == "PLA");
        const json rec = mock_printer.client.mock_db_get("lane_data", "T" + std::to_string(i));
        REQUIRE(!rec.is_null());
        CHECK_FALSE(rec.contains("spool_name"));
        CHECK(rec.value("vendor", "") != "Snapmaker");
        CHECK(rec.value("material", "") == "PLA");
    }
    CHECK(backend->get_slot_info(1).color_rgb == 0xE2DEDBu);
    CHECK(backend->get_slot_info(2).brand == "Generic");
    CHECK(backend->get_slot_info(3).brand == "Generic");
    CHECK(SnapmakerTestAccess::get_override(*backend, 3)->brand == "Generic");
    CHECK(mock_printer.client.mock_db_get("lane_data", "T3").value("vendor", "") == "Generic");
}

// The retraction needs the whole channel silent. A tag that names a spool, or
// an official print_task_config entry, is the firmware standing behind an
// identity, and a remembered brand and product line stay beside it.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker keeps remembered identity while firmware states one",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    const json record = {
        {"color", "#080A0D"},      {"helix_declared", json::array({"color_rgb", "material"})},
        {"helix_material", "PLA"}, {"lane", "0"},
        {"material", "PLA"},       {"spool_name", "SnapSpeed"},
        {"vendor", "Snapmaker"}};
    mock_printer.client.mock_db_set("lane_data", "T0", record);

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);

    json silent_ptc = {{"print_task_config",
                        json{{"filament_vendor", json::array({"NONE", "NONE", "NONE", "NONE"})},
                             {"filament_official", json::array({false, false, false, false})}}}};

    SECTION("an official entry") {
        silent_ptc["print_task_config"]["filament_official"] =
            json::array({true, false, false, false});
        SnapmakerTestAccess::handle_status(
            *backend,
            json{{"filament_detect", json{{"info", json::array({json{{"MAIN_TYPE", "NONE"},
                                                                     {"SUB_TYPE", "NONE"}}})}}}});
        SnapmakerTestAccess::handle_status(*backend, silent_ptc);
    }
    SECTION("a tag naming the spool") {
        SnapmakerTestAccess::handle_status(
            *backend, json{{"filament_detect",
                            json{{"info", json::array({json{{"MAIN_TYPE", "PLA"},
                                                            {"VENDOR", "Snapmaker"},
                                                            {"SUB_TYPE", "SnapSpeed"}}})}}}});
        SnapmakerTestAccess::handle_status(*backend, silent_ptc);
    }
    SECTION("no tag reading yet") {
        SnapmakerTestAccess::handle_status(*backend, silent_ptc);
    }

    auto ovr = SnapmakerTestAccess::get_override(*backend, 0);
    REQUIRE(ovr.has_value());
    CHECK(ovr->brand == "Snapmaker");
    CHECK(ovr->spool_name == "SnapSpeed");
    CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
}

namespace {
/// A Snapmaker PLA record in a real U1's lane_data shape: colour and material
/// declared, the brand and SnapSpeed name only remembered.
json remembered_snapspeed_record(int slot_index) {
    return json{
        {"color", "#080A0D"},      {"helix_declared", json::array({"color_rgb", "material"})},
        {"helix_material", "PLA"}, {"lane", std::to_string(slot_index)},
        {"material", "PLA"},       {"spool_name", "SnapSpeed"},
        {"vendor", "Snapmaker"}};
}

/// One frame of each firmware source naming nothing on every channel.
void feed_silent_channels(AmsBackendSnapmaker& backend) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(json{{"MAIN_TYPE", "NONE"}, {"SUB_TYPE", "NONE"}});
    }
    SnapmakerTestAccess::handle_status(backend, json{{"filament_detect", json{{"info", info}}}});
    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_vendor", json::array({"NONE", "NONE", "NONE", "NONE"})},
                            {"filament_official", json::array({false, false, false, false})}}}});
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load keeps a product line the record declares",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    json record = {{"color", "#C0C0C0"},      {"lane", "0"},          {"material", "PLA"},
                   {"helix_material", "PLA"}, {"spool_name", "Silk"}, {"vendor", "eSUN"}};
    SECTION("declared by name") {
        record["helix_declared"] = json::array({"color_rgb", "material", "spool_name"});
    }
    SECTION("declared by a record older than the declared set") {
        record["helix_locked_color"] = true;
    }
    mock_printer.client.mock_db_set("lane_data", "T0", record);

    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);

    auto ovr = SnapmakerTestAccess::get_override(*backend, 0);
    REQUIRE(ovr.has_value());
    CHECK(ovr->spool_name == "Silk");
    CHECK(mock_printer.client.mock_db_get("lane_data", "T0").value("spool_name", "") == "Silk");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker retracts a silent channel's product line once",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    mock_printer.client.mock_db_set("lane_data", "T0", remembered_snapspeed_record(0));
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);

    feed_silent_channels(*backend);
    REQUIRE(backend->get_slot_info(0).spool_name.empty());
    REQUIRE_FALSE(mock_printer.client.mock_db_get("lane_data", "T0").contains("spool_name"));

    // Anything saved again would overwrite this marker.
    json marker = mock_printer.client.mock_db_get("lane_data", "T0");
    marker["test_marker"] = true;
    mock_printer.client.mock_db_set("lane_data", "T0", marker);
    feed_silent_channels(*backend);
    feed_silent_channels(*backend);
    CHECK(mock_printer.client.mock_db_get("lane_data", "T0").value("test_marker", false));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker retraction leaves a stored record it does not describe alone",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    mock_printer.client.mock_db_set("lane_data", "T0", remembered_snapspeed_record(0));
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);
    const auto loaded = SnapmakerTestAccess::get_override(*backend, 0);
    REQUIRE(loaded.has_value());
    helix::ams::FilamentSlotOverride record = *loaded;

    SECTION("a record bound to a Spoolman spool") {
        record.spoolman_id = 42;
        SnapmakerTestAccess::replace_override_record(*backend, 0, record);
        feed_silent_channels(*backend);
        CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
        const auto kept = SnapmakerTestAccess::get_override(*backend, 0);
        REQUIRE(kept.has_value());
        CHECK(kept->spool_name == "SnapSpeed");
        CHECK(kept->brand == "Snapmaker");
    }
    SECTION("a record holding other values") {
        record.spool_name = "Bench reel";
        record.brand = "Polymaker";
        SnapmakerTestAccess::replace_override_record(*backend, 0, record);
        feed_silent_channels(*backend);
        CHECK(backend->get_slot_info(0).spool_name.empty());
        const auto kept = SnapmakerTestAccess::get_override(*backend, 0);
        REQUIRE(kept.has_value());
        CHECK(kept->spool_name == "Bench reel");
        CHECK(kept->brand == "Polymaker");
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker re-hears channel silence after a reconnect",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    mock_printer.client.mock_db_set("lane_data", "T0", remembered_snapspeed_record(0));
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&api, nullptr);
    SnapmakerTestAccess::call_on_started(*backend);

    // The tag's silence, heard before the reconnect.
    json info = json::array({json{{"MAIN_TYPE", "NONE"}, {"SUB_TYPE", "NONE"}}});
    SnapmakerTestAccess::handle_status(*backend, json{{"filament_detect", json{{"info", info}}}});

    SnapmakerTestAccess::call_on_started(*backend);
    SnapmakerTestAccess::handle_status(
        *backend, json{{"print_task_config",
                        json{{"filament_vendor", json::array({"NONE", "NONE", "NONE", "NONE"})},
                             {"filament_official", json::array({false, false, false, false})}}}});

    // print_task_config alone is half a verdict; the tag has not spoken since.
    CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
    CHECK(SnapmakerTestAccess::get_override(*backend, 0)->spool_name == "SnapSpeed");
}

// Keeping a slot's identity across a Spoolman unlink is the user's choice, so
// what they kept is theirs. A channel whose firmware states nothing (stock
// firmware, where our write-back 404s, leaves every untagged channel so) must
// not retract it the way it retracts a brand a record merely remembered. A
// product-line literal ("Silk") is firmware vocabulary only beside the vendor
// that sells it, so beside eSUN it names an eSUN product and stands too.
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker keeps the identity an unlink kept on a silent channel",
                 "[ams][snapmaker][spoolman][spool_name]") {
    const std::pair<const char*, const char*> kept[] = {{"Polymaker", "PolyLite"},
                                                        {"eSUN", "Silk"}};
    for (const auto& [brand, line] : kept) {
        INFO(brand << " " << line);
        MockPrinter mock_printer;
        helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(&mock_printer.api, nullptr);
        SnapmakerTestAccess::call_on_started(*backend);

        SlotInfo linked = backend->get_slot_info(0);
        linked.spoolman_id = 42;
        linked.brand = brand;
        linked.material = "PLA";
        linked.spool_name = line;
        helix::test::edit_slot_as_user(*backend, 0, linked);
        SpoolInfo spool;
        spool.id = 42;
        spool.vendor = brand;
        spool.material = "PLA";
        spool.filament_name = line;
        helix::test::spool_states(*backend, 0, spool);

        SlotInfo unlinked = backend->get_slot_info(0);
        REQUIRE(unlinked.brand == brand);
        unlinked.spoolman_id = 0;
        unlinked.spoolman_filament_id = 0;
        unlinked.spoolman_vendor_id = 0;
        helix::test::edit_slot_as_user(*backend, 0, unlinked);
        REQUIRE(backend->get_slot_info(0).spoolman_id == 0);
        REQUIRE(backend->get_slot_info(0).spool_name == line);

        feed_silent_channels(*backend);

        CHECK(backend->get_slot_info(0).brand == brand);
        CHECK(backend->get_slot_info(0).spool_name == line);
        auto ovr = SnapmakerTestAccess::get_override(*backend, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == brand);
        CHECK(ovr->spool_name == line);
    }
}

// A reconnect starts from no verdict, whether or not a store loads: a frame
// arriving while the load runs must not be judged against the last session.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker forgets channel silence as a start begins",
                 "[ams][snapmaker][filament_slot_override][spool_name]") {
    helix::test::RegisteredBackend<AmsBackendSnapmaker> backend(nullptr, nullptr);
    helix::ams::FilamentSlotOverride record;
    record.brand = "Snapmaker";
    record.material = "PLA";
    record.spool_name = "SnapSpeed";
    SnapmakerTestAccess::seed_override(*backend, 0, record);

    json info = json::array({json{{"MAIN_TYPE", "NONE"}, {"SUB_TYPE", "NONE"}}});
    SnapmakerTestAccess::handle_status(*backend, json{{"filament_detect", json{{"info", info}}}});
    REQUIRE(backend->get_slot_info(0).spool_name == "SnapSpeed");

    SnapmakerTestAccess::call_on_started(*backend);
    SnapmakerTestAccess::handle_status(
        *backend, json{{"print_task_config",
                        json{{"filament_vendor", json::array({"NONE", "NONE", "NONE", "NONE"})},
                             {"filament_official", json::array({false, false, false, false})}}}});

    CHECK(backend->get_slot_info(0).spool_name == "SnapSpeed");
}

// ============================================================================
// print_task_config filament record: SET_PRINT_FILAMENT_CONFIG write + resume repair
// ============================================================================
//
// The U1 refuses RESUME ("e<N> not edit filament") while a head the print uses
// has no filament_type in print_task_config. Only an RFID spool or
// SET_PRINT_FILAMENT_CONFIG fills it, so a material set in HelixScreen has to
// reach that record too.

namespace {

/// print_task_config frame naming each head's filament type and official flag,
/// plus the heads the task uses when @p used is given.
json make_task_filament(const std::array<std::string, 4>& types,
                        const std::array<bool, 4>& official = {false, false, false, false},
                        std::optional<std::vector<bool>> used = std::nullopt) {
    json ptc = {
        {"filament_type", json::array({types[0], types[1], types[2], types[3]})},
        {"filament_official", json::array({official[0], official[1], official[2], official[3]})}};
    if (used) {
        json arr = json::array();
        for (bool b : *used) {
            arr.push_back(b);
        }
        ptc["extruders_used"] = arr;
    }
    return json{{"print_task_config", ptc}};
}

struct FilamentRecordHarness {
    SnapmakerTmpCacheDir tmp;
    MockPrinter printer;
    helix::test::RegisteredBackend<AmsBackendSnapmaker> reg;
    AmsBackendSnapmaker& backend;

    /// @p print_state is the print_stats.state the backend's first frame
    /// names; empty sends no print_stats at all.
    explicit FilamentRecordHarness(const std::string& name,
                                   const std::string& print_state = "standby")
        : tmp(name), reg(&printer.api, nullptr), backend(*reg) {
        // The API mock's halt gate reads its own state.
        printer.state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &printer.api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));
        if (!print_state.empty()) {
            print(print_state);
        }
    }

    /// A frame naming only print_stats.state.
    void print(const std::string& state) {
        frame(json{{"print_stats", {{"state", state}}}});
    }

    void frame(const json& status) {
        SnapmakerTestAccess::handle_status(backend, status);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    [[nodiscard]] std::vector<std::string> history() const {
        return printer.client.gcode_script_history();
    }

    [[nodiscard]] std::vector<std::string> config_writes() const {
        std::vector<std::string> out;
        for (const auto& s : printer.client.gcode_script_history()) {
            if (s.find("SET_PRINT_FILAMENT_CONFIG") != std::string::npos) {
                out.push_back(s);
            }
        }
        return out;
    }
};

SlotInfo inland_pla() {
    SlotInfo edit;
    edit.brand = "Inland";
    edit.spool_name = "My Custom Spool";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    return edit;
}

/// Global printer state paused on a plain runout: recoverable, not Terminal.
void pause_on_runout() {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                               {"print_stats", {{"state", "paused"}}},
                               {"virtual_sdcard", {{"is_active", false}}}});
}

size_t index_of_containing(const std::vector<std::string>& history, const std::string& needle) {
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].find(needle) != std::string::npos) {
            return i;
        }
    }
    return history.size();
}

} // namespace

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

namespace {
/// filament_detect as a U1 with its reader off reports it: every head a scalar
/// CARD_UID, no identity, OFFICIAL as the endpoint last left it.
json make_rig_detect(const std::array<json, 4>& card_uids,
                     const std::array<bool, 4>& official = {false, false, false, false}) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(json{{"MAIN_TYPE", "NONE"},
                            {"CARD_UID", card_uids[i]},
                            {"OFFICIAL", official[i]},
                            {"SKU", 0}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}
} // namespace

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

namespace {
/// filament_detect frame whose entry for @p head carries @p card_uid and no
/// filament identity: an untagged channel, or a filament_detect/set write that
/// was later cleared.
json make_detect_card(int head, const json& card_uid) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", "NONE"}, {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit forces the record of an official head with no card",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_forced");
    h.frame(make_detect_card(2, json::array({0, 0, 0, 0})));
    h.frame(make_task_filament({"NONE", "NONE", "ASA", "NONE"}, {false, false, true, false}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 2, inland_pla()).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");

    // The same head with a real card is the tag's to keep.
    h.frame(make_detect_card(2, json::array({144, 32, 196, 2})));
    h.printer.client.clear_gcode_script_history();
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(helix::test::apply_edit(h.backend, 2, petg).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

namespace {
/// print_task_config frame carrying only a routing table and the used-heads flags.
json make_task_routing(std::vector<int> map, std::vector<bool> used) {
    map.resize(32, 0);
    json map_arr = json::array();
    for (int head : map) {
        map_arr.push_back(head);
    }
    json used_arr = json::array();
    for (bool b : used) {
        used_arr.push_back(b);
    }
    return json{
        {"print_task_config", json{{"extruder_map_table", map_arr}, {"extruders_used", used_arr}}}};
}
} // namespace

// Before RESUME, a head the print uses whose firmware record is blank gets the
// material the user declared, so the firmware does not refuse the resume.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume repairs a used head's blank record",
                 "[ams][snapmaker][filament_record][resume]") {
    const std::string set = "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=";
    const std::vector<bool> uses_1 = {false, true, false, false};
    const json blank_1 =
        make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false}, uses_1);
    struct Row {
        const char* name;
        bool terminal; // dirty-bed pause instead of a runout
        std::vector<int> declared;
        std::vector<json> frames;
        int slot;
        size_t writes;
        std::vector<std::string> prefixes; // of the first writes, in order
        std::optional<AmsResult> result;
    };
    const Row rows[] = {
        {"a used head's blank record",
         false,
         {1},
         {blank_1},
         1,
         2,
         {set + "1 VENDOR=Generic FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic"},
         AmsResult::SUCCESS},
        {"a head whose material is unknown", false, {}, {blank_1}, 1, 0, {}, AmsResult::SUCCESS},
        {"a head the print does not use",
         false,
         {2},
         {make_task_filament({"NONE", "PLA", "NONE", "NONE"}, {false, false, false, false},
                             uses_1)},
         1,
         0,
         {},
         AmsResult::SUCCESS},
        {"a Terminal pause", true, {1}, {blank_1}, 1, 0, {}, AmsResult::RESUME_REQUIRES_RESTART},
        {"a Terminal pause with no active tool",
         true,
         {1},
         {blank_1},
         -1,
         0,
         {},
         AmsResult::RESUME_REQUIRES_RESTART},
        // The Extended Firmware's filament_detect/set marks a head official and a
        // clear leaves it so; with no card behind it a plain write is refused.
        {"a head official only by an endpoint write",
         false,
         {1},
         {make_detect_card(1, 0),
          make_task_filament({"NONE", "NONE", "ASA", "ASA"}, {false, true, false, true}, uses_1)},
         1,
         2,
         {set + "1 VENDOR=Generic FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1",
          set + "1 FILAMENT_COLOR_RGBA=FF5500FF FORCE=1"},
         AmsResult::SUCCESS},
        {"a real RFID card's record",
         false,
         {1},
         {make_detect_card(1, json::array({144, 32, 196, 2})),
          make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, true, false, false},
                             uses_1)},
         1,
         0,
         {},
         AmsResult::SUCCESS},
        // Heads 0 and 2, paused on 0: extruders_used reads all false mid-print,
        // so head 2 is known only from the task's map.
        {"a blank head the task routes to but is not paused on",
         false,
         {1, 2},
         {make_task_routing({0, 2, 0, 0}, {true, false, true, false}),
          make_task_routing({0, 2, 0, 0}, {false, false, false, false}),
          make_task_filament({"PLA", "NONE", "NONE", "NONE"})},
         0,
         2,
         {set + "2 "},
         AmsResult::SUCCESS},
        {"all-false used heads repair the head paused on",
         false,
         {1, 2},
         {make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                             std::vector<bool>{false, false, false, false})},
         1,
         2,
         {set + "1 "},
         std::nullopt},
        {"before the firmware record has been seen", false, {1}, {}, 1, 0, {}, AmsResult::SUCCESS},
    };
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.name) {
            if (row.terminal) {
                PrinterState& ps = get_printer_state();
                PrinterStateTestAccess::reset(ps);
                ps.init_subjects(false);
                ps.update_from_status(
                    json{{"print_stats",
                          {{"state", "paused"},
                           {"exception",
                            {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}},
                         {"virtual_sdcard", {{"is_active", false}}}});
            } else {
                pause_on_runout();
            }
            FilamentRecordHarness h("resume_repair");
            for (const int slot : row.declared)
                helix::test::edit_slot_as_user(h.backend, slot, inland_pla());
            for (const auto& f : row.frames)
                h.frame(f);
            SnapmakerTestAccess::set_current_slot(h.backend, row.slot);
            h.printer.client.clear_gcode_script_history();

            std::optional<AmsError> captured;
            h.backend.prepare_for_resume(row.slot, [&](const AmsError& err) { captured = err; });
            FilamentRecordHarness::drain();

            const auto writes = h.config_writes();
            REQUIRE(writes.size() == row.writes);
            for (size_t i = 0; i < row.prefixes.size(); ++i)
                CHECK(writes[i].rfind(row.prefixes[i], 0) == 0);
            if (row.result) {
                REQUIRE(captured.has_value());
                CHECK(captured->result == *row.result);
            }
        }
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a refused head repair still repairs the next head and readies once",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_refused");
    helix::test::edit_slot_as_user(h.backend, 1, inland_pla());
    helix::test::edit_slot_as_user(h.backend, 2, inland_pla());
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, true, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.printer.client.clear_gcode_script_history();
    h.printer.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "refused",
                                            "CONFIG_EXTRUDER=1");

    int ready_calls = 0;
    bool success = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) {
        ++ready_calls;
        success = err.success();
    });
    FilamentRecordHarness::drain();

    CHECK(ready_calls == 1);
    CHECK(success);
    const auto hist = h.history();
    CHECK(index_of_containing(hist, "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 ") < hist.size());
}

// ============================================================================
// Official-flag repair: a card-less head flagged official by an endpoint write
// ============================================================================

namespace {

/// A head as an Extended Firmware filament_detect/set write leaves it: flagged
/// official in print_task_config, holding @p type in @p rgba.
json make_official_task(int head, const std::string& type, const std::string& rgba) {
    json ptc = make_task_filament({"NONE", "NONE", "NONE", "NONE"});
    ptc["print_task_config"]["filament_type"][head] = type;
    ptc["print_task_config"]["filament_official"][head] = true;
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FFFFFFFF", "FFFFFFFF", "FFFFFFFF"});
    ptc["print_task_config"]["filament_color_rgba"][head] = rgba;
    return ptc;
}

/// The filament_detect entry that same write leaves: filled, no card behind it.
json make_written_entry(int head, const std::string& type, const json& card_uid = 0) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", type},
                                        {"VENDOR", "Generic"},
                                        {"ARGB_COLOR", 0xFFFF5500},
                                        {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}

} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker clears the official flag on a card-less head it wrote",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    FilamentRecordHarness h("official_repair_ours");
    helix::test::edit_slot_as_user(h.backend, 3, inland_pla());
    h.printer.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");
    CHECK(writes[1] ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 FILAMENT_COLOR_RGBA=FF5500FF FORCE=1");

    // Once per session: the same frames again send nothing.
    h.printer.client.clear_gcode_script_history();
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker leaves an official flag it cannot prove it wrote",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    FilamentRecordHarness h("official_repair_not_ours");
    helix::test::edit_slot_as_user(h.backend, 3, inland_pla());
    h.printer.client.clear_gcode_script_history();

    SECTION("a real card behind the head") {
        h.frame(make_written_entry(3, "PLA", json::array({144, 32, 196, 2})));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("an empty entry, which a reader that is off reports for a real spool") {
        h.frame(make_detect_card(3, 0));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("a channel that showed a card earlier this session") {
        h.frame(make_written_entry(3, "PLA", json::array({144, 32, 196, 2})));
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("no card reading seen yet") {
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("a record naming another material") {
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PETG", "FF5500FF"));
    }
    SECTION("a record naming another colour") {
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PLA", "00FF00FF"));
    }
    SECTION("a head with no stored edit") {
        h.frame(make_written_entry(2, "PLA"));
        h.frame(make_official_task(2, "PLA", "FF5500FF"));
    }
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker holds the official-flag repair while a print holds the heads",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    const std::string state = GENERATE(std::string("printing"), std::string("paused"));
    FilamentRecordHarness h("official_repair_mid_print", state);
    helix::test::edit_slot_as_user(h.backend, 3, inland_pla());
    h.printer.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());

    h.print("complete");
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker sends no repair before its own frames name a print state",
                 "[ams][snapmaker][filament_record][official_repair]") {
    // The app's print state may not be known when the first frames land; the
    // backend's own print_stats is the only answer it trusts.
    lv_init_safe();
    FilamentRecordHarness h("official_repair_no_state", /*print_state=*/"");
    helix::test::edit_slot_as_user(h.backend, 3, inland_pla());
    h.printer.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());

    h.print("standby");
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker an unchanged save on a forced head sends nothing",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_forced_unchanged");
    json ptc = make_official_task(1, "PLA", "FF5500FF");
    ptc["print_task_config"]["filament_vendor"] = json::array({"NONE", "Generic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] = json::array({"NONE", "Basic", "NONE", "NONE"});
    h.frame(make_detect_card(1, 0));
    h.frame(ptc);
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    // The control: a change on the same head goes out forced.
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(helix::test::apply_edit(h.backend, 1, petg).success());
    FilamentRecordHarness::drain();
    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].find("FORCE=1") != std::string::npos);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker holds a save made mid-print until the heads are free",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_mid_print", "printing");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    h.print("printing");
    CHECK(h.config_writes().empty());

    // A pause the print raised (a runout, an M600) is no moment for it.
    h.print("paused");
    CHECK(h.config_writes().empty());

    h.print("complete");
    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                          "FILAMENT_TYPE=PLA",
                          0) == 0);

    // Sent once.
    h.printer.client.clear_gcode_script_history();
    h.print("standby");
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a save made while paused goes straight out",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_while_paused", "paused");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker drops a held save with the head's override",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_held_dropped", "printing");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();
    h.printer.client.clear_gcode_script_history();

    SECTION("a Clear Spool") {
        h.backend.clear_slot_override(1);
    }
    SECTION("a restart") {
        SnapmakerTestAccess::call_on_started(h.backend);
    }
    h.print("complete");
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a product-line change alone rewrites the identity",
                 "[ams][snapmaker][filament_record]") {
    // The record's vendor already matches, so only print_task_config's
    // filament_sub_type can tell this save from a re-save.
    lv_init_safe();
    FilamentRecordHarness h("record_subtype_only");
    json ptc = make_task_filament({"NONE", "PLA", "NONE", "NONE"});
    ptc["print_task_config"]["filament_vendor"] =
        json::array({"NONE", "Snapmaker", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] = json::array({"NONE", "Basic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FF5500FF", "FFFFFFFF", "FFFFFFFF"});
    h.frame(ptc);
    h.printer.client.clear_gcode_script_history();

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

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker drops a held save when the spool is swapped",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_held_swap", "printing");
    h.frame(make_written_entry(1, "PLA", json::array({1, 2, 3, 4})));
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
    FilamentRecordHarness::drain();
    REQUIRE(SnapmakerTestAccess::get_override(h.backend, 1).has_value());

    // A different card with a different material: a new spool.
    h.frame(make_written_entry(1, "PETG", json::array({9, 9, 9, 9})));
    REQUIRE_FALSE(SnapmakerTestAccess::get_override(h.backend, 1).has_value());
    h.printer.client.clear_gcode_script_history();
    h.print("complete");
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker an edit never POSTs to filament_detect/set",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_never_posts");
    h.frame(make_written_entry(0, "PLA"));
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));

    SlotInfo snap = inland_pla();
    snap.brand = "Snapmaker";
    snap.spool_name = "SnapSpeed";
    snap.nozzle_temp_min = 190;
    snap.bed_temp = 60;
    for (int head = 0; head < 4; ++head) {
        REQUIRE(helix::test::apply_edit(h.backend, head, snap).success());
    }
    FilamentRecordHarness::drain();
    CHECK_FALSE(h.config_writes().empty());
    CHECK(h.printer.api.rest_mock().mock_get_post_history().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker files no vendor reading from a filled entry with no card",
                 "[ams][snapmaker][rfid]") {
    lv_init_safe();
    FilamentRecordHarness h("written_entry_not_a_reading");
    h.frame(make_written_entry(2, "PETG", json::array({144, 32, 196, 2})));
    REQUIRE(helix::ams::lane_sources(h.backend.lane_id(2)).vendor_cache.has_value());

    // The same spool's entry rewritten over the API: CARD_UID 0.
    h.frame(make_written_entry(2, "ABS"));
    CHECK_FALSE(helix::ams::lane_sources(h.backend.lane_id(2)).vendor_cache.has_value());
    CHECK(h.backend.get_slot_info(2).material != "ABS");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker sync_external_identity writes nothing to firmware",
                 "[ams][snapmaker][filament_record]") {
    // A preview edit is in-memory only.
    lv_init_safe();
    FilamentRecordHarness h("record_sync_external");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(h.backend.sync_external_identity(0, inland_pla()).success());
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
    CHECK(h.printer.api.rest_mock().mock_get_post_history().empty());
}

// ============================================================================
// print_task_config drift: the firmware forgets a head's record when filament
// leaves the toolhead (an unload, unload-at-end, a runout), while the lane's
// declaration stands for the spool still in the feeder.
// ============================================================================

namespace {

/// print_task_config frame naming one head's type and colour; the other heads
/// are left out.
json make_head_record(int head, const std::string& type, const std::string& rgba) {
    json types = json::array();
    json colours = json::array();
    for (int i = 0; i < 4; ++i) {
        types.push_back(i == head ? type : "PLA");
        colours.push_back(i == head ? rgba : "FFFFFFFF");
    }
    return json{{"print_task_config",
                 {{"filament_type", types},
                  {"filament_color_rgba", colours},
                  {"filament_official", json::array({false, false, false, false})}}}};
}

SlotInfo polymaker_asa() {
    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "ASA";
    edit.color_rgb = 0xFF9800;
    return edit;
}

/// A harness whose head 0 holds a declared Polymaker ASA, written to the
/// firmware and echoed back, with its spool resting in the feeder port.
void declare_head0_asa(FilamentRecordHarness& h) {
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();
}

} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes a declared spool back after the firmware drops its record",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore");
    declare_head0_asa(h);

    // Filament left the toolhead: the firmware resets the head to NONE / white.
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic "
                       "FILAMENT_TYPE=ASA FILAMENT_SUBTYPE=Basic");
    CHECK(writes[1] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 FILAMENT_COLOR_RGBA=FF9800FF");
    CHECK(h.backend.get_slot_info(0).material == "ASA");

    SECTION("the same silence again sends nothing more") {
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().empty());
    }
    SECTION("a later drop, after the record came back, is written back again") {
        h.frame(make_head_record(0, "ASA", "FF9800FF"));
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().size() == 2);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker holds a dropped record's write-back until the print stops",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_printing");
    declare_head0_asa(h);

    h.print("printing");
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().empty());

    h.print("complete");
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes no dropped record back for a spool that may have changed",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_spool_left");
    declare_head0_asa(h);

    SECTION("the feeder port is empty") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().empty());
    }
    SECTION("a spool went into the port since the declaration") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().empty());

        // Saving the slot again declares the new spool, and writes it.
        helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().size() == 2);
    }
    SECTION("a feed is still moving filament on the head") {
        h.frame(make_feed_status(0, "unload_doing", /*filament_detected=*/true));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().empty());

        h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
        CHECK(h.config_writes().size() == 2);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker holds a dropped record's write-back while the offset calibration "
                 "holds the heads",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_calibrating");
    declare_head0_asa(h);

    h.frame(json{{"machine_state_manager", {{"main_state", 2}}}});
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().empty());

    SECTION("the calibration ends: it is written back") {
        h.frame(json{{"machine_state_manager", {{"main_state", 0}}}});
        CHECK(h.config_writes().size() == 2);
    }
    SECTION("a user edit waits too") {
        helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().empty());
        h.frame(json{{"machine_state_manager", {{"main_state", 0}}}});
        CHECK_FALSE(h.config_writes().empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker forgets a pending calibration exit once the state reads free",
                 "[ams][snapmaker][nozzle_offset][reconcile]") {
    lv_init_safe();
    FilamentRecordHarness h("pending_exit_state_free");
    helix::snapmaker::nozzle_offset::mark_exit_pending();
    REQUIRE(helix::snapmaker::nozzle_offset::exit_pending());

    h.frame(json{{"machine_state_manager", {{"main_state", 2}}}});
    CHECK(helix::snapmaker::nozzle_offset::exit_pending());

    h.frame(json{{"machine_state_manager", {{"main_state", 0}}}});
    CHECK_FALSE(helix::snapmaker::nozzle_offset::exit_pending());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes no dropped record back while a tag read is pending",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_read_pending");
    declare_head0_asa(h);

    // A spool goes in and is declared before the reader answers for it.
    h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
    h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
    REQUIRE(h.backend.slot_insert_identity(0) == InsertIdentity::Pending);
    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();

    REQUIRE(h.backend.slot_insert_identity(0) == InsertIdentity::Pending);
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes nothing back for a head with no declared material",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_undeclared");
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();

    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit's own write is not repeated before the firmware echoes it",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_edit_echo");
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    h.printer.client.clear_gcode_script_history();

    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    REQUIRE(h.config_writes().size() == 2);

    // A frame from before the write landed still reads NONE.
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    CHECK(h.config_writes().size() == 2);
}

// ============================================================================
// At a print's start, each head its file uses that its feeder reports not
// loaded, and that the firmware will not load itself, gets one warning.
// ============================================================================

namespace {
json used_heads(const std::vector<bool>& used) {
    json arr = json::array();
    for (bool b : used) {
        arr.push_back(b);
    }
    return json{{"print_task_config", {{"extruders_used", arr}}}};
}

/// Head @p head's feeder, served automatically, reading @p state with no error.
json feeder(int head, const std::string& state) {
    const char* side = head <= 1 ? "filament_feed left" : "filament_feed right";
    return json{{side,
                 {{"extruder" + std::to_string(head),
                   {{"filament_detected", true},
                    {"channel_state", state},
                    {"channel_error", "ok"},
                    {"module_exist", true},
                    {"disable_auto", false}}}}}};
}

struct Warnings {
    std::vector<std::string> seen;
    Warnings() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { seen.push_back(m); });
    }
    ~Warnings() {
        helix::ui::set_test_notification_warning_hook(nullptr);
    }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
};

/// The file read answers @p tools and records the files it was asked about.
struct FileToolsProbe {
    std::vector<std::string> asked;
    FileToolsProbe(AmsBackendSnapmaker& b, std::set<int> tools) {
        SnapmakerTestAccess::set_file_tools_probe(
            b, [this, tools](const std::string& file, std::function<void(std::set<int>)> answer) {
                asked.push_back(file);
                answer(tools);
            });
    }
};

/// A slicer-started print: no task, heads 0 and 1 loaded, 2 and 3 not.
void slicer_print_ready(FilamentRecordHarness& h) {
    h.frame(used_heads({false, false, false, false}));
    h.frame(feeder(0, "load_finish"));
    h.frame(feeder(1, "manual_sta_finish"));
    h.frame(feeder(2, "unload_finish"));
    h.frame(feeder(3, "wait_insert"));
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker warns once about a used head that is not loaded",
                 "[ams][snapmaker][unloaded_head_notice]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("unloaded_head_used");
    FileToolsProbe probe(h.backend, {0, 2});
    slicer_print_ready(h);

    h.frame(json{{"print_stats", {{"state", "printing"}, {"filename", "orca.gcode"}}}});
    REQUIRE(probe.asked == std::vector<std::string>{"orca.gcode"});
    REQUIRE(warnings.seen.size() == 1);
    CHECK(warnings.seen[0] ==
          "Toolhead 3 (T2) isn't loaded. The print will pause when it needs it.");

    // Frames keep arriving through the print, a pause and its resume.
    h.frame(used_heads({false, false, false, false}));
    h.print("paused");
    h.print("printing");
    CHECK(warnings.seen.size() == 1);

    // The next print is checked afresh.
    h.print("complete");
    h.print("printing");
    CHECK(warnings.seen.size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker says nothing when every used head is loaded",
                 "[ams][snapmaker][unloaded_head_notice]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("unloaded_head_all_loaded");

    SECTION("used heads loaded, unused heads empty") {
        FileToolsProbe probe(h.backend, {0, 1});
        slicer_print_ready(h);
        h.print("printing");
        CHECK(probe.asked.size() == 1);
    }
    SECTION("the file's tools cannot be read") {
        FileToolsProbe probe(h.backend, {});
        slicer_print_ready(h);
        h.print("printing");
        CHECK(probe.asked.size() == 1);
    }
    SECTION("a head the task names: the firmware loads it itself") {
        FileToolsProbe probe(h.backend, {0, 2});
        slicer_print_ready(h);
        h.frame(used_heads({true, false, true, false}));
        h.print("printing");
    }
    SECTION("a head still loading") {
        FileToolsProbe probe(h.backend, {0, 2});
        slicer_print_ready(h);
        h.frame(feeder(2, "load_feeding"));
        h.print("printing");
    }
    CHECK(warnings.seen.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker checks a restart against the file it starts",
                 "[ams][snapmaker][unloaded_head_notice]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("unloaded_head_restart_file");
    FileToolsProbe probe(h.backend, {2});
    slicer_print_ready(h);
    h.frame(json{{"print_stats", {{"state", "printing"}, {"filename", "a.gcode"}}}});
    REQUIRE(probe.asked == std::vector<std::string>{"a.gcode"});

    // A restart into another file: the cancel and the new start never leave
    // the print, and a frame of the old one still reads printing.
    h.print("paused");
    h.backend.note_print_start_from_here();
    h.print("printing");
    CHECK(probe.asked.size() == 1);
    h.frame(json{{"print_stats", {{"filename", "b.gcode"}}}});
    REQUIRE(probe.asked.size() == 2);
    CHECK(probe.asked[1] == "b.gcode");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker routes a used tool through the task's map",
                 "[ams][snapmaker][unloaded_head_notice]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("unloaded_head_task_map");
    FileToolsProbe probe(h.backend, {0});
    slicer_print_ready(h);
    // The task maps T0 to head 2, unloaded and not among the heads it names.
    // Under the default routing T0 would be head 0, loaded, and silent.
    json task = {{"print_task_config",
                  {{"extruders_used", {false, true, false, false}},
                   {"extruder_map_table", {2, 1, 2, 3, 0, 0, 0, 0}}}}};
    h.frame(task);
    h.print("printing");
    REQUIRE(warnings.seen.size() == 1);
    CHECK(warnings.seen[0].find("Toolhead 3 (T2)") == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker drops a file answer that lands after its print ended",
                 "[ams][snapmaker][unloaded_head_notice]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("unloaded_head_late");
    std::function<void(std::set<int>)> held;
    SnapmakerTestAccess::set_file_tools_probe(
        h.backend,
        [&](const std::string&, std::function<void(std::set<int>)> answer) { held = answer; });
    slicer_print_ready(h);
    h.print("printing");
    h.print("cancelled");
    REQUIRE(held);
    held({2});
    CHECK(warnings.seen.empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker starts a print's state over when a restart never leaves the print",
                 "[ams][snapmaker][unloaded_head_notice][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    Warnings warnings;
    FilamentRecordHarness h("restart_coalesced");
    FileToolsProbe probe(h.backend, {2});
    slicer_print_ready(h);
    h.print("printing");
    REQUIRE(warnings.seen.size() == 1);
    h.print("paused");
    h.frame(make_feed_status(1, "load_fail"));

    // The cancel and the new start reach this backend as one unbroken print.
    h.backend.note_print_start_from_here();
    h.frame(feeder(1, "wait_insert")); // no failure now, and no load either
    h.print("printing");
    h.frame(json{{"print_stats", {{"filename", "restarted.gcode"}}}});
    CHECK(warnings.seen.size() == 2); // the new print is checked

    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    SnapmakerTestAccess::set_sensor_present(h.backend, 1, true);
    h.print("paused");
    bool ok = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { ok = err.success(); });
    FilamentRecordHarness::drain();
    CHECK(ok); // the old print's failed load no longer holds the new one
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit after a spool went in names that spool for later drops",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_edit_names_insert");
    declare_head0_asa(h);

    h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
    h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
    h.frame(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
    REQUIRE(h.backend.slot_insert_identity(0) == InsertIdentity::NoTag);
    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();

    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a restart judges a dropped record afresh",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_restart");
    declare_head0_asa(h);

    SECTION("a write-back the previous session sent") {
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        REQUIRE(h.config_writes().size() == 2);
    }
    SECTION("a spool the previous session saw go in") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
        h.frame(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        REQUIRE(h.config_writes().empty());
    }
    SECTION("an offset calibration the previous session saw hold the heads") {
        h.frame(json{{"machine_state_manager", {{"main_state", 2}}}});
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        REQUIRE(h.config_writes().empty());
    }

    SnapmakerTestAccess::call_on_started(h.backend);
    FilamentRecordHarness::drain();
    h.printer.client.clear_gcode_script_history();
    h.print("standby");
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK(h.config_writes().size() == 2);
}
