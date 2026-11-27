// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 77 to 115 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs], [general], [ad5x_ifs][1065], [ad5x_ifs][phase], [ad5x][ifs][1065], and more

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

// parse_save_variables does not read color/type from <prefix>_colors /
// <prefix>_types, nor infer port_presence from color emptiness: CHANGE_ZCOLOR /
// GET_ZCOLOR are the sole color/type source (lessWaste/bambufy save_variables
// don't reflect zmod's authoritative state).

// can_unload_from_toolhead — #995: active slot stays unloadable after runout
//
// A filament runout clears the head sensor, dropping the display status of the
// firmware's active slot below LOADED. Unload is exactly the recovery the user
// needs at that moment, so the action gate is decoupled from the head-sensor-
// derived display status: the active slot stays unloadable while the spool
// still renders empty/available.

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS can_unload_from_toolhead keeps active slot unloadable after runout",
                 "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Load slot 0: active tool T0 (→ port 1 → slot 0), port + head sensors set.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);
    }
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.can_unload_from_toolhead(0));

    // Runout: head sensor clears and the lane sensor empties, but the firmware
    // still reports slot 0 as the active/current slot.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", false}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        backend.fire_status(notification);
    }
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // Display status is not LOADED — the spool renders empty/available...
    REQUIRE(backend.get_slot_info(0).status != SlotStatus::LOADED);
    // ...but the action gate keeps the active slot unloadable (#995).
    REQUIRE(backend.can_unload_from_toolhead(0));

    // An inactive, empty slot is NOT unloadable.
    REQUIRE_FALSE(backend.can_unload_from_toolhead(2));

    // Out-of-range indices are never unloadable. The negative case exercises the
    // slot_index >= 0 guard: current_slot is 0 here, so -1 must NOT match it via
    // the active-slot short-circuit — and the base LOADED check rejects it too.
    REQUIRE_FALSE(backend.can_unload_from_toolhead(-1));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(AmsBackendAd5xIfs::kDefaultPorts));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS can_unload_from_toolhead with no active slot is never unloadable",
                 "[ams][ad5x_ifs]") {
    // With no filament loaded, current_slot is -1. The slot_index >= 0 guard must
    // prevent a caller passing -1 (or 0) from matching the -1 active-slot
    // sentinel; every slot then falls through to the base LOADED check, which is
    // false for an unloaded backend.

    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;
    backend.fire_status(make_save_variables(vars));
    REQUIRE(backend.get_system_info().current_slot == -1);

    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(-1));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS toolhead filament unloadable when firmware drops active slot (#995)",
                 "[ams][ad5x_ifs]") {
    // Stock-ZMOD recovery scenario: after a runout or print-end the firmware can
    // emit "Extruder: None" and drop its active-slot pointer to current_slot==-1
    // while filament is STILL seated in the toolhead (head sensor true). The
    // active-slot short-circuit can never fire (nothing matches -1), so the only
    // signal that there is removable filament is the head sensor itself.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    // Native ZMOD: no per-port sensors.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // No active tool (current_slot == -1) but filament present at the head.
    {
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = -1;
        json notification;
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);
    }
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // The gate must open even though no slot is the active slot — filament is
    // physically in the toolhead and must be removable. (The dispatched command,
    // the _IFS_REMOVE_CURRENT_PRUTOK macro, is asserted by the unload_filament
    // dispatch test; the firmware reads FFMInfo.channel, not our slot index, so
    // the unknown origin lane is fine.)
    REQUIRE(backend.can_unload_from_toolhead(0));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS no toolhead filament leaves slot non-unloadable (#995 regression)",
                 "[ams][ad5x_ifs]") {
    // The mirror of the recovery case: no filament anywhere (head sensor false,
    // current_slot == -1) must keep Unload disabled. The head_filament gate must
    // not open when there is nothing seated in the toolhead.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    {
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = -1;
        json notification;
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        backend.fire_status(notification);
    }
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS runout does not flip active slot display status to LOADED",
                 "[ams][ad5x_ifs]") {
    // Decoupling the unload gate must NOT alter display status.
    // Native ZMOD path (no per-port sensors), active slot, head sensor clear —
    // the slot must still report a non-LOADED status so the spool renders
    // empty/available.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    seed_standard_colors(backend);

    // Motion sensor present briefly to establish active slot via head detection,
    // then cleared to simulate the runout.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_motion_sensor ifs_motion_sensor"] =
            json{{"filament_detected", false}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        backend.fire_status(notification);
    }

    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(backend.get_slot_info(0).status != SlotStatus::LOADED);
    // Gate stays open for the active slot.
    REQUIRE(backend.can_unload_from_toolhead(0));
}

// IFS_STATUS Chan as seated-channel authority (2026-06-15 plan)
//
// zmod's IFS_STATUS macro emits one clean-JSON line via respond_info carrying
// the current engaged port in "Chan" (1-based, 0 = none). Unlike the
// GET_ZCOLOR "Extruder:" feed line — which reads "None (N)" while loaded-idle —
// Chan persists at the seated port. apply_zcolor_result must derive
// active_tool_/current_slot from it even on prompt-fallback (old zmod where
// GET_ZCOLOR degrades to a dialog).

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS IFS_STATUS Chan is seated authority over stale Extruder: None",
                 "[ams][ad5x_ifs]") {
    // The field defect: GET_ZCOLOR says "Extruder: None (4)" (idle-extruder view)
    // while port 4 is physically seated. IFS_STATUS Chan=4 must win, so the active
    // slot is 3 (port 4) — making Unload available ONLY on slot 3 and Eject on the
    // rest, not Unload-everywhere via the current_slot<0 recovery branch.

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Filament physically seated at the head (RS-485 reports true).
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // GET_ZCOLOR reports "Extruder: None (4)" (no active feed) but IFS_STATUS
    // Chan=4 is the seated channel.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 4; // stale "(4)" — never used
    // extruder_slot deliberately absent (the "None" feed view).
    r.ifs_chan = 4; // 1-based seated channel from IFS_STATUS

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // current_slot recomputed immediately to port 4 -> slot 3.
    REQUIRE(backend.get_system_info().current_slot == 3);
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 3);

    // Unload is offered ONLY on the seated slot; the rest fall through to the
    // base capability (false), so the UI shows Eject on them instead.
    REQUIRE(backend.can_unload_from_toolhead(3));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(1));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS cold-lane eject does not pollute seated channel (#1065 Bug 3)",
                 "[ams][ad5x_ifs]") {
    // Field repro (bundle CGR6C7PA + state table): a lane is loaded to the
    // toolhead, then the user ejects a DIFFERENT, cold lane. The firmware's
    // switching mechanism engages the ejected port, so the follow-up IFS_STATUS
    // reports that port as "Chan" even though it is now EMPTY (its Ports[] bit drops
    // to false). The capture shows, after ejecting channel 1, `Chan: 1` with
    // `Ports: [false, true, true, true]`. Trusting that stale Chan would reclassify
    // the genuinely-seated lane as a cold lane, so the loaded lane would offer Eject
    // and a tap would cold-grind seated filament.
    //
    // The seated channel must only follow a Chan whose lane actually has filament.

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Channel 2 seated at the toolhead, all four lanes present.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Baseline: ch2 (slot 1) is seated -> Unload; ch1 (slot 0) is cold -> Eject.
    REQUIRE(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    REQUIRE_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));

    // Eject cold lane 1. Its post-eject IFS_STATUS: Chan=1 (last-engaged channel)
    // but port 1 now empty. seated_chan_ must NOT move to channel 1.
    AmsBackendAd5xIfs::ZColorSilentResult after_eject;
    after_eject.saw_valid_response = true;
    after_eject.ifs_active = true;
    after_eject.ifs_chan = 1; // stale: engaged-but-now-empty lane
    after_eject.ifs_ports = std::vector<bool>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, after_eject);

    // Channel 2 is still the seated lane -> must still route to the heated
    // toolhead Unload, NOT a cold Eject. Channel 1, now empty, stays cold-eject.
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS dialog slot-select does not steal the seated channel (#1065 Bug 3)",
                 "[ams][ad5x_ifs][1065]") {
    // Field repro (bundle ZT8Y9WPM + state table): channel 2 is physically seated at
    // the toolhead (Adventurer5M.json FFMInfo.channel=2). The user opens the zmod
    // COLOR menu and merely SELECTS a different lane to edit its colour/type
    // (RUN_ZCOLOR SLOT=3 / CHANGE_ZCOLOR SLOT=3) — no filament moves. IFS_STATUS then
    // reports "Chan": 3 (the switching mechanism tracks the last lane the dialog
    // referenced) while all four ports stay PRESENT, so the eject-stale guard (which
    // only rejects a Chan pointing at an EMPTY lane) does not fire. Adopting it would
    // move current_slot 1 -> 2 (channel 2 -> channel 3) with zero physical motion, so
    // the genuinely-seated lane 2 would offer Eject and lane 3 wrongly offer Unload.
    //
    // The seated authority is FFMInfo.channel (which stays 2 the whole time), NOT
    // IFS_STATUS Chan. A Chan that diverges from a known FFMInfo.channel must not
    // move the seated lane. (The GET_ZCOLOR "// Extruder:" line reads "None (3)"
    // while loaded-idle on this firmware — its paren also chases the dialog — so it
    // is NOT the authority; verified against the raw bundle.)

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    Ad5xIfsTestAccess::set_head_filament(backend, true);
    // Firmware's own seated record: channel 2 (as parse_adventurer_json would set
    // from Adventurer5M.json FFMInfo.channel).
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);

    // Channel 2 seated: IFS_STATUS Chan=2 agrees with FFMInfo.channel, all present.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Baseline: ch2 (slot 1) seated -> Unload; ch3 (slot 2) cold -> Eject.
    REQUIRE(backend.get_system_info().current_slot == 1);
    REQUIRE(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    REQUIRE_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));

    // Dialog selects slot 3 to edit. FFMInfo.channel UNCHANGED (still 2 — no
    // filament moved), but IFS_STATUS now reports Chan=3 with lane 3 still present.
    AmsBackendAd5xIfs::ZColorSilentResult dialog_select;
    dialog_select.saw_valid_response = true;
    dialog_select.ifs_active = true;
    dialog_select.ifs_chan = 3; // dialog-tracked, NOT a physical seating
    dialog_select.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, dialog_select);

    // Seated lane must still be channel 2. Lane 2 offers Unload; lane 3 stays cold.
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS seated channel follows a moved FFMInfo.channel despite a stale Chan "
                 "(#1065 Bug 3)",
                 "[ams][ad5x_ifs][1065]") {
    // Counterpart to the dialog-select test: the FFMInfo.channel authority must NOT
    // freeze the seated lane. When the firmware genuinely moves filament to a new
    // lane, FFMInfo.channel updates, and the seated lane must follow it — even if
    // IFS_STATUS Chan still carries a stale value from an earlier dialog reference
    // (loading a new channel correctly moves current_slot). This is what proves the
    // seated lane tracks real head changes rather than pinning to the first-seen
    // lane.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Channel 2 seated (FFMInfo.channel=2, Chan 2).
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Real load of lane 1: firmware moves FFMInfo.channel to 1. Chan still reads a
    // stale 3 from an earlier dialog touch. Seated must follow FFMInfo.channel to
    // channel 1, NOT the stale Chan.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 1);
    AmsBackendAd5xIfs::ZColorSilentResult moved;
    moved.saw_valid_response = true;
    moved.ifs_active = true;
    moved.ifs_chan = 3; // stale dialog value, must be ignored
    moved.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, moved);

    // Seated followed FFMInfo.channel to channel 1 (slot 0).
    CHECK(backend.get_system_info().current_slot == 0);
    CHECK(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS parse_adventurer_json reads FFMInfo.channel as the seated lane (#1065 Bug 3)",
    "[ams][ad5x_ifs][1065]") {
    // The seated authority must actually be wired from the file. parse_adventurer_json
    // must read FFMInfo.channel and set the seated lane (recomputing current_slot),
    // so a plain file poll — with no IFS_STATUS frame at all — establishes which lane
    // is seated. FFMInfo.channel is 1-based (channel 2 -> slot 1).
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Minimal Adventurer5M.json: FFMInfo with the seated channel + per-port
    // colour/type (firmware writes channel 2 as seated).
    const std::string json =
        R"({"FFMInfo":{"channel":2,)"
        R"("ffmColor1":"#161616","ffmColor2":"#FFFFFF",)"
        R"("ffmColor3":"#F72224","ffmColor4":"#898989",)"
        R"("ffmType1":"PETG","ffmType2":"PETG","ffmType3":"PETG","ffmType4":"PETG"}})";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, json);

    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1); // channel 2 -> 0-based slot 1
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

// #1065 "row 28": sticky FFMInfo.channel + head-switch gate on the seated lane
//
// The firmware never blanks FFMInfo.channel / IFS_STATUS Chan on eject (same
// stickiness as ffmColor/ffmType). After ejecting a cold lane whose stale
// FFMInfo.channel still points at it, an un-gated adoption re-seats that empty
// lane, so it keeps offering Unload ("shows loaded"). The fix gates adoption on
// the toolhead SWITCH sensor: adopt only when it corroborates filament at the
// head; clear when it authoritatively reads empty. The switch (not the
// loaded-idle-false-negating motion sensor) is the authority, so a genuinely
// seated lane is never dropped, and motion-only firmware falls back to the old
// behaviour.

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS stale FFMInfo.channel is dropped when the head switch reads empty "
                 "(#1065 row 28)",
                 "[ams][ad5x_ifs][1065]") {
    // Post-eject re-poll: a cold lane 3 was ejected but FFMInfo.channel is still 3
    // (firmware kept it) and the toolhead switch reads EMPTY (nothing seated). The
    // follow-up IFS_STATUS carries the sticky Chan=3 with port 3 now absent.
    // A stale current_slot move to 2 would make lane 3 offer Unload ("shows loaded").
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Toolhead switch authoritatively reads empty (drives head_switch_seen_ +
    // head_switch_present_=false + head_filament_=false).
    backend.fire_status(make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_present(backend));

    // Firmware's sticky record still points at lane 3.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 3);

    // Post-eject IFS_STATUS: Chan=3 (last-engaged, sticky), port 3 empty.
    AmsBackendAd5xIfs::ZColorSilentResult after_eject;
    after_eject.saw_valid_response = true;
    after_eject.ifs_active = true;
    after_eject.ifs_chan = 3;
    after_eject.ifs_ports = std::vector<bool>{true, true, false, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, after_eject);

    // No lane is seated: the empty head switch rejects the stale channel.
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
    CHECK_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS FFMInfo.channel poll is not adopted while the head switch is empty "
                 "(#1065 row 28)",
                 "[ams][ad5x_ifs][1065]") {
    // Same gate on the plain file-poll path (parse_adventurer_json): a stale
    // FFMInfo.channel=3 with the toolhead switch empty must not seat lane 3.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    backend.fire_status(make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));

    const std::string json =
        R"({"FFMInfo":{"channel":3,)"
        R"("ffmColor1":"#161616","ffmColor2":"#FFFFFF",)"
        R"("ffmColor3":"#F72224","ffmColor4":"#898989",)"
        R"("ffmType1":"PETG","ffmType2":"PETG","ffmType3":"PETG","ffmType4":"PETG"}})";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, json);

    // ffm_channel_ reflects the file (3), but it is NOT adopted as seated.
    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 3);
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
    CHECK_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS loaded-idle lane stays seated when the head switch is present but motion "
    "reads empty (#1065 row 28)",
    "[ams][ad5x_ifs][1065]") {
    // Motion-false-negative guard. A lane is
    // genuinely loaded (head SWITCH present=true), but the ifs_motion_sensor reads
    // filament_detected=false while idle (device-confirmed). That motion frame
    // clobbers the conflated head_filament_ to false — but the switch authority
    // still says present, so the head-gate must NOT drop the seated lane.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Lane 2 genuinely seated: switch reports present.
    backend.fire_status(make_head_sensor(true));
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Idle motion frame reads empty and clobbers head_filament_ to false — but the
    // switch's own last reading (present) is untouched.
    backend.fire_status(make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend)); // motion clobbered it
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend)); // switch still present

    // A follow-up IFS_STATUS poll while motion says empty must keep lane 2 seated.
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.can_unload_from_toolhead(1));
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS motion-only firmware (no switch) still adopts FFMInfo.channel (#1065 row 28)",
    "[ams][ad5x_ifs][1065]") {
    // On firmware that publishes ONLY the motion sensor, head_switch_seen_ never
    // latches, so the head-gate cannot fire. A loaded lane whose motion sensor
    // false-negates while idle must remain seated.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Only the motion sensor ever reports (reads empty while idle). No switch.
    backend.fire_status(make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_seen(backend));

    Ad5xIfsTestAccess::set_ffm_channel(backend, 3);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 3;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Adopted as before (no switch authority to gate on).
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 3);
    CHECK(backend.get_system_info().current_slot == 2);
    CHECK(backend.can_unload_from_toolhead(2));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS eject clears a stale seated pointer at the ejected lane (#1065 row 28)",
                 "[ams][ad5x_ifs][1065]") {
    // Belt-and-suspenders: when a stale seated pointer (seated_chan_ / ffm_channel_)
    // targets the just-ejected lane, eject must zero BOTH so the Unload affordance
    // dies at once instead of persisting until the next head-gated poll.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Seat lane 2 (channel 2) via a corroborated poll.
    backend.fire_status(make_head_sensor(true));
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    REQUIRE(Ad5xIfsTestAccess::ffm_channel(backend) == 2);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Ejecting a DIFFERENT lane (0) leaves the seated pointer untouched.
    CHECK_FALSE(Ad5xIfsTestAccess::clear_seated_if_ejected(backend, 0));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1);

    // Ejecting the FFMInfo-pointed lane (1 -> channel 2) zeroes both and recomputes.
    CHECK(Ad5xIfsTestAccess::clear_seated_if_ejected(backend, 1));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
}

// BUG-B (#1065): native Z-Mod head-loaded state
//
// On native Z-Mod (v0.99.84) the head switch / motion sensors never push under
// the stock filament_*_sensor sections, so head_filament_ was never written and
// the seated lane reported AVAILABLE instead of LOADED. Two fixes:
//   PART A — derive head-loaded from GET_ZCOLOR's "Extruder:" summary.
//   PART B — also subscribe to the zmod_ifs_*_sensor namespaces.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS native Z-Mod derives head-loaded from GET_ZCOLOR Extruder summary "
                 "(BUG-B Part A)",
                 "[ams][ad5x][ifs][1065]") {
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Device-confirmed loaded frame: GET_ZCOLOR "// Extruder: 1: PLA/white | IFS:
    // True" (extruder_slot 0, 0-based) riding the same response as IFS_STATUS
    // {"Chan":1,"Ports":[true,false,false,false]}. head_filament_ is NOT seeded —
    // the backend must derive it from the Extruder summary.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true; // "// Extruder: 1" line present
    loaded.ifs_active = true;
    loaded.extruder_slot = 0; // "Extruder: 1" -> 0-based slot 0
    loaded.ifs_chan = 1;      // 1-based seated channel
    loaded.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
    CHECK(backend.get_system_info().filament_loaded);
    CHECK(backend.is_filament_loaded());
    CHECK(backend.get_slot_info(0).status == SlotStatus::LOADED);

    // Genuine unload/eject: GET_ZCOLOR "// Extruder: None | IFS: True" AND the
    // seated lane's port has gone absent (IFS_STATUS Ports all false). Physical
    // presence corroborates an empty head, so the derivation clears head-loaded.
    // (The Extruder:None-WHILE-still-present case is covered by the C1/#995 test
    // below, where head-loaded is deliberately RETAINED.)
    AmsBackendAd5xIfs::ZColorSilentResult none;
    none.saw_valid_response = true;
    none.saw_silent_content = true;
    none.saw_extruder_summary = true; // summary present, but reads "None"
    none.ifs_active = true;
    // none.extruder_slot deliberately absent ("None")
    none.ifs_chan = 1;
    none.ifs_ports = std::vector<bool>{false, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, none);

    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    CHECK_FALSE(backend.get_system_info().filament_loaded);
    CHECK_FALSE(backend.is_filament_loaded());
    CHECK(backend.get_slot_info(0).status != SlotStatus::LOADED);
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS Extruder:None does NOT clear head-loaded while the seated lane is present "
    "(C1 / #995 post-runout-while-seated)",
    "[ams][ad5x][ifs][1065]") {
    // Some AD5X firmware drops the GET_ZCOLOR extruder pointer to "Extruder: None"
    // after a runout/print-end while filament is STILL physically seated at the
    // toolhead. Clearing head-loaded then would strand the filament (Unload
    // disappears — can_unload_from_toolhead's `head_filament_ && current_slot<0`
    // fallback can't fire) and wrongly re-enable Load (-> cold grind). The
    // derivation must RETAIN head-loaded as long as the seated lane reads present.
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    // Establish loaded: Extruder: 1, lane 1 present + seated.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true;
    loaded.ifs_active = true;
    loaded.extruder_slot = 0;
    loaded.ifs_chan = 1;
    loaded.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // Firmware quirk: Extruder: None, but lane 1 is STILL present (Ports[0] true).
    AmsBackendAd5xIfs::ZColorSilentResult none_seated;
    none_seated.saw_valid_response = true;
    none_seated.saw_silent_content = true;
    none_seated.saw_extruder_summary = true; // summary present, reads "None"
    none_seated.ifs_active = true;
    // extruder_slot deliberately absent ("None")
    none_seated.ifs_chan = 1;
    none_seated.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, none_seated);

    // Head-loaded MUST be retained — the seated lane is still present.
    CHECK(Ad5xIfsTestAccess::head_filament(backend));
    CHECK(backend.get_system_info().filament_loaded);
    CHECK(backend.can_unload_from_toolhead(0));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS prefers the head sensor namespace that carries filament_detected "
                 "(I1 / #1065 boot scenario)",
                 "[ams][ad5x][ifs][1065]") {
    // On a fresh boot Moonraker can return the stock filament_switch_sensor object
    // as an empty {} compat view (no filament_detected) while the live
    // zmod_ifs_switch_sensor carries the real reading. Selecting the head sensor
    // key purely by presence would let the empty stock object win and the real
    // zmod reading be ignored, leaving head-loaded false with filament at the head.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    json frame;
    frame["filament_switch_sensor head_switch_sensor"] = json::object(); // empty compat view
    frame["zmod_ifs_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};
    backend.fire_status(frame);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS can_load gate: cold-seated stays loadable, truly-loaded does not, AFC "
          "keeps the loaded-hint (I3 / BUG-A)",
          "[ams][ad5x][ifs][1065]") {
    // The context menu computes can_load = !system_busy && !toolhead_unload &&
    // slot_has_filament, where toolhead_unload = slot_unloads_to_toolhead(slot,
    // is_loaded). BUG-A swapped the gate from the permissive is_loaded to this
    // backend-aware signal so a cold-seated AD5X lane (present but NOT at the
    // head) still offers Load. Assert the backend semantics the gate rests on.
    SECTION("AD5X cold-seated lane is not a toolhead unload (Load stays enabled)") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        REQUIRE(backend.set_tool_mapping(0, 0).success());
        // Lane 1 present + seated, but nothing at the head (Extruder: None).
        AmsBackendAd5xIfs::ZColorSilentResult cold;
        cold.saw_valid_response = true;
        cold.saw_silent_content = true;
        cold.saw_extruder_summary = true; // "Extruder: None"
        cold.ifs_active = true;
        cold.ifs_chan = 1;
        cold.ifs_ports = std::vector<bool>{true, false, false, false};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, cold);
        REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
        // slot_unloads_to_toolhead false -> !toolhead_unload true -> Load enabled.
        CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    }
    SECTION("AD5X truly-loaded lane is a toolhead unload (Load disabled)") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        REQUIRE(backend.set_tool_mapping(0, 0).success());
        AmsBackendAd5xIfs::ZColorSilentResult loaded;
        loaded.saw_valid_response = true;
        loaded.saw_silent_content = true;
        loaded.saw_extruder_summary = true;
        loaded.ifs_active = true;
        loaded.extruder_slot = 0; // "Extruder: 1" -> head loaded
        loaded.ifs_chan = 1;
        loaded.ifs_ports = std::vector<bool>{true, false, false, false};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
        CHECK(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    }
    SECTION("AFC backend returns the loaded-hint unchanged (no can_load regression)") {
        AmsBackendAfc afc(nullptr, nullptr);
        // For non-AD5X backends !toolhead_unload == !is_loaded, so the gate is
        // exactly the pre-BUG-A behavior.
        CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/true) == true);
        CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/false) == false);
    }
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS Extruder-summary head derivation ignores frames without the summary line "
    "(BUG-B Part A guard)",
    "[ams][ad5x][ifs][1065]") {
    // A frame that carries only IFS_STATUS JSON (no "// Extruder:" line) has
    // extruder_slot absent for an unrelated reason — it must NOT clear a
    // established head-loaded state. saw_extruder_summary gates this.
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true;
    loaded.ifs_active = true;
    loaded.extruder_slot = 0;
    loaded.ifs_chan = 1;
    loaded.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // IFS_STATUS-only follow-up: no Extruder summary, extruder_slot absent. Head
    // state must be left untouched (still loaded).
    AmsBackendAd5xIfs::ZColorSilentResult status_only;
    status_only.saw_valid_response = true;
    status_only.saw_silent_content = false;
    status_only.saw_extruder_summary = false; // no "// Extruder:" line this frame
    status_only.ifs_chan = 1;
    status_only.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, status_only);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS routes native Z-Mod head switch sensor namespace to head_filament_ "
                 "(BUG-B Part B)",
                 "[ams][ad5x][ifs][1065]") {
    // The live AD5X publishes its head switch sensor as
    // "zmod_ifs_switch_sensor head_switch_sensor", not the stock
    // "filament_switch_sensor head_switch_sensor". handle_status must route
    // the zmod namespace to parse_head_sensor().

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    backend.fire_status(make_zmod_head_sensor(true));
    CHECK(Ad5xIfsTestAccess::head_filament(backend));

    backend.fire_status(make_zmod_head_sensor(false));
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS routes native Z-Mod motion sensor namespace to head_filament_ (BUG-B Part B)",
    "[ams][ad5x][ifs][1065]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    backend.fire_status(make_zmod_motion_sensor(true));
    CHECK(Ad5xIfsTestAccess::head_filament(backend));

    backend.fire_status(make_zmod_motion_sensor(false));
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS toolhead-unload predicate stays false on a cold-seated lane (BUG-A #1065)",
    "[ams][ad5x][ifs][1065]") {
    // BUG-A: the context-menu Load gate used the recovery-broadened is_loaded
    // (can_unload_from_toolhead, true for the seated channel regardless of head
    // state), greying out Load on a lane whose filament sits in the lane but never
    // reached the toolhead. The real toolhead-loaded signal is
    // slot_unloads_to_toolhead(), which returns false when the head is empty — so
    // the corrected gate (!toolhead_unload) keeps Load enabled. This pins the
    // backend predicate the gate reads; the UI gate itself lives in
    // ui_ams_context_menu.cpp and is verified by the program build.
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    // Lane 1 present + seated (Chan=1, Ports[0]=true) but NOTHING at the head
    // (GET_ZCOLOR "Extruder: None"); saw_extruder_summary resolves head to false.
    AmsBackendAd5xIfs::ZColorSilentResult cold;
    cold.saw_valid_response = true;
    cold.saw_silent_content = true;
    cold.saw_extruder_summary = true;
    cold.ifs_active = true;
    // extruder_slot absent: head empty
    cold.ifs_chan = 1;
    cold.ifs_ports = std::vector<bool>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, cold);

    // Backend truth: filament present in the lane, head empty.
    CHECK(backend.get_slot_info(0).is_present());
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // The ACTUAL toolhead-unload predicate is false because the head is empty,
    // even when the recovery-broadened loaded_hint is passed in — so the UI Load
    // gate (!toolhead_unload) keeps the cold-seated lane a valid Load target.
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS restores persisted seated lane after power-cycle Chan=0 (#1065 power-cycle floor)",
    "[ams][ad5x_ifs]") {
    // The firmware forgets the seated channel across a power cycle: IFS_STATUS
    // comes back Chan=0 even with a lane physically at the head (bundle CGR6C7PA).
    // With no seated lane and head loaded, every lane wrongly labels as Unloadable.
    // We remember the last loaded lane and restore it provisionally on cold boot.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Last session had channel 2 (slot 1) loaded — restored from the lane_data DB.
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1);

    // Power cycle: a lane is at the head (head sensor true) but firmware lost which
    // one (Chan=0). All four lanes still physically present.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult boot;
    boot.saw_valid_response = true;
    boot.ifs_active = true;
    boot.ifs_chan = 0; // firmware forgot the seated channel
    boot.ifs_ports = std::vector<bool>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, boot);

    // Channel 2 (slot 1) is restored as seated -> Unload on it, Eject on cold lanes.
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS does not restore a seated lane whose port is now empty (#1065)",
                 "[ams][ad5x_ifs]") {
    // Corroboration: the filament in the remembered lane was pulled while powered
    // off. head_filament_ says SOMETHING is loaded, but it is not the remembered
    // lane (its port reads empty), so we must NOT claim it as seated.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());

    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1); // remembered ch2

    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult boot;
    boot.saw_valid_response = true;
    boot.ifs_active = true;
    boot.ifs_chan = 0;
    // Channel 2 (index 1) is now empty -> the remembered lane can't be seated.
    boot.ifs_ports = std::vector<bool>{true, false, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, boot);

    // No false claim: the seated slot stays unknown.
    CHECK(backend.get_system_info().current_slot == -1);
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS remembers the seated lane on load and forgets it on unload (#1065)",
                 "[ams][ad5x_ifs]") {
    // The persisted marker is written when a load seats a channel (Chan>0 with the
    // lane present) and cleared when an unload empties the head (Chan==0, head off).
    REQUIRE(backend.set_tool_mapping(2, 2).success()); // tool 2 -> port 3

    // Load channel 3 (slot 2): head rises, Chan=3, port 3 present.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 3;
    loaded.ifs_ports = std::vector<bool>{false, false, true, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    CHECK(Ad5xIfsTestAccess::persisted_seated_slot(backend) == std::optional<int>(2));

    // Unload: head drops, Chan back to 0 -> forget the remembered lane.
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    AmsBackendAd5xIfs::ZColorSilentResult unloaded;
    unloaded.saw_valid_response = true;
    unloaded.ifs_active = true;
    unloaded.ifs_chan = 0;
    unloaded.ifs_ports = std::vector<bool>{false, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, unloaded);
    CHECK_FALSE(Ad5xIfsTestAccess::persisted_seated_slot(backend).has_value());
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS IFS_STATUS Chan is applied on prompt-fallback (old zmod)",
                 "[ams][ad5x_ifs]") {
    // On old zmod, GET_ZCOLOR SILENT=1 degrades to an action:prompt dialog
    // (is_prompt_fallback). IFS_STATUS rides the same buffer as clean JSON
    // (respond_info, not a dialog), so its Chan must still drive the seated slot
    // even though the prompt-fallback flag is set and silent support is disabled.
    REQUIRE(backend.set_tool_mapping(3, 3).success()); // tool 3 -> port 4

    Ad5xIfsTestAccess::set_head_filament(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true; // GET_ZCOLOR dialog on old zmod
    r.saw_valid_response = true; // IFS_STATUS JSON still recognised
    r.ifs_chan = 4;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Prompt-fallback still disables silent support for the session...
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
    // ...but the seated channel from IFS_STATUS was applied regardless.
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 3);
    REQUIRE(backend.get_system_info().current_slot == 3);
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS native ZMOD: IFS_STATUS Chan resolves the loaded slot without a populated "
    "tool map (raza616 loaded-status defect, bundle UQG4RNUA)",
    "[ams][ad5x_ifs]") {
    // Native ZMOD has no lessWaste/bambufy _IFS_VARS, so parse_save_variables
    // never populates tool_map_ — it stays all-UNMAPPED. The seated channel from
    // IFS_STATUS ("Chan") is therefore the ONLY authority for which slot is
    // loaded: recompute_current_slot_locked must not launder the seated channel
    // through the empty tool_map_ (find_first_tool_for_port -> -1 -> active_tool_
    // -1 -> current_slot -1), which would pin current_slot at -1 for the whole
    // session. slot_is_actively_loaded is (slot == current_slot && filament_loaded),
    // so it would ALWAYS be false even with filament demonstrably at the toolhead —
    // the UI would show "not loaded" and disable Unload right after a successful load
    // (bundle UQG4RNUA: Chan=1, head sensor detected, current_slot stuck at -1).
    // (bundle UQG4RNUA: Chan=1, head sensor detected, current_slot stuck at -1).

    // Production native path: has_ifs_vars_ defaults false and NO set_tool_mapping
    // is ever called — that 1:1 map is exactly what the field device lacks.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Filament physically at the toolhead. handle_status also sets
    // system_info_.filament_loaded (= head_filament_) via the state_changed path.
    backend.fire_status(make_head_sensor(true));
    REQUIRE(backend.is_filament_loaded());

    // Firmware reports port 1 seated (matches the bundle's IFS_STATUS).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 1; // 1-based seated channel -> slot 0
    r.ifs_ports = std::vector<bool>{true, true, false, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // The seated channel — not the empty tool_map_ — resolves the loaded slot.
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE(backend.slot_is_actively_loaded(0));
    REQUIRE_FALSE(backend.slot_is_actively_loaded(1));

    // A later Chan=0 (nothing seated) correctly clears the loaded slot.
    AmsBackendAd5xIfs::ZColorSilentResult cleared;
    cleared.saw_valid_response = true;
    cleared.ifs_chan = 0;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, cleared);
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE_FALSE(backend.slot_is_actively_loaded(0));
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS a confirmed SILENT device is NOT demoted by a later prompt "
                 "(raza616 #981 false latch, EE5L8LY2)",
                 "[ams][ad5x_ifs]") {
    // A device that supports GET_ZCOLOR SILENT=1 (it returned clean silent content
    // at boot) can, mid-session, have the user open zmod's interactive "Select print
    // materials" colour menu — a prompt dialog — while a SILENT query is in flight.
    // HelixScreen must not attribute that prompt to its own query and latch
    // zcolor_silent_supported_=false, which would demote a capable device to the
    // resurrection-prone JSON-inference path. Once SILENT has been confirmed
    // (genuine summary/slot content seen), a later prompt must NOT demote it.
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    // A genuine SILENT response with content confirms the device speaks SILENT.
    AmsBackendAd5xIfs::ZColorSilentResult ok;
    ok.saw_valid_response = true;
    ok.saw_silent_content = true; // a GET_ZCOLOR summary/slot line was parsed
    ok.ifs_active = true;
    ok.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, ok);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend)); // still supported

    // The user's colour menu prompt (IFS_STATUS rides the same response).
    AmsBackendAd5xIfs::ZColorSilentResult prompt;
    prompt.is_prompt_fallback = true;
    prompt.saw_valid_response = true;
    prompt.ifs_chan = 2;
    prompt.ifs_ports = std::vector<bool>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, prompt);

    // NOT demoted — the prompt was external (user menu), not our query degrading.
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: IFS_STATUS Chan=0 after head drop finalizes unload to IDLE",
                 "[ams][ad5x_ifs][phase]") {
    // During a tracked unload, after the head sensor drops, an IFS_STATUS with
    // Chan=0 (nothing seated) is the clean terminal signal — finalize to IDLE
    // even though extruder_slot is also absent. Works on prompt-fallback.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_chan = 0; // nothing seated -> unload complete
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: head drop during unload advances past HEATING (no heat event)",
                 "[ams][ad5x_ifs][phase]") {
    // _IFS_REMOVE_CURRENT_PRUTOK runs with BYPASS_TEMPERATURE_CHECK and sends no
    // preheat, so the nozzle never reaches target and reached_target_once stays
    // false -> stuck in HEATING forever (only the 300s timeout recovered). A head
    // drop physically proves the cut/retract started, so it must mark
    // reached_target_once and advance the phase past HEATING.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // No temp ever reaches target. Head sensor drops (cut underway).
    backend.fire_status(make_head_sensor(false));

    // Must have advanced past HEATING despite never seeing a heat event.
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS apply_zcolor_result writes presence, colour and material",
                 "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 1;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "00FF00"};
    r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "2750E0"};
    // slots 2 and 3 left empty: they read as not present

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 1));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));
    // build_colors/build_types are the comma-separated _IFS_VARS payloads
    CHECK(Ad5xIfsTestAccess::build_colors(backend).find("00FF00") != std::string::npos);
    CHECK(Ad5xIfsTestAccess::build_types(backend).find("PETG") != std::string::npos);
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS apply_zcolor_result skips on prompt fallback",
                 "[ams][ad5x_ifs]") {
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // One prompt-style response flips silent_supported to false permanently
    // for this session; subsequent query_zcolor_silent() becomes a no-op.
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

// A response that carries nothing usable leaves what the slot already holds: a
// transient one with no slot or summary line keeps presence, and an old-format
// zmod response (slot lines without /HEX) updates presence but keeps the colour.
TEST_CASE("AD5X IFS apply_zcolor_result does not overwrite with what a response lacks",
          "[ams][ad5x_ifs]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);

    SECTION("a response with no valid content") {
        Ad5xIfsTestAccess::apply_zcolor_result(backend, AmsBackendAd5xIfs::ZColorSilentResult{});
        CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    }
    SECTION("an old-format response with no hex") {
        AmsBackendAd5xIfs::ZColorSilentResult old_fmt;
        old_fmt.saw_valid_response = true;
        old_fmt.is_old_format = true;
        old_fmt.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", ""};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, old_fmt);
        CHECK(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);
        CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    }
}

// parse_zcolor_silent extracts extruder_slot from the GET_ZCOLOR summary line
// ("// Extruder: 3: PLA/HEX | IFS: True") and apply_zcolor_result must use it:
// active_tool_ is otherwise only updated from lessWaste/bambufy save_variables,
// so stock-ZMOD users would never get an active-tool signal and the panel would
// keep showing a filament that was unloaded.
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS apply_zcolor_result derives active_tool from extruder_slot (stock ZMOD)",
                 "[ams][ad5x_ifs]") {
    // Establish an identity tool map so find_first_tool_for_port(N) -> tool N-1.
    // Default tool_map_ is all kUnmappedPort, so without this find_first_tool_for_port
    // returns -1 for every port and the test would always assert -1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    SECTION("extruder_slot present → active_tool follows tool_map_") {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 2; // 0-based → port 3 → tool 2 under identity map
        r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 2);
    }

    SECTION("extruder_slot absent (unloaded) → active_tool clears to -1") {
        // Seed with a loaded slot first so we can detect the clear.
        AmsBackendAd5xIfs::ZColorSilentResult loaded;
        loaded.saw_valid_response = true;
        loaded.ifs_active = true;
        loaded.extruder_slot = 1;
        loaded.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 1);

        // Now an "Extruder: None (N)" response — extruder_slot becomes nullopt.
        AmsBackendAd5xIfs::ZColorSilentResult unloaded;
        unloaded.saw_valid_response = true;
        unloaded.ifs_active = true;
        unloaded.current_channel = 1;
        // extruder_slot deliberately not set; slots all empty
        Ad5xIfsTestAccess::apply_zcolor_result(backend, unloaded);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }

    SECTION("extruder_slot maps to unmapped tool → active_tool becomes -1") {
        // Wipe the map for slot 0 (port 1), then claim slot 0 is in the extruder.
        // No tool maps to port 1, so find_first_tool_for_port(1) returns -1.
        REQUIRE(backend.set_tool_mapping(0, /*slot=*/-1).success()); // unmap T0

        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 0;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "808080"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS apply_zcolor_result leaves active_tool alone when has_ifs_vars",
                 "[ams][ad5x_ifs]") {
    // lessWaste/bambufy users get active_tool from <prefix>_current_tool in
    // save_variables, which is authoritative for them. GET_ZCOLOR's view must
    // not race against it — verify by setting has_ifs_vars=true and checking
    // active_tool_ is unchanged after apply.

    // Seed the tool map BEFORE flipping has_ifs_vars=true. set_tool_mapping
    // tries to persist via write_ifs_var when has_ifs_vars is true, which
    // requires an api_ connection — using nullptr would return "No API
    // connection" before the in-memory mutation completes.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.extruder_slot = 2;
    r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // active_tool defaulted to -1 and was not updated — lessWaste's save_variables
    // path retains exclusive ownership.
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
}

// Material whitelist + normalization

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS get_supported_materials returns firmware whitelist",
                 "[ams][ad5x_ifs]") {
    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    REQUIRE(supported->size() == 7);

    // Exact strings as firmware expects (order mirrors the firmware error message).
    REQUIRE((*supported)[0] == "PLA");
    REQUIRE((*supported)[1] == "PLA-CF");
    REQUIRE((*supported)[2] == "SILK");
    REQUIRE((*supported)[3] == "TPU");
    REQUIRE((*supported)[4] == "ABS");
    REQUIRE((*supported)[5] == "PETG");
    REQUIRE((*supported)[6] == "PETG-CF");
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS normalize_material coerces input to whitelist",
                 "[ams][ad5x_ifs]") {
    SECTION("exact match passes through") {
        REQUIRE(backend.normalize_material("PLA") == "PLA");
        REQUIRE(backend.normalize_material("PETG-CF") == "PETG-CF");
        REQUIRE(backend.normalize_material("TPU") == "TPU");
    }

    SECTION("case-insensitive match returns canonical case") {
        REQUIRE(backend.normalize_material("pla") == "PLA");
        REQUIRE(backend.normalize_material("Pla") == "PLA");
        REQUIRE(backend.normalize_material("petg-cf") == "PETG-CF");
        REQUIRE(backend.normalize_material("silk") == "SILK");
    }

    SECTION("PLA+ collapses via compat_group to PLA") {
        // PLA+ shares compat_group "PLA" with PLA in the filament DB.
        REQUIRE(backend.normalize_material("PLA+") == "PLA");
    }

    SECTION("ASA collapses via compat_group to ABS") {
        // ASA has compat_group "ABS_ASA"; ABS is the first whitelist entry
        // with matching compat_group.
        REQUIRE(backend.normalize_material("ASA") == "ABS");
    }

    SECTION("PEEK falls back to first entry (no compat_group match)") {
        // PEEK's compat_group is "HIGH_TEMP" which no whitelist entry shares.
        REQUIRE(backend.normalize_material("PEEK") == "PLA");
    }

    SECTION("empty string falls back to first entry") {
        REQUIRE(backend.normalize_material("") == "PLA");
    }

    SECTION("unknown material falls back to first entry") {
        REQUIRE(backend.normalize_material("Nonsense") == "PLA");
    }

    SECTION("silk variants map to SILK via AD5X-specific override") {
        // AD5X treats SILK as distinct from PLA. The shared filament DB
        // groups silk variants under compat_group "PLA" because most
        // printers don't make that distinction, so the default fallback
        // would route them to "PLA". The AD5X normalize_material()
        // override catches common silk names before delegating.
        REQUIRE(backend.normalize_material("Silk PLA") == "SILK");
        REQUIRE(backend.normalize_material("PLA Silk") == "SILK");
        REQUIRE(backend.normalize_material("Silk") == "SILK");
        REQUIRE(backend.normalize_material("silk pla") == "SILK");
    }
}

TEST_CASE("AFC backend has no whitelist and passes material through unchanged",
          "[ams][whitelist]") {
    // AFC (like Happy Hare, ACE, CFS) treats material as a free-form label.
    helix::test::RegisteredBackend<AmsBackendAfc> backend_reg(nullptr, nullptr);
    AmsBackendAfc& backend = *backend_reg;

    REQUIRE_FALSE(backend.get_supported_materials().has_value());
    REQUIRE(backend.normalize_material("PLA+") == "PLA+");
    REQUIRE(backend.normalize_material("Some Random String") == "Some Random String");
    REQUIRE(backend.normalize_material("") == "");
}
