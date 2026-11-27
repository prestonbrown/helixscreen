// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 100 to 135 of test_ams_backend_happy_hare.cpp before it was split, in its order. Mostly
// [hh_v4], [hh_multi_unit], [error-center][1323], [hh_v4][clog], [config], and more

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

TEST_CASE("Happy Hare classify_error: which lines it claims, what it says, how to recover",
          "[ams][happy_hare][error-center][1323]") {
    // Happy Hare's fault shape is print_state pause_locked plus reason_for_pause;
    // its action string has no error value. A paused fault leads with Resume, a
    // standalone one (a failed MMU_LOAD outside a print) offers no Resume and
    // leads with Recover. A recognised keyword alone never claims a line unless
    // the print is paused.
    using J = nlohmann::json;
    const J paused_gate2 = {{"print_state", "pause_locked"},
                            {"reason_for_pause", "Gate 2 is empty!"}};
    const auto paused_line = [](const std::string& reason) {
        return "!! MMU issue detected. Print will be paused\nReason: " + reason;
    };
    struct Row {
        const char* name;
        J frame; // null = none
        bool paused;
        std::string line;
        bool claimed;
        std::string detail; // exact ("" = not checked)
        std::string title;  // "" = not checked
        std::vector<const char*> offers;
        std::vector<const char*> withholds;
        const char* first; // first recovery's log tag ("" = not checked)
    };
    const Row rows[] = {
        {"a generic Klipper error",
         nullptr,
         false,
         "Error: generic klipper error",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"an ok line", nullptr, false, "ok", false, "", "", {}, {}, ""},
        {"!! while paused without a fault",
         nullptr,
         true,
         "!! something unrelated",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"a stale reason while not paused",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Clog detected on gate 0"}},
         false,
         "!! Clog detected",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"an unrelated !! line",
         nullptr,
         false,
         "!! Move out of range: 0.000 400.000 10.000",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"a standalone MMU issue",
         nullptr,
         false,
         "!! MMU issue: Gate 2 is empty!",
         true,
         "Gate 2 is empty!",
         "Filament System Error",
         {"hh::recover", "hh::unlock"},
         {"hh::resume"},
         "hh::recover"},
        {"a paused empty gate",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Gate 2 is empty!"},
           {"filament_pos", 8},
           {"filament", "Loaded"}},
         true,
         paused_line("Gate 2 is empty!"),
         true,
         "Gate 2 is empty!",
         "",
         {"hh::recover", "hh::unload", "hh::unlock"},
         {},
         "hh::resume"},
        // The same fault with no filament state at all.
        {"a paused empty gate, no filament state",
         paused_gate2,
         true,
         paused_line("Gate 2 is empty!"),
         true,
         "Gate 2 is empty!",
         "",
         {},
         {},
         "hh::resume"},
        {"a paused homing failure",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Failed to home the selector"},
           {"filament_pos", 8},
           {"filament", "Loaded"}},
         true,
         paused_line("Failed to home the selector"),
         true,
         "Failed to home the selector",
         "",
         {"hh::recover", "hh::unload", "hh::unlock"},
         {},
         ""},
        {"a second fault names its own reason",
         paused_gate2,
         true,
         "!! MMU issue detected whilst printer is paused\nReason: Encoder not detecting",
         true,
         "Encoder not detecting",
         "",
         {},
         {},
         ""},
        {"a multi-line reason",
         J{{"print_state", "pause_locked"},
           {"reason_for_pause", "Runout on gate 0\nEndlessSpool is off"}},
         true,
         paused_line("Runout on gate 0\nEndlessSpool is off"),
         true,
         "Runout on gate 0. EndlessSpool is off",
         "Filament runout",
         {},
         {},
         ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
        AmsBackendHappyHareTestHelper& hh = *hh_reg;
        hh.initialize_test_gates(4);
        if (!row.frame.is_null())
            hh.test_parse_mmu_state(row.frame);
        helix::ClassifyContext ctx;
        ctx.is_paused = row.paused;

        const auto ev = hh.classify_error(row.line, ctx);
        REQUIRE(ev.has_value() == row.claimed);
        if (!row.claimed)
            continue;
        if (!row.detail.empty())
            CHECK(ev->detail == row.detail);
        if (!row.title.empty())
            CHECK(ev->title == row.title);
        for (const char* tag : row.offers)
            CHECK(has_recovery(*ev, tag));
        for (const char* tag : row.withholds)
            CHECK_FALSE(has_recovery(*ev, tag));
        if (*row.first) {
            REQUIRE_FALSE(ev->recovery_actions.empty());
            CHECK(ev->recovery_actions.front().log_tag == row.first);
            CHECK(ev->recovery_actions.front().style == "primary");
            for (const auto& a : ev->recovery_actions)
                if (a.log_tag != row.first && a.log_tag == "hh::recover")
                    CHECK(a.style.empty());
        }
    }
}

TEST_CASE("Happy Hare classify_error: a runout pause is a CRITICAL fault resumed first",
          "[ams][happy_hare][error-center]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);
    hh.test_parse_mmu_state({{"action", "Idle"},
                             {"print_state", "pause_locked"},
                             {"filament_pos", 8},
                             {"reason_for_pause", "Runout detected on gate 0  EndlessSpool mode is "
                                                  "off - manual intervention is required"}});
    helix::ClassifyContext ctx;
    ctx.is_paused = true;

    const auto ev = hh.classify_error("!! Runout detected", ctx);
    REQUIRE(ev.has_value());
    // The runout fault the hook raises is what the trait claims.
    CHECK(hh.traits().raises_runout_fault);
    CHECK(hh.traits().raises_runout_fault_during_job);
    CHECK(ev->source == helix::ErrorSource::HAPPY_HARE);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE_FALSE(ev->recovery_actions.empty());
    CHECK(ev->recovery_actions.front().gcode == "RESUME");
    // The detail is the descriptive reason, not the bare !! line.
    CHECK(ev->detail.find("Runout detected on gate 0") != std::string::npos);
}

TEST_CASE("Happy Hare classify_error: the pause hint follows the reason",
          "[ams][happy_hare][error-center][pause_hint]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    nlohmann::json mmu;
    mmu["print_state"] = "pause_locked";
    mmu["reason_for_pause"] = "Gate 2 is empty!";
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    const std::string line =
        "!! MMU issue detected. Print will be paused\nReason: Gate 2 is empty!";

    SECTION("v4.2 names what to do next") {
        mmu["pause_hint"] = "Load filament into gate 2\nthen RESUME";
        hh.test_parse_mmu_state(mmu);
        auto ev = hh.classify_error(line, ctx);
        REQUIRE(ev.has_value());
        CHECK(ev->detail == "Gate 2 is empty!\nLoad filament into gate 2. then RESUME");
        // The dedup identity stays the reason the console line carries.
        CHECK(ev->raw_detail == "Gate 2 is empty!");
    }
    SECTION("an empty hint adds nothing") {
        mmu["pause_hint"] = "";
        hh.test_parse_mmu_state(mmu);
        auto ev = hh.classify_error(line, ctx);
        REQUIRE(ev.has_value());
        CHECK(ev->detail == "Gate 2 is empty!");
    }
    SECTION("before v4.2 there is no hint") {
        hh.test_parse_mmu_state(mmu);
        auto ev = hh.classify_error(line, ctx);
        REQUIRE(ev.has_value());
        CHECK(ev->detail == "Gate 2 is empty!");
        CHECK(ev->raw_detail.empty());
    }
    SECTION("a later fault's reason does not take the first fault's hint") {
        mmu["pause_hint"] = "Load filament into gate 2";
        hh.test_parse_mmu_state(mmu);
        auto ev = hh.classify_error(
            "!! MMU issue detected whilst printer is paused\nReason: Encoder not detecting", ctx);
        REQUIRE(ev.has_value());
        CHECK(ev->detail == "Encoder not detecting");
    }
    SECTION("the hint ends with the pause") {
        mmu["pause_hint"] = "Load filament into gate 2";
        hh.test_parse_mmu_state(mmu);
        // Resume: reason_for_pause goes empty in a frame that leaves pause_hint out.
        hh.test_parse_mmu_state({{"print_state", "printing"}, {"reason_for_pause", ""}});
        hh.test_parse_mmu_state(
            {{"print_state", "pause_locked"}, {"reason_for_pause", "Gate 2 is empty!"}});
        auto ev = hh.classify_error(line, ctx);
        REQUIRE(ev.has_value());
        CHECK(ev->detail == "Gate 2 is empty!");
    }
}

TEST_CASE("Happy Hare's pre-gate runout lights the gate's prep sensor",
          "[ams][happy_hare][fault][sensor][prep]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    nlohmann::json fault;
    fault["action"] = "Idle";
    fault["gate"] = 2;
    fault["filament_pos"] = 3;
    fault["print_state"] = "pause_locked";
    fault["reason_for_pause"] = "Runout detected on T2 (gate 2) by mmu_entry_2\n"
                                "No alternative gates available after checking Gate 3: empty";
    hh.test_parse_mmu_state(fault);

    REQUIRE(hh.get_slot_info(2).error.has_value());
    CHECK(hh.get_slot_info(2).error->sensor == helix::LaneSensor::PREP);
}

TEST_CASE("Happy Hare pause_locked marks the slot and path, and cancel clears them",
          "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    nlohmann::json fault;
    fault["action"] = "Idle";
    fault["gate"] = 2;
    fault["filament_pos"] = 3;
    fault["print_state"] = "pause_locked";
    fault["reason_for_pause"] = "Gate 2 is empty!";
    hh.test_parse_mmu_state(fault);

    auto slot = hh.get_slot_info(2);
    REQUIRE(slot.error.has_value());
    CHECK(slot.error->message == "Gate 2 is empty!");
    CHECK(hh.infer_error_segment() == path_segment_from_happy_hare_pos(3));

    // MMU_UNLOCK: still paused, HH keeps publishing the same reason (unchanged, so
    // a delta frame omits it).
    nlohmann::json unlocked;
    unlocked["print_state"] = "paused";
    hh.test_parse_mmu_state(unlocked);
    CHECK(hh.get_slot_info(2).error.has_value());

    // Cancel leaves the paused states; the published reason goes empty.
    nlohmann::json cancelled;
    cancelled["print_state"] = "cancelled";
    cancelled["reason_for_pause"] = "";
    hh.test_parse_mmu_state(cancelled);
    CHECK_FALSE(hh.get_slot_info(2).error.has_value());
    CHECK(hh.infer_error_segment() == PathSegment::NONE);
}

TEST_CASE("Happy Hare claims its own error notice prompt as a duplicate of the fault",
          "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    // v3 to v4.1 title the pause dialog "Error Notice"; v4.2 builds it in Python
    // as "Pause Notice" (mmu_dialog.py ERROR_DIALOG_TITLE).
    CHECK(hh.duplicates_firmware_prompt("Happy Hare Error Notice"));
    CHECK(hh.duplicates_firmware_prompt("Happy Hare Pause Notice"));
    CHECK_FALSE(hh.duplicates_firmware_prompt("Happy Hare"));
    CHECK_FALSE(hh.duplicates_firmware_prompt("Filament Change"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Happy Hare's own error notice closes when the HH recovery popup shows",
                 "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    // _MMU_ERROR_DIALOG runs ahead of the `!!` line, so its prompt is up first.
    prompts.process_line("// action:prompt_begin Happy Hare Error Notice");
    prompts.process_line("// action:prompt_text Reason: Gate 2 is empty!");
    prompts.process_line("// action:prompt_show");
    REQUIRE(helix::ActionPromptManager::is_showing());

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(presenter.is_visible());
    CHECK_FALSE(helix::ActionPromptManager::is_showing());

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Happy Hare v4.2's pause notice closes when the HH recovery popup shows",
                 "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    // mmu_dialog.py shows the prompt ahead of the `!!` line, so it is up first.
    prompts.process_line("// action:prompt_begin Happy Hare Pause Notice");
    prompts.process_line("// action:prompt_text Gate 2 is empty.");
    prompts.process_line("// action:prompt_show");
    REQUIRE(helix::ActionPromptManager::is_showing());

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(presenter.is_visible());
    CHECK_FALSE(helix::ActionPromptManager::is_showing());

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Happy Hare's pause notice with a macro's own button survives the popup",
                 "[ams][happy_hare][error-center][pause_dialog_buttons]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);
    CHECK(hh.firmware_prompt_button_is_own("RESUME"));
    CHECK(hh.firmware_prompt_button_is_own("mmu_unlock "));
    CHECK(hh.firmware_prompt_button_is_own("CANCEL_PRINT"));
    CHECK_FALSE(hh.firmware_prompt_button_is_own("_BOX_RETRY_LOAD"));

    const bool extra = GENERATE(false, true);
    INFO("extra button: " << extra);
    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    prompts.process_line("// action:prompt_begin Happy Hare Pause Notice");
    prompts.process_line("// action:prompt_text Reason: Gate 2 is empty!");
    if (extra) {
        prompts.process_line("// action:prompt_footer_button Retry load|_BOX_RETRY_LOAD|primary");
    }
    prompts.process_line("// action:prompt_footer_button ABORT|CANCEL_PRINT|error");
    prompts.process_line("// action:prompt_footer_button RESUME|RESUME|warning");
    prompts.process_line("// action:prompt_footer_button UNLOCK|MMU_UNLOCK|primary");
    prompts.process_line("// action:prompt_show");
    REQUIRE(helix::ActionPromptManager::is_showing());

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(presenter.is_visible());
    CHECK(helix::ActionPromptManager::is_showing() == extra);

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "An unrelated firmware prompt survives the HH recovery popup",
                 "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    prompts.process_line("// action:prompt_begin Filament Change");
    prompts.process_line("// action:prompt_show");

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(helix::ActionPromptManager::is_showing());

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare connect asks configfile once and applies it to every reader",
                 "[ams][happy_hare][config]") {
    HappyHareTestAccess::on_started(helper);

    REQUIRE(client.rpc_count(kObjectsQuery) == 1);

    REQUIRE(client.reply_to_last(
        kObjectsQuery,
        nlohmann::json{{"result",
                        {{"status",
                          {{"configfile",
                            {{"settings",
                              {{"mmu",
                                {{"form_tip_macro", "_MMU_CUT_TIP"},
                                 {"gear_from_buffer_speed", "175"},
                                 {"heater_max_temp", 65.0}}},
                               {"mmu_machine",
                                {{"selector_type", "VirtualSelector"},
                                 {"filament_heater", "heater_generic box1_heater"}}}}}}}}}}}}));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helper.get_system_info().tip_method == TipMethod::CUT);
    CHECK(helper.get_topology() == PathTopology::HUB);
    CHECK(helper.get_dryer_info().supported);
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    CHECK(HappyHareTestAccess::config_defaults(helper).gear_from_buffer_speed ==
          Catch::Approx(175.0f));
    CHECK(HappyHareTestAccess::config_defaults(helper).loaded);
}

TEST_CASE_METHOD(HappyHareGoldenFixture, "Happy Hare golden status sequence",
                 "[ams][happy_hare][golden]") {
    using json = nlohmann::json;
    const std::vector<std::pair<std::string, json>> frames = {
        {"full frame", golden_hh_full_frame()},
        {"load: loading",
         json{{"action", "Loading"}, {"filament_pos", 3}, {"bowden_progress", 40}}},
        {"load: loaded", json{{"action", "Idle"},
                              {"filament_pos", 8},
                              {"bowden_progress", -1},
                              {"gate", 2},
                              {"tool", 2},
                              {"filament", "Loaded"}}},
        {"unload: unloading", json{{"action", "Unloading"}, {"filament_pos", 4}}},
        {"unload: unloaded", json{{"action", "Idle"},
                                  {"filament_pos", 0},
                                  {"filament", "Unloaded"},
                                  {"gate", -1},
                                  {"tool", -1}}},
        {"fault: paused", json{{"gate", 1},
                               {"tool", 1},
                               {"filament_pos", 5},
                               {"reason_for_pause", "Filament is stuck in the extruder"}}},
        {"fault: resumed", json{{"reason_for_pause", ""}}},
        {"insert: gate 3 fills", json{{"gate_status", json::array({1, 2, 0, 1})}}},
        {"insert: gate 2 fills", json{{"gate_status", json::array({1, 2, 1, 1})}}},
        {"hex colours only", json{{"gate_color", json::array({"ff8800", "", "#00ff00", "zzz"})}}},
        {"temperature + name deltas",
         json{{"gate_temperature", json::array({215})}, {"gate_name", json::array({"Crimson"})}}},
        {"per-gate drying array",
         json{{"drying_state", json::array({"active", "", "queued", ""})}}},
        {"aggregate sensors", json{{"gate", 3}, {"sensors", json{{"mmu_pre_gate", true}}}}},
        {"topology string", json{{"num_gates", "6,4"}, {"num_units", 2}, {"unit", 1}}},
        {"idle sparse frame", json{{"filament_pos", 7}}},
    };

    std::string all;
    for (const auto& [name, frame] : frames) {
        all += name + "\n" + feed(frame) + "\n";
    }
    CHECK(all == kHappyHareGolden);
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare v4 reads its tunables from the split config sections",
                 "[ams][happy_hare][hh_v4]") {
    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));

    const auto info = helper.get_system_info();
    CHECK(info.version == "4.0.0");
    CHECK(info.tip_method == TipMethod::CUT);
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    const auto& d = HappyHareTestAccess::config_defaults(helper);
    CHECK(d.loaded);
    CHECK(d.gear_from_spool_speed == Catch::Approx(80.0f));
    CHECK(d.gear_from_buffer_speed == Catch::Approx(150.0f));
    CHECK(d.gear_unload_speed == Catch::Approx(120.0f));
    CHECK(d.extruder_load_speed == Catch::Approx(12.0f));
    CHECK(d.extruder_unload_speed == Catch::Approx(12.0f));
    CHECK(d.toolhead_extruder_to_nozzle == Catch::Approx(87.0f));
    CHECK(d.toolhead_sensor_to_nozzle == Catch::Approx(1.0f));
    CHECK(d.toolhead_entry_to_extruder == Catch::Approx(6.0f));
    CHECK(d.sync_to_extruder == 1);
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 multi-unit reads unit parameters from the first unit's section",
                 "[ams][happy_hare][hh_v4]") {
    helper.initialize_test_gates(10);

    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json"));

    CHECK(helper.get_system_info().tip_method == TipMethod::TIP_FORM);
    const auto& d = HappyHareTestAccess::config_defaults(helper);
    CHECK(d.gear_from_spool_speed == Catch::Approx(90.0f));
    CHECK(d.selector_move_speed == Catch::Approx(220.0f));
    CHECK(d.extruder_unload_speed == Catch::Approx(20.0f));
    CHECK(d.toolhead_ooze_reduction == Catch::Approx(1.5f));
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 takes bypass support from the units, not printer.mmu",
                 "[ams][happy_hare][hh_v4]") {
    SECTION("no unit has a bypass: status first, then config") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK_FALSE(helper.get_system_info().supports_bypass);
        // printer.mmu.has_bypass stays a constant true on later frames.
        helper.test_parse_mmu_state({{"has_bypass", true}});
        CHECK_FALSE(helper.get_system_info().supports_bypass);
    }
    SECTION("no unit has a bypass: config first, then status") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        CHECK_FALSE(helper.get_system_info().supports_bypass);
    }
    SECTION("one unit of two has a bypass") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        CHECK(helper.get_system_info().supports_bypass);
    }
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v3 still takes bypass support from printer.mmu.has_bypass",
                 "[ams][happy_hare][hh_v4]") {
    // v3.4 mmu_machine: per-unit objects, a has_bypass of its own, no version.
    const nlohmann::json fx = {
        {"configfile_settings", {{"mmu", {{"form_tip_macro", "_MMU_FORM_TIP"}}}}},
        {"mmu_machine", {{"unit_0", {{"name", "ERCF"}, {"has_bypass", false}}}, {"num_units", 1}}}};
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state({{"gate_status", {1, 1}}, {"has_bypass", true}});
    CHECK(helper.get_system_info().supports_bypass);
    helper.test_parse_mmu_state({{"has_bypass", false}});
    CHECK_FALSE(helper.get_system_info().supports_bypass);
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v4 reads per-gate entry sensors from their Klipper objects",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    // The first frame carries printer.mmu (gate 7 selected, so its sensors dict
    // holds only generic names) beside the four entry sensor objects of unit 1.
    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    feed_params(helper, params);

    for (int gate = 0; gate < 6; ++gate) {
        INFO("gate " << gate);
        CHECK_FALSE(helper.slot_has_prep_sensor(gate));
    }
    CHECK(helper.slot_has_prep_sensor(6));
    CHECK(helper.get_gate_sensor(6)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(7)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(8)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(9)->pre_gate_triggered);
    CHECK(helper.get_slot_filament_segment(9) == PathSegment::PREP);

    // A later sensors dict naming only the selected gate leaves the rest alone.
    helper.test_parse_mmu_state({{"sensors", {{"mmu_entry", false}, {"mmu_pre_gate", false}}}});
    CHECK(helper.get_gate_sensor(6)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(9)->pre_gate_triggered);

    // Deltas: one field at a time.
    feed_params(helper, {{"filament_switch_sensor mmu_entry_9", {{"filament_detected", false}}}});
    CHECK_FALSE(helper.get_gate_sensor(9)->pre_gate_triggered);
    feed_params(helper, {{"filament_switch_sensor mmu_entry_8", {{"filament_detected", true}}}});
    CHECK(helper.get_gate_sensor(8)->pre_gate_triggered);
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v4 entry sensor that is disabled reads as not triggered",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    feed_params(helper, params);
    int state_events = 0;
    helper.set_event_callback([&](const std::string& name, const std::string&) {
        state_events += name == helix::AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
    });

    REQUIRE(helper.slot_has_prep_sensor(3));
    CHECK_FALSE(helper.get_gate_sensor(3)->pre_gate_triggered); // detected, but disabled
    CHECK(helper.get_gate_sensor(0)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(2)->pre_gate_triggered);
    feed_params(helper, {{"filament_switch_sensor mmu_entry_3", {{"enabled", true}}}});
    CHECK(helper.get_gate_sensor(3)->pre_gate_triggered);
    CHECK(state_events == 1); // a sensor-only frame still reaches the UI
    CHECK(helper.get_system_info().units[0].has_slot_sensors);
}

TEST_CASE_METHOD(HhHelper,
                 "Happy Hare v4 sensors dict with no gate selected names gates mmu_entry_N",
                 "[ams][happy_hare][hh_v4]") {
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 0, 1}},
                                 {"gate", -1},
                                 {"sensors",
                                  {{"mmu_entry_0", true},
                                   {"mmu_entry_1", false},
                                   {"mmu_entry_2", nullptr},
                                   {"mmu_shared_exit", false}}}});
    REQUIRE(helper.get_gate_sensor(0));
    CHECK(helper.get_gate_sensor(0)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(1)->pre_gate_triggered);
    REQUIRE(helper.get_gate_sensor(2));
    CHECK_FALSE(helper.get_gate_sensor(2)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(3));
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare MMU_TEST_CONFIG names gear speeds the way each version does",
                 "[ams][happy_hare][hh_v4]") {
    SECTION("v3") {
        helper.set_config_defaults_for_test();
        helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
        helper.execute_device_action("gear_from_buffer_speed", std::any(160.0));
        helper.execute_device_action("toolhead_ooze_reduction", std::any(2.5));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG GEAR_FROM_BUFFER_SPEED=160",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=2.5"});
    }
    SECTION("v4") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        helper.clear_captured_gcodes();
        helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
        // The QIDI unit has no filament buffer: v4 would refuse the parameter.
        CHECK_FALSE(helper.execute_device_action("gear_from_buffer_speed", std::any(160.0)));
        helper.execute_device_action("toolhead_ooze_reduction", std::any(2.5));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=70",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=2.5"});
    }
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare v4 reapplies persisted overrides under the v4 names",
                 "[ams][happy_hare][hh_v4]") {
    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
    auto& overrides = HappyHareTestAccess::user_overrides(helper);
    overrides = {};
    overrides.gear_from_spool_speed = 75.0f;
    overrides.extruder_load_speed = 20.0f;
    // The QIDI unit is always gripped, has no buffer and no encoder: v4 refuses
    // all three, so they are left out rather than sinking the rest.
    overrides.sync_to_extruder = 0;
    overrides.gear_from_buffer_speed = 160.0f;
    helper.clear_captured_gcodes();

    helper.test_reapply_overrides();

    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=75",
                                   "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20"});
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare names the unit only where v4 multi-unit requires it",
                 "[ams][happy_hare][hh_v4]") {
    SECTION("v3 single unit") {
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        CHECK(unit_scoped_commands(helper, 0) ==
              std::vector<std::string>{"MMU_HOME", "MMU_MOTORS_ON", "MMU_MOTORS_OFF",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER TEMP=55",
                                       "MMU_HEATER STOP=1", "MMU_SERVO POS=up",
                                       "MMU_CALIBRATE_GATES", "MMU_CALIBRATE_BOWDEN",
                                       "MMU_TEST_GRIP", "MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0",
                                       "MMU_TEST_CONFIG SYNC_TO_EXTRUDER=1"});
    }
    SECTION("v4 single unit") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(unit_scoped_commands(helper, 0) ==
              std::vector<std::string>{
                  "MMU_HOME", "MMU_MOTORS_ON", "MMU_MOTORS_OFF",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER TEMP=55", "MMU_HEATER STOP=1",
                  // A VirtualSelector, encoder-less, always-gripped
                  // unit: no servo, gate calibration or sync.
                  "MMU_CALIBRATE_BOWDEN", "MMU_TEST_GRIP", "MMU_TEST_CONFIG GEAR_LOAD_SPEED=70",
                  "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                  "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0"});
    }
    SECTION("v4 two units, unit 1 selected") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(unit_scoped_commands(helper, 1) ==
              std::vector<std::string>{"MMU_HOME UNIT=ALL", "MMU_MOTORS_ON UNIT=ALL",
                                       "MMU_MOTORS_OFF UNIT=ALL",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                                       // The selected Box Turtle has no servo, encoder or ungripped
                                       // drive, so those go to the ERCF.
                                       "MMU_SERVO POS=up UNIT=0", "MMU_CALIBRATE_GATE ALL=1 UNIT=0",
                                       "MMU_CALIBRATE_BOWDEN", "MMU_TEST_GRIP UNIT=1",
                                       "MMU_TEST_CONFIG GEAR_LOAD_SPEED=70 UNIT=1",
                                       "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0 UNIT=1",
                                       "MMU_TEST_CONFIG SYNC_TO_EXTRUDER=1 UNIT=0"});

        auto& overrides = HappyHareTestAccess::user_overrides(helper);
        overrides = {};
        overrides.gear_from_spool_speed = 75.0f;
        overrides.gear_from_buffer_speed = 160.0f;
        overrides.selector_move_speed = 180.0f;
        overrides.extruder_unload_speed = 25.0f;
        helper.clear_captured_gcodes();
        helper.test_reapply_overrides();
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_FILAMENT_BUFFER_SPEED=160 UNIT=0",
                                       "MMU_TEST_CONFIG GEAR_LOAD_SPEED=75 UNIT=1",
                                       "MMU_TEST_CONFIG SELECTOR_MOVE_SPEED=180 UNIT=0",
                                       "MMU_TEST_CONFIG EXTRUDER_UNLOAD_SPEED=25"});

        helper.set_current_slot(-2);
        helper.clear_captured_gcodes();
        CHECK(helper.disable_bypass().success());
        CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_HOME UNIT=ALL"});
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v4 null telemetry clears the selected unit's readings",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");

    helper.test_parse_mmu_state(fx["mmu_status"]);
    auto info = helper.get_system_info();
    CHECK_FALSE(helix::buffer_reading(info, -1).present());
    CHECK(info.sync_feedback_bias == Catch::Approx(-2.0f));
    CHECK(info.sync_feedback_flow_rate == Catch::Approx(-1.0f));
    CHECK(info.sync_feedback_state.empty());

    // A unit with a buffer and an encoder is selected...
    helper.test_parse_mmu_state(
        {{"sync_feedback_bias_modelled", 0.25},
         {"sync_feedback_bias_raw", 0.5},
         {"sync_feedback_state", "neutral"},
         {"encoder", {{"flow_rate", 95}}},
         {"flowguard", {{"enabled", true}, {"level", 0.4}, {"encoder_mode", 2}}}});
    info = helper.get_system_info();
    REQUIRE(info.sync_feedback_bias == Catch::Approx(0.25f));
    REQUIRE(helix::buffer_reading(info, -1).present());
    REQUIRE(info.encoder_info.enabled);
    REQUIRE(info.flowguard_info.enabled);

    // ...then one with neither: its nulls are "no reading", not the last unit's.
    helper.test_parse_mmu_state({{"sync_feedback_bias_modelled", nullptr},
                                 {"sync_feedback_bias_raw", nullptr},
                                 {"encoder", nullptr},
                                 {"flowguard", nullptr},
                                 {"tangle_prevention", nullptr}});
    info = helper.get_system_info();
    CHECK(info.sync_feedback_bias == Catch::Approx(-2.0f));
    CHECK(info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    CHECK_FALSE(helix::buffer_reading(info, -1).present());
    CHECK_FALSE(info.encoder_info.enabled);
    CHECK_FALSE(info.flowguard_info.enabled);
    // Fields nothing reset keep their value.
    CHECK(info.encoder_info.flow_rate == 95);
    CHECK(info.flowguard_info.level == Catch::Approx(0.4f));
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v4 shows the selected gate's eSpooler operation",
                 "[ams][happy_hare][hh_v4]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    // gate 7, list says "assist", deprecated espooler_active says "".
    helper.test_parse_mmu_state(fx["mmu_status"]);
    CHECK(helper.get_system_info().espooler_state == "assist");

    // The gate moves without the list being resent.
    helper.test_parse_mmu_state({{"gate", 9}});
    CHECK(helper.get_system_info().espooler_state == "rewind");
    CHECK(HappyHareTestAccess::espooler_active(helper) == "rewind");

    // The deprecated alias no longer speaks once the list has.
    helper.test_parse_mmu_state({{"espooler_active", "print"}});
    CHECK(helper.get_system_info().espooler_state == "rewind");

    helper.test_parse_mmu_state({{"gate", -1}});
    CHECK(helper.get_system_info().espooler_state.empty());
}

// Multi-unit from mmu_machine

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare takes each unit's split, name and topology from mmu_machine",
                 "[ams][happy_hare][hh_multi_unit]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    auto check_units = [&] {
        const auto info = helper.get_system_info();
        REQUIRE(info.units.size() == 2);
        CHECK(info.units[0].name == "ERCF");
        CHECK(info.units[0].first_slot_global_index == 0);
        CHECK(info.units[0].slot_count == 6);
        CHECK(info.units[0].topology == PathTopology::LINEAR);
        CHECK(info.units[0].has_encoder);
        CHECK(info.units[1].name == "Box Turtle");
        CHECK(info.units[1].first_slot_global_index == 6);
        CHECK(info.units[1].slot_count == 4);
        CHECK(info.units[1].topology == PathTopology::HUB);
        CHECK_FALSE(info.units[1].has_encoder);
        CHECK(helper.get_unit_topology(1) == PathTopology::HUB);
        CHECK(info.total_slots == 10);
    };

    SECTION("status first: the re-split keeps every gate's state") {
        helper.test_parse_mmu_state(fx["mmu_status"]);
        helper.test_parse_mmu_state(
            {{"gate_color", {"FF0000", "", "", "", "", "", "", "", "", "00FF00"}}});
        REQUIRE(helper.get_system_info().units.size() == 1);
        connect_with_fixture(helper, client, fx);
        check_units();
        CHECK(helper.get_slot_info(0).color_rgb == 0xFF0000u);
        CHECK(helper.get_slot_info(9).color_rgb == 0x00FF00u);
        CHECK(helper.get_slot_info(3).status == SlotStatus::EMPTY);
    }
    SECTION("config first") {
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        check_units();
    }
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare reports each unit's vendor as its model",
                 "[ams][happy_hare][hh_multi_unit]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state(fx["mmu_status"]);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK(info.units[0].model == "ERCF");
    CHECK(info.units[1].model == "BoxTurtle");
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare names a unit's model once mmu_machine describes the split it kept",
                 "[ams][happy_hare][hh_multi_unit]") {
    // gate_status arrives first and builds the lone unit as "MMU", with no
    // model known. mmu_machine then repeats the same split under the same
    // name: the re-split keeps every gate, and the model is all that is new.
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    fx["mmu_machine"]["unit_0"]["display_name"] = "";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    REQUIRE(helper.get_system_info().units.size() == 1);
    REQUIRE(helper.get_system_info().units[0].name == "MMU");
    REQUIRE(helper.get_system_info().units[0].model.empty());

    connect_with_fixture(helper, client, fx);

    const auto units = helper.get_system_info().units;
    REQUIRE(units.size() == 1);
    CHECK(units[0].name == "MMU");
    CHECK(units[0].model == "QIDI");
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare single v4 unit is named by its display_name",
                 "[ams][happy_hare][hh_multi_unit]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state(fx["mmu_status"]);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].name == "QIDI-0");
    // The configured name is one to show; the AMS header names the unit by it.
    CHECK(info.units[0].display_name == "QIDI-0");
    CHECK(info.units[0].topology == PathTopology::HUB);
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare reads every unit's enclosure heaters, not only unit 0's",
                 "[ams][happy_hare][hh_multi_unit]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state(fx["mmu_status"]);

    CHECK(helper.get_dryer_info().supported);
    const auto& heaters = HappyHareTestAccess::filament_heaters(helper);
    REQUIRE(heaters.size() == 10);
    CHECK(heaters[0].empty());
    CHECK(heaters[6] == "heater_generic bt_heat0");
    CHECK(heaters[9] == "heater_generic bt_heat1");

    const auto zones = helper.get_environment_zones(-1);
    REQUIRE(zones.size() == 2);
    CHECK(zones[0].gates == std::vector<int>{6, 7});
    CHECK(zones[0].unit_index == 1);
    CHECK(zones[1].gates == std::vector<int>{8, 9});
    CHECK(helper.get_environment_zones(0).empty());
}

TEST_CASE_METHOD(HhHelper, "Happy Hare v4 reads clog detection mode from the encoder mode",
                 "[ams][happy_hare][hh_v4][clog]") {
    helper.test_parse_mmu_state({{"gate_status", {1, 1}}, {"clog_detection_enabled", false}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 0);

    // A unit with a buffer and an encoder: buffer FlowGuard plus the mode.
    helper.test_parse_mmu_state(
        {{"clog_detection_enabled", false},
         {"flowguard", {{"enabled", true}, {"level", 0.1}, {"trigger", ""}, {"encoder_mode", 1}}}});
    auto info = helper.get_system_info();
    CHECK(info.clog_detection == 1);
    CHECK(info.encoder_info.detection_mode == 1);
    CHECK(info.encoder_info.enabled);
    CHECK(info.flowguard_info.enabled);

    // An encoder-only unit publishes flowguard as {active, enabled,
    // encoder_mode}: the mode, but no buffer FlowGuard to meter.
    helper.test_parse_mmu_state(
        {{"flowguard", {{"active", false}, {"enabled", 1}, {"encoder_mode", 2}}},
         {"encoder", {{"detection_mode", 1}, {"flow_rate", 100}}}});
    info = helper.get_system_info();
    CHECK(info.encoder_info.detection_mode == 2); // configured, not the runtime state
    CHECK(info.encoder_info.enabled);
    CHECK_FALSE(info.flowguard_info.enabled);
    // A runtime encoder state on its own is not the configured mode.
    helper.test_parse_mmu_state({{"encoder", {{"detection_mode", 0}}}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 2);

    // v3's integer wins whenever it is published.
    helper.test_parse_mmu_state(
        {{"clog_detection_enabled", 2}, {"flowguard", {{"encoder_mode", 1}}}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 2);
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 writes clog detection mode as the selected unit's encoder mode",
                 "[ams][happy_hare][hh_v4][clog]") {
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_status"]["unit"] = 0;
    fx["mmu_status"]["gate"] = 2;
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);

    CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
          std::optional<std::string>(
              "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0 UNIT=0"));
    CHECK(helper.clog_detection_mode_gcode(2, 12.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
    helper.clear_captured_gcodes();
    helper.execute_device_action("clog_detection", std::string("Auto"));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=2 UNIT=0"});

    // With the encoder-less Box Turtle selected, the mode goes to the ERCF.
    helper.test_parse_mmu_state({{"unit", 1}, {"gate", 7}});
    CHECK(helper.clog_detection_mode_gcode(2, 0.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare clog detection mode command on v3 and a single v4 unit",
                 "[ams][happy_hare][hh_v4][clog]") {
    helper.set_config_defaults_for_test();

    SECTION("v3") {
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=12.0"));
        helper.execute_device_action("clog_detection", std::string("Manual"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=1"});
    }
    SECTION("v4 single unit with an encoder") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
        fx["configfile_settings"]["mmu_unit unit0"]["encoder"] = "unit0_encoder";
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0"));
        helper.execute_device_action("clog_detection", std::string("Off"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=0"});
    }
    SECTION("v4 single Type B unit has no encoder mode") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK_FALSE(helper.clog_detection_mode_gcode(2, 0.0f));
    }
}

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare v4 disables the actions no unit's hardware takes",
                 "[ams][happy_hare][hh_v4]") {
    SECTION("one QIDI unit: hub, no encoder, no buffer, always gripped, no toolhead sensor") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        for (const char* id : {"servo_up", "selector_speed", "calibrate_encoder", "calibrate_gates",
                               "clog_detection", "sync_to_extruder", "gear_from_buffer_speed",
                               "toolhead_sensor_to_nozzle"}) {
            INFO(id);
            CHECK_FALSE(action_enabled(helper, id));
        }
        for (const char* id : {"gear_from_spool_speed", "toolhead_entry_to_extruder",
                               "toolhead_ooze_reduction", "calibrate_bowden"}) {
            INFO(id);
            CHECK(action_enabled(helper, id));
        }
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(30.0)));
        CHECK_FALSE(helper.execute_device_action("servo_up", {}));
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("ERCF beside a Box Turtle: every action has a unit to go to") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        for (const char* id : {"servo_up", "selector_speed", "calibrate_gates", "sync_to_extruder",
                               "gear_from_buffer_speed", "toolhead_sensor_to_nozzle"}) {
            INFO(id);
            CHECK(action_enabled(helper, id));
        }
    }
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v4 whole-machine drying sends one MMU_HEATER per heated unit",
                 "[ams][happy_hare][hh_v4]") {
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);

    SECTION("only the Box Turtle has heaters") {
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.stop_drying(-1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("both units heated") {
        fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, -1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=0 GATES=0,1,2,3,4,5",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare re-split keeps sensors, the tool map and the hub state",
                 "[ams][happy_hare][hh_multi_unit]") {
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    params["mmu"]["ttg_map"] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    params["mmu"]["filament_pos"] = 5;
    feed_params(helper, params);
    REQUIRE(helper.get_system_info().units.size() == 1);
    REQUIRE(helper.get_system_info().units[0].hub_sensor_triggered);

    connect_with_fixture(helper, client, fx);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK_FALSE(info.units[0].hub_sensor_triggered);
    CHECK(info.units[1].hub_sensor_triggered); // gate 7 is the Box Turtle's
    CHECK(info.units[1].has_slot_sensors);
    CHECK(helper.get_gate_sensor(7)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(8)->pre_gate_triggered);
    CHECK(helper.get_tool_mapping() == std::vector<int>{9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
    CHECK(helper.get_slot_info(0).mapped_tool == 9);
}

TEST_CASE_METHOD(HhHelperWithClient,
                 "Happy Hare v3 connect keeps the hub state of a split it already has",
                 "[ams][happy_hare][hh_multi_unit]") {
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}}, {"gate", 1}, {"filament_pos", 5}});
    REQUIRE(helper.get_system_info().units[0].hub_sensor_triggered);
    connect_with_fixture(helper, client,
                         {{"configfile_settings",
                           {{"mmu", {{"happy_hare_version", 3.42}}},
                            {"mmu_machine", {{"selector_type", "LinearSelector"}}}}},
                          {"mmu_machine",
                           {{"unit_0", {{"name", "ERCF"}, {"num_gates", 4}, {"first_gate", 0}}},
                            {"num_units", 1}}}});
    const auto info = helper.get_system_info();
    CHECK(info.version == "3.42");
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].hub_sensor_triggered);
}
