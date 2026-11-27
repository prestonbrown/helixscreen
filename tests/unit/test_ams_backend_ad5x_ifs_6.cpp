// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 182 to 225 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [general], [ad5x_ifs][1065], [ad5x_ifs], [ad5x_ifs][load], [ad5x_ifs][2086], and more

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

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS load_filament does NOT set swap_expected for a plain load (#1065 regression)",
    "[ams][ad5x_ifs][phase][1065]") {
    // When no other lane is seated, swap_expected MUST NOT flip — otherwise the
    // extended budget would mask a genuinely stuck load.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // No seated lane.
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 0);

    // Dispatch load on ch2 (slot 1). No swap → flag MUST stay false.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE_FALSE(Ad5xIfsTestAccess::swap_expected(backend));
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS load_filament does NOT set swap_expected when same slot is already seated (#1065)",
    "[ams][ad5x_ifs][phase][1065]") {
    // Edge case: load the SAME slot that's already seated (a re-load / resume).
    // Firmware doesn't run an implicit unload in that case, so swap_expected
    // must NOT flip — otherwise we'd mask a real failure on a same-slot reload.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2; // ch2 (slot 1) seated
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);

    // Reload the SAME slot (slot 1 = ch2 = port 2). seated_chan_ == port →
    // no swap, no flag.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE_FALSE(Ad5xIfsTestAccess::swap_expected(backend));
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload_filament with empty toolhead routes to cold lane eject (7AC4SDEX)",
    "[ams][ad5x_ifs]") {
    // Filament is in the lane (ifs_motion_sensor present) but NOT at the extruder
    // (head_switch_sensor empty). The toolhead unload would early-return in firmware
    // after homing — "homes and nothing happens." Unload must instead pull the lane
    // back cold via IFS_F11.
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    REQUIRE(backend.unload_filament(2).success());

    // 0-based slot 2 -> 1-based port 3, cold clamp/retract/unclamp, no heat, no homing.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=3"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=3"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("M104"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
}

TEST_CASE("AD5X IFS load-vs-swap: INSERT_PRUTOK_IFS self-swaps, so no helix unload-before-load",
          "[ams][ad5x_ifs][load]") {
    // ZMOD's _INSERT_PRUTOK_IFS runs IFS_REMOVE_CURRENT_PRUTOK itself (no-op on
    // an empty head sensor, self-heats to the seated lane's temp otherwise), so
    // the sidebar must dispatch load_filament directly. The unload-first swap
    // path degraded to eject_lane(-1) when the head sensor read empty and the
    // requested load was silently dropped — the sidebar froze at "Heat nozzle
    // 195/200" (bundle Z5V4K3NL).
    TestableAd5xIfsBackend backend;

    AmsSystemInfo info;
    info.filament_loaded = true;
    info.current_slot = 0;
    REQUIRE_FALSE(backend.needs_unload_before_load(info, /*target_slot=*/1));

    info.filament_loaded = false;
    info.current_slot = -1;
    REQUIRE_FALSE(backend.needs_unload_before_load(info, /*target_slot=*/1));
}

TEST_CASE("AD5X IFS load: INSERT_PRUTOK_IFS self-heats, so backend reports auto-heat",
          "[ams][ad5x_ifs][load]") {
    // The firmware macro resolves the target lane's configured material temp and
    // does its own M104 + TEMPERATURE_WAIT. Reporting auto-heat skips the UI
    // preheat poll (whose completion the sidebar could strand) — the backend
    // phase tracker still synthesizes the Heat-nozzle step for the step bar.
    TestableAd5xIfsBackend backend;
    REQUIRE(backend.supports_auto_heat_on_load());
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload_filament(-1) with empty toolhead resolves the active lane for the "
    "cold eject",
    "[ams][ad5x_ifs][unload]") {
    // "Unload whatever is active" with nothing at the head must eject a concrete
    // lane, not forward -1 into eject_lane (which fails validate_slot_index, an
    // error the sidebar swap path would silently discard).
    Ad5xIfsTestAccess::set_current_slot(backend, 1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    REQUIRE(backend.unload_filament(-1).success());

    // 0-based slot 1 -> 1-based port 2, cold clamp/retract/unclamp.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=2"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=2"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS unload_filament(-1) with empty toolhead and no known lane fails loudly",
                 "[ams][ad5x_ifs][unload]") {
    // Nothing at the head, no seated channel, no active slot: there is nothing
    // to unload. Return a real error (the sidebar toasts it) rather than
    // eject_lane(-1)'s invalid-slot, which callers swallow.
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    AmsError err = backend.unload_filament(-1);
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(err.user_msg.empty());
    REQUIRE(backend.captured_gcodes.empty());
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload_filament clears toolhead when firmware dropped the active slot (#995)",
    "[ams][ad5x_ifs]") {
    // current_slot == -1 (stock-ZMOD "Extruder: None") but filament is seated at
    // the head: Unload must still dispatch the current-channel toolhead unload.
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_ffm_file_channel(backend, 0);

    REQUIRE(backend.unload_filament(2).success());
    // FFMInfo.channel is 0, so the macro would find no lane: the tapped lane is
    // declared first (#1324).
    REQUIRE(backend.has_gcode("SET_EXTRUDER_SLOT SLOT=3\n_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_PRUTOK"));
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload_filament on a non-active slot ejects that lane, not the loaded one "
    "(raza616 HKHZFYB2)",
    "[ams][ad5x_ifs]") {
    // Channel 3 (0-based slot 2) is loaded at the toolhead; the user asks to
    // unload channel 1 (slot 0). Keying off the global head sensor would fire
    // _IFS_REMOVE_CURRENT_PRUTOK, which the firmware resolves from FFMInfo.channel —
    // it would heat the nozzle and back out the loaded channel 3, never touching
    // channel 1, then stall. A non-active slot's filament is in the lane, so unload
    // must cold-eject THAT port instead of running the toolhead unload.
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // 0-based slot 0 -> 1-based port 1, cold clamp/retract/unclamp. Crucially NOT
    // the toolhead unload, which would back out the loaded channel 3.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=1"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=1"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload of the IFS_STATUS-seated slot cuts even when current_slot disagrees "
    "(raza616 #981 v0.99.80)",
    "[ams][ad5x_ifs]") {
    // Tapping Unload on the channel physically seated at the extruder must not
    // cold-eject it (IFS_F11, no cut), which "grinds away without removing."
    //
    // Mechanism: the unload router keyed off system_info_.current_slot, which is
    // derived through tool_map_/active_tool_. On the plugin path (has_ifs_vars_),
    // the IFS_STATUS Chan seated-channel authority is gated out of that derivation
    // (it persists at the seated port while save_variables _current_tool can go
    // stale loaded-idle), so current_slot points at a DIFFERENT slot than the one
    // physically seated. Tapping the seated slot would then satisfy
    // "slot_index != current_slot" and route to the cold lane eject, which
    // grinds a seated, un-cut filament instead of the heated toolhead unload.
    //
    // IFS_STATUS Chan is therefore stored unconditionally and the unload router
    // treats the seated channel as the active slot regardless of the tool_map_-derived
    // current_slot, so the seated slot always routes to _IFS_REMOVE_CURRENT_PRUTOK.
    // Plugin user: Chan does NOT drive current_slot (gated on !has_ifs_vars_).
    // Plugin user: Chan does NOT drive current_slot (gated on !has_ifs_vars_).
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    // Stale active pointer at slot 0 while channel 4 is the one truly seated.
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // IFS_STATUS reports Chan=4 (port 4 = slot 3) as the physically seated channel.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    // current_slot stays stale (0) because the plugin path owns it.
    REQUIRE(backend.get_system_info().current_slot == 0);

    // User taps Unload on the seated slot 3.
    REQUIRE(backend.unload_filament(3).success());

    // Must run the heated toolhead unload (cut + retract), NOT the cold lane eject.
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F24"));
}

TEST_CASE_METHOD(
    RunningAd5xIfs,
    "AD5X IFS IFS_STATUS Ports is the presence authority — no resurrection from persisted "
    "ffmColor after a false SILENT demotion (raza616 #981 EE5L8LY2)",
    "[ams][ad5x_ifs]") {
    // Bundle EE5L8LY2 (v0.99.80): ch1 is physically empty, but Adventurer5M.json
    // persists ffmColor1=#F95D73 / ffmType1=ABS across the unload (zmod never
    // blanks the colour). The user opened zmod's interactive "Select print
    // materials" colour menu — a prompt dialog — while a GET_ZCOLOR SILENT=1
    // query was in flight; HelixScreen misattributed that prompt to its own query
    // and latched zcolor_silent_supported_=false, demoting a SILENT-capable
    // device to the legacy JSON presence inference, which then resurrected ch1
    // from the persisted colour ("Slot 0 status: 1 → 2").
    //
    // IFS_STATUS "Ports" carries the RS-485 per-port presence truth — it read
    // [false,true,true,true] throughout the incident — and rides the same clean-
    // JSON response (even behind a prompt fallback). It is the presence
    // authority: once seen, the JSON colour inference must never run, so ch1
    // stays empty regardless of the SILENT latch or the persisted ffmColor1.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // worst case: already demoted

    // IFS_STATUS reports ch1 empty, ch2-4 loaded — arriving in the SAME response
    // as a prompt fallback (the user's colour menu collided with our query).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.is_prompt_fallback = true; // GET_ZCOLOR degraded to a prompt this turn
    r.ifs_chan = 2;
    r.ifs_ports = std::vector<bool>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Ports established presence: ch1 empty, ch2-4 present.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(1).status != SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(2).status != SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(3).status != SlotStatus::EMPTY);

    // The persisted Adventurer5M.json still carries a colour/type for the empty
    // ch1. With SILENT latched off, inferring presence from it would resurrect ch1.
    // Ports-as-authority must suppress that inference.
    std::string content = R"({
        "FFMInfo": {
            "channel": 2,
            "ffmColor1": "#F95D73",
            "ffmColor2": "#FFFFFF",
            "ffmColor3": "#E53935",
            "ffmColor4": "#898989",
            "ffmType1": "ABS",
            "ffmType2": "PETG",
            "ffmType3": "PETG",
            "ffmType4": "PETG"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // ch1 must STAY empty — not resurrected by the persisted colour.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(1).status != SlotStatus::EMPTY);
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS RUN_ZCOLOR (display-only) leaves a locked override intact (#981)",
                 "[ams][ad5x_ifs]") {
    // RUN_ZCOLOR only renders the material menu; it is NOT an edit, so a
    // user-locked override must survive it — only CHANGE_ZCOLOR is a real change.

    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "RUN_ZCOLOR SLOT=1 HEX=46328E TYPE=PETG"));

    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// The release reaches BOTH stores that hold a lane's truth - #981, #1646
//
// overrides_ and the lane source store overlap. The override strip alone is
// not a release: the lane's LocalUser record outranks the vendor cache, so a
// colour left standing there goes on painting over the firmware truth the
// CHANGE_ZCOLOR just authored. These three pin the whole retraction - what it
// takes, what it keeps, and that it happens only on a real release.

TEST_CASE("AD5X IFS a firmware frame that releases nothing leaves the user's colour winning",
          "[ams][ad5x_ifs][981]") {
    // The counterweight to the two above. Only a deliberate CHANGE_ZCOLOR
    // releases the locks; an ordinary firmware reading is what the locks exist
    // to outrank, so the user's choice has to survive one intact.
    Ad5xIfsTmpJsonFile tmp("981_frame_releases_nothing",
                           R"({"FFMInfo":{"ffmColor1":"#898989","ffmType1":"PETG"}})");
    helix::test::RegisteredBackend<TestableAd5xIfsBackend> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "898989");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    SlotInfo edit = backend.get_slot_info(0);
    edit.color_rgb = 0xFFFFFF;
    edit.material = "SILK";
    edit.brand = "Sunlu";
    helix::test::edit_slot_as_user(backend, 0, edit);

    // Firmware states a different colour and type, with no CHANGE_ZCOLOR
    // behind it: the post-print FFMInfo revert this lock was added for.
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    const SlotInfo after = backend.get_slot_info(0);
    CHECK(after.color_rgb == 0xFFFFFF);
    CHECK(after.material == "SILK");
    CHECK(after.brand == "Sunlu");

    const helix::ams::LaneSources sources = helix::ams::lane_sources(backend_reg.lane(0));
    REQUIRE(sources.pending.has_value());
    CHECK(sources.pending->color_rgb == 0xFFFFFFu);
    CHECK(sources.pending->material == "SILK");
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS CHANGE_ZCOLOR with no locked override is a harmless no-op (#981)",
                 "[ams][ad5x_ifs]") {
    // External edit to a slot HelixScreen never touched: nothing to clear, and we
    // must not crash or fabricate an override.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "0DE2A0");
    Ad5xIfsTestAccess::set_material(backend, 1, "SILK");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=2 HEX=0DE2A0 TYPE=SILK"));

    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 1).has_value());
    SlotInfo info = backend.get_slot_info(1);
    REQUIRE(info.material == "SILK");
    REQUIRE(info.color_rgb == 0x0DE2A0);
}

// External CHANGE_ZCOLOR must NOT wipe the user's BRAND override (#981).
//
// The #981 clear-on-external-edit path (clear_override_locked) erases the
// ENTIRE per-slot override — color, material AND brand/spool_name/spoolman_id.
// That is correct for the firmware-carried fields (color/type): an external
// CHANGE_ZCOLOR is a deliberate edit and firmware truth should win. But brand
// is metadata the firmware CANNOT carry, and the AD5X native LCD emits a bare
// `CHANGE_ZCOLOR SLOT=N TYPE=<material>` (no brand) on every load/insert. So a
// routine physical load silently resets the user's saved vendor to empty.
//
// The #1071 insert/eject presence paths already retain the override across a
// physical spool event; this external-clear path must retain brand the same
// way. Uses the persist-capable setup (real MoonrakerAPIMock + injected
// FilamentSlotOverrideStore, as in the "stores override" test) so the
// user edit runs through the PRODUCTION apply_user_edit() path — which
// stages a user-locked override the #981 external-clear then fires on — instead
// of seeding the override directly. Then feeds the bare firmware CHANGE_ZCOLOR
// and asserts the brand survives.
//
// CHANGE_ZCOLOR TYPE=/HEX= in-gcode parameter extraction (#1065).
//
// When zmod's COLOR macro is invoked (from Mainsail, the AD5X LCD, or — most
// commonly — HelixScreen's own ActionPromptModal rendering zmod's prompt), it
// emits `CHANGE_ZCOLOR SLOT=N [TYPE=X] [HEX=YYYYYY]` to the gcode stream.
// The TYPE=/HEX= parameters ARE the user's intent at the moment of emission,
// BEFORE any firmware propagation lag.
//
// Deferring entirely to a follow-up GET_ZCOLOR SILENT=1 query for the new
// color/material is too slow: that query queues on Klipper's serial gcode line
// behind any running IFS operation (INSERT_PRUTOK_IFS, IFS_F11 eject, even
// unrelated long macros), so it can take 1-3 minutes to return — or hit the 60s
// RPC timeout and miss entirely (bundle NJB2U558: GET_ZCOLOR timed out 1m after a
// ch4 type change while a load op was running; the display stayed stale for
// minutes).
//
// So TYPE=/HEX= are extracted from the gcode token itself and written straight
// to colors_/materials_ + baselines + update_slot_from_state. The follow-up
// GET_ZCOLOR is a confirming no-op rather than the only path to refresh.

// The CHANGE_ZCOLOR token itself carries the new TYPE/HEX, so the slot updates
// in the same frame instead of waiting on a GET_ZCOLOR that can lag minutes
// behind a queued IFS op (#1065). SILENT is off here, so nothing but the gcode
// line can have done it. Hex is upper-cased to match parse_adventurer_json, so
// the follow-up GET_ZCOLOR compares equal.
TEST_CASE("AD5X IFS CHANGE_ZCOLOR TYPE=/HEX= update get_slot_info immediately",
          "[ams][ad5x_ifs][1065]") {
    struct Row {
        const char* name;
        int slot;
        const char* color;
        const char* material;
        const char* line;
        const char* expected_material;
        uint32_t expected_color;
    };
    const Row rows[] = {
        {"TYPE= alone", 3, "FEF043", "PETG", "CHANGE_ZCOLOR SLOT=4 TYPE=ABS", "ABS", 0xFEF043},
        {"HEX= alone", 1, "FFFFFF", "PETG", "CHANGE_ZCOLOR SLOT=2 HEX=2750E0", "PETG", 0x2750E0},
        // Real zmod format (bundle NJB2U558)
        {"TYPE= and HEX= together", 0, "000000", "PLA", "CHANGE_ZCOLOR SLOT=1 TYPE=PETG HEX=F95D73",
         "PETG", 0xF95D73},
        {"lower-case HEX=", 0, "000000", "PLA", "CHANGE_ZCOLOR SLOT=1 TYPE=PETG HEX=fef043", "PETG",
         0xFEF043},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            TestableAd5xIfsBackend backend;
            Ad5xIfsTestAccess::set_running(backend, true);
            Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
            Ad5xIfsTestAccess::set_port_presence(backend, r.slot, true);
            Ad5xIfsTestAccess::set_color(backend, r.slot, r.color);
            Ad5xIfsTestAccess::set_material(backend, r.slot, r.material);

            REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(backend, r.line));

            SlotInfo info = backend.get_slot_info(r.slot);
            CHECK(info.material == r.expected_material);
            CHECK(info.color_rgb == r.expected_color);
        }
    }
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS CHANGE_ZCOLOR TYPE= updates baseline so the eventual GET_ZCOLOR response is a "
    "no-op (#1065)",
    "[ams][ad5x_ifs][1065]") {
    // The whole point of the gcode-path extraction: when GET_ZCOLOR eventually
    // returns (potentially minutes later, queued behind an IFS op), its
    // color/material for this slot must EQUAL the values we already wrote —
    // so apply_zcolor_result's `colors_[idx] != parsed->hex` check skips, no
    // double-sync fires, no spurious "external edit" log entry. We verify
    // this by checking the baselines last_firmware_color_/last_firmware_material_
    // were advanced to the gcode-supplied values.
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    Ad5xIfsTestAccess::set_color(backend, 3, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 3, "PETG");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=4 TYPE=ABS HEX=FEF043"));

    // Baselines now match the gcode-supplied values, so a subsequent
    // observation of the same values (e.g. from GET_ZCOLOR) won't trip
    // check_external_*_change's "delta vs baseline" detector.
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 3).value_or("") == "ABS");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 3).value_or(0) == 0xFEF043);
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS successive CHANGE_ZCOLOR TYPE= macros each refresh the display (#1065)",
                 "[ams][ad5x_ifs][1065]") {
    // Successive COLOR macros — change color, then change material type — must
    // each refresh the type display. A GET_ZCOLOR-queue-only refresh lags behind;
    // with gcode-path extraction both changes land synchronously and visibly.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    // First macro: change color (HEX=) only.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=2 HEX=2750E0"));
    {
        SlotInfo info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x2750E0);
        REQUIRE(info.material == "PETG"); // still PETG
    }

    // Second macro: change type (TYPE=) only.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=2 TYPE=PLA"));
    {
        SlotInfo info = backend.get_slot_info(1);
        REQUIRE(info.material == "PLA");     // <-- must not stay "PETG"
        REQUIRE(info.color_rgb == 0x2750E0); // color retained
    }
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS CHANGE_ZCOLOR with malformed TYPE= value doesn't crash (#1065)",
                 "[ams][ad5x_ifs][1065]") {
    // Defensive: a garbled CHANGE_ZCOLOR line must not corrupt state or crash.
    // The extractor skips TYPE=/HEX= values it can't make sense of and falls
    // back to the existing SLOT=-only behavior (override clear if locked,
    // debounced re-read).
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    // Garbled HEX (not hex digits) — must not throw, must not write a bogus
    // color to entry->info.
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 HEX=NOTHEX TYPE=PETG"));

    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.material == "PETG");    // valid TYPE= still applies
    REQUIRE(info.color_rgb == 0xFF0000); // invalid HEX= left color unchanged
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS CHANGE_ZCOLOR does NOT create an override for an auto-tracked slot "
                 "(#1065 echo-flood guard)",
                 "[ams][ad5x_ifs][1065]") {
    // The refresh above must be surgical: a slot with NO override stays
    // override-free. zmod re-emits CHANGE_ZCOLOR button-definition lines on
    // every prompt render, so syncing unconditionally would fabricate an
    // auto-mirror override (and a lane_data write) for a slot the user never
    // touched. With no override, firmware truth already shows through unaided.
    Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
    Ad5xIfsTestAccess::set_color(backend, 2, "0000FF");
    Ad5xIfsTestAccess::set_material(backend, 2, "ABS");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=3 TYPE=ABS HEX=00FF00"));

    // Value applied directly (no override to mask it) and none fabricated.
    SlotInfo info = backend.get_slot_info(2);
    REQUIRE(info.color_rgb == 0x00FF00);
    REQUIRE(info.material == "ABS");
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 2).has_value());
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS CHANGE_ZCOLOR TYPE= stops at '|' so echoed button payloads don't poison "
                 "the material (#1065 raza616)",
                 "[ams][ad5x_ifs][1065]") {
    // The AD5X's native screen re-echoes received commands into a quoted
    // RESPOND MSG="…", so a button payload's `|style|hex` suffix can ride right
    // up against TYPE= on a line that is NOT an action:prompt_ definition. A
    // greedy TYPE=(\S+) captured "SILK|primary|F72224" into materials_ +
    // last_firmware_material_ — the pollution seen in bundle H2X5QMCU as
    // "firmware material changed SILK|primary|F72224 -> SILK". The extractor
    // must stop the TYPE token at the '|'.
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    Ad5xIfsTestAccess::set_color(backend, 3, "F72224");
    Ad5xIfsTestAccess::set_material(backend, 3, "SILK");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "RESPOND MSG=\"CHANGE_ZCOLOR SLOT=4 TYPE=SILK|primary|F72224\""));

    // Material is the clean token, not the button descriptor — both the
    // UI-visible value and the change-detection baseline.
    SlotInfo info = backend.get_slot_info(3);
    REQUIRE(info.material == "SILK");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 3).value_or("") == "SILK");
}

// COLOR-menu echo classification (#1065, bundle 482NB943).
//
// zmod renders every COLOR-macro dialog by echoing its buttons down the gcode
// console as `// action:prompt_button <label>|<gcode>|<style>[|<hex>]`. Those
// lines are the menu's OFFER LIST — the 24-swatch palette, the material
// whitelist — not a record of anything the firmware did. Feeding them to the
// CHANGE_ZCOLOR extractor would apply all 24 candidates in sequence, so the slot
// would land on the LAST swatch (#161616) / the last type (PETG-CF) until the
// confirming GET_ZCOLOR corrected it ~1s later, and each phantom apply would push
// a lane_data write (25 server.database.post_item requests queued in 300ms on
// a 473MB MIPS AD5X).
//
// The root "Select print materials" render is the opposite case: its per-slot
// rows carry a genuine four-slot firmware snapshot, emitted ~100ms after the
// user's tap — the freshest truth available anywhere in the stream.

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS echoed CHANGE_ZCOLOR menu buttons are offers, not edits "
                 "(#1065 bundle 482NB943)",
                 "[ams][ad5x_ifs][1065]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "SILK");

    // "Select color": the swatch grid, ending on black.
    for (const char* hex : {"ffffff", "fef043", "75d9f3", "161616"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
            backend, std::string("// action:prompt_button _ |CHANGE_ZCOLOR SLOT=1 TYPE=SILK HEX=") +
                         hex + "|primary|" + hex));
    }
    // "Select material type": the whitelist, ending on PETG-CF.
    for (const char* type : {"PLA", "TPU", "PETG-CF"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
            backend, std::string("// action:prompt_button ") + type +
                         "|CHANGE_ZCOLOR SLOT=1 TYPE=" + type + " HEX=F330F9|primary|F330F9"));
    }

    // The slot still shows firmware truth, not the last candidate in either list.
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xF330F9);
    CHECK(info.material == "SILK");
    // Baselines held too — a moved baseline makes the next clean GET_ZCOLOR read
    // look like an external edit and fires a spurious lane_data write.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0).value_or(0) == 0xF330F9);
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "SILK");
    // And nothing was fabricated on a slot the user never committed a change to.
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS counts menu renders apart from external colour edits",
                 "[ams][ad5x_ifs][1065]") {
    // Both burst counters feed one consolidated log line, and that line is
    // often the only account of what a printer did. Folding menu renders into
    // the edit count reports a colour change nobody made: a reader chasing a
    // filament-identity report sees "24 external colour changes" from a dialog
    // whose options were never picked.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "SILK");

    REQUIRE(Ad5xIfsTestAccess::external_change_burst_count(backend) == 0);
    REQUIRE(Ad5xIfsTestAccess::menu_render_burst_count(backend) == 0);

    SECTION("echoed menu buttons count as renders, never as edits") {
        for (const char* hex : {"ffffff", "fef043", "75d9f3", "161616"}) {
            Ad5xIfsTestAccess::on_gcode_response_line(
                backend,
                std::string("// action:prompt_button _ |CHANGE_ZCOLOR SLOT=1 TYPE=SILK HEX=") +
                    hex + "|primary|" + hex);
        }
        CHECK(Ad5xIfsTestAccess::menu_render_burst_count(backend) == 4);
        CHECK(Ad5xIfsTestAccess::external_change_burst_count(backend) == 0);
    }

    SECTION("a bare CHANGE_ZCOLOR counts as an edit, never as a render") {
        Ad5xIfsTestAccess::on_gcode_response_line(backend,
                                                  "// CHANGE_ZCOLOR SLOT=1 TYPE=PETG HEX=BCBCBC");
        CHECK(Ad5xIfsTestAccess::external_change_burst_count(backend) == 1);
        CHECK(Ad5xIfsTestAccess::menu_render_burst_count(backend) == 0);
    }
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS COLOR-menu slot row is a firmware snapshot (#1065 bundle 482NB943)",
                 "[ams][ad5x_ifs][1065]") {
    // The user taps SILK; zmod applies it and re-renders the root menu with the
    // new value ~100ms later. Before this, HelixScreen ignored that row and
    // waited on GET_ZCOLOR — which in bundle 482NB943 raced the firmware write
    // and returned the OLD type, leaving the multi-filament menu showing PETG
    // for 40 seconds (17:34:37.765 tap -> 17:35:17.342 label catch-up).
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend,
        "// action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=F330F9 TYPE=SILK|primary|F330F9"));

    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.material == "SILK");
    CHECK(info.color_rgb == 0xF330F9);
    // Baseline advanced, so the GET_ZCOLOR that lands moments later is the
    // confirming no-op it was always meant to be.
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "SILK");
}

// Current Z-Mod puts the button color in the third field and drops the fourth;
// the slot row reads the same (prestonbrown/helixscreen#1806).
TEST_CASE_METHOD(RunningAd5xIfs, "AD5X IFS COLOR-menu slot row with the color in the third field",
                 "[ams][ad5x_ifs][1806]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "// action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=21CC3D TYPE=SILK|21CC3D"));

    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.material == "SILK");
    CHECK(info.color_rgb == 0x21CC3D);
}

// Z-Mod reports a freshly inserted lane's type as "?", its "nothing assigned"
// marker. Taken in as a material it reaches lane_data and makes an unlabelled
// lane look like it holds a spool (prestonbrown/helixscreen#2086).
TEST_CASE("AD5X IFS GET_ZCOLOR reads Z-Mod's ? type as no material", "[ams][ad5x_ifs][2086]") {
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 2: ?/",
        "// 4: ?",
    };
    auto r = helix::ad5x::parse_zcolor_silent(lines);
    REQUIRE(r.slots[0].has_value());
    CHECK(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[1].has_value());
    CHECK(r.slots[1]->material.empty());
    REQUIRE(r.slots[3].has_value());
    CHECK(r.slots[3]->material.empty());
}

TEST_CASE("AD5X IFS COLOR-menu row with Z-Mod's ? type leaves the lane unlabelled",
          "[ams][ad5x_ifs][2086]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    REQUIRE_FALSE(backend.get_slot_info(3).has_filament_info());

    // The root menu's row for a lane just inserted, verbatim from bundle KHACGM8W.
    Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "// action:prompt_button 4: ?|RUN_ZCOLOR SLOT=4 HEX= TYPE=?|primary|");
    // A CHANGE_ZCOLOR carrying the placeholder is no edit either.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=4 TYPE=?");

    const SlotInfo info = backend.get_slot_info(3);
    CHECK(info.material.empty());
    // has_filament_info() is what the "Same spool?" offer keys on.
    CHECK_FALSE(info.has_filament_info());
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 3).value_or("") != "?");
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 3).has_value());
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS regression: COLOR palette render never moves the slot (#1065 bundle 482NB943)",
    "[ams][ad5x_ifs][1065][regression]") {
    // Field repro. Opening the COLOR macro's colour picker on slot 1 made the
    // swatch flicker to black before snapping back, over and over —
    // "multi-filament menu failed to keep up". Every one of the 24 echoed
    // candidates was applied in sequence, so the state MID-storm is the bug,
    // not just the end state. Assert after every single line.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "898989");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    const size_t syncs_before = Ad5xIfsTestAccess::external_sync_count(backend);

    for (const auto& hex : kZmodPalette) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, palette_button(1, "PETG", hex)));
        // Not just "ends correct" — never moves at all.
        REQUIRE(backend.get_slot_info(0).color_rgb == 0x898989);
        REQUIRE(backend.get_slot_info(0).material == "PETG");
    }

    for (const auto& type : kZmodMaterials) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, material_button(1, type, "898989")));
        REQUIRE(backend.get_slot_info(0).material == "PETG");
    }

    // Baselines held, so the confirming GET_ZCOLOR arriving ~1s later reads as
    // "unchanged" instead of firing a phantom "firmware color changed
    // #161616 -> #898989" the log was full of.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0).value_or(0) == 0x898989);
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "PETG");
    // And zero lane_data writes: 31 echoed buttons produced 31 Moonraker DB
    // round-trips before this (25 server.database.post_item requests queued in
    // a 300ms window on a 473MB MIPS AD5X, oldest aging to 1.15s).
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == syncs_before);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE_METHOD(QuietRunningAd5xIfs,
                 "AD5X IFS regression: type change surfaces on the menu re-render, not 40s later "
                 "(#1065 bundle 482NB943)",
                 "[ams][ad5x_ifs][1065][regression]") {
    // The user tapped SILK at 17:34:37.765. zmod applied it and re-rendered the
    // root menu 70ms later carrying "1: SILK". HelixScreen ignored that row,
    // waited on the debounced GET_ZCOLOR — which raced the firmware write and
    // returned the OLD type — and then had no trigger left, so the AMS panel
    // showed PETG until 17:35:17.342. Forty seconds of a wrong label.
    for (size_t i = 0; i < 4; ++i) {
        Ad5xIfsTestAccess::set_port_presence(backend, i, true);
    }
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    // "Select material type" renders; every entry is a candidate, PETG-CF last.
    for (const auto& type : kZmodMaterials) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, material_button(1, type, "F330F9")));
    }
    REQUIRE(backend.get_slot_info(0).material == "PETG"); // no phantom PETG-CF

    // User taps SILK. HelixScreen sends the gcode itself, so nothing about the
    // tap comes back down the response stream — the ONLY evidence is zmod's
    // re-render of the root menu with the new value.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(2, "PETG", "FFFFFF")));

    // Label tracks on the re-render, ~100ms after the tap.
    CHECK(backend.get_slot_info(0).material == "SILK");
    CHECK(backend.get_slot_info(0).color_rgb == 0xF330F9);
    // The untouched slot stays untouched — a menu render covers all four rows,
    // so it must not churn the ones that didn't change.
    CHECK(backend.get_slot_info(1).material == "PETG");
    CHECK(backend.get_slot_info(1).color_rgb == 0xFFFFFF);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 1).has_value());
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS regression: repeated COLOR-menu renders are idempotent (#1065 bundle 482NB943)",
    "[ams][ad5x_ifs][1065][regression]") {
    // The COLOR macro re-renders its root menu on every open and after every
    // edit — bundle 482NB943 has six renders in 80 seconds. A row that resynced
    // unconditionally would write lane_data each time; only a real delta may.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    const size_t before = Ad5xIfsTestAccess::external_sync_count(backend);
    // First render carries a real change (PETG -> SILK): one sync.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    const size_t after_first = Ad5xIfsTestAccess::external_sync_count(backend);
    CHECK(after_first > before);

    // Three more identical renders: nothing new to say, nothing written.
    for (int i = 0; i < 3; ++i) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    }
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == after_first);
    CHECK(backend.get_slot_info(0).material == "SILK");
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS regression: action-submenu buttons are not slot rows (#1065 bundle 482NB943)",
    "[ams][ad5x_ifs][1065][regression]") {
    // The per-slot action submenu ("Spool 1: SILK/magenta") renders Load/Unload
    // buttons alongside Change color/Change type. None of them is a `<n>: `
    // labelled row, so none may be read as a firmware snapshot — and IN_ZCOLOR
    // button definitions in particular fire at prompt-render time, not when the
    // load actually runs.
    //
    // The buttons below deliberately carry values that DISAGREE with our cached
    // firmware state. In the field they'd agree, but then "ignored the line" and
    // "applied a value that happened to match" are indistinguishable — and the
    // assertion has to be able to tell those apart to be worth anything.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "898989");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    const size_t syncs_before = Ad5xIfsTestAccess::external_sync_count(backend);

    for (const char* button :
         {"// action:prompt_button Change color|CHANGE_ZCOLOR SLOT=1 TYPE=SILK|primary|F330F9",
          "// action:prompt_button Change type|CHANGE_ZCOLOR SLOT=1 HEX=F330F9|primary",
          "// action:prompt_button Load|IN_ZCOLOR SLOT=1 NAPR=0|primary",
          "// action:prompt_button Unload|IN_ZCOLOR SLOT=1 NAPR=1|primary",
          "// action:prompt_footer_button Reset colors|RESET_ZCOLOR"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(backend, button));
    }

    CHECK(backend.get_slot_info(0).material == "PETG");
    CHECK(backend.get_slot_info(0).color_rgb == 0x898989);
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == syncs_before);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// Unload busy-state latency: publish the busy action synchronously on dispatch
// so the sidebar action buttons hide / context menu disables immediately,
// closing the re-tap window that produced "Unload failed: AMS is busy"
// (debug bundle 7L44W2B7). The UI observes the backend-agnostic ams_action
// subject, which AmsState only updates on EVENT_STATE_CHANGED — so the
// dispatch path MUST emit it, not wait for the next ~1.4s status frame.

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS unload publishes busy state + EVENT_STATE_CHANGED on dispatch",
                 "[ams][ad5x_ifs][unload]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    // Filament seated at the toolhead so unload routes to the heated toolhead
    // unload (not the cold lane-eject early return). current_slot < 0 + head
    // loaded is the unknown-origin recovery case; the firmware's FFMInfo.channel
    // record names the lane, so the unload needs no question.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_current_slot(backend, -1, true);
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    Ad5xIfsTestAccess::set_ffm_file_channel(backend, 2);

    int state_changed_events = 0;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == AmsBackend::EVENT_STATE_CHANGED) {
            ++state_changed_events;
        }
    });

    AmsError err = backend.unload_filament(-1);
    REQUIRE(err.success());

    // The busy action is published synchronously, BEFORE any temp/sensor frame.
    REQUIRE(backend.get_current_action() != AmsAction::IDLE);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    // And an EVENT_STATE_CHANGED fired on dispatch so the ams_action subject
    // updates immediately — this is the bit that was missing.
    REQUIRE(state_changed_events >= 1);
}

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS load publishes busy state + EVENT_STATE_CHANGED on dispatch",
                 "[ams][ad5x_ifs][load]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    int state_changed_events = 0;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == AmsBackend::EVENT_STATE_CHANGED) {
            ++state_changed_events;
        }
    });

    AmsError err = backend.load_filament(0);
    REQUIRE(err.success());

    REQUIRE(backend.get_current_action() != AmsAction::IDLE);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(state_changed_events >= 1);
}

// Granular step tracker (mirror Snapmaker): the AD5X synthesizes 3 phases for
// load/unload from extruder temp + head sensor; expose them as a backend step
// model + the operation_phase subject so the right-side vertical tracker shows
// "Heat nozzle -> Cut filament -> Retract" (unload) and
// "Heat nozzle -> Feed filament -> Purge" (load).

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS operation_step_model is the 3-phase synth sequence per direction",
                 "[ams][ad5x_ifs][stepmodel]") {
    const std::vector<std::string> unload{"Heat nozzle", "Cut filament", "Retract"};
    const std::vector<std::string> load{"Heat nozzle", "Feed filament", "Purge"};
    for (const auto& [op, labels] : {std::pair{StepOperationType::UNLOAD, unload},
                                     std::pair{StepOperationType::LOAD_FRESH, load},
                                     std::pair{StepOperationType::LOAD_SWAP, load}}) {
        auto model = operation_step_model(backend, op);
        REQUIRE(model.steps.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            CHECK(std::string(model.steps[i].label) == labels[i]);
            // phase_id mirrors the synthesized operation_phase index.
            CHECK(model.steps[i].phase_id == static_cast<int>(i));
            // Only the heat step shows a live nozzle temperature.
            CHECK(model.steps[i].live_temp == (i == 0));
        }
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS steps project the action its phase machine assigns",
                 "[ams][ad5x_ifs][coarse]") {
    const auto unload = backend.get_operation_step_model(StepOperationType::UNLOAD);
    CHECK(unload.action_at(0) == AmsAction::HEATING);
    CHECK(unload.action_at(1) == AmsAction::CUTTING);
    CHECK(unload.action_at(2) == AmsAction::UNLOADING);

    for (auto op : {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP}) {
        const auto load = backend.get_operation_step_model(op);
        CHECK(load.action_at(0) == AmsAction::HEATING);
        CHECK(load.action_at(1) == AmsAction::LOADING);
        CHECK(load.action_at(2) == AmsAction::PURGING);
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS get_operation_step_index_subject is the AmsState phase subject",
                 "[ams][ad5x_ifs][stepmodel]") {
    for (auto op :
         {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP, StepOperationType::UNLOAD}) {
        CHECK(operation_step_index_subject(backend, op) ==
              AmsState::instance().get_ams_operation_phase_subject());
    }
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: operation_phase advances 0->1->2 during unload",
                 "[ams][ad5x_ifs][phase]") {
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    // HEATING → step 0.
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0);

    // Reach target → CUTTING → step 1.
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);

    // Head drop → UNLOADING → step 2.
    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 2);

    // Timeout → ERROR; tracker clears the step (no active step shown).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == -1);
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: operation_phase advances 0->1->2 during load",
                 "[ams][ad5x_ifs][phase]") {
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);

    backend.fire_status(make_head_sensor(true));
    REQUIRE(backend.get_current_action() == AmsAction::PURGING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 2);

    // PURGING has a dedicated 240s budget (#1065 Bug 2), so age past it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(250));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == -1);
}

// Error-center bridge: IFS surfaces ERROR on timeout, current_error(), recover

// An operation that times out lands on ERROR, never IDLE, and surfaces a sticky
// CRITICAL IFS event whose first recovery is IFS_UNLOCK; recover() clears both.
TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS error-center: a timed-out op raises a recoverable CRITICAL event",
                 "[ams][ad5x_ifs][error-center]") {
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    REQUIRE_FALSE(backend.current_error().has_value());

    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->source == helix::ErrorSource::IFS);
    CHECK(ev->sticky);
    REQUIRE_FALSE(ev->recovery_actions.empty());
    CHECK(ev->recovery_actions[0].gcode == "IFS_UNLOCK");

    backend.recover();
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
    CHECK_FALSE(backend.current_error().has_value());
}

// Firmware-dropped-pointer wrong-lane unload cut (bundle 5HR3HHS6)
//
// current_slot < 0 (firmware dropped its active pointer) while IFS_STATUS
// "Chan" still reports a physically seated port. A tap on a NON-seated slot
// must cold-eject that lane, NOT toolhead-cut the seated one. A tap ON the
// seated slot must still take the heated toolhead unload.

TEST_CASE_METHOD(
    RunningAd5xIfs,
    "AD5X IFS unload of a NON-seated slot cold-ejects when firmware dropped active pointer "
    "(raza616 5HR3HHS6)",
    "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    // Plugin path: Chan does not drive current_slot (gated on !has_ifs_vars_),
    // so seated_chan_ can be set independently of current_slot.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // IFS_STATUS Chan=2 establishes the seated slot (port 2 -> slot 1).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Firmware has dropped its active pointer (current_slot = -1) but the head
    // sensor still reads loaded.
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // User taps Unload on slot 3 (NOT the seated slot 1).
    REQUIRE(backend.unload_filament(3).success());

    // Must cold-eject port 4 (slot 3 + 1), NOT toolhead-cut the seated slot 1.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=4"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=4 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=4"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE_METHOD(
    RunningAd5xIfs,
    "AD5X IFS unload of the seated slot still toolhead-cuts when firmware dropped pointer "
    "(raza616 5HR3HHS6)",
    "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // IFS_STATUS Chan=2 -> seated slot 1.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_ffm_file_channel(backend, 0);

    // User taps Unload on the seated slot 1 -> heated toolhead unload (cut),
    // declaring the lane because FFMInfo.channel is 0 (#1324).
    REQUIRE(backend.unload_filament(1).success());

    REQUIRE(backend.has_gcode("SET_EXTRUDER_SLOT SLOT=2\n_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F24"));
}

TEST_CASE_METHOD(
    QuietRunningAd5xIfs,
    "AD5X IFS unload routing regression: active-slot cut + non-active cold-eject preserved "
    "(FIX 1 existing-guard regression)",
    "[ams][ad5x_ifs]") {
    // current_slot >= 0 path must be untouched by the new firmware-dropped branch.
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    SECTION("active slot still toolhead-cuts") {
        REQUIRE(backend.unload_filament(2).success());
        REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
        REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    }

    SECTION("non-active slot still cold-ejects (existing #981 guard)") {
        REQUIRE(backend.unload_filament(0).success());
        REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=1"));
        REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
        REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=1"));
        REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    }
}

// FIX 2: eject_lane schedules a status re-read on success

TEST_CASE_METHOD(RunningAd5xIfs,
                 "AD5X IFS eject_lane schedules a status re-read on success (FIX 2)",
                 "[ams][ad5x_ifs]") {
    // Leave zcolor_silent_supported_ at its default (true) so schedule_zcolor_query
    // passes its gate and bumps the diagnostic schedule counter synchronously.
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    const uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(backend.eject_lane(1).success());

    // The successful eject must request a fresh status read so the UI refreshes.
    REQUIRE(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

// FIX 3: live RS-485 authority blocks stale JSON color/type for present slots

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS live authority blocks stale JSON color/type for a present slot (FIX 3)",
                 "[ams][ad5x_ifs]") {
    // Establish the live RS-485 authority via IFS_STATUS Ports (ifs_status_ports_seen_).
    // Slot 0 (port 1) is present; this is the authoritative presence + colour source.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 1;
    r.ifs_ports = std::vector<bool>{true, false, false, false};
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "00FF00"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Live colour/material the RS-485 source owns.
    Ad5xIfsTestAccess::set_color(backend, 0, "00FF00");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // A JSON poll carrying a DIFFERENT stale ffmColor/ffmType arrives.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FFA500",
            "ffmType1": "Silk"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // The live colour/material must survive — not be resurrected by stale cache.
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x00FF00);
    CHECK(info.material == "PLA");
}

TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS pre-SILENT JSON still seeds color/type when no live authority (FIX 3 regression)",
    "[ams][ad5x_ifs]") {
    // No live authority: SILENT demoted and IFS_STATUS Ports never seen.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FFA500",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // With no live source competing, the JSON cache IS the authority.
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xFFA500);
    CHECK(info.material == "ABS");
}
