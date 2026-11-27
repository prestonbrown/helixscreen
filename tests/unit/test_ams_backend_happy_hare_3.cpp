// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 67 to 99 of test_ams_backend_happy_hare.cpp before it was split, in its order. Mostly
// [device_actions], [1479], [happyhare][override], [emu][zones], [bypass][1229], and more

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

TEST_CASE("Happy Hare offers each setting in its section", "[ams][happy_hare][device_actions]") {
    using helix::printer::ActionType;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto actions = helper.get_device_actions();
    auto find = [&](const std::string& id) -> const helix::printer::DeviceAction* {
        for (const auto& a : actions) {
            if (a.id == id)
                return &a;
        }
        return nullptr;
    };

    struct Row {
        const char* id;
        const char* section;
        std::optional<ActionType> type;
        double shipped; // value before the config loads (< 0 = not checked)
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", "speed", std::nullopt, 150.0},
        {"gear_from_spool_speed", "speed", std::nullopt, 60.0},
        {"extruder_load_speed", "speed", std::nullopt, -1},
        {"extruder_unload_speed", "speed", std::nullopt, -1},
        {"toolhead_sensor_to_nozzle", "toolhead", std::nullopt, -1},
        {"toolhead_extruder_to_nozzle", "toolhead", std::nullopt, -1},
        {"toolhead_entry_to_extruder", "toolhead", std::nullopt, -1},
        {"toolhead_ooze_reduction", "toolhead", std::nullopt, -1},
        {"sync_to_extruder", "accessories", ActionType::TOGGLE, -1},
        {"test_move", "maintenance", ActionType::BUTTON, -1},
    };
    for (const auto& row : rows) {
        INFO(row.id);
        const auto* a = find(row.id);
        REQUIRE(a != nullptr);
        CHECK(a->section == row.section);
        if (row.type)
            CHECK(a->type == *row.type);
        if (row.shipped >= 0)
            CHECK(std::any_cast<double>(a->current_value) == Catch::Approx(row.shipped));
    }
    for (const char* servo : {"servo_up", "servo_move", "servo_down"}) {
        INFO(servo);
        CHECK(find(servo) != nullptr);
    }
    // The single gear speed is split in two; the servo has no calibration.
    CHECK(find("gear_load_speed") == nullptr);
    CHECK(find("calibrate_servo") == nullptr);
}

TEST_CASE("get_device_actions shows the configured and live values",
          "[ams][happy_hare][device_actions]") {
    using Value = std::variant<double, bool, std::string>;
    struct Row {
        const char* id;
        nlohmann::json frame; // status first sent (null = none)
        Value value;
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", nullptr, 180.0},
        {"gear_unload_speed", nullptr, 90.0},
        {"sync_to_extruder", nullptr, false},
        {"clog_detection", {{"flowguard", {{"encoder_mode", 2}}}}, std::string("Auto")},
        {"led_mode",
         {{"leds", {{"unit0", {{"exit_effect", "breathing"}}}}}},
         std::string("breathing")},
        {"espooler_mode", {{"espooler_active", "assist"}}, std::string("assist")},
    };
    for (const auto& row : rows) {
        INFO(row.id);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        if (!row.frame.is_null())
            helper.test_parse_mmu_state(row.frame);

        const auto actions = helper.get_device_actions();
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [&](const auto& a) { return a.id == row.id; });
        REQUIRE(it != actions.end());
        if (const auto* d = std::get_if<double>(&row.value))
            CHECK(std::any_cast<double>(it->current_value) == Catch::Approx(*d));
        else if (const auto* b = std::get_if<bool>(&row.value))
            CHECK(std::any_cast<bool>(it->current_value) == *b);
        else
            CHECK(std::any_cast<std::string>(it->current_value) ==
                  std::get<std::string>(row.value));
    }
}

TEST_CASE_METHOD(HhFourGates,
                 "get_device_actions waits for the config only on config-backed settings",
                 "[ams][happy_hare][device_actions]") {
    // Do NOT call set_config_defaults_for_test() — config_defaults_.loaded is false

    auto actions = helper.get_device_actions();
    for (const char* id : {"gear_from_buffer_speed", "sync_to_extruder", "clog_detection"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK_FALSE(a->enabled);
        CHECK(a->disable_reason == "Loading configuration...");
    }
    for (const char* id : {"gear_sync", "motors_toggle", "led_mode", "espooler_mode"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->enabled);
    }

    // A live command runs; a config-backed setting is refused until the config loads.
    REQUIRE(helper.execute_device_action("gear_sync", std::any(true)).success());
    CHECK(helper.has_gcode("MMU_SYNC_GEAR_MOTOR SYNC=1"));
    helper.clear_captured_gcodes();
    CHECK(helper.execute_device_action("gear_from_buffer_speed", std::any(200.0f)).result ==
          AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE_METHOD(HhFourGates,
                 "clog detection shows the encoder's reported mode while it waits for the config",
                 "[ams][happy_hare][device_actions]") {
    // config_defaults_.loaded stays false: the configfile has not arrived.

    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"encoder_mode", 2}};
    helper.test_parse_mmu_state(mmu_data);

    const auto actions = helper.get_device_actions();
    const auto it = std::find_if(actions.begin(), actions.end(),
                                 [](const auto& a) { return a.id == "clog_detection"; });
    REQUIRE(it != actions.end());
    CHECK_FALSE(it->enabled);
    CHECK(it->disable_reason == "Loading configuration...");
    const auto* shown = std::any_cast<std::string>(&it->current_value);
    REQUIRE(shown != nullptr);
    CHECK(*shown == "Auto");
}

TEST_CASE("A VirtualSelector disables the servo, selector and encoder actions",
          "[ams][happy_hare][device_actions]") {
    for (const bool type_b : {true, false}) {
        INFO((type_b ? "VirtualSelector" : "default selector"));
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        if (type_b)
            helper.set_selector_type("VirtualSelector");

        const auto actions = helper.get_device_actions();
        for (const char* id : {"servo_buzz", "calibrate_encoder", "servo_up", "servo_move",
                               "servo_down", "selector_speed", "clog_detection"}) {
            INFO(id);
            const auto* a = find_action(actions, id);
            REQUIRE(a != nullptr);
            CHECK(a->enabled == !type_b);
            if (type_b)
                CHECK_FALSE(a->disable_reason.empty());
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a Happy Hare speed slider in the section overlay opens at its value and applies",
                 "[ams][happy_hare][device_actions][device-section]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();

    auto& overlay = helix::ui::get_ams_device_section_detail_overlay();
    overlay.init_subjects();
    overlay.show(test_screen(), "speed", "Speed");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* slider =
        helix::test::find_device_action_slider(overlay.get_root(), "gear_from_buffer_speed");
    REQUIRE(slider != nullptr);
    CHECK(lv_slider_get_value(slider) == 180);

    lv_slider_set_value(slider, 200, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    CHECK(helper.has_gcode_containing("GEAR_FROM_BUFFER_SPEED=200"));

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    helix::ui::destroy_static_panels();
}

TEST_CASE_METHOD(HhFourGates, "reapply_overrides batches into single MMU_TEST_CONFIG command",
                 "[ams][happy_hare][device_actions]") {
    helper.set_config_defaults_for_test();

    // Set multiple overrides
    helper.execute_device_action("gear_from_buffer_speed", std::any(200.0));
    helper.execute_device_action("extruder_load_speed", std::any(50.0));
    helper.captured_gcodes.clear();

    // Reapply should batch all overrides
    helper.test_reapply_overrides();

    // One command per parameter: v3 refuses a whole MMU_TEST_CONFIG over any
    // one name it does not take.
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.has_gcode("MMU_TEST_CONFIG GEAR_FROM_BUFFER_SPEED=200"));
    REQUIRE(helper.has_gcode_containing("EXTRUDER_LOAD_SPEED=50"));
}

TEST_CASE("Happy Hare selector and gate-check commands, and their refusals",
          "[ams][happy_hare][device_actions]") {
    // move_selector jogs from the current gate (none counts as gate 0) and
    // clamps to the gates that exist.
    enum class Op { Select, Move, Check, CheckAll };
    struct Row {
        const char* name;
        Op op;
        bool running;
        int current;
        int arg;
        const char* gcode; // "" = refused, nothing sent
    };
    const Row rows[] = {
        {"select gate 2", Op::Select, true, -1, 2, "MMU_SELECT GATE=2"},
        {"select out of range", Op::Select, true, -1, 99, ""},
        {"select when stopped", Op::Select, false, -1, 0, ""},
        {"jog forward", Op::Move, true, 1, 1, "MMU_SELECT GATE=2"},
        {"jog back", Op::Move, true, 1, -1, "MMU_SELECT GATE=0"},
        {"jog back from the first gate", Op::Move, true, 0, -1, "MMU_SELECT GATE=0"},
        {"jog forward from the last gate", Op::Move, true, 3, 1, "MMU_SELECT GATE=3"},
        {"jog with no gate selected", Op::Move, true, -1, 1, "MMU_SELECT GATE=1"},
        {"jog when stopped", Op::Move, false, 1, 1, ""},
        {"check gate 1", Op::Check, true, -1, 1, "MMU_CHECK_GATE GATE=1"},
        {"check when stopped", Op::Check, false, -1, 0, ""},
        {"check all", Op::CheckAll, true, -1, 0, "MMU_CHECK_GATE"},
        {"check all when stopped", Op::CheckAll, false, -1, 0, ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_running(row.running);
        helper.set_current_slot(row.current);

        AmsError err;
        switch (row.op) {
        case Op::Select:
            err = helper.select_gate(row.arg);
            break;
        case Op::Move:
            err = helper.move_selector(row.arg);
            break;
        case Op::Check:
            err = helper.check_gate(row.arg);
            break;
        case Op::CheckAll:
            err = helper.check_all_gates();
            break;
        }
        if (*row.gcode) {
            CHECK(err.result == AmsResult::SUCCESS);
            CHECK(helper.has_gcode(row.gcode));
        } else {
            CHECK_FALSE(err.success());
            CHECK(helper.captured_gcodes.empty());
        }
    }
}

// reset() sends bare MMU_HOME, and with no FORCE_UNLOAD parameter Happy Hare
// runs its unload sequence before homing the selector whenever filament_pos is
// not UNLOADED. That is the whole reason the sidebar may grey Reset here and
// nowhere else, so the capability has to answer true (prestonbrown/helixscreen#1523).
TEST_CASE_METHOD(HhHelper, "Happy Hare declares that reset moves filament",
                 "[ams][happy_hare][capability]") {
    REQUIRE(helper.reset_moves_filament());
    // The command the capability describes.
    helper.initialize_test_gates(4);
    helper.set_running(true);
    REQUIRE(helper.reset().success());
    REQUIRE(helper.has_gcode("MMU_HOME"));
}

TEST_CASE("Happy Hare config sync action is relabeled, distinct from runtime",
          "[ams][happy_hare][device_actions]") {
    using namespace helix::printer;
    auto actions = hh_default_actions();
    const DeviceAction* config_sync = nullptr;
    const DeviceAction* runtime_sync = nullptr;
    for (const auto& a : actions) {
        if (a.id == "sync_to_extruder")
            config_sync = &a;
        if (a.id == "gear_sync")
            runtime_sync = &a;
    }
    REQUIRE(config_sync != nullptr);
    REQUIRE(runtime_sync != nullptr);
    REQUIRE(config_sync->label == "Sync during printing");
    REQUIRE(runtime_sync->label == "Gear motor synced");
    REQUIRE(runtime_sync->section == "maintenance");
}

TEST_CASE("Happy Hare toolchange_phase_template: ops declare ordered phases",
          "[ams][happy_hare][narration]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    auto swap = hh.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(swap.empty());
    CHECK(swap.front().id == "heat");
    CHECK(swap.back().id == "load");
    // AmsAction maps to these exact positions; lock them in.
    REQUIRE(swap.size() == 8);
    CHECK(swap[5].id == "feed");
    CHECK(swap[6].id == "purge");
    // Fresh load skips the unload/cut phases.
    auto fresh = hh.toolchange_phase_template(StepOperationType::LOAD_FRESH);
    REQUIRE_FALSE(fresh.empty());
    CHECK(std::none_of(fresh.begin(), fresh.end(), [](const auto& p) { return p.id == "unload"; }));
    // Unload op ends at "unload".
    auto unload = hh.toolchange_phase_template(StepOperationType::UNLOAD);
    REQUIRE_FALSE(unload.empty());
    CHECK(unload.back().id == "unload");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Happy Hare narration: action transitions advance the step subject",
                 "[ams][happy_hare][narration][ui_integration]") {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");

    auto hh = std::make_unique<AmsBackendHappyHareTestHelper>();
    hh->initialize_test_gates(4);
    auto* hh_raw = hh.get();
    ams.set_backend(std::move(hh)); // backend now reachable for the index subject

    auto feed_action = [&](const char* action) {
        nlohmann::json mmu;
        mmu["action"] = action;
        hh_raw->test_parse_mmu_state(mmu);
        helix::ui::UpdateQueue::instance().drain(); // flush the deferred set
    };

    feed_action("Heating");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 0); // "heat" = index 0
    feed_action("Loading");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 5); // "feed" = index 5
    feed_action("Purging");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 6); // "purge" = index 6

    feed_action("Spinning up"); // no translation key for an unknown word: shown as sent
    CHECK(std::string(lv_subject_get_string(ams.get_ams_action_detail_subject())) == "Spinning up");

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Happy Hare narration: a phase outside the current template lands when the "
                 "operation settles",
                 "[ams][happy_hare][narration][ui_integration]") {
    // The sidebar can start a toolchange as UNLOAD and upgrade it to LOAD_SWAP once
    // it learns a load follows. A phase reported in between belongs to the operation
    // the step bar settles on.
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");
    ams.set_active_step_operation(StepOperationType::UNLOAD);

    auto hh = std::make_unique<AmsBackendHappyHareTestHelper>();
    hh->initialize_test_gates(4);
    auto* hh_raw = hh.get();
    ams.set_backend(std::move(hh));

    nlohmann::json mmu;
    mmu["action"] = "Selecting";
    hh_raw->test_parse_mmu_state(mmu);
    helix::ui::UpdateQueue::instance().drain();
    // UNLOAD has no "select" step, so nothing lights yet.
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == -1);

    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 4); // "select" = index 4

    ams.set_narration_phase(-1, "");
    ams.set_backend(nullptr);
}

// Override store — user identity Happy Hare's gate map cannot hold
//
// Same rationale as AFC (see test_ams_backend_afc.cpp): Happy Hare's gate map
// carries spool_id / material / colour, but not brand, spool_name,
// total_weight_g, colour name or the Spoolman filament+vendor ids. Those live
// only in the override store, and the store must use a PRIVATE namespace
// because lane_data belongs to the Happy Hare plugin itself.
//
// NOTE: written blind — there is no Happy Hare hardware on hand. It deliberately
// mirrors the AFC integration rather than inventing anything.

TEST_CASE_METHOD(HhHelper, "an outside re-bind takes the old spool's brand off a Happy Hare gate",
                 "[ams][happyhare][lane][1672]") {
    // Something outside HelixScreen - Mainsail, the MMU's own screen, a macro -
    // points a gate at a different spool. The gate map is the only thing Happy
    // Hare states about a binding, so that reading retires the records
    // describing the old spool, and parse_mmu_state paints the gate from what
    // is left before the frame ends. A brand only the old spool's record ever
    // carried has nothing to restate it, and apply_resolved() leaves a field no
    // source observed alone, so without the retire the incoming spool wears the
    // outgoing one's brand.
    helper.initialize_test_gates(4);

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PETG";
    spool.material = "PETG";
    spool.color_hex = "FF00FF";
    spool.filament_id = 55;
    spool.remaining_weight_g = 800.0;
    spool.initial_weight_g = 1000.0;
    helix::test::spool_states(helper, 1, spool);
    helper.repaint_slot_from_lane(1);
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");
    REQUIRE(helper.get_slot_info(1).spoolman_filament_id == 55);
    REQUIRE(helper.get_slot_info(1).remaining_weight_g == Catch::Approx(800.0F));
    // Happy Hare's gate map names spool 42 on gate 1 first.
    helper.feed_mmu_gate_spool_ids({0, 42, 7, 0});
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");

    // Gate 2 is the control: same shape, and the frame below reports the spool
    // its lane already names, so its binding holds and nothing is retired.
    SpoolInfo held;
    held.id = 7;
    held.vendor = "Sunlu";
    held.filament_name = "PLA Meta";
    held.material = "PLA";
    held.color_hex = "00FF00";
    helix::test::spool_states(helper, 2, held);
    helper.repaint_slot_from_lane(2);
    REQUIRE(helper.get_slot_info(2).brand == "Sunlu");

    // Gate 1 now reports a spool its lane does not describe: a re-bind, which
    // needs no setting turned on, unlike the eject verdict. Gate 2 reports the
    // spool it already had.
    helper.feed_mmu_gate_spool_ids({0, 99, 7, 0});

    CHECK(helper.get_slot_info(1).brand.empty());
    CHECK(helper.get_slot_info(1).spoolman_filament_id == 0);
    // The old spool's weights describe nothing on the gate now.
    CHECK(helper.get_slot_info(1).remaining_weight_g < 0.0F);
    CHECK(helper.get_slot_info(1).total_weight_g < 0.0F);
    CHECK(helper.get_slot_info(2).brand == "Sunlu");
}

TEST_CASE("HappyHare override survives a gate-map update that omits identity",
          "[ams][happyhare][override]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SlotInfo info = helper.get_slot_info(0);
    info.brand = "Polymaker";
    info.spool_name = "PolyLite Grey";
    info.spoolman_id = 42;
    info.spoolman_filament_id = 55;
    info.total_weight_g = 1000.0f;
    helix::test::edit_slot_as_user(helper, 0, info);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 55);

    // A gate-map refresh that clears the spool id upstream.
    helper.feed_mmu_gate_spool_ids({0, 0, 0, 0});

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand == "Polymaker");
    CHECK(after.spool_name == "PolyLite Grey");
    CHECK(after.spoolman_id == 42);
    CHECK(after.spoolman_filament_id == 55);
    CHECK(after.total_weight_g == Catch::Approx(1000.0f));
}

TEST_CASE_METHOD(HhHelper, "Happy Hare's own gate writes carry the filament id",
                 "[ams][happyhare][1632]") {
    // The lane repaint that closes a full commit restates the field from the
    // override, so an edit-driven assertion cannot tell the backend's own copy
    // from the restatement. The pin sits below any repaint: apply_user_edit()
    // writes the gate and returns before commit_user_edit() repaints.
    helper.initialize_test_gates(2);

    SlotInfo info = helper.get_slot_info(0);
    info.spoolman_id = 42;
    info.spoolman_filament_id = 55;
    const AmsError err = helper.apply_user_edit(0, info);
    REQUIRE(err.success());
    CHECK(helper.get_slot_info(0).spoolman_filament_id == 55);
}

TEST_CASE_METHOD(HhFourGates, "HappyHare clear_slot_override drops the retained identity",
                 "[ams][happyhare][override][1625]") {
    SlotInfo info;
    info.brand = "Polymaker";
    info.spoolman_id = 42;
    helix::test::apply_edit(helper, 0, info);
    helper.set_gate_spoolman_link(0, 42, 77, 3);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 77);

    helper.clear_slot_override(0);

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand.empty());
    CHECK(after.spoolman_id == 0);
    // All three handles die together: a surviving filament handle would keep
    // naming a Spoolman record the cleared slot is no longer linked to.
    CHECK(after.spoolman_filament_id == 0);
    CHECK(after.spoolman_vendor_id == 0);
}

TEST_CASE("HappyHare persist_override records pure black but not the no-color sentinel",
          "[ams][happyhare][override]") {
    struct Row {
        const char* name;
        uint32_t color;
        bool recorded;
    };
    const Row rows[] = {
        {"a deliberate pure black", 0x000000, true},
        {"the no-color sentinel", helix::kAmsDefaultSlotColor, false},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);

        SlotInfo info;
        info.material = "PLA";
        info.color_rgb = row.color;
        helix::test::apply_edit(helper, 0, info);

        REQUIRE(helper.has_gate_override(0));
        const auto& ovr = helix::HappyHareTestAccess::overrides(helper).at(0);
        CHECK(ovr.color_set == row.recorded);
        if (row.recorded)
            CHECK(ovr.color_rgb == row.color);
        CHECK(ovr.material == "PLA");
    }
}

TEST_CASE_METHOD(HhFourGates, "HappyHare persist_override wires nozzle/bed temps into the override",
                 "[ams][happyhare][override]") {
    SlotInfo info;
    info.material = "PETG";
    info.bed_temp = 80;
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    helix::test::apply_edit(helper, 0, info);

    REQUIRE(helper.has_gate_override(0));
    const auto& ovr = helix::HappyHareTestAccess::overrides(helper).at(0);
    CHECK(ovr.bed_temp == 80);
    CHECK(ovr.nozzle_temp == 240); // midpoint of min/max
}

// Bypass: Happy Hare has to answer for itself, same as AFC (#1229)
//
// The twin of "AFC enable_bypass refuses while filament is at the toolhead" in
// test_ams_backend_afc.cpp. allows_implicit_chaining() == false means the
// sidebar sends one command and lets the backend refuse; execute_gcode() is
// fire-and-forget (returns success before Klipper answers), so without this
// precondition MMU_SELECT_BYPASS reports success and changes nothing.
//
// Happy Hare's own cmd_MMU_SELECT_BYPASS runs check_if_loaded(), which refuses
// unless filament_pos is UNLOADED (0) or UNKNOWN (-1) and answers only with a
// `!!` line. Mirroring that exact predicate — rather than system_info_
// .filament_loaded, which is set solely from filament == "Loaded" — is what
// makes an intermediate position refuse here instead of silently no-opping.

TEST_CASE("Happy Hare enable_bypass refuses unless the filament is parked",
          "[ams][happy_hare][bypass][1229]") {
    auto mmu_at = [](int filament_pos, const char* filament_state) {
        return nlohmann::json{{"gate", 0},
                              {"tool", 0},
                              {"filament", filament_state},
                              {"action", "Idle"},
                              {"has_bypass", true},
                              {"filament_pos", filament_pos},
                              {"gate_status", {1, 0, 2, 1}},
                              {"ttg_map", {0, 1, 2, 3}}};
    };

    SECTION("loaded to the nozzle refuses and sends nothing") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(10, "Loaded")); // FILAMENT_POS_LOADED
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE_FALSE(err.user_msg.empty());
        REQUIRE(helper.captured_gcodes.empty());
    }

    // The case system_info_.filament_loaded would miss: mid-bowden is not
    // "Loaded", but Happy Hare still refuses, so we must too.
    SECTION("mid-bowden refuses even though filament is not 'Loaded'") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(3, "Unloading")); // FILAMENT_POS_IN_BOWDEN
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE(helper.captured_gcodes.empty());
    }

    SECTION("parked at the gate sends MMU_SELECT_BYPASS") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(0, "Unloaded")); // FILAMENT_POS_UNLOADED
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(std::find(helper.captured_gcodes.begin(), helper.captured_gcodes.end(),
                          "MMU_SELECT_BYPASS") != helper.captured_gcodes.end());
    }
}

// Happy Hare v4 machine-field layout
//
// v3 puts filament_heater / environment_sensor directly on [mmu_machine] in
// configfile.settings. v4 leaves that object holding only happy_hare_version
// and units, and publishes the real values per unit in the LIVE mmu_machine
// status object. Reading configfile alone finds a populated-looking object with
// none of the fields in it, so the miss is silent: no warning, no log, just an
// environment indicator that never fills in.

TEST_CASE_METHOD(HhFourGates, "Happy Hare v4 takes machine fields from live status, not configfile",
                 "[ams][happy_hare][1479]") {
    // Exactly the shape a v4 machine reports: configfile carries the version and
    // the unit list and nothing else usable.
    nlohmann::json configfile_settings = {
        {"mmu_machine",
         {{"happy_hare_version", "4.0.0"}, {"units", nlohmann::json::array({"unit0"})}}}};

    nlohmann::json live_mmu_machine = {{"happy_hare_version", "4.0.0"},
                                       {"num_units", 1},
                                       {"unit_0",
                                        {{"name", "unit0"},
                                         {"environment_sensor", "aht10 unit0_Env"},
                                         {"filament_heater", "heater_generic box1_heater"}}}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});

    // Feeding a status keyed on the live name is the proof the name resolved:
    // nothing surfaces unless environment_sensor_name_ was assigned from unit_0.
    helper.test_apply_environment_sensor_status(
        {{"aht10 unit0_Env", {{"temperature", 26.5}, {"humidity", 41.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(26.5f));
    CHECK(info.units[0].environment->has_humidity);
    CHECK(info.units[0].environment->humidity_pct == Catch::Approx(41.0f));
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare v4 resolves the filament heater from live status",
                 "[ams][happy_hare][1479]") {
    nlohmann::json configfile_settings = {
        {"mmu_machine",
         {{"happy_hare_version", "4.0.0"}, {"units", nlohmann::json::array({"unit0"})}}},
        {"mmu_unit_parameters unit0",
         {{"heater_max_temp", 65.0}, {"heater_default_dry_humidity", 30.0}}}};

    nlohmann::json live_mmu_machine = {
        {"happy_hare_version", "4.0.0"},
        {"num_units", 1},
        {"unit_0", {{"name", "unit0"}, {"filament_heater", "heater_generic box1_heater"}}}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});
    helper.test_apply_filament_heater_status(
        {{"heater_generic box1_heater", {{"temperature", 52.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(52.0f));

    // v4 keeps heater_max_temp on the unit's own parameters section.
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    // The default humidity target sits beside it.
    CHECK(HappyHareTestAccess::dryer_info(helper).default_humidity_target == 30.0f);
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v3 config still resolves when no live unit object exists",
                 "[ams][happy_hare][1479]") {
    // The fallback half. A v3 machine publishes no unit_N sub-object, so the
    // resolver has to keep answering from configfile or this change would break
    // every existing install to fix the new one.
    helper.initialize_test_gates(4);

    nlohmann::json configfile_settings = {
        {"mmu_machine", {{"environment_sensor", "bme280 mmu_box"}}}};

    // Live object present but carrying no per-unit fields, which is what a v3
    // machine looks like when the query asks for it anyway.
    nlohmann::json live_mmu_machine = {{"happy_hare_version", "3.3.0"}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});
    helper.test_apply_environment_sensor_status(
        {{"bme280 mmu_box", {{"temperature", 24.5}, {"humidity", 38.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(24.5f));
    CHECK(info.units[0].environment->humidity_pct == Catch::Approx(38.0f));
}

TEST_CASE("Happy Hare offers a dryer only with a heater, which takes a live temperature",
          "[ams][happy_hare][v4][dryer][emu][capability]") {
    // Happy Hare's environment manager always publishes drying_state, so a
    // passive box with only sensors must not offer drying (MMU_HEATER would be
    // refused with "No MMU heater configured"). MMU_HEATER TEMP= retargets a
    // running cycle; TIMER is read only on DRY=1, which is refused mid-cycle,
    // so the duration cannot change live, whichever form the heater config takes.
    using J = nlohmann::json;
    struct Row {
        const char* name;
        J machine;
        bool supported;
    };
    const Row rows[] = {
        {"sensors only",
         J{{"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1, "
                                   "temperature_sensor Lane_2, temperature_sensor Lane_3"}},
         false},
        {"one shared heater", J{{"filament_heater", "heater_generic MMU_heater"}}, true},
        {"a heater per gate",
         J{{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"}},
         true},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config({{"mmu_machine", row.machine}});
        helper.test_parse_mmu_state({{"drying_state", J::array({"", "", "", ""})}});

        const auto d = helper.get_dryer_info();
        CHECK(d.supported == row.supported);
        if (row.supported) {
            CHECK(d.supports_live_temp);
            CHECK_FALSE(d.supports_live_duration);
        }
    }
}

TEST_CASE("A per-gate drying_state array reaches every gate's zone",
          "[ams][happy_hare][emu][zones]") {
    using namespace helix::printer;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1"}}}});
    // The second gate's state must survive scanning past the first: stopping at
    // the first hit would leave every later gate unrecorded.
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"queued", "active"})}});

    const auto zones = helper.get_environment_zones();

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].state == ZoneDryingState::Queued);
    CHECK(zones[1].state == ZoneDryingState::Active);
}

TEST_CASE("A non-string drying_state entry still occupies its gate",
          "[ams][happy_hare][emu][zones]") {
    using namespace helix::printer;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(3);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater, "
                                "heater_generic Lane_2_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1, "
                                   "temperature_sensor Lane_2"}}}});
    // A non-string entry between two strings must still occupy its gate slot, or
    // gate 2's state lands on gate 1 instead.
    helper.test_parse_mmu_state(
        {{"drying_state", nlohmann::json::array({"queued", nullptr, "active"})}});

    const auto zones = helper.get_environment_zones();

    REQUIRE(zones.size() == 3);
    CHECK(zones[2].state == ZoneDryingState::Active);
}

TEST_CASE("Happy Hare updates a running dryer: TEMP alone, a restart for a new duration",
          "[ams][happy_hare][dryer][update]") {
    // A bare TEMP= retargets without touching the clock; a new duration needs
    // STOP then DRY=1, since DRY=1 mid-cycle is refused.
    struct Row {
        const char* name;
        float temp;
        int minutes;
        std::vector<const char*> sent;
        std::vector<const char*> never;
    };
    const Row rows[] = {
        {"temperature only", 50.0f, -1, {"MMU_HEATER TEMP=50"}, {"DRY=1", "TIMER=", "STOP=1"}},
        {"a new duration",
         50.0f,
         90,
         {"MMU_HEATER STOP=1", "MMU_HEATER DRY=1 TEMP=50 TIMER=90"},
         {}},
        {"nothing changed", -1, -1, {}, {"MMU_HEATER"}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.test_apply_heater_config(
            {{"mmu_machine", {{"filament_heater", "heater_generic MMU_heater"}}}});
        helper.clear_captured_gcodes();

        REQUIRE(helper.update_drying(row.temp, row.minutes).success());
        for (const char* g : row.sent)
            CHECK(helper.has_gcode(g));
        for (const char* fragment : row.never)
            CHECK_FALSE(helper.has_gcode_containing(fragment));
        if (row.sent.empty())
            CHECK(helper.captured_gcodes.empty());
    }
}

// A persisted override IS a user edit. The lock flags are how any consumer of
// the record tells one from a firmware reading (#965, #1649).
// Maintenance / accessories device actions

TEST_CASE("Happy Hare motors toggle turns motors on and off without homing",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads

    REQUIRE(helper.execute_device_action("motors_toggle", std::any(true)).success());
    REQUIRE(helper.execute_device_action("motors_toggle", std::any(false)).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_MOTORS_ON", "MMU_MOTORS_OFF"});
}

TEST_CASE("Happy Hare extruder-only load and unload send EXTRUDER_ONLY=1",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_running(true);
    const auto actions = helper.get_device_actions();

    for (const char* id : {"load_extruder", "unload_extruder"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->section == "maintenance");
        CHECK(a->type == helix::printer::ActionType::BUTTON);
    }

    REQUIRE(helper.execute_device_action("load_extruder").success());
    REQUIRE(helper.execute_device_action("unload_extruder").success());
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_LOAD EXTRUDER_ONLY=1", "MMU_UNLOAD EXTRUDER_ONLY=1"});
}

TEST_CASE("Happy Hare Refresh Spoolman follows HH's spoolman_support",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;

    SECTION("spoolman off: offered disabled, and refused if sent anyway") {
        helper.set_spoolman_mode(SpoolmanMode::OFF);
        const auto actions = helper.get_device_actions();
        const auto* a = find_action(actions, "spoolman_refresh");
        REQUIRE(a != nullptr);
        CHECK(a->section == "accessories");
        CHECK_FALSE(a->enabled);
        CHECK_FALSE(a->disable_reason.empty());
        CHECK_FALSE(helper.execute_device_action("spoolman_refresh").success());
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("spoolman on") {
        helper.set_spoolman_mode(SpoolmanMode::PULL);
        const auto actions = helper.get_device_actions();
        const auto* a = find_action(actions, "spoolman_refresh");
        REQUIRE(a != nullptr);
        CHECK(a->enabled);
        REQUIRE(helper.execute_device_action("spoolman_refresh").success());
        CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_SPOOLMAN REFRESH=1"});
    }
}

TEST_CASE("Happy Hare extruder-only load and unload are refused mid-print",
          "[ams][happy_hare][device_actions]") {
    // HH puts no print check on MMU_LOAD/MMU_UNLOAD, and an EXTRUDER_ONLY
    // unload forms a tip and retracts over the part.
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);

    CHECK(helper.execute_device_action("load_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.execute_device_action("unload_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare runs only the device actions it offers and has enabled",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    CHECK(helper.execute_device_action("not_an_action").result == AmsResult::NOT_SUPPORTED);

    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();
    helper.set_selector_type("VirtualSelector"); // no servo on any unit
    const auto actions = helper.get_device_actions();
    const auto* buzz = find_action(actions, "servo_buzz");
    REQUIRE(buzz != nullptr);
    REQUIRE_FALSE(buzz->enabled);
    CHECK(helper.execute_device_action("servo_buzz").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare machine-motion actions are refused while a print holds the printer",
          "[ams][happy_hare][device_actions]") {
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads
    helper.initialize_test_gates(4);
    helper.set_running(true);

    const auto actions = helper.get_device_actions();
    for (const char* id : {"calibrate_bowden", "load_extruder", "test_move"}) {
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->moves_machine);
    }

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::PAUSED}) {
        helix::test::set_wire_state(fx.state, state);
        const AmsError refused = helper.execute_device_action("calibrate_bowden");
        CHECK(refused.result == AmsResult::WRONG_STATE);
        CHECK(refused.user_msg == "Printer is busy with a print");
        CHECK(helper.captured_gcodes.empty());
        // An action that leaves the printer still is not the gate's business.
        CHECK(helper.execute_device_action("led_mode", std::any(std::string("off"))).success());
        helper.clear_captured_gcodes();
    }

    // Extruder-only load/unload is how a paused print recovers, so pausing
    // releases it; printing still refuses it.
    const auto* load = find_action(actions, "load_extruder");
    REQUIRE(load != nullptr);
    CHECK(load->allowed_while_paused);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);
    CHECK(helper.execute_device_action("load_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PAUSED);
    REQUIRE(helper.execute_device_action("load_extruder").success());
    CHECK(helper.has_gcode_containing("MMU_LOAD EXTRUDER_ONLY=1"));
    helper.clear_captured_gcodes();

    helix::test::set_wire_state(fx.state, helix::PrintJobState::STANDBY);
    REQUIRE(helper.execute_device_action("calibrate_bowden").success());
    CHECK(helper.has_gcode_containing("MMU_CALIBRATE_BOWDEN"));
}
