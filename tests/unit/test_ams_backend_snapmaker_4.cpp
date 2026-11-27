// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 117 to 158 of test_ams_backend_snapmaker.cpp before it was split, in its order. Mostly
// [general], [filament_record][record_drift], [filament_record],
// [filament_record][official_repair], [unloaded_head_notice], and more

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
                 "Snapmaker a real card's official record is the tag's, a card-less one is not",
                 "[ams][snapmaker][filament_record][translucent]") {
    FilamentRecordHarness h("tag_owns_record");
    CHECK_FALSE(h.backend.tag_owns_filament_record(1));
    h.frame(make_detect_card(1, json::array({144, 32, 196, 2})));
    h.frame(make_task_filament({"NONE", "PLA", "NONE", "NONE"}, {false, true, false, false}));
    CHECK(h.backend.tag_owns_filament_record(1));
    CHECK_FALSE(h.backend.tag_owns_filament_record(2));
    CHECK_FALSE(h.backend.tag_owns_filament_record(9));
    // Official only by an endpoint write, no card behind it: an edit is forced through.
    h.frame(make_detect_card(1, 0));
    CHECK_FALSE(h.backend.tag_owns_filament_record(1));
}

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
                 "Snapmaker an edit changing color and translucency keeps it in the record",
                 "[ams][snapmaker][filament_record][translucent]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    FilamentRecordHarness h("translucent_echo");
    translucent_head_echoed(h);
    const auto record = SnapmakerTestAccess::get_override(h.backend, 1);
    REQUIRE(record.has_value());
    CHECK(record->color_rgb == 0xE8E8FFu);
    CHECK(record->translucent == std::optional<bool>(true));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit's translucency waits in the record for the firmware's echo",
                 "[ams][snapmaker][filament_record][translucent]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    FilamentRecordHarness h("translucent_waits");
    h.frame(make_feed_status(1, "load_finish"));
    SlotInfo clear;
    clear.brand = "Inland";
    clear.material = "PETG";
    clear.color_rgb = 0xE8E8FF;
    clear.translucent = true;
    helix::test::edit_slot_as_user(h.backend, 1, clear);
    const auto record = SnapmakerTestAccess::get_override(h.backend, 1);
    REQUIRE(record.has_value());
    // The firmware has not taken it yet, so the record does not state it.
    CHECK_FALSE(record->translucent.has_value());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a translucent head the firmware cleared is repaired with its alpha",
                 "[ams][snapmaker][filament_record][resume][translucent]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("translucent_repair");
    translucent_head_echoed(h);
    // The firmware resets the head's record: its type NONE, its color FFFFFFFF.
    json cleared =
        make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                           std::vector<bool>{false, true, false, false});
    cleared["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FFFFFFFF", "FFFFFFFF", "FFFFFFFF"});
    h.frame(cleared);
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.printer.client.clear_gcode_script_history();

    h.backend.prepare_for_resume(1, [](const AmsError&) {});
    FilamentRecordHarness::drain();
    const auto hist = h.history();
    CHECK(index_of_containing(hist, "CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=E8E8FF80") <
          hist.size());
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

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker an official-flag repair keeps the head\x27s sub-type",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    FilamentRecordHarness h("official_repair_subtype");
    SlotInfo edit = inland_pla();
    edit.spool_name.clear();
    helix::test::edit_slot_as_user(h.backend, 3, edit);
    h.printer.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    json ptc = make_official_task(3, "PLA", "FF5500FF");
    ptc["print_task_config"]["filament_sub_type"] =
        json::array({"NONE", "NONE", "NONE", "SnapSpeed"});
    h.frame(ptc);

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=SnapSpeed FORCE=1");
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
    FilamentRecordHarness h("official_repair_mid_print");
    helix::test::edit_slot_as_user(h.backend, 3, inland_pla());
    h.print(state);
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
    // The edit goes out on the app's print state; the repair waits for the
    // backend's own print_stats, the only answer it trusts.
    lv_init_safe();
    get_printer_state().init_subjects(false);
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

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker lane truth: a type or colour edit mid-print is refused, not held",
                 "[ams][snapmaker][filament_record][lane_truth]") {
    lv_init_safe();
    FilamentRecordHarness h("record_mid_print", "printing");
    h.frame(make_task_filament({"NONE", "PLA", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();

    SlotInfo shown = h.backend.get_slot_info(1);
    SlotInfo petg = shown;
    petg.material = "PETG";
    const AmsError refused = h.backend.commit_user_edit(1, shown, petg);
    CHECK(refused.result == AmsResult::WRONG_STATE);
    CHECK_FALSE(helix::ams::lane_sources(h.backend.lane_id(1)).pending.has_value());
    CHECK(h.backend.get_slot_info(1).material == "PLA");

    // Nothing waits to go out when the print ends: the firmware and the lane
    // still agree on what the head holds.
    h.print("complete");
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    // A spool link changes nothing the firmware holds, so it is saved.
    h.print("printing");
    SlotInfo linked = h.backend.get_slot_info(1);
    linked.spoolman_id = 42;
    REQUIRE(h.backend.commit_user_edit(1, h.backend.get_slot_info(1), linked).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
    const json stored = h.printer.client.mock_db_get("lane_data", "T1");
    REQUIRE(stored.is_object());
    CHECK(stored["spool_id"] == 42);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a type edit while paused is refused unless the firmware forgot the "
                 "head",
                 "[ams][snapmaker][filament_record][lane_truth]") {
    lv_init_safe();
    FilamentRecordHarness h("record_paused_typed", "paused");
    SECTION("the head still holds a type: the write would reset its pressure advance") {
        h.frame(make_task_filament({"NONE", "PLA", "NONE", "NONE"}));
        h.printer.client.clear_gcode_script_history();
        SlotInfo petg = h.backend.get_slot_info(1);
        petg.material = "PETG";
        CHECK(helix::test::apply_edit(h.backend, 1, petg).result == AmsResult::WRONG_STATE);
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().empty());
    }
    SECTION("the runout reload: the firmware reset the head, the resume writes it anyway") {
        h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
        h.printer.client.clear_gcode_script_history();
        REQUIRE(helix::test::apply_edit(h.backend, 1, inland_pla()).success());
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().size() == 2);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a type edit before any print state is read is refused",
                 "[ams][snapmaker][filament_record][lane_truth]") {
    lv_init_safe();
    // Neither this backend's frames nor the app have read a print state.
    PrinterStateTestAccess::reset(get_printer_state());
    FilamentRecordHarness h("record_state_unknown", /*print_state=*/"");
    REQUIRE_FALSE(get_printer_state().are_subjects_initialized());
    h.frame(make_task_filament({"NONE", "PLA", "NONE", "NONE"}));
    h.printer.client.clear_gcode_script_history();
    SlotInfo petg = h.backend.get_slot_info(1);
    petg.material = "PETG";
    CHECK(helix::test::apply_edit(h.backend, 1, petg).result == AmsResult::WRONG_STATE);
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    h.print("standby");
    CHECK(helix::test::apply_edit(h.backend, 1, petg).success());
    get_printer_state().init_subjects(false);
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

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker an edit naming no product line keeps the sub-type",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_subtype_kept");
    json ptc = make_task_filament({"NONE", "PLA", "NONE", "NONE"});
    ptc["print_task_config"]["filament_vendor"] =
        json::array({"NONE", "Snapmaker", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] =
        json::array({"NONE", "SnapSpeed", "NONE", "NONE"});
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FF5500FF", "FFFFFFFF", "FFFFFFFF"});

    SlotInfo edit;
    edit.brand = "Snapmaker";
    edit.material = "PLA";
    edit.color_rgb = 0x00FF00;

    SECTION("a colour change sends only the colour") {
        h.frame(ptc);
        h.printer.client.clear_gcode_script_history();
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=00FF00FF");
    }

    SECTION("a head rewritten whole sends the sub-type it already holds") {
        h.frame(make_written_entry(1, "PLA"));
        ptc["print_task_config"]["filament_official"][1] = true;
        h.frame(ptc);
        h.printer.client.clear_gcode_script_history();
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 2);
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Snapmaker "
                           "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=SnapSpeed FORCE=1");
    }

    SECTION("a new material does not inherit the old one\x27s sub-type") {
        h.frame(ptc);
        h.printer.client.clear_gcode_script_history();
        edit.material = "PETG";
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE_FALSE(writes.empty());
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Snapmaker "
                           "FILAMENT_TYPE=PETG FILAMENT_SUBTYPE=Basic");
    }

    SECTION("a named product line still replaces it") {
        h.frame(ptc);
        h.printer.client.clear_gcode_script_history();
        edit.spool_name = "Basic";
        edit.color_rgb = 0xFF5500;
        REQUIRE(helix::test::apply_edit(h.backend, 1, edit).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Snapmaker "
                           "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic");
    }
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
    REQUIRE(helix::ams::lane_sources(h.backend.lane_id(2)).firmware.has_value());

    // The same spool's entry rewritten over the API: CARD_UID 0.
    h.frame(make_written_entry(2, "ABS"));
    CHECK_FALSE(helix::ams::lane_sources(h.backend.lane_id(2)).firmware.has_value());
    CHECK(h.backend.get_slot_info(2).material != "ABS");
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
                 "Snapmaker writes a record back again when the firmware's clear overtook the "
                 "write",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_overtaken");
    declare_heads_0_and_2(h);

    // A print that ends at once: the toolhead switches' remove events clear
    // heads 0 and 2, and the write-back goes out.
    h.print("printing");
    h.frame(heads_0_and_2_cleared());
    h.print("cancelled");
    REQUIRE(writes_for_head(h, 0) == 2);
    REQUIRE(writes_for_head(h, 2) == 2);
    h.printer.client.clear_gcode_script_history();

    // The clears queued before the writes land after them, inside one status
    // tick: the record never names a type. Within the settle time nothing is
    // repeated.
    h.frame(heads_0_and_2_cleared());
    CHECK(h.config_writes().empty());

    SECTION("the next print runs a runout reload on head 2 and is cancelled") {
        SnapmakerTestAccess::age_record_restores(h.backend);
        h.print("printing");
        h.frame(json{{"filament_motion_sensor e2_filament", {{"filament_detected", false}}}});
        h.print("paused");
        h.frame(make_feed_status(2, "load_picking", /*filament_detected=*/true));
        h.frame(make_feed_status(2, "load_feeding", /*filament_detected=*/true));
        h.frame(json{{"filament_motion_sensor e2_filament", {{"filament_detected", true}}}});
        h.frame(make_feed_status(2, "load_finish", /*filament_detected=*/true));
        h.print("printing");
        h.frame(heads_0_and_2_cleared());
        CHECK(h.config_writes().empty());

        h.print("cancelled");
        CHECK(writes_for_head(h, 0) == 2);
        CHECK(writes_for_head(h, 2) == 2);
    }
    SECTION("a spool that really left gets no second write") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_feed_status(2, "wait_insert", /*filament_detected=*/false));
        SnapmakerTestAccess::age_record_restores(h.backend);
        h.frame(heads_0_and_2_cleared());
        CHECK(h.config_writes().empty());
    }
    SECTION("a spool inserted since the declaration gets no second write") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
        SnapmakerTestAccess::age_record_restores(h.backend);
        h.frame(heads_0_and_2_cleared());
        CHECK(writes_for_head(h, 0) == 0);
        CHECK(writes_for_head(h, 2) == 2);
    }
    SECTION("a firmware that clears every write is written a bounded number of times") {
        for (int i = 0; i < 6; ++i) {
            SnapmakerTestAccess::age_record_restores(h.backend);
            h.frame(heads_0_and_2_cleared());
        }
        // Three writes in all, the first before the history was cleared.
        CHECK(writes_for_head(h, 0) == 2 * 2);
        CHECK(writes_for_head(h, 2) == 2 * 2);
    }
    SECTION("a reconnect gives a write-back its full count of tries again") {
        for (int i = 0; i < 4; ++i) {
            SnapmakerTestAccess::age_record_restores(h.backend);
            h.frame(heads_0_and_2_cleared());
        }
        REQUIRE(writes_for_head(h, 0) == 2 * 2);
        h.printer.client.clear_gcode_script_history();

        h.backend.before_disconnect();
        SnapmakerTestAccess::call_on_started(h.backend);
        h.print("standby");
        h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
        h.frame(make_feed_status(2, "preload_finish", /*filament_detected=*/true));
        for (int i = 0; i < 6; ++i) {
            SnapmakerTestAccess::age_record_restores(h.backend);
            h.frame(heads_0_and_2_cleared());
        }
        CHECK(writes_for_head(h, 0) == 3 * 2);
        CHECK(writes_for_head(h, 2) == 3 * 2);
    }
    SECTION("an edit the record never echoed is written again as the edit, three times in all") {
        // The edit's write fails or a queued clear overtakes it: the record
        // still names no type, and the lane's override still holds the type
        // and color the firmware last stated.
        helix::test::edit_slot_as_user(h.backend, 0, petg_blue());
        FilamentRecordHarness::drain();
        REQUIRE(writes_for_head(h, 0) == 2);
        h.printer.client.clear_gcode_script_history();

        for (int i = 0; i < 4; ++i) {
            SnapmakerTestAccess::age_record_restores(h.backend);
            h.frame(heads_0_and_2_cleared());
        }
        CHECK(writes_for_head(h, 0) == 2 * 2);
        check_head_writes(h, 0, "PETG", "0000FFFF");
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes an edit the firmware has not echoed back, never the filament "
                 "it replaced",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    FilamentRecordHarness h("drift_restore_unechoed_edit");
    edit_head0_unechoed(h);

    SECTION("a frame from before the edit, then the firmware's clear") {
        h.frame(make_head_record(0, "PLA", "FF0000FF"));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "PETG", "0000FFFF");
    }
    SECTION("a reconnect, a frame from before the edit, then the firmware's clear") {
        h.backend.before_disconnect();
        SnapmakerTestAccess::call_on_started(h.backend);
        h.print("standby");
        h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
        h.frame(make_head_record(0, "PLA", "FF0000FF"));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "PETG", "0000FFFF");
    }
    SECTION("the clear lands in a print paused on the head: RESUME's repair writes the edit") {
        h.print("printing");
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        h.print("paused");
        h.printer.client.clear_gcode_script_history();
        SnapmakerTestAccess::set_current_slot(h.backend, 0);
        SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
        h.backend.prepare_for_resume(0, [](const AmsError&) {});
        FilamentRecordHarness::drain();
        check_head_writes(h, 0, "PETG", "0000FFFF");
    }
    SECTION("the firmware echoes the edit, another writer restates PLA red, then a clear") {
        h.frame(make_head_record(0, "PETG", "0000FFFF"));
        h.frame(make_head_record(0, "PLA", "FF0000FF"));
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "PLA", "FF0000FF");
    }
    SECTION("another writer names a third type before the echo, then a clear") {
        h.frame(make_head_record(0, "ABS", "00FF00FF"));
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "ABS", "00FF00FF");
    }
    SECTION("the user clears the lane, then the firmware's clear: nothing is written") {
        h.backend.clear_slot_override(0);
        FilamentRecordHarness::drain();
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "PLA", "FF0000FF"));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(writes_for_head(h, 0) == 0);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker writes a translucent edit the firmware has not echoed back with its "
                 "alpha",
                 "[ams][snapmaker][filament_record][record_drift][translucent]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    FilamentRecordHarness h("drift_restore_unechoed_translucent");
    edit_head0_unechoed(h, petg_blue_translucent());

    SECTION("a frame from before the edit, then the firmware's clear") {
        h.frame(make_head_record(0, "PLA", "FF0000FF"));
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "PETG", blue_translucent_rgba());
    }
    SECTION("a frame naming the edit's type and color but full opacity is no echo") {
        h.frame(make_head_record(0, "PETG", "0000FFFF"));
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        check_head_writes(h, 0, "PETG", blue_translucent_rgba());
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit the firmware never echoed is not written over a spool that "
                 "changed",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    FilamentRecordHarness h("drift_restore_edit_spool_changed");
    edit_head0_unechoed(h);
    // A frame from before the edit: the lane shows the filament it replaced.
    h.frame(make_head_record(0, "PLA", "FF0000FF"));

    SECTION("a spool goes into the port: RESUME's repair writes the lane, not the edit") {
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/false));
        h.frame(make_feed_status(0, "wait_insert", /*filament_detected=*/true));
        h.print("printing");
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        h.print("paused");
        h.printer.client.clear_gcode_script_history();
        SnapmakerTestAccess::set_current_slot(h.backend, 0);
        SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
        h.backend.prepare_for_resume(0, [](const AmsError&) {});
        FilamentRecordHarness::drain();
        for (const auto& w : h.config_writes()) {
            INFO(w);
            CHECK(w.find("PETG") == std::string::npos);
            CHECK(w.find("RGBA=0000FF") == std::string::npos);
        }
    }
    SECTION("the tag reader names a different spool: nothing is written back") {
        h.frame(make_filament_detect_status(0, "PLA", 0xFFFF0000u, "Polymaker",
                                            json::array({1, 2, 3, 4})));
        h.frame(make_filament_detect_status(0, "ABS", 0xFF00FF00u, "Generic",
                                            json::array({5, 6, 7, 8})));
        h.printer.client.clear_gcode_script_history();
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(writes_for_head(h, 0) == 0);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker lane truth: an edit's type goes to print_task_config and the record "
                 "copies it from the echo",
                 "[ams][snapmaker][filament_record][lane_truth]") {
    lv_init_safe();
    FilamentRecordHarness h("lane_truth_u1_edit");
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    h.frame(make_head_record(0, "PLA", "FFFFFFFF"));
    h.printer.client.clear_gcode_script_history();

    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    REQUIRE(h.config_writes().size() == 2);
    // A frame from before the write landed: the slot shows the edit, the
    // firmware does not hold it yet.
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    json stored = h.printer.client.mock_db_get("lane_data", "T0");
    REQUIRE(stored.is_object());
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(stored["material"] != "ASA");
    CHECK(h.backend.get_slot_info(0).material == "ASA");

    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    FilamentRecordHarness::drain();
    stored = h.printer.client.mock_db_get("lane_data", "T0");
    CHECK(stored["material"] == "ASA");
    CHECK(stored["color"] == "#FF9800");
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(h.backend.get_slot_info(0).material == "ASA");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker lane truth: print_task_config is the lane's firmware reading",
                 "[ams][snapmaker][filament_record][lane_truth]") {
    lv_init_safe();
    FilamentRecordHarness h("lane_truth_u1_firmware");
    helix::ams::FilamentSlotOverride stored;
    stored.brand = "Polymaker";
    stored.material = "ABS";
    stored.color_rgb = 0x123456;
    stored.color_set = true;
    SnapmakerTestAccess::seed_override(h.backend, 0, stored);
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));

    // Another client set the head to PETG: it outranks the stored record.
    h.frame(make_head_record(0, "PETG", "00FF00FF"));
    const auto fw = helix::ams::lane_sources(h.backend.lane_id(0)).firmware;
    REQUIRE(fw.has_value());
    CHECK(fw->material == std::optional<std::string>("PETG"));
    CHECK(fw->color_rgb == std::optional<uint32_t>(0x00FF00u));
    SlotInfo shown = h.backend.get_slot_info(0);
    CHECK(shown.material == "PETG");
    CHECK(shown.color_rgb == 0x00FF00u);
    CHECK(shown.brand == "Polymaker");

    // An edit stands as pending until print_task_config echoes it.
    SlotInfo asa = h.backend.get_slot_info(0);
    asa.material = "ASA";
    asa.color_rgb = 0xFF9800;
    helix::test::edit_slot_as_user(h.backend, 0, asa);
    FilamentRecordHarness::drain();
    CHECK(h.backend.get_slot_info(0).material == "ASA");
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    const auto pending = helix::ams::lane_sources(h.backend.lane_id(0)).pending;
    CHECK_FALSE((pending.has_value() && pending->material.has_value()));
    CHECK_FALSE((pending.has_value() && pending->color_rgb.has_value()));

    // A record the firmware reset states nothing: the lane record shows.
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    CHECK_FALSE(helix::ams::lane_sources(h.backend.lane_id(0)).firmware.has_value());
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
    SECTION("a user edit to the record is refused") {
        SlotInfo pla = polymaker_asa();
        pla.material = "PLA";
        CHECK(helix::test::apply_edit(h.backend, 0, pla).result == AmsResult::WRONG_STATE);
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().empty());
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
                 "Snapmaker writes nothing back for a head whose record names no spool",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore_undeclared");
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();

    // The record holds the type it copied from the firmware.
    helix::ams::FilamentSlotOverride copied;
    copied.material = "ASA";
    copied.color_rgb = 0xFF9800;
    copied.color_set = true;
    SECTION("and nothing else: no write-back") {
        SnapmakerTestAccess::seed_override(h.backend, 0, copied);
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().empty());
    }
    SECTION("and the spool an edit named: written back") {
        copied.brand = "Polymaker";
        SnapmakerTestAccess::seed_override(h.backend, 0, copied);
        h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
        CHECK(h.config_writes().size() == 2);
    }
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
    SECTION("a head loaded by hand past its feeder") {
        FileToolsProbe probe(h.backend, {0, 2});
        slicer_print_ready(h);
        h.frame(json{{"filament_feed right", {{"extruder2", {{"filament_detected", false}}}}}});
        h.frame(json{{"filament_motion_sensor e2_filament", {{"filament_detected", true}}}});
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
                 "Snapmaker writes a declared spool back after the firmware drops its record",
                 "[ams][snapmaker][filament_record][record_drift]") {
    lv_init_safe();
    FilamentRecordHarness h("drift_restore");
    declare_head0_asa(h);

    // Filament left the toolhead: the firmware resets the head to NONE / white.
    h.frame(make_head_record(0, "NONE", "FFFFFFFF"));
    // The lane shows the record Moonraker holds once it is read.
    h.backend.refresh_lane_records();
    FilamentRecordHarness::drain();

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
                 "Snapmaker a failed load with a token of no wording reads as the failed load",
                 "[ams][snapmaker][action_state][feed_failure]") {
    helix::test::StartedBackend<CapturingSnapmakerBackend> backend;

    backend.fire_status(make_feed_status(0, "load_extruding", true));
    backend.fire_status(make_feed_status(0, "load_fail", true, "move_extrude"));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail == "Feeder 1: Load failed");

    // With no failed operation, the token stays: it names a novel fault.
    backend.fire_status(make_feed_status(1, "load_feeding", true));
    backend.fire_status(make_feed_status(1, "load_feeding", true, "move_extrude"));
    CHECK(backend.get_system_info().operation_detail.find("move_extrude") != std::string::npos);
}
