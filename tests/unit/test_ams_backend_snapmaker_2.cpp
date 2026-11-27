// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 48 to 74 of test_ams_backend_snapmaker.cpp before it was split, in its order. Mostly
// [filament_slot_override], [filament_slot_override][rfid], [general],
// [filament_slot_override][1710], [lane][1654], and more

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

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker motion-sensor runout path is independent of the loaded latch",
                 "[ams][snapmaker][channel_state][runout]") {
    // Mid-print runout is driven by the per-tool motion
    // sensor (e{N}_filament), a different question from "is the lane loaded".
    // The loaded latch must NOT be touched by a motion-sensor runout — filament
    // that stops moving is still loaded at the toolhead, it just ran out upstream.
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // Active tool 0 loaded to its nozzle, with filament present everywhere.
    json loaded = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    backend.fire_status(loaded);
    REQUIRE(backend.get_system_info().filament_loaded);

    // Active lane's motion sensor drops during extrusion → runout.
    json runout = json{{"filament_motion_sensor e0_filament", json{{"filament_detected", false}}}};
    backend.fire_status(runout);

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
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    SECTION("unloaded lane with motion sensor still present draws PREP, not NOZZLE") {
        // Exact live condition: unload_finish but the toolhead motion sensor and
        // buffer port sensor both still read present.
        backend.fire_status(make_feed_status(2, "load_finish"));
        SnapmakerTestAccess::set_sensor_present(backend, 2, true);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, true);
        backend.fire_status(make_feed_status(2, "unload_finish",
                                             /*filament_detected=*/true));
        CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
        // Filament is at the feeder but NOT at the nozzle.
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::PREP);
    }
}

// Extruder State Parser Tests

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

// RFID Info Parser Tests

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

// Filament slot override integration

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker clear_slot_override drops the Spoolman handles",
                 "[ams][snapmaker][filament_slot_override][1625]") {
    SnapmakerTmpCacheDir tmp("clear_spoolman_link");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;

    // A filament_detect parse populates the units a clear walks.
    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;

    // "Basic" is the placeholder SUB_TYPE firmware keeps beside any vendor.
    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;

    backend.fire_status(
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
    backend.fire_status(untagged);
    CHECK_FALSE(helix::ams::lane_sources(backend_reg.lane(1)).firmware.has_value());

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
    backend.fire_status(untagged);
    CHECK(events == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker print_task_config type NONE is no material",
                 "[ams][snapmaker][print_task_config]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{
        {"print_task_config", json{{"filament_type", json::array({"PLA", "NONE", "PETG", ""})}}}});
    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(1).material.empty());
    CHECK(backend.get_slot_info(2).material == "PETG");
    CHECK(backend.get_slot_info(3).material.empty());

    // The firmware stops a print on a head whose record names no type.
    CHECK_FALSE(backend.get_slot_info(0).material_required);
    CHECK(backend.get_slot_info(1).material_required);
    CHECK_FALSE(backend.get_slot_info(2).material_required);
    CHECK(backend.get_slot_info(3).material_required);

    backend.fire_status(json{
        {"print_task_config", json{{"filament_type", json::array({"PLA", "PLA", "PETG", ""})}}}});
    CHECK_FALSE(backend.get_slot_info(1).material_required);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "a U1 head with no material set blocks a print that calibrates it",
                 "[ams][snapmaker][print_task_config][preflight]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{
        {"print_task_config", json{{"filament_type", json::array({"ASA", "NONE", "PC", "NONE"})},
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
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // The helper's tag states 190-220 nozzle, 60 bed, 1000 g.
    backend.fire_status(
        make_filament_detect_status(2, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(2).nozzle_temp_max == 220);
    REQUIRE(backend.get_slot_info(2).total_weight_g == 1000.0f);

    backend.fire_status(json{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}});
    const SlotInfo blank;
    auto info = backend.get_slot_info(2);
    CHECK(info.nozzle_temp_min == blank.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == blank.nozzle_temp_max);
    CHECK(info.bed_temp == blank.bed_temp);
    CHECK(info.total_weight_g == blank.total_weight_g);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker user's edit stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("edit_over_untagged");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});

    // The helper's tag: PLA, Snapmaker, 190-220 nozzle, 60 bed.
    const json tag =
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4}));
    backend.fire_status(untagged_frame());
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
        // The firmware takes the edit's type into print_task_config.
        backend.fire_status(
            json{{"print_task_config",
                  {{"filament_type", json::array({"PLA", "NONE", "NONE", "NONE"})}}}});
        backend.fire_status(tag);
    }
    // The edit's record write lands, and the read it starts files the record.
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE(backend.get_slot_info(0).material == edit.material);

    // The lane's record carries the edit's identity past the tag's removal.
    backend.fire_status(untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.material == edit.material);
    CHECK(info.brand == edit.brand);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker bound spool stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("spool_over_untagged");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});

    backend.fire_status(
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

    backend.fire_status(untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == spool.material);
    CHECK(info.brand == spool.vendor);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker restart forgets the previous session's tag and task values",
                 "[ams][snapmaker][rfid]") {
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    // print_task_config names one colour, then the tag reads another, so the
    // slot shows the tag's while the task colour waits behind it.
    backend.fire_status(
        json{{"print_task_config",
              json{{"filament_color_rgba",
                    json::array({"AA0000FF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).color_rgb == 0x202020u);
    REQUIRE(backend.get_slot_info(0).nozzle_temp_max == 220);

    SnapmakerTestAccess::call_on_started(backend);
    backend.fire_status(untagged_frame());

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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));
    backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});

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
    // data. The user's edit must survive: apply_overrides re-layers the
    // saved override over the parse output.
    json status =
        make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand", json::array({1, 2, 3, 4}));
    backend.fire_status(status);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");                // survived
    CHECK(info.spool_name == "PolyLite PLA Orange"); // survived
    CHECK(info.spoolman_id == 42);                   // survived
    CHECK(info.spoolman_filament_id == 55);          // survived
    CHECK(info.material == "PLA");                   // override material wins
    CHECK(info.color_rgb == 0xFF5500u);              // override color wins
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker RFID UID change clears override (hardware swap detected)",
                 "[ams][snapmaker][filament_slot_override]") {
    SnapmakerTmpCacheDir tmp("task12_uid_swap_clears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
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
    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));

    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    REQUIRE(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Second parse: DIFFERENT CARD_UID — physical swap detected. Override
    // must be cleared in-memory AND the Moonraker DB entry deleted.
    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
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
    backend.fire_status(make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                                    json::array({99, 99, 99, 99})));

    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // A second parse of the SAME UID stays the baseline — no clear, no
    // "weird state" that fires on unchanged polls.
    backend.fire_status(make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
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
        helix::test::RegisteredWired<AmsBackendSnapmaker> session1(&api);
        // The record on the wire loaded as this session's override.
        REQUIRE(SnapmakerTestAccess::get_override(*session1, 0).has_value());
        session1->fire_status(
            make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", uid_f1));
        auto stored = mock_printer.client.mock_db_get("lane_data", "T0");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "1,2,3,4");
    }

    SECTION("a swap made while the app was down clears the override") {
        helix::test::RegisteredWired<AmsBackendSnapmaker> session2(&api);
        // The loaded record's fingerprint is the baseline, so the first frame
        // after restart is a comparison, not a first observation.
        REQUIRE(SnapmakerTestAccess::get_override(*session2, 0).has_value());
        session2->fire_status(
            make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", uid_f2));
        CHECK(SnapmakerTestAccess::last_rfid_uid(*session2, 0) == "5,6,7,8");
        CHECK_FALSE(SnapmakerTestAccess::get_override(*session2, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }

    SECTION("the same spool across a restart keeps the override") {
        helix::test::RegisteredWired<AmsBackendSnapmaker> session2(&api);
        session2->fire_status(
            make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", uid_f1));
        auto ovr = SnapmakerTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }

    SECTION("a record without a stored fingerprint stays a first-observation baseline") {
        // The shape every record written before the field existed has.
        mock_printer.client.mock_db_set("lane_data", "T0",
                                        helix::ams::to_lane_data_record(0, saved));
        helix::test::RegisteredWired<AmsBackendSnapmaker> session2(&api);
        session2->fire_status(
            make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", uid_f2));
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

// A start opens a new lane record store, which frees the one before it. The
// tracker's baseline sink runs inline on every Baseline and Unchanged
// observation and outlives that start, so it must follow the backend's pointer
// to its store rather than hold the store: the one the last read saw is freed.
//
// The record in the window is a meter's: update_slot_weight with persist, the
// call the consumption tracker makes, stages one whenever it lands. The store
// the start opened has not read, so the gate refuses that record's own save,
// and the fingerprint a poll confirms must not slip past the gate through the
// old store.
//
// Break: bind the sink to override_store_.get() instead of override_store_.
// The sink the first read bound then names the store open_lane_record_store()
// freed, and the poll in the window writes through it: a heap-use-after-free
// under ASAN.
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a restart never leaves the fingerprint sink on the replaced store",
                 "[ams][snapmaker][store_restart]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendSnapmaker> harness(&api);
    auto& backend = *harness;
    const json uid = json::array({1, 2, 3, 4});
    const auto poll = [&backend, uid]() {
        backend.fire_status(make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", uid));
    };

    // The first start reads its store, and that read binds the sink.
    poll();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // A restart frees that store and opens another, whose first read is held.
    mock_printer.client.defer_next("server.database.get_item");
    SnapmakerTestAccess::call_on_started(backend);
    REQUIRE_FALSE(backend.lane_records_ready());

    // A meter's flush lands in that window and stages the lane's record.
    backend.update_slot_weight(0, 300.0f, 1000.0f, /*persist=*/true);
    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());

    // The reader states the same tag again.
    poll();
    CHECK_FALSE(mock_printer.client.mock_db_get("lane_data", "T0").contains("helix_fingerprint"));

    // The read lands. The same poll now persists, through the new store.
    mock_printer.client.fire_deferred("server.database.get_item");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(backend.lane_records_ready());
    backend.update_slot_weight(0, 300.0f, 1000.0f, /*persist=*/true);
    poll();
    const auto stored = mock_printer.client.mock_db_get("lane_data", "T0");
    REQUIRE(stored.contains("helix_fingerprint"));
    CHECK(stored.at("helix_fingerprint") == "1,2,3,4");
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid UID "1,2,3,4" — baseline established.
    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // Second parse: the tag decoded (PLA / FF5500) but its CARD_UID came back
    // empty. The frame answered, so the baseline takes the material-and-colour
    // reading - and the record stands, because with no UID on the new side
    // there is nothing to call a different spool over.
    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PLA|FF5500");
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());

    // Third parse: the same spool's UID decodes again. The reading changed
    // spelling, but material and colour match what the unreadable pass saw,
    // so the verdict is same spool and the override survives the glitch.
    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // The reader answers NONE with no UID: no evidence either way.
    backend.fire_status(
        make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // A channel the reader cannot answer stays unanswered across reads: the
    // baseline never moves, nothing clears, nothing crashes.
    backend.fire_status(
        make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));
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
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

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
    backend.fire_status(make_feed_status(0, "wait_insert", false));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // Filament goes in; the reader's next entry carries no tag behind it: the
    // record stands and the notice asks.
    backend.fire_status(make_feed_status(0, "load_finish", true));
    backend.fire_status(make_filament_detect_status(0, "NONE", 0u, "NONE", json::array()));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());

    // A channel whose tag was read never asks: the reader's entry after the
    // edge vouches for the insert, and the UID rule owns the swap verdict.
    backend.fire_status(make_feed_status(1, "wait_insert", false));
    backend.fire_status(make_feed_status(1, "load_finish", true));
    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Baseline attempt: NONE answers carry no reading, so none is taken.
    backend.fire_status(
        make_filament_detect_status(0, "NONE", 0xFFFF5500u, "Polymaker", json::array()));
    REQUIRE_FALSE(SnapmakerTestAccess::last_rfid_uid(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());

    backend.fire_status(
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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "T0",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    backend.fire_status(
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PLA|FF5500");

    SECTION("a different material is a different spool") {
        backend.fire_status(
            make_filament_detect_status(0, "PETG", 0x00FF00u, "Polymaker", json::array()));

        CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "T0").is_null());
        CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "M|PETG|00FF00");
    }

    SECTION("the same reading repeated is not an event") {
        backend.fire_status(
            make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));

        CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
        CHECK(!mock_printer.client.mock_db_get("lane_data", "T0").is_null());
    }
}

// Auto-mirror

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

    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
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
    backend.fire_status(status);

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

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a frame repeating a linked lane's values writes nothing",
                 "[ams][snapmaker][lane][1654]") {
    SnapmakerLaneRig rig("mirror_idle_frame");
    auto& backend = *rig.backend_reg;

    // The record already carries the tag's fingerprint, so the repeat has nothing to file.
    backend.fire_status(seated_tag_frame(0xFFFF5500u, "PLA"));
    auto linked = linked_to_spool_42();
    linked.fingerprint = SnapmakerTestAccess::last_rfid_uid(backend, 0).value();
    SnapmakerTestAccess::seed_override(backend, 0, linked);
    const int posts_before = rig.client.call_count("server.database.post_item");

    backend.fire_status(seated_tag_frame(0xFFFF5500u, "PLA"));

    const auto sources = helix::ams::lane_sources(rig.backend_reg.lane(0));
    // The frame was parsed, and the slot is one the mirror runs on.
    REQUIRE(sources.sensed.has_value());
    REQUIRE(sources.sensed->present == true);
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    CHECK(rig.client.call_count("server.database.post_item") == posts_before);
    CHECK_FALSE(sources.pending.has_value());
    REQUIRE(sources.record.has_value());
    CHECK(sources.record->spoolman_id == 42);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unlinked lane with no declaration still follows firmware",
                 "[ams][snapmaker][lane][1654]") {
    SnapmakerLaneRig rig("mirror_unlinked_follows");
    auto& backend = *rig.backend_reg;

    // Undeclared and unlinked: what the store remembers.
    helix::ams::FilamentSlotOverride remembered;
    remembered.color_rgb = 0xABCDEF;
    remembered.color_set = true;
    remembered.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, remembered);
    {
        const auto loaded = helix::ams::lane_sources(rig.backend_reg.lane(0));
        REQUIRE_FALSE(loaded.spoolman.has_value());
        REQUIRE_FALSE(loaded.pending.has_value());
    }

    backend.fire_status(seated_tag_frame(0xFF112233u, "PETG"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);

    const auto stored = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(stored.has_value());
    CHECK(stored->color_rgb == 0x112233u);
    CHECK(stored->material == "PETG");
    CHECK(backend.get_slot_info(0).color_rgb == 0x112233u);
}

// prepare_for_resume: classify_pause() decides whether a pause can resume.
// An inactive virtual_sdcard alone is not terminal; the terminal matchers
// (dirty bed, defect exception 532 code 1) answer RESUME_REQUIRES_RESTART up
// front. A recoverable pause whose head reads filament present skips the
// recovery load; one reading absent needs the API to load it.

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
