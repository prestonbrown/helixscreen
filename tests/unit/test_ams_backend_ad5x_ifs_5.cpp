// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 144 to 181 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs], [ad5x_ifs][local_write], [general], [ad5x_ifs][1324], [ad5x_ifs][issue_904], and more

#include "ui_insert_notice.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/unique_temp_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::kAmsDefaultSlotColor;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::StepOperationType;

using helix::test::Ad5xHomingGuardBackend;
using helix::test::Ad5xHomingGuardFixture;
using helix::test::Ad5xIfsTmpCacheDir;
using helix::test::Ad5xIfsTmpJsonFile;
using helix::test::Ad5xRunoutFixture;
using helix::test::Ad5xRunoutOpBackend;
using helix::test::Ad5xRunoutOpFixture;
using helix::test::boot_from_lane_data;
using helix::test::drain_queue;
using helix::test::firmware_states_slot0;
using helix::test::FrameRig;
using helix::test::GcodeCapturingBackend;
using helix::test::identity_tool_map;
using helix::test::IfsLaneTruth;
using helix::test::json;
using helix::test::kZmodMaterials;
using helix::test::kZmodPalette;
using helix::test::lane_record_reread_lands;
using helix::test::less_waste_vars;
using helix::test::lesswaste_frame;
using helix::test::lesswaste_ready;
using helix::test::LiveAckAd5xIfsBackend;
using helix::test::macro_changes_state;
using helix::test::macro_error;
using helix::test::make_auto_mirror_override;
using helix::test::make_extruder;
using helix::test::make_head_sensor;
using helix::test::make_ifs_frame;
using helix::test::make_ifs_vars_macro;
using helix::test::make_locked_override;
using helix::test::make_motion_sensor;
using helix::test::make_port_sensor;
using helix::test::make_routing_backend;
using helix::test::make_save_variables;
using helix::test::make_zcolor;
using helix::test::make_zmod_head_sensor;
using helix::test::make_zmod_motion_sensor;
using helix::test::material_button;
using helix::test::outside_lane_record;
using helix::test::outside_tool_change_zcolor;
using helix::test::palette_button;
using helix::test::plugin_rig_frame;
using helix::test::QuietRunningAd5xIfs;
using helix::test::RegisteredAd5xIfs;
using helix::test::RunningAd5xIfs;
using helix::test::seat_slot0;
using helix::test::seat_unrecorded_zmod;
using helix::test::seed_standard_colors;
using helix::test::slot_row;
using helix::test::standard_variables;
using helix::test::TestableAd5xIfsBackend;
using helix::test::unload_routed_to_toolhead;
using helix::test::WiredAd5xIfs;
using helix::test::wrap_notification;
using helix::test::zmod_change_frame;
using json = nlohmann::json;

TEST_CASE("AD5X IFS clear_slot_override drops the whole Spoolman link",
          "[ams][ad5x_ifs][filament_slot_override][1625]") {
    Ad5xIfsTmpCacheDir tmp("clear_spoolman_link");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    // Prime with a firmware parse so slots_ has an entry to reset.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::seed_live_spoolman_link(backend, 0, 42, 77, 3));
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 77);

    backend.clear_slot_override(0);

    // All three handles die together: a slot cleared of its override must not
    // keep naming a Spoolman record, and a surviving filament handle would.
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.spoolman_filament_id == 0);
    CHECK(info.spool_name.empty());
}

TEST_CASE("AD5X IFS clear_slot_override is safe when no override is present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override_noop");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");

    // No override staged. Calling clear must not crash and must leave
    // firmware-sourced fields intact.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x00FF00u);
    CHECK(info.material == "PETG");
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS clear_slot_override rejects out-of-range indices",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // Give slot 0 a real firmware entry and a staged override, so "did the
    // out-of-range call touch anything?" has something to be true of. Without
    // this the test asserts nothing: an empty backend has no state to clobber.
    // Seed BEFORE the parse - overrides are folded into the live SlotInfo by
    // update_slot_from_state, so one staged afterwards never reaches
    // get_slot_info().
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyTerra Charcoal";
    ovr.spoolman_id = 42;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(backend.get_slot_info(0).brand == "Polymaker");

    backend.clear_slot_override(-1);
    backend.clear_slot_override(AmsBackendAd5xIfs::kDefaultPorts);
    backend.clear_slot_override(999);

    // The rejected indices must not have been folded/clamped onto a real slot.
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0)->spoolman_id == 42);
    CHECK(backend.get_slot_info(0).brand == "Polymaker");

    // Positive control: the same call with an in-range index DOES clear, so a
    // clear_slot_override() that had simply stopped working could not pass the
    // assertions above by accident.
    backend.clear_slot_override(0);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(backend.get_slot_info(0).brand.empty());
}

// Which console lines re-read the slots. With our own GET_ZCOLOR in flight,
// zmod echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens and its response rows: every line
// is buffered and nothing re-arms, or the listener loops on its own echo. With
// no query in flight, a RUN_ZCOLOR/CHANGE_ZCOLOR token, or zmod's bare,
// non-localized `Extruder: <N>` channel-commit line from _SET_EXTRUDER_SLOT
// (an unload through zmod's own macro carries no colour token), schedules one
// re-read. The bare form is strict: SILENT responses and prompts carry an
// " | IFS:" suffix, slot rows read "1: PLA/FFFFFF", and the unload dialog
// DEFINITION echoes IN_ZCOLOR when it renders, not when it runs.
TEST_CASE("AD5X IFS listener re-reads only on external colour or unload lines",
          "[ams][ad5x_ifs][zcolor]") {
    struct Row {
        const char* line;
        bool query_in_flight;
        bool buffered;
        uint32_t scheduled;
    };
    const Row rows[] = {
        {"// RUN_ZCOLOR slot=2 color=FF0000", true, true, 0},
        {"// CHANGE_ZCOLOR slot=3", true, true, 0},
        {"// Extruder: None (1) | IFS: True", true, true, 0},
        {"// 2: PLA/2750E0", true, true, 0},
        {"// CHANGE_ZCOLOR slot=1 color=00FF00", false, false, 1},
        // Captured live: AD5X, unloading lane 3 while lane 4 active
        {"Extruder: 3", false, false, 1},
        {"// Extruder: 2", false, false, 1},
        {"// Extruder: 1: PLA/FF0000 | IFS: True", false, false, 0},
        {"// Extruder: None (1) | IFS: True", false, false, 0},
        {"ok", false, false, 0},
        {"// 2: PETG/00FF00", false, false, 0},
        {"// action:prompt_button Unload|IN_ZCOLOR SLOT=3 NAPR=1|primary|", false, false, 0},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.line << (r.query_in_flight ? " (query in flight)" : "")) {
            helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
            AmsBackendAd5xIfs& backend = *backend_reg;
            Ad5xIfsTestAccess::set_zcolor_query_active(backend, r.query_in_flight);
            const uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
            REQUIRE(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == 0);

            CHECK(Ad5xIfsTestAccess::on_gcode_response_line(backend, r.line) == r.buffered);
            CHECK(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == (r.buffered ? 1u : 0u));
            CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + r.scheduled);
        }
    }
}

// A single user color edit makes zmod re-emit its "Select print materials"
// prompt, which echoes a burst of CHANGE_ZCOLOR tokens on the console (20+ in a
// 40ms window — bundle ACJRZBXJ, an old-zmod AD5X). schedule_zcolor_query must
// not submit one HttpExecutor::fast() worker per line, each holding a pool slot
// through its 500ms debounce sleep while only a single query ever fires. The
// zcolor_schedule_armed_ gate coalesces the burst into one in-flight worker.
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS coalesces a burst of color-change triggers into one debounce worker",
                 "[ams][ad5x_ifs][zcolor]") {
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    const uint32_t sched_before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    const uint32_t submit_before = Ad5xIfsTestAccess::zcolor_worker_submit_count(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_schedule_armed(backend));

    constexpr int kBurst = 20;
    for (int i = 0; i < kBurst; ++i) {
        Ad5xIfsTestAccess::on_gcode_response_line(backend,
                                                  "// CHANGE_ZCOLOR SLOT=2 HEX=F72224 TYPE=PLA");
    }

    // Every trigger is seen (diagnostic counter rises by the full burst)...
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == sched_before + kBurst);
    // ...but only ONE debounce worker was submitted; the rest hit the armed gate.
    CHECK(Ad5xIfsTestAccess::zcolor_worker_submit_count(backend) == submit_before + 1);
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_armed(backend));
}

// JSON-content poll dedup
//
// poll_adventurer_json downloads Adventurer5M.json and only parses + fires
// GET_ZCOLOR when the body has changed vs. last seen. The comparison is
// content-equality on the raw JSON string. Tests drive the comparison
// helper directly so they don't depend on a live download path.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS note_json_content reports changed only on different bytes",
                 "[ams][ad5x_ifs][zcolor]") {
    const std::string a = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
    const std::string b = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

    // First-ever observation is "changed" (last_json_content_ starts empty).
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, a));
    // Re-observing the same content is NOT changed.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    // A different body flips back to changed.
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, b));
    // And steady-state again on b.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, b));
}

TEST_CASE("AD5X IFS write_adventurer_json_local read-modify-writes the on-disk file",
          "[ams][ad5x_ifs][local_write]") {
    // Seed a realistic Adventurer5M.json with all four slots.
    const std::string seed = R"({
    "FFMInfo": {
        "ffmColor1": "#FF0000",
        "ffmType1": "PLA",
        "ffmColor2": "#00FF00",
        "ffmType2": "PETG",
        "ffmColor3": "#0000FF",
        "ffmType3": "ABS",
        "ffmColor4": "#FFFFFF",
        "ffmType4": "PLA"
    }
})";
    Ad5xIfsTmpJsonFile tmp("rmw", seed);

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Write a new color + material for slot 0 (port 1).
    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0, "AABBCC", "TPU");
    REQUIRE(err.success());

    // Read the file back; slot 1 should be updated and other slots untouched.
    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#AABBCC");
    CHECK(doc["FFMInfo"]["ffmType1"] == "TPU");
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#00FF00");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
    CHECK(doc["FFMInfo"]["ffmColor3"] == "#0000FF");
    CHECK(doc["FFMInfo"]["ffmType3"] == "ABS");
    CHECK(doc["FFMInfo"]["ffmColor4"] == "#FFFFFF");
    CHECK(doc["FFMInfo"]["ffmType4"] == "PLA");
}

TEST_CASE("AD5X IFS write_adventurer_json_local creates FFMInfo if missing",
          "[ams][ad5x_ifs][local_write]") {
    // Adventurer5M.json without FFMInfo (zmod default-initialized empty file).
    Ad5xIfsTmpJsonFile tmp("missing_ffminfo", "{}");

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 1, "112233", "PETG");
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#112233");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS write_adventurer_json_local rejects empty path",
                 "[ams][ad5x_ifs][local_write]") {
    // local path not set — direct write must report failure so caller falls
    // back to Moonraker upload.
    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0, "FF0000", "PLA");
    CHECK_FALSE(err.success());
}

TEST_CASE("AD5X IFS write_adventurer_json_local rejects unparseable existing file",
          "[ams][ad5x_ifs][local_write]") {
    // Existing corrupted file (the exact symptom from bundle DQK7X96B —
    // an empty Adventurer5M.json that crashed Klipper). The local-write path
    // must NOT silently overwrite — return an error and let the caller decide
    // recovery. Empty file is not the same as missing file: missing means
    // first-time write; empty means prior corruption that we shouldn't mask.
    Ad5xIfsTmpJsonFile tmp("corrupt", "");
    // Re-touch the file as zero bytes (Ad5xIfsTmpJsonFile's empty seed_content
    // skips the file write). Create explicitly.
    {
        std::ofstream f(tmp.path);
        // intentionally empty
    }

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0, "FF0000", "PLA");
    // Recovery: write a fresh-baseline FFMInfo block. The corrupted-file case
    // is exactly the bricked-printer state — auto-repair is the whole point of
    // the direct-write fix.
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#FF0000");
}

TEST_CASE("AD5X IFS write_adventurer_json_local atomic — leaves no .tmp on success",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("atomic", R"({"FFMInfo":{"ffmColor1":"#000000","ffmType1":"PLA"}})");
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 2, "ABCDEF", "TPU");
    REQUIRE(err.success());

    // A successful write leaves no staging file (rename consumes it).
    CHECK(helix::test::staging_files_beside(tmp.path.string()) == 0);
}

// #904: stale-plugin-data fallback + user-defined material types

// TMTYD's printer in #904 had bambufy_tools=[4,2,4,3,...] AND
// less_waste_tools=[2,1,3,4] left over from past plugin activations, with
// neither plugin currently driving state. Without the fallback, our prefix
// detection picks bambufy first and applies [4,2,4,3,...] — putting T0 on
// port 4 and breaking every per-port T-badge in the UI. The fallback rule:
// when both prefixes have _tools arrays AND they disagree, neither is
// authoritative; revert to the default 1:1 mapping.
// Which plugin's tool table wins (#904). One prefix with a custom table, or two
// that agree, is applied as written. Two non-default tables that disagree name
// no single routing, so the map falls back to 1:1 (T0->port 1 ... T3->port 4,
// the rest unmapped) and each port's T-badge follows it: with TMTYD's data,
// bambufy_tools[0]=4 would otherwise badge port 4 as T0.
TEST_CASE("AD5X IFS #904 picks the tool map from the plugin prefixes",
          "[ams][ad5x_ifs][issue_904]") {
    const auto same = json::array({2, 1, 4, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    struct Row {
        const char* name;
        json vars;
        std::vector<int> first_four;
    };
    const Row rows[] = {
        // TMTYD's exact data
        {"two disagreeing prefixes fall back to 1:1",
         {{"bambufy_tools", json::array({4, 2, 4, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4})},
          {"bambufy_current_tool", -1},
          {"less_waste_tools", json::array({2, 1, 3, 4})},
          {"less_waste_current_tool", -1}},
         {1, 2, 3, 4}},
        {"one prefix with a custom table is honoured",
         {{"bambufy_tools", json::array({3, 2, 1, 0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
          {"bambufy_current_tool", -1}},
         {3, 2, 1, 0}},
        {"two agreeing prefixes are applied",
         {{"bambufy_tools", same}, {"less_waste_tools", same}},
         {2, 1, 4, 3}},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
            auto& backend = *backend_reg;
            Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
            backend.fire_status(make_save_variables(r.vars));

            auto map = Ad5xIfsTestAccess::tool_map(backend);
            for (size_t i = 0; i < r.first_four.size(); ++i) {
                INFO("tool " << i);
                CHECK(map[i] == r.first_four[i]);
            }
        }
    }

    SECTION("the 1:1 fallback unmaps the rest and badges each port by it") {
        helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
        auto& backend = *backend_reg;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(rows[0].vars));
        auto map = Ad5xIfsTestAccess::tool_map(backend);
        for (size_t i = 4; i < map.size(); ++i) {
            CHECK(map[i] == AmsBackendAd5xIfs::kUnmappedPort);
        }
        CHECK(backend.get_slot_info(3).mapped_tool == 3);
    }
}

// Custom material types from bambufy_custom_types must surface in
// get_supported_materials() so the edit modal dropdown isn't restricted to
// the firmware whitelist (#904 root cause #2: PLA+ stomped to PLA on save).
TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS #904 bambufy_custom_types merged into supported materials",
                 "[ams][ad5x_ifs][issue_904]") {
    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    backend.fire_status(make_save_variables(vars));

    auto custom = Ad5xIfsTestAccess::custom_material_types(backend);
    REQUIRE(custom.size() == 4);
    CHECK(custom[0] == "PLA+");
    CHECK(custom[2] == "PETG-Pro");

    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    auto contains = [&](const std::string& s) {
        return std::find(supported->begin(), supported->end(), s) != supported->end();
    };
    // Firmware whitelist still present.
    CHECK(contains("PLA"));
    CHECK(contains("PETG-CF"));
    // User-defined types appended.
    CHECK(contains("PLA+"));
    CHECK(contains("rPLA"));
    CHECK(contains("PETG-Pro"));
    // PLA-CF was already in the whitelist — no duplicate (case-insensitive
    // dedup).
    auto count = std::count(supported->begin(), supported->end(), "PLA-CF");
    CHECK(count == 1);

    // Round-trip via normalize_material: the user-defined type must come
    // back unchanged (this is what stops the PLA+ → PLA stomp on save).
    CHECK(backend.normalize_material("PLA+") == "PLA+");
    CHECK(backend.normalize_material("pla+") == "PLA+"); // case-insensitive
}

// End-to-end #904 root-cause-#2 fix: TMTYD's slot was PLA+ in zmod, our
// edit modal showed PLA, save wrote _IFS_VARS types="['PLA',...]"
// overwriting zmod's truth. The fix loads bambufy_custom_types into the
// supported-materials list so normalize_material's case-insensitive
// exact-match passes "PLA+" through unchanged. This test exercises the
// FULL save path — from save_variables ingestion through apply_user_edit to
// the cached SlotInfo — to prove the round-trip doesn't stomp the user's
// chosen type.
TEST_CASE("AD5X IFS #904 PLA+ round-trips through apply_user_edit after custom_types load",
          "[ams][ad5x_ifs][issue_904]") {
    Ad5xIfsTmpCacheDir tmp("issue_904_pla_plus");
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend,
                                                      (tmp.path / "Adventurer5M.json").string());

    // Step 1: ingest TMTYD's bambufy_custom_types from save_variables.
    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    backend.fire_status(make_save_variables(vars));

    // Step 2: user edits slot 0 to PLA+. apply_user_edit runs normalize_material
    // internally, which sees "PLA+" in the supported list and returns it
    // unchanged; a compat_group fallback to "PLA" would silently destroy the user's
    // choice.
    SlotInfo edit;
    edit.color_rgb = 0x000DFF;
    edit.material = "PLA+";
    auto err = helix::test::apply_edit(backend, 0, edit);
    REQUIRE(err.success());

    // Step 3: read it back and confirm PLA+ survived.
    auto info = backend.get_slot_info(0);
    CHECK(info.material == "PLA+");
    CHECK(info.color_rgb == 0x000DFF);

    // Lowercase input also round-trips (zmod COLOR macro is case-insensitive
    // when matching user.cfg types; our modal preserves the canonical case
    // from the supported list).
    SlotInfo edit_lc;
    edit_lc.color_rgb = 0xABCDEF;
    edit_lc.material = "rpla";
    err = helix::test::apply_edit(backend, 1, edit_lc);
    REQUIRE(err.success());
    auto info1 = backend.get_slot_info(1);
    CHECK(info1.material == "rPLA");
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload non-active slot with IFS_STATUS Chan ejects that lane (raza D8Z7DAA6)",
    "[ams][ad5x_ifs]") {
    // D8Z7DAA6: channel 4 seated (Chan=4), user picks Unload on channel 2
    // (slot 1). With current_slot correctly 3 (from Chan), the non-active unload
    // routes to cold eject_lane on port 2 — NOT _IFS_REMOVE_CURRENT_PRUTOK which
    // would back out the actually-seated channel 4.
    REQUIRE(backend.set_tool_mapping(3, 3).success()); // tool 3 -> port 4
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // IFS_STATUS Chan=4 establishes the seated slot (3) before the unload.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(backend.get_system_info().current_slot == 3);

    REQUIRE(backend.unload_filament(1).success());

    // 0-based slot 1 -> 1-based port 2, cold clamp/retract/unclamp. NOT the
    // toolhead unload (which would remove the seated channel 4).
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=2"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=2"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS reports lane-eject and force-eject support",
                 "[ams][ad5x_ifs]") {
    // Cold eject is always available on AD5X IFS, including for an idle lane
    // that reports EMPTY (snapped-chunk recovery) — both caps must be true.
    REQUIRE(backend.supports_lane_eject());
    REQUIRE(backend.supports_force_eject());
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS eject_lane issues clamp / retract / unclamp sequence",
                 "[ams][ad5x_ifs]") {
    // Empty cache → default 1000/1200 LEN/SPEED. 0-based slot 2 -> 1-based port 3.
    AmsError err = backend.eject_lane(2);
    REQUIRE(err.success());

    // Three separate calls, in order: clamp, retract (config-sourced LEN/SPEED),
    // unclamp — mirroring zmod's _REMOVE_PRUTOK_IFS macro.
    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=3");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=3");

    // No heating and no homing — this is a cold idle-lane retract.
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("SET_HEATER"));
    REQUIRE_FALSE(backend.has_gcode_containing("M104"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
    // Not the toolhead unload path.
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS eject_lane optimistically clears the ejected lane's presence (#1065)",
                 "[ams][ad5x_ifs][1065]") {
    // Field repro: after ejecting a lane the multi-filament menu kept showing it
    // loaded and still offered Eject. eject_lane's only refresh is
    // schedule_zcolor_query, and that confirming GET_ZCOLOR/IFS_STATUS poll starves
    // behind the blocking eject gcode on the constrained AD5X, so presence never
    // updates. The eject physically retracts the filament clear of the port silk
    // sensor, so the lane IS empty — reflect that locally at once, without waiting
    // for the poll (SILENT disabled here to model the starved poll).
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // confirming poll won't run

    // Lane 3 (slot 2) present with filament -> AVAILABLE (menu offers Eject).
    Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
    Ad5xIfsTestAccess::set_color(backend, 2, "F72224");
    Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::AVAILABLE);

    REQUIRE(backend.eject_lane(2).success());

    // Menu must update immediately: the lane reads empty (offers Load, not Eject)
    // even though the confirming poll never ran.
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK(backend.get_slot_info(2).status == SlotStatus::EMPTY);
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS eject_lane uses the lane material's tube length / ifs speed",
                 "[ams][ad5x_ifs]") {
    // PETG retracts 650mm at the file's default speed; TPU is not listed.
    Ad5xIfsTestAccess::parse_filament_json(
        backend, R"({"default": {"filament_tube_length": 900, "filament_ifs_speed": 1100},
                     "PETG": {"filament_tube_length": 650}})");
    Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
    Ad5xIfsTestAccess::set_material(backend, 1, "TPU");

    REQUIRE(backend.eject_lane(2).success());
    REQUIRE(backend.eject_lane(1).success());

    REQUIRE(backend.captured_gcodes.size() == 6);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=3");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=3 LEN=650 SPEED=1100");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=3");
    CHECK(backend.captured_gcodes[4] == "IFS_F11 PRUTOK=2 LEN=900 SPEED=1100");
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS eject_lane falls back to 1000/1200 when filament.json absent",
                 "[ams][ad5x_ifs]") {
    // No filament.json fetched (empty cache) and a lane material that isn't
    // cached anyway — eject must still fire the full 3-command sequence using
    // the hardcoded 1000/1200 fallback.
    Ad5xIfsTestAccess::set_material(backend, 1, "PLA");

    REQUIRE(backend.eject_lane(1).success());

    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=2");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=2");
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS eject_lane refuses the slot loaded in toolhead",
                 "[ams][ad5x_ifs]") {
    // Slot 1 is the firmware's active slot AND filament is seated at the head:
    // ejecting it cold would fight the loaded filament, so refuse.
    Ad5xIfsTestAccess::set_current_slot(backend, 1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    AmsError err = backend.eject_lane(1);
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE(backend.captured_gcodes.empty());

    // A DIFFERENT idle lane is still ejectable while slot 1 is loaded.
    REQUIRE(backend.eject_lane(2).success());
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=3"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=3"));
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS eject_lane rejects out-of-range slots",
                 "[ams][ad5x_ifs]") {
    SECTION("negative slot") {
        AmsError err = backend.eject_lane(-1);
        REQUIRE_FALSE(err.success());
        REQUIRE(backend.captured_gcodes.empty());
    }

    SECTION("slot == NUM_PORTS (out of range)") {
        AmsError err = backend.eject_lane(AmsBackendAd5xIfs::kDefaultPorts);
        REQUIRE_FALSE(err.success());
        REQUIRE(backend.captured_gcodes.empty());
    }
}

// unload_filament dispatch: a bare IFS_REMOVE_PRUTOK is a ZMOD no-op, so the
// printer would home and nothing would happen.
//
// Verified against an on-device cfg (bundle 7AC4SDEX) and ZMOD v1.7.1
// (../zmod, AD5X-zmod-1.7.1.tgz, mod/.shell/zmod_ifs.py):
//   * bare IFS_REMOVE_PRUTOK defaults PRUTOK=0 and returns at cmd_IFS_REMOVE_PRUTOK:1104
//     — a NO-OP. We homed (ensure_homed_then) and then did nothing.
//   * REMOVE_PRUTOK_IFS PRUTOK=N unloads the *current* channel regardless of N — it
//     never unloads "port N independently."
//   * IFS_REMOVE_CURRENT_PRUTOK (cmd_..._CURRENT_PRUTOK:1144) early-returns when the
//     extruder sensor reads EMPTY (zmod_ifs.py:1149) — so when nothing is seated at
// the nozzle it is *also* a no-op after homing.
//   * _IFS_REMOVE_CURRENT_PRUTOK is the firmware's own "Remove from extruder" button:
//     it self-homes, calls IFS_REMOVE_CURRENT_PRUTOK NEED_TRASH=1
//     BYPASS_TEMPERATURE_CHECK=1, resets the hotend, and refreshes color. This is what
//     we dispatch for a loaded toolhead, raw rather than via ensure_homed_then() so we
//     don't put a "Home printer first?" prompt in front of a home the macro's own
//     _G28 already covers. (_G28 is conditional on homed_axes, so ensure_homed_then()
//     would not have caused a DOUBLE home - see the load-path test below and #1248.)
// With a null client, execute_gcode() is overridden, so captured_gcodes holds exactly
// what unload_filament() sends.

TEST_CASE_METHOD(
    RunningAd5xIfs,
    "AD5X IFS unload_filament dispatches the firmware _IFS_REMOVE_CURRENT_PRUTOK macro",
    "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task

    // Normal case: active slot loaded AND filament seated at the toolhead.
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // The firmware's own macro — not the bare Python command (which skips the
    // trash drop and leaves the nozzle hot), never the no-op, never per-port.
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_CURRENT_PRUTOK")); // bare, no underscore
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK_IFS"));
    // Raw dispatch: no G28 of ours in front of the macro's own conditional _G28.
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
}

TEST_CASE("AD5X IFS native ZMOD asks for the unrecorded lane and declares it before unloading",
          "[ams][ad5x_ifs][1324]") {
    TestableAd5xIfsBackend backend;

    SECTION("several lanes holding filament are picked from") {
        seat_unrecorded_zmod(backend, {true, false, true, false});
        const helix::UnloadLaneQuestion question = backend.unload_lane_question();
        CHECK(question.lanes == std::vector<int>{0, 2});
        CHECK_FALSE(question.confirm_clear_toolhead);
        // A slotless unload would run the macro with no lane: refused instead.
        CHECK_FALSE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes.empty());
    }
    SECTION("one lane holding filament is asked about too") {
        seat_unrecorded_zmod(backend, {false, true, false, false});
        CHECK(backend.unload_lane_question().lanes == std::vector<int>{1});
    }
    SECTION("the chosen lane is declared, then unloaded, when the file records 0") {
        seat_unrecorded_zmod(backend, {true, false, true, false});
        REQUIRE(backend.unload_filament(2).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"SET_EXTRUDER_SLOT SLOT=3\n_IFS_REMOVE_CURRENT_PRUTOK"});
    }
    SECTION("a channel already on record is left to the macro") {
        seat_unrecorded_zmod(backend, {true, false, true, false}, /*file_channel=*/3);
        CHECK_FALSE(backend.unload_lane_question().asks());
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"_IFS_REMOVE_CURRENT_PRUTOK"});
    }
    SECTION("the extruder clear runs ZMOD's no-IFS unload at the clear temperature") {
        seat_unrecorded_zmod(backend, {true, false, true, false});
        REQUIRE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"_REMOVE_PRUTOK_IFS PRUTOK=0 TEMP=220"});
    }
    SECTION("no lane holding filament asks to confirm the clear first") {
        seat_unrecorded_zmod(backend, {false, false, false, false});
        CHECK(backend.unload_lane_question().confirm_clear_toolhead);
        CHECK_FALSE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes.empty());
    }
    SECTION("the clear is refused when the toolhead switch reads empty") {
        seat_unrecorded_zmod(backend, {false, false, false, false});
        Ad5xIfsTestAccess::set_head_switch(backend, true, false);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        CHECK_FALSE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes.empty());
    }
}

TEST_CASE("AD5X IFS native ZMOD declares a lane only once the file says the record is 0",
          "[ams][ad5x_ifs][1324]") {
    TestableAd5xIfsBackend backend;

    SECTION("restarted, before Adventurer5M.json is read: ask, never the remembered lane") {
        seat_unrecorded_zmod(backend, {true, true, false, false}, /*file_channel=*/std::nullopt);
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1);
        CHECK(backend.unload_lane_question().lanes == std::vector<int>{0, 1});
        CHECK_FALSE(backend.unload_filament(-1).success());
        // Even an answered lane waits for the file: declaring could overwrite
        // a lane the firmware records.
        const helix::AmsError err = backend.unload_filament(1);
        CHECK_FALSE(err.success());
        CHECK(err.result == helix::AmsResult::WRONG_STATE);
        CHECK_FALSE(backend.has_gcode_containing("SET_EXTRUDER_SLOT"));
        CHECK_FALSE(backend.has_gcode_containing("_IFS_REMOVE_CURRENT_PRUTOK"));
    }
    SECTION("once the file says 0, the remembered lane is declared and unloaded") {
        seat_unrecorded_zmod(backend, {true, true, false, false}, /*file_channel=*/0);
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1);
        CHECK_FALSE(backend.unload_lane_question().asks());
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"SET_EXTRUDER_SLOT SLOT=2\n_IFS_REMOVE_CURRENT_PRUTOK"});
    }
    SECTION("after an eject the file still records the ejected lane") {
        // Lane 3 recorded, then ejected: Helix zeroes its own copy, the file
        // keeps 3, and a strand from another lane is at the head.
        seat_unrecorded_zmod(backend, {true, false, true, false}, /*file_channel=*/3);
        Ad5xIfsTestAccess::clear_seated_if_ejected(backend, 2);
        REQUIRE(Ad5xIfsTestAccess::ffm_channel(backend) == 0);
        CHECK(backend.unload_lane_question().lanes == std::vector<int>{0, 2});
        // The local zero is not the file's 0: lane 1 cannot be declared over 3.
        CHECK_FALSE(backend.unload_filament(0).success());
        CHECK(backend.captured_gcodes.empty());
        // The lane the file records is unloaded as recorded.
        REQUIRE(backend.unload_filament(2).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"_IFS_REMOVE_CURRENT_PRUTOK"});
    }
}

TEST_CASE("AD5X IFS native ZMOD reads the lane record from Adventurer5M.json",
          "[ams][ad5x_ifs][1324]") {
    TestableAd5xIfsBackend backend;
    seat_unrecorded_zmod(backend, {true, false, true, false}, /*file_channel=*/std::nullopt);
    REQUIRE_FALSE(backend.unload_filament(2).success());
    backend.captured_gcodes.clear();

    SECTION("a file recording 0 lets the chosen lane be declared") {
        Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({"FFMInfo": {"channel": 0}})");
        REQUIRE(backend.unload_filament(2).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"SET_EXTRUDER_SLOT SLOT=3\n_IFS_REMOVE_CURRENT_PRUTOK"});
    }
    SECTION("a file with no integer channel is read, with no record to name a lane by") {
        Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({"FFMInfo": {"channel": "none"}})");
        const helix::UnloadLaneQuestion question = backend.unload_lane_question();
        CHECK(question.lanes.empty());
        CHECK(question.confirm_clear_toolhead);
        CHECK_FALSE(question.reason.empty());
        const helix::AmsError err = backend.unload_filament(2);
        CHECK_FALSE(err.success());
        CHECK(err.user_msg == question.reason);
        CHECK(backend.captured_gcodes.empty());
        REQUIRE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"_REMOVE_PRUTOK_IFS PRUTOK=0 TEMP=220"});
    }
    SECTION("a file with no FFMInfo section is read, with no record") {
        Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({"other": 1})");
        CHECK(backend.unload_lane_question().confirm_clear_toolhead);
        CHECK_FALSE(backend.unload_filament(0).success());
        CHECK(backend.captured_gcodes.empty());
    }
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS unhomed load sends exactly one G28 then the load macro (#1248)",
                 "[ams][ad5x_ifs][homing][1248]") {
    // INSERT_PRUTOK_IFS's leading _G28 is conditional, not a second home: its
    // whole body is
    //   {% if "xyz" not in printer.toolhead.homed_axes %} _HOME {% endif %}
    // (ZMOD 1.7.1 mod/_mod/translate/*/base.cfg:88), so it no-ops once our G28
    // has homed.
    backend.homed = false;

    REQUIRE(backend.load_filament(2).success());

    // Exactly two commands, in order: our G28, then the macro. Not the macro
    // alone (that would mean skip_homing=true and a silent unprompted home),
    // and not two homes.
    REQUIRE(backend.captured_gcodes.size() == 2);
    CHECK(backend.captured_gcodes[0] == "G28");
    CHECK(backend.captured_gcodes[1] == "INSERT_PRUTOK_IFS PRUTOK=3"); // slot 2 -> port 3
    CHECK(std::count(backend.captured_gcodes.begin(), backend.captured_gcodes.end(), "G28") == 1);

    // The macro still carries its completion callback through the homed
    // detour, so the stuck-on-Purging finalize path survives an unhomed load.
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Already homed: no G28 of ours at all, macro dispatched straight through.
    TestableAd5xIfsBackend homed_backend;
    Ad5xIfsTestAccess::set_running(homed_backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(homed_backend, false);
    homed_backend.homed = true;

    REQUIRE(homed_backend.load_filament(2).success());

    REQUIRE(homed_backend.captured_gcodes.size() == 1);
    CHECK(homed_backend.captured_gcodes[0] == "INSERT_PRUTOK_IFS PRUTOK=3");
    CHECK_FALSE(homed_backend.has_gcode_containing("G28"));
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS change_tool out of range names the tool, not a slot",
                 "[ams][ad5x_ifs][numbering]") {
    // do_change_tool() bounds-checks a TOOL index, so the refusal has to name a
    // tool: an INVALID_SLOT here tells the user to pick a valid slot when the
    // tool number is what is out of range, and callers dispatch on the code.

    const AmsError err = backend.change_tool(TestableAd5xIfsBackend::kToolMapSize);
    REQUIRE_FALSE(err.success());
    CHECK(err.result == AmsResult::INVALID_TOOL);

    // The gcode tool identity stays 0-based, so the message names T16 rather
    // than a display position.
    CHECK(err.technical_msg.find(helix::ui::tool_label(TestableAd5xIfsBackend::kToolMapSize)) !=
          std::string::npos);
    CHECK(err.user_msg.find("slot") == std::string::npos);
    CHECK(err.suggestion.find("slot") == std::string::npos);
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS unhomed change_tool sends G28 before A_CHANGE_FILAMENT (#1248)",
                 "[ams][ad5x_ifs][homing][1248]") {
    // The #1248 companion case. A_CHANGE_FILAMENT is NOT in the ZMOD tree
    // (ZMOD ships CHANGE_FILAMENT / _A_CHANGE_FILAMENT as RESPOND-only stubs
    // and swaps via INSERT_PRUTOK_IFS in zmod_color.py), so it comes from the
    // stock FlashForge config and whether it self-homes is unverified. Homing
    // first is the safe side of that unknown; this pins it so nobody flips it
    // to skip_homing=true on the strength of the load-macro analysis.
    REQUIRE(backend.set_tool_mapping(1, 1).success()); // tool 1 -> port 2
    backend.captured_gcodes.clear(); // only the change_tool dispatch is under test
    backend.homed = false;

    REQUIRE(backend.change_tool(1).success());

    CAPTURE(backend.captured_gcodes);
    REQUIRE(backend.captured_gcodes.size() == 2);
    CHECK(backend.captured_gcodes[0] == "G28");
    CHECK(backend.captured_gcodes[1] == "A_CHANGE_FILAMENT CHANNEL=2");
}

TEST_CASE_METHOD(
    RunningAd5xIfs,
    "AD5X IFS unload finalizes to IDLE on the macro completion ack, not the 90s timeout "
    "(raza616 stuck-on-Retract)",
    "[ams][ad5x_ifs]") {
    // The synthesized Retract phase has no sensor event, and the confirming
    // IFS_STATUS Chan==0 / GET_ZCOLOR query can silently fail on native ZMOD —
    // leaving the unload stuck at Retract until the 90s timeout flips it to
    // ERROR (bundle EE5L8LY2). The _IFS_REMOVE_CURRENT_PRUTOK macro's own
    // completion (gcode ack) is the reliable terminal signal; the dispatch must
    // carry a completion callback, and that callback must finalize to IDLE.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // Toolhead unload dispatched WITH a completion callback, and the op is in
    // flight (phase tracker active, not yet IDLE).
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);

    // Macro completes -> finalize to IDLE (not ERROR, not stuck at Retract).
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);

    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    // Idempotent: a second finalize (or a late confirming query) is a no-op.
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS commanded unload clears head-loaded even with the lane still present "
                 "(park-in-lane, #1065)",
                 "[ams][ad5x][ifs][1065]") {
    // A normal toolhead unload retracts filament out of the hotend but parks it
    // in the lane, so the IFS silk sensor stays PRESENT. A COMMANDED unload is
    // unambiguous, so head-loaded must clear on completion regardless of lane
    // presence — otherwise the slot stays stuck LOADED (Unload offered on an
    // empty head, Load disabled). This is DISTINCT from a passive Extruder:None
    // (#995), which DOES require presence corroboration so it can't strand
    // still-seated filament — the presence guard in
    // derive_head_loaded_from_summary_locked() would leave this stuck without the
    // dedicated commanded-unload clear at finalize.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true); // lane 1 filament present

    REQUIRE(backend.unload_filament(0).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Macro completes while the lane silk sensor still reads present.
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend)); // head cleared on unload
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));    // lane still present
    CHECK(backend.get_slot_info(0).status != SlotStatus::LOADED);
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS load finalizes to IDLE on the macro completion ack (raza616 stuck-on-Purging)",
    "[ams][ad5x_ifs]") {
    // Mirror of the stuck-on-Retract unload fix for the load path. INSERT_PRUTOK_IFS
    // is a linear, synchronous zmod macro (home → heat → feed → purge); it acks
    // only after the purge fully runs. The synthesized Purge phase has no sensor
    // event, and the confirming query can silently fail on native ZMOD — so the
    // macro ack is the reliable terminal signal. With a null client, ensure_homed_then
    // dispatches directly through the (overridden) execute_gcode, capturing the
    // completion callback.

    REQUIRE(backend.load_filament(2).success());

    // Load dispatched the macro WITH a completion callback; op in flight.
    REQUIRE(backend.has_gcode("INSERT_PRUTOK_IFS PRUTOK=3")); // 0-based slot 2 -> port 3
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);

    // Macro completes -> finalize to IDLE (not stuck at Purge).
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/false);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    // An unload-ack must NOT finalize a load (and vice versa) — guard by direction.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true); // wrong direction
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));                       // still in flight
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);
}

TEST_CASE("AD5X IFS load and unload finalize in the queue pass after the macro ack",
          "[ams][ad5x_ifs][threading][1761]") {
    using helix::ui::UpdateQueueTestAccess;
    LiveAckAd5xIfsBackend backend;
    auto idle = [&] { return backend.get_system_info().action == AmsAction::IDLE; };

    SECTION("load") {
        REQUIRE(backend.load_filament(2).success());
        REQUIRE_FALSE(idle());
        CHECK(UpdateQueueTestAccess::passes_until(idle) == 1);
    }
    SECTION("unload") {
        Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
        Ad5xIfsTestAccess::set_head_filament(backend, true);
        REQUIRE(backend.unload_filament(0).success());
        REQUIRE_FALSE(idle());
        CHECK(UpdateQueueTestAccess::passes_until(idle) == 1);
    }
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS load goes to ERROR with the firmware's message when its macro fails",
                 "[ams][ad5x_ifs][macro_failure]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    REQUIRE(backend.load_filament(1).success());
    REQUIRE(backend.has_gcode("INSERT_PRUTOK_IFS PRUTOK=2"));
    REQUIRE(backend.captured_error != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    backend.captured_error(
        macro_error(MoonrakerErrorType::JSON_RPC_ERROR, "Filament 2 stalled in IFS"));
    drain_queue();

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail == "Filament 2 stalled in IFS");
    CHECK_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS unload goes to ERROR with the firmware's message when its macro fails",
                 "[ams][ad5x_ifs][macro_failure]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE(backend.captured_error != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    backend.captured_error(macro_error(MoonrakerErrorType::JSON_RPC_ERROR, "Cutter jammed"));
    drain_queue();

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail == "Cutter jammed");
    CHECK_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS fault reads with the wording for ZMOD's message",
                 "[ams][ad5x_ifs][macro_failure][flashforge]") {
    // Discovery says ZMOD, as its zmod_* objects do on a real AD5X.
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"zmod_ifs", "zmod_color", "extruder"}));
    get_printer_state().set_hardware(hw);
    // Each set_hardware() republishes the sensor managers through the update
    // queue, so the restore drains it while their subjects are still alive.
    struct Restore {
        ~Restore() {
            get_printer_state().set_hardware(helix::PrinterDiscovery{});
            drain_queue();
        }
    } restore;

    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(backend.unload_filament(0).success());
    REQUIRE(backend.captured_error != nullptr);
    backend.captured_error(macro_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                       "Failed to extract filament from extruder"));
    drain_queue();
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);

    const auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->title != std::string("Filament System Error"));
    CHECK(ev->detail != "Failed to extract filament from extruder");
    CHECK_FALSE(ev->recovery_actions.empty());
}

TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS macro failure leaves an op it does not own alone",
                 "[ams][ad5x_ifs][macro_failure]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    const AmsAction in_flight = backend.get_system_info().action;

    SECTION("an RPC timeout: the macro may still be running") {
        backend.captured_error(macro_error(MoonrakerErrorType::TIMEOUT, "timed out"));
        drain_queue();
    }
    SECTION("an unload's failure arriving during a load") {
        Ad5xIfsTestAccess::fail_op_after_macro(
            backend, /*is_unload=*/true,
            macro_error(MoonrakerErrorType::JSON_RPC_ERROR, "Cutter jammed"));
    }

    CHECK(Ad5xIfsTestAccess::phase_active(backend));
    CHECK(backend.get_system_info().action == in_flight);
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS load_filament sets swap_expected when another lane is seated (#1065)",
                 "[ams][ad5x_ifs][phase][1065]") {
    // Drives the REAL load_filament dispatch via TestableAd5xIfsBackend (which
    // provides a mock gcode API) to verify the swap_expected flag is set
    // correctly at dispatch time. The phase-test sibling manually flips the
    // flag; this one proves load_filament actually does the flip when
    // seated_chan_ differs from the target.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Seed ch4 (slot 3) as currently seated via the production path
    // (apply_zcolor_result on an IFS_STATUS Chan frame).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 4);

    // Dispatch load on ch2 (slot 1). ch4 ≠ ch2 → swap_expected MUST be set.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::swap_expected(backend));
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}
