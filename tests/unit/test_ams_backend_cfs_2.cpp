// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 43 to 68 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly [general],
// [filament_slot_override], [remap], [firmware_writeback], [slot-bounds][1623], and more

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/cfs_test_fixtures.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/force_bypass_controls_guard.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toast_log.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system/telemetry_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;
using helix::printer::AmsBackendCfs;
using helix::printer::CfsMaterialDb;
using helix::test::make_cfs_status_json;
using helix::test::make_unit_box_explicit;

using helix::test::CfsAutoRefillHelper;
using helix::test::CfsCalibHelper;
using helix::test::CfsInsertRuleRig;
using helix::test::CfsK1RemapHelper;
using helix::test::CfsK2CalibHelper;
using helix::test::CfsLifecycleHelper;
using helix::test::CfsLiveAckBackend;
using helix::test::CfsOverrideRig;
using helix::test::CfsRemapHelper;
using helix::test::CfsTmpCacheDir;
using helix::test::check_not_single_spool;
using helix::test::configfile_queries;
using helix::test::drain_calib_queue;
using helix::test::feed_toolhead_filament;
using helix::test::find_action;
using helix::test::FlatFailDispatchCfs;
using helix::test::HeldAckCfs;
using helix::test::json;
using helix::test::k2_plus_startup_box;
using helix::test::k2_plus_t1_connected;
using helix::test::kEditedRemainingG;
using helix::test::link_lane_four_to_spool_137;
using helix::test::MacroCacheScope;
using helix::test::make_boot_box_all_units_down;
using helix::test::make_cfs_notification;
using helix::test::make_empty_bus_box;
using helix::test::make_flat_box;
using helix::test::make_flat_fork_box;
using helix::test::make_fork_box_with_boxes;
using helix::test::make_gapped_fork_box;
using helix::test::make_multi_unit_box;
using helix::test::make_runout_box;
using helix::test::make_runout_removed_box;
using helix::test::make_runout_seated_box;
using helix::test::make_single_unit_box;
using helix::test::make_unbranded_bay_box;
using helix::test::make_unit_up_delta;
using helix::test::no_unit_settings;
using helix::test::paused_ctx;
using helix::test::per_slot_box;
using helix::test::per_slot_unit;
using helix::test::registered_unit_settings;
using helix::test::script_has_command;
using helix::test::SyncCfs;
using helix::test::toolhead_sensor_frame;
using helix::test::UnansweredClientCfs;
using helix::test::UnhomedCfsBackend;
using json = nlohmann::json;

TEST_CASE("CFS GCode helpers", "[ams][cfs]") {
    // The CR_BOX_* primitives don't park the toolhead — without wrapping,
    // CR_BOX_FLUSH extrudes onto the build plate instead of into the K2's
    // waste port. These assertions enforce the SAVE_GCODE_STATE +
    // BOX_GO_TO_EXTRUDE_POS / BOX_MOVE_TO_SAFE_POS envelope that mirrors
    // the K2 stock screen's LOAD_MATERIAL macro chain.
    SECTION("load gcode uses CR_BOX commands with TNN, wrapped in park envelope") {
        const std::string expected_a = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                       "BOX_SAVE_FAN\n"
                                       "BOX_GO_TO_EXTRUDE_POS\n"
                                       "BOX_MODE_WAIT\n"
                                       "CR_BOX_PRE_OPT\nCR_BOX_EXTRUDE TNN=T1A\n"
                                       "CR_BOX_WASTE\nCR_BOX_FLUSH TNN=T1A\nCR_BOX_END_OPT\n"
                                       "BOX_NOZZLE_CLEAN\n"
                                       "BOX_RESTORE_FAN\n"
                                       "BOX_MOVE_TO_SAFE_POS\n"
                                       "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0) == expected_a);

        REQUIRE(AmsBackendCfs::load_gcode(1).find("TNN=T1B") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        // Envelope: mode-wait + fan save/restore around the op.
        // No BOX_SET_TEMP — we deliberately don't lower a hotter pre-set
        // extruder target (e.g. PETG @ 240°C); cold extruders surface a
        // friendly key111 modal instead.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MODE_WAIT") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_RESTORE_FAN") != std::string::npos);
        // Load ends with fresh filament in the nozzle — wipe before parking.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_NOZZLE_CLEAN") != std::string::npos);

        REQUIRE(AmsBackendCfs::load_gcode(4).find("TNN=T2A") != std::string::npos);
    }

    SECTION("unload gcode uses CR_BOX commands inside park envelope") {
        const std::string g = AmsBackendCfs::unload_gcode();
        REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(g.find("CR_BOX_PRE_OPT\nBOX_CUT_MATERIAL\nBOX_MODE_WAIT\n"
                       "CR_BOX_RETRUDE\nCR_BOX_END_OPT") != std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        // Unload ends with retrude — nozzle is empty; no wipe needed.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("swap gcode combines unload and load inside park envelope") {
        for (int idx : {0, 1, 3}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx);
            REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
            REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
            REQUIRE(g.find("CR_BOX_PRE_OPT\nBOX_CUT_MATERIAL\nBOX_MODE_WAIT\nCR_BOX_RETRUDE\n"
                           "BOX_MODE_WAIT\nCR_BOX_EXTRUDE TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            // Swap ends with flush of the new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
        }
        // Invalid index
        REQUIRE(AmsBackendCfs::swap_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16).empty());
    }

    SECTION("reset gcode") {
        REQUIRE(AmsBackendCfs::reset_gcode() == "BOX_ERROR_CLEAR");
    }

    SECTION("recover gcode") {
        using V = helix::printer::CfsMacroVariant;
        // K2 keeps the box-specific resume.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K2) == "BOX_ERROR_RESUME_PROCESS");

        // K1 must NOT emit it: the K1 box extension registers no
        // cmd_error_resume_process (symbol-grepped from box_wrapper .so in
        // CR4CU220812S11 v2.3.5.34), so it returns "Unknown command" and the
        // box is never resumed. #1278.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) != "BOX_ERROR_RESUME_PROCESS");
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) == "RESUME");

        // BOX_TNN_RETRY_PROCESS exists on K1 but is NOT a substitute — it
        // retries a specific tool change and needs TNN/LAST_TNN context.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) != "BOX_TNN_RETRY_PROCESS");
    }
}

TEST_CASE("CFS K1 macro variant (#968)", "[ams][cfs]") {
    // K1 official CFS upgrade firmware uses plain BOX_* macros — no CR_ prefix,
    // no fan-save/mode-wait helpers. Reporter's K1C gcode/help shows
    // BOX_GO_TO_EXTRUDE_POS and BOX_MOVE_TO_SAFE_POS succeed but every
    // CR_BOX_* command returns key61 "Unknown command".
    using V = helix::printer::CfsMacroVariant;

    SECTION("K1 load gcode mirrors BOX_LOAD_MATERIAL_WITHOUT_MATERIAL (#968)") {
        // Fresh load, nozzle empty. Feed steps follow the firmware
        // BOX_LOAD_MATERIAL_WITHOUT_MATERIAL chain, with explicit TNN= on the
        // two commands that take it, and ADD the missing BOX_EXTRUDER_EXTRUDE
        // (root cause of "no auto-extrude after load"). BOX_MATERIAL_FLUSH is
        // bare: it takes LEN/VELOCITY/TEMP only, never TNN. Homing is handled
        // upstream by dispatch_action_script.
        const std::string expected_a = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                       "BOX_SAVE_FAN\n"
                                       "BOX_ERROR_CLEAR\n"
                                       "BOX_CHECK_MATERIAL\n"
                                       "BOX_GO_TO_EXTRUDE_POS\n"
                                       "BOX_EXTRUDE_MATERIAL TNN=T1A\n"
                                       "BOX_EXTRUDER_EXTRUDE TNN=T1A\n"
                                       "BOX_MATERIAL_FLUSH\n"
                                       "BOX_RESTORE_FAN\n"
                                       "BOX_MOVE_TO_SAFE_POS\n"
                                       "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0, V::K1) == expected_a);

        // No CR_BOX_* primitives on K1.
        const std::string g = AmsBackendCfs::load_gcode(1, V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("TNN=T1B") != std::string::npos);

        // The fix: BOX_EXTRUDER_EXTRUDE must appear between EXTRUDE and FLUSH.
        const auto pos_extrude = g.find("BOX_EXTRUDE_MATERIAL TNN=T1B");
        const auto pos_extruder = g.find("BOX_EXTRUDER_EXTRUDE TNN=T1B");
        const auto pos_flush = g.find("BOX_MATERIAL_FLUSH\n");
        REQUIRE(pos_extrude != std::string::npos);
        REQUIRE(pos_extruder != std::string::npos);
        REQUIRE(pos_flush != std::string::npos);
        REQUIRE(pos_extrude < pos_extruder);
        REQUIRE(pos_extruder < pos_flush);

        // K1 firmware lacks mode-wait — must not be emitted. Fan-save DOES
        // exist here and is required; see the envelope guard section (#1278).
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);

        // Fresh load mirror must NOT cut (nozzle is empty).
        REQUIRE(g.find("BOX_CUT_MATERIAL") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);

        // Envelope helpers that K1 DOES support.
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);

        // BOX_MATERIAL_FLUSH ends with a nozzle clean of its own, so a
        // trailing BOX_NOZZLE_CLEAN wipes a nozzle that was just wiped.
        // The firmware's WITHOUT_MATERIAL chain ends at the flush for the
        // same reason. The flush assertion keeps this from passing against
        // an empty or truncated script.
        REQUIRE(g.find("BOX_MATERIAL_FLUSH") != std::string::npos);
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);

        // A swap still wipes: there the clean precedes the new feed and
        // clears the OLD material after the cut, which no flush has run
        // for. Dropping it from the fresh load must not reach that path.
        REQUIRE(AmsBackendCfs::swap_gcode(1, V::K1).find("BOX_NOZZLE_CLEAN") != std::string::npos);
    }

    SECTION("K1 load TNN covers full unit/slot range") {
        // K1 firmware (per K1-Max box.cfg) uses the same TNN encoding as K2.
        // Spot-check first slot of each unit + last slot to lock in the
        // CfsMaterialDb::slot_to_tnn translation under the K1 path.
        struct Case {
            int idx;
            const char* tnn;
        };
        for (const auto& c : {Case{0, "T1A"}, Case{1, "T1B"}, Case{3, "T1D"}, Case{4, "T2A"},
                              Case{8, "T3A"}, Case{12, "T4A"}, Case{15, "T4D"}}) {
            const std::string g = AmsBackendCfs::load_gcode(c.idx, V::K1);
            const std::string needle = std::string("TNN=") + c.tnn;
            INFO("idx=" << c.idx << " expected " << needle);
            REQUIRE(g.find(needle) != std::string::npos);
        }
        // Out-of-range stays empty (same contract as K2).
        REQUIRE(AmsBackendCfs::load_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16, V::K1).empty());
    }

    SECTION("K1 unload gcode mirrors BOX_QUIT_MATERIAL (#968)") {
        // Mirrors the firmware BOX_QUIT_MATERIAL step list:
        // ERROR_CLEAR → CHECK_MATERIAL → CUT → RETRUDE → safe park.
        const std::string expected = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                     "BOX_SAVE_FAN\n"
                                     "BOX_ERROR_CLEAR\n"
                                     "BOX_CHECK_MATERIAL\n"
                                     "BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL\n"
                                     "BOX_RESTORE_FAN\n"
                                     "BOX_MOVE_TO_SAFE_POS\n"
                                     "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::unload_gcode(V::K1) == expected);
        // Sanity: no K2-only macros leaked in.
        const std::string g = AmsBackendCfs::unload_gcode(V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
        // BOX_SAVE_FAN is NOT K2-only — it exists on K1 too (symbol-verified in
        // CR4CU220812S11_ota_img_V2.3.5.34) and the envelope now emits it.
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        // Cut + retrude present.
        REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
        // Nozzle empty post-cut → no wipe.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("K1 swap gcode mirrors BOX_LOAD_MATERIAL_WITH_MATERIAL for slot 5 (T2B) (#968)") {
        // Nozzle loaded: cut old, retract, position, clean, then load new slot
        // with the missing BOX_EXTRUDER_EXTRUDE between EXTRUDE and FLUSH.
        // BOX_MATERIAL_FLUSH is bare — it has no TNN parameter.
        const std::string expected = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                     "BOX_SAVE_FAN\n"
                                     "BOX_ERROR_CLEAR\n"
                                     "BOX_CHECK_MATERIAL\n"
                                     "BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL\n"
                                     "BOX_GO_TO_EXTRUDE_POS\n"
                                     "BOX_NOZZLE_CLEAN\n"
                                     "BOX_EXTRUDE_MATERIAL TNN=T2B\n"
                                     "BOX_EXTRUDER_EXTRUDE TNN=T2B\n"
                                     "BOX_MATERIAL_FLUSH\n"
                                     "BOX_RESTORE_FAN\n"
                                     "BOX_MOVE_TO_SAFE_POS\n"
                                     "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::swap_gcode(5, V::K1) == expected);
        REQUIRE(expected.find("CR_BOX_") == std::string::npos);
    }

    SECTION("K1 swap spot checks across slot range") {
        for (int idx : {0, 1, 3, 5, 11, 15}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx, V::K1);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MATERIAL_FLUSH\n") != std::string::npos);
            // The missing extrude primitive must sit between EXTRUDE and FLUSH.
            REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=") < g.find("BOX_EXTRUDER_EXTRUDE TNN="));
            REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=") < g.find("BOX_MATERIAL_FLUSH\n"));
            // Swap ends with flush of new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        }
        REQUIRE(AmsBackendCfs::swap_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16, V::K1).empty());
    }

    SECTION("K1 envelope omits all K2-only helpers across all three operations") {
        // Regression guard. BOX_MODE_WAIT and the CR_BOX_* primitives are
        // genuinely absent from K1 and re-introducing them means key61 (#968).
        //
        // BOX_SAVE_FAN / BOX_RESTORE_FAN are NOT in that set, though this guard
        // once asserted they were. They are C-extension commands registered from
        // box_wrapper.cpython-38-mipsel-linux-gnu.so, never [gcode_macro]s, so
        // the box.cfg dump they were "verified absent" in could not have listed
        // them either way. Symbol grep of the extension in
        // CR4CU220812S11_ota_img_V2.3.5.34 shows cmd_save_fan / cmd_restore_fan
        // present. Omitting them left part-cooling blowing across the nozzle
        // through every K1 load, cut and flush (#1278).
        for (const std::string& g :
             {AmsBackendCfs::load_gcode(0, V::K1), AmsBackendCfs::unload_gcode(V::K1),
              AmsBackendCfs::swap_gcode(0, V::K1)}) {
            REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
            REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
            // Suppress before the body runs, restore after it — never the
            // reverse, or the op runs with the fan on and ends with it off.
            REQUIRE(g.find("BOX_SAVE_FAN") < g.find("BOX_RESTORE_FAN"));
            // The restore must precede the park so a raise mid-park still
            // leaves the fan correct.
            REQUIRE(g.find("BOX_RESTORE_FAN") < g.find("BOX_MOVE_TO_SAFE_POS"));
            REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            // Save/restore + park are common to all three K1 operations.
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
            REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        }
        // BOX_GO_TO_EXTRUDE_POS positions the toolhead over the waste port
        // before feeding fresh filament — load + swap need it, unload does NOT
        // (the BOX_QUIT_MATERIAL mirror only cuts + retracts, then parks).
        REQUIRE(AmsBackendCfs::load_gcode(0, V::K1).find("BOX_GO_TO_EXTRUDE_POS") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::swap_gcode(0, V::K1).find("BOX_GO_TO_EXTRUDE_POS") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode(V::K1).find("BOX_GO_TO_EXTRUDE_POS") ==
                std::string::npos);
    }

    SECTION("K1 BOX_MATERIAL_FLUSH is emitted bare — it has no TNN parameter (#968)") {
        // Genuine K1 CFS firmware (Creality OTA V2.3.5.34, board CR4CU220812S11):
        // the shipped box.cfg — byte-identical across all 11 per-model configs —
        // documents the parameter forms as
        //     BOX_EXTRUDE_MATERIAL TNN=T1A
        //     BOX_EXTRUDER_EXTRUDE TNN=T1A
        //     BOX_MATERIAL_FLUSH LEN=100 VELOCITY=360 TEMP=220
        //     BOX_RETRUDE_MATERIAL_WITH_TNN TNN=T1A
        // TNN is on three commands and deliberately not on the flush, and every
        // shipped BOX_LOAD_MATERIAL_* macro invokes the flush bare. Disassembly
        // of box_wrapper...so agrees: cmd_material_flush reads only LEN /
        // VELOCITY / TEMP and never touches TNN or the Tnn_map lookup.
        //
        // Sending TNN= is inert rather than fatal (Klipper's GCodeCommand
        // ignores unread parameters), which is exactly why it needs pinning —
        // nothing at runtime would ever complain if it came back.
        //
        // Extract each emitted flush line and require it to be exactly bare.
        auto flush_lines = [](const std::string& g) {
            std::vector<std::string> out;
            const std::string needle = "BOX_MATERIAL_FLUSH";
            for (size_t p = g.find(needle); p != std::string::npos; p = g.find(needle, p + 1)) {
                const size_t end = g.find('\n', p);
                out.push_back(g.substr(p, end == std::string::npos ? std::string::npos : end - p));
            }
            return out;
        };

        // Load and swap both flush; unload does not (nozzle is empty post-cut).
        for (int idx : {0, 1, 5, 15}) {
            for (const std::string& g :
                 {AmsBackendCfs::load_gcode(idx, V::K1), AmsBackendCfs::swap_gcode(idx, V::K1)}) {
                const auto lines = flush_lines(g);
                INFO("idx=" << idx << " gcode:\n" << g);
                REQUIRE(lines.size() == 1);
                REQUIRE(lines[0] == "BOX_MATERIAL_FLUSH");
            }
        }
        // Unload emits no flush at all.
        REQUIRE(flush_lines(AmsBackendCfs::unload_gcode(V::K1)).empty());

        // No K1 builder may put TNN on the flush, and none may reach for the
        // TNN-aware BOX_MATERIAL_CHANGE_FLUSH — it exists in this firmware but
        // no shipped macro uses it, so adopting it would be a behavior change.
        for (const std::string& g :
             {AmsBackendCfs::load_gcode(0, V::K1), AmsBackendCfs::unload_gcode(V::K1),
              AmsBackendCfs::swap_gcode(0, V::K1)}) {
            REQUIRE(g.find("BOX_MATERIAL_FLUSH TNN=") == std::string::npos);
            REQUIRE(g.find("BOX_MATERIAL_CHANGE_FLUSH") == std::string::npos);
        }

        // The commands that genuinely DO take TNN must keep it — this guard
        // must not be "fixed" by stripping TNN everywhere.
        const std::string load = AmsBackendCfs::load_gcode(5, V::K1);
        REQUIRE(load.find("BOX_EXTRUDE_MATERIAL TNN=T2B") != std::string::npos);
        REQUIRE(load.find("BOX_EXTRUDER_EXTRUDE TNN=T2B") != std::string::npos);
    }

    SECTION("K2 default preserved when variant omitted") {
        // Existing call sites without variant arg must still emit K2 macros.
        REQUIRE(AmsBackendCfs::load_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode().find("CR_BOX_RETRUDE") != std::string::npos);
        REQUIRE(AmsBackendCfs::swap_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
    }
}

// #968 — WITH/WITHOUT-material selection keys on filament-at-nozzle,
// NOT a preloaded (cassette) slot. K1 CFS reports a preloaded slot index with
// the nozzle still EMPTY; cutting in that state is the reporter's "hallucinated
// cut on empty nozzle" bug.
TEST_CASE("CFS change_tool selects load-vs-swap from filament_loaded (#968)", "[ams][cfs][k1]") {
    helix::test::StartedBackend<CfsK1RemapHelper> backend;
    // change_tool's slot bound is the attached unit count, so the box must
    // have reported its lanes before a tool can name one.
    backend.fire_status(make_cfs_notification(make_multi_unit_box(1)));

    SECTION("nozzle EMPTY but slot preloaded → fresh load (no cut)") {
        // The exact K1 CFS state from the report: current_slot >= 0 (a cassette
        // is staged) but filament_loaded == false (nothing at the nozzle yet).
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/false,
                                        /*current_slot=*/2);
        REQUIRE(backend.change_tool(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        const std::string& g = backend.dispatched[0];
        // Fresh load mirror — NO cut/retract.
        REQUIRE(g.find("BOX_CUT_MATERIAL") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);
        // Load primitives present, including the EXTRUDER_EXTRUDE fix.
        REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=T1A") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=T1A") != std::string::npos);
    }

    SECTION("filament physically at nozzle → swap (cut first)") {
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true,
                                        /*current_slot=*/2);
        REQUIRE(backend.change_tool(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        const std::string& g = backend.dispatched[0];
        // Swap mirror — cut + retract the old filament before loading new.
        REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=T1A") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=T1A") != std::string::npos);
    }
}

// push_slot_identity_to_firmware (#968 material-type writeback)
//
// The push writes color_value always, and material_type as the code
// material_code_locked() finds: a code this box reported, else the catalog's
// generic code in the 0-prefix form. A branded code the box never reported is
// never synthesized: its prefix digit is the box's own.
//
// K1 codes below (000001 = Generic PLA, 000003 = Generic PETG) come from the
// #968 reporter's real tn_data.json / box dumps.
TEST_CASE("CFS push_slot_identity_to_firmware writes color + material code",
          "[ams][cfs][firmware_writeback][968]") {
    helix::test::StartedBackend<CfsRemapHelper> helper;

    SECTION("no code reported: the generic catalog code, then the same-material refresh") {
        helper.fire_status(make_cfs_notification(make_multi_unit_box(1)));
        REQUIRE(helper.push(0, "PETG", "Generic", "", 0xFF0000).success());
        // The follow-up BOX_UPDATE_SAME_MATERIAL_LIST mirrors Creality's own
        // master-server, which refreshes the auto-refill equivalence groups
        // after every BOX_MODIFY_TN_DATA write (group membership requires
        // exact color equality).
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=material_type DATA=000003\n"
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("a branded product the box never reported falls back to the generic code") {
        helper.fire_status(make_cfs_notification(
            make_single_unit_box({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"})));
        REQUIRE(helper.push(1, "PLA", "Creality", "creality-hyper-pla", 0x00FF00).success());
        REQUIRE(helper.captured.size() == 2);
        CAPTURE(helper.captured[0]);
        CHECK(helper.captured[0].find("PART=material_type DATA=000001") != std::string::npos);
    }

    SECTION("K1: brand|type observed on another slot → material + color written") {
        // Slot 1 carries the PETG spool (K1 code 000003); the user edits
        // slot 0. The observed vocabulary is system-wide, not per-slot.
        helper.fire_status(make_cfs_notification(
            make_single_unit_box({"-1", "000003", "-1", "-1"}, {"-1", "01B04AE", "-1", "-1"})));
        helper.push(0, "PETG", "Generic", "", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0] ==
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=material_type DATA=000003\n"
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000");
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("a branded code the box reported is replayed in its full form") {
        helper.fire_status(make_cfs_notification(
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF0000", "-1", "-1", "-1"})));
        helper.push(1, "PLA", "Creality", "creality-hyper-pla", 0x00FF00);
        REQUIRE(helper.captured[0].find("PART=material_type DATA=101001") != std::string::npos);
    }

    SECTION("catalog product pick resolves through its own cfs code id") {
        helper.fire_status(make_cfs_notification(
            make_single_unit_box({"-1", "-1", "000003", "-1"}, {"-1", "-1", "0000000", "-1"})));
        helper.push(0, "PETG", "Generic", "generic-petg", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0].find("PART=material_type DATA=000003") != std::string::npos);
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("a material with no code is not written; the record holds it") {
        helper.fire_status(make_cfs_notification(
            make_single_unit_box({"000003", "-1", "-1", "-1"}, {"01B04AE", "-1", "-1", "-1"})));
        helper.push(2, "Unobtanium", "ACME", "", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0] ==
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=C PART=color_value DATA=0FF0000");
    }

    SECTION("an emptied material and colour write the box's own unset value") {
        helper.fire_status(make_cfs_notification(make_multi_unit_box(1)));
        helper.push(0, "", "", "", helix::kAmsDefaultSlotColor);
        REQUIRE(helper.captured[0] == "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=material_type DATA=-1\n"
                                      "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=-1");
    }

    SECTION("invalid slot index refuses the write (must not crash klippy)") {
        CHECK_FALSE(helper.push(99, "PETG", "Generic", "", 0xFF0000).success());
        REQUIRE(helper.captured.empty());
    }
}

TEST_CASE("CFS backend ctor latches macro variant from PrinterDetector (#968)", "[ams][cfs]") {
    // The constructor reads PrinterDetector::is_creality_k1() once and caches
    // the result. Verify both routes resolve correctly.
    auto* config = Config::get_instance();
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1C");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K1 Max printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1 Max");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K2 Plus printer type → K2 dialect") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K2);
    }

    SECTION("Unset printer type → K2 dialect (safe default)") {
        config->set<std::string>(type_path, "");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K2);
    }

    // Restore prior config so we don't bleed into adjacent tests.
    config->set<std::string>(type_path, saved);
}

TEST_CASE("PrinterDiscovery enables CFS for K1 box object (#968 gate)", "[ams][cfs][discovery]") {
    // K1 + `box` → CFS enabled, K1 dialect chosen downstream by AmsBackendCfs
    // ctor.
    auto* config = Config::get_instance();
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C + box object enables CFS") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("K2 Plus + box object enables CFS (regression check)") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("No box object → no CFS regardless of printer type") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_mmu());
    }

    config->set<std::string>(type_path, saved);
}

TEST_CASE("Material comfort ranges", "[filament]") {
    auto range = filament::get_comfort_range("PLA");
    REQUIRE(range.has_value());
    REQUIRE(range->max_humidity_good == Catch::Approx(50.0f));
    REQUIRE(range->max_humidity_warn == Catch::Approx(65.0f));

    auto petg = filament::get_comfort_range("PETG");
    REQUIRE(petg.has_value());
    REQUIRE(petg->max_humidity_good == Catch::Approx(40.0f));

    REQUIRE_FALSE(filament::get_comfort_range("UNKNOWN_MATERIAL").has_value());
}

TEST_CASE("CFS backend has environment sensors", "[ams][cfs]") {
    // CFS units have built-in temperature and humidity sensors; every other
    // backend inherits AmsBackend's default of false, and ui_ams_detail.cpp
    // gates the whole environment card on this returning true.
    //
    // (The old body was REQUIRE(true) with a comment claiming a compile-time
    // check. AmsBackendCfs is perfectly constructible with a null API - the
    // rest of this file does it all over - so just ask the object.)
    helix::test::RegisteredBackend<AmsBackendCfs> backend_reg(nullptr, nullptr);
    AmsBackendCfs& backend = *backend_reg;

    static_assert(
        std::is_same_v<decltype(std::declval<const AmsBackendCfs&>().has_environment_sensors()),
                       bool>,
        "ui_ams_detail.cpp branches on a bool from has_environment_sensors()");

    REQUIRE(backend.has_environment_sensors());
}

// CFS BOX_MODIFY_TN tool remap (set_tool_mapping)

TEST_CASE("CFS set_tool_mapping emits BOX_MODIFY_TN with TNN keys/values", "[ams][cfs][remap]") {
    helix::test::StartedBackend<CfsRemapHelper> helper;
    // The whole TNN alphabet is addressable, so this box reports all four
    // units: 16 bays, and index 16 past the last one.
    helper.fire_status(make_cfs_notification(make_multi_unit_box(4)));

    SECTION("Identity within unit 1: T1A → T1A") {
        auto err = helper.set_tool_mapping(0, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured.size() == 1);
        REQUIRE(helper.captured[0] == "BOX_MODIFY_TN T1A=T1A");
    }

    SECTION("Cross-unit remap: tool 0 (T1A) → slot 5 (T2B)") {
        auto err = helper.set_tool_mapping(0, 5);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T1A=T2B"});
    }

    SECTION("Last slot: tool 15 (T4D) → slot 0 (T1A)") {
        auto err = helper.set_tool_mapping(15, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T4D=T1A"});
    }

    SECTION("Tool number out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(16, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(-1, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Slot index out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(0, 16);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(0, -1);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Multiple calls send gcode in order") {
        REQUIRE(helper.set_tool_mapping(0, 1).result == AmsResult::SUCCESS);
        REQUIRE(helper.set_tool_mapping(2, 7).result == AmsResult::SUCCESS);
        REQUIRE(helper.captured ==
                std::vector<std::string>{"BOX_MODIFY_TN T1A=T1B", "BOX_MODIFY_TN T1C=T2D"});
    }
}

// CFS refresh_rfid → BOX_INFO_REFRESH RFID probe (prestonbrown/helixscreen#1077)
//
// Inserting a spool does not auto-read its RFID tag; the box reports sentinel
// vender/color/material until BOX_INFO_REFRESH scans it. The "Refresh RFID"
// device action probes every connected unit: ADDR = 1-based unit index, NUM=15
// (0b1111) = all four slots. Verified on K2 Plus.
TEST_CASE("CFS refresh_rfid probes each connected unit via BOX_INFO_REFRESH",
          "[ams][cfs][refresh]") {
    // Neutralize on_started() — its printer.objects.query needs a live client.
    struct Helper : CfsRemapHelper {
        void on_started() override {}
    };
    Helper h;

    SECTION("single connected unit → ADDR=1 NUM=15") {
        CfsTestAccess::set_connected_units(h, 1);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured == std::vector<std::string>{"BOX_INFO_REFRESH ADDR=1 NUM=15"});
    }

    SECTION("two connected units → one probe each, ADDR follows unit index") {
        CfsTestAccess::set_connected_units(h, 2);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured == std::vector<std::string>{"BOX_INFO_REFRESH ADDR=1 NUM=15",
                                                       "BOX_INFO_REFRESH ADDR=2 NUM=15"});
    }

    SECTION("no connected units → no gcode emitted") {
        CfsTestAccess::set_connected_units(h, 0);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured.empty());
    }
}

// CFS BOX_MODIFY_TN_DATA color firmware-writeback (push_slot_identity_to_firmware)
//
// Format reverse-engineered from K2's master-server binary
// (`strings /usr/bin/master-server | grep BOX_MODIFY_TN_DATA`):
//   BOX_MODIFY_TN_DATA ADDR=<1..4> NUM=<A|B|C|D> PART=color_value DATA=0RRGGBB
//
// Validated round-trip on K2 Plus 2026-05-10 — see
// .claude/scratchpad/research/k2-box-firmware-writeback.md.
//
// CRITICAL: invalid args trigger box_wrapper TypeError → klippy invoke_shutdown.
// These tests cover the validation guards in push_slot_identity_to_firmware that
// prevent any out-of-range slot index or zero-color sentinel from reaching the
// firmware as a malformed gcode.

TEST_CASE("CFS push_slot_identity_to_firmware emits BOX_MODIFY_TN_DATA",
          "[ams][cfs][firmware_writeback]") {
    helix::test::StartedBackend<CfsRemapHelper> helper;
    // The ADDR/NUM arithmetic spans the whole TNN alphabet, so this box
    // reports all four units and every index below is an attached bay.
    helper.fire_status(make_cfs_notification(make_multi_unit_box(4)));

    SECTION("Slot 0 (T1A) red 0xFF0000 → ADDR=1 NUM=A DATA=0FF0000") {
        helper.push(0, "Unobtanium", "", "", 0xFF0000);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Slot 5 (T2B) green 0x00FF00 → ADDR=2 NUM=B DATA=000FF00") {
        helper.push(5, "Unobtanium", "", "", 0x00FF00);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=2 NUM=B PART=color_value DATA=000FF00",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Slot 15 (T4D) white 0xFFFFFF → ADDR=4 NUM=D DATA=0FFFFFF") {
        helper.push(15, "Unobtanium", "", "", 0xFFFFFF);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=4 NUM=D PART=color_value DATA=0FFFFFF",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Color masks high bits — alpha byte from caller is ignored") {
        // If a caller passes 0xAA112233, the alpha byte is dropped. The
        // firmware's leading nibble is always our own '0'.
        helper.push(0, "Unobtanium", "", "", 0xAA112233);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0112233",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }
}

TEST_CASE("CFS push_slot_identity_to_firmware skips invalid inputs (must NOT crash klippy)",
          "[ams][cfs][firmware_writeback]") {
    helix::test::StartedBackend<CfsRemapHelper> helper;

    SECTION("color_rgb == 0 (pure black) is a valid color and DOES dispatch") {
        // Pure black is a legitimate user choice and a real firmware-detected
        // color (K2 reports loaded black PLA as 0x000000). The push helper
        // must NOT skip it — that's the bug fixed by the color_set boolean.
        // Caller (apply_user_edit) is responsible for not invoking when color
        // wasn't actually set (color_set=false on the override).
        helper.fire_status(make_cfs_notification(make_multi_unit_box(1)));
        helper.push(0, "Unobtanium", "", "", 0);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0000000",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Negative slot index is skipped") {
        CHECK_FALSE(helper.push(-1, "", "", "", 0xFF0000).success());
        REQUIRE(helper.captured.empty());
    }

    SECTION("slot_index >= 16 is skipped") {
        helper.push(16, "", "", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
        helper.push(99, "", "", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
    }
}

TEST_CASE("CFS set_tool_mapping updates local tool_to_slot_map", "[ams][cfs][remap]") {
    // Print start snapshots get_tool_mapping() (AmsState::pending_tool_map_restore())
    // before sending remaps and replays it on print end to restore the prior
    // configuration. If get_tool_mapping() returned an empty vector after a
    // successful set_tool_mapping, the restore path would be a no-op.
    helix::test::StartedBackend<CfsRemapHelper> helper;
    auto status = make_cfs_status_json();
    json notification = {{"method", "notify_status_update"}, {"params", json::array({status, 0})}};
    helper.fire_status(notification);

    auto baseline = helper.get_tool_mapping();
    REQUIRE_FALSE(baseline.empty()); // box.map populated by handle_status

    SECTION("a remap onto a connected lane moves BOTH directions") {
        // This payload connects T1 only — T2 reports state "None" — so the box
        // has four lanes, 0..3, and lane 3 is one of them.
        REQUIRE(helper.set_tool_mapping(1, 3).result == AmsResult::SUCCESS);

        auto after = helper.get_tool_mapping();
        REQUIRE(after.size() == baseline.size());
        REQUIRE(after[1] == 3);

        // The reverse direction has to follow, or the AMS panel badges one lane
        // while the filament panel's Load/Unload buttons act on another.
        auto info = helper.get_system_info();
        const auto* lane3 = info.get_slot_global(3);
        REQUIRE(lane3 != nullptr);
        CHECK(lane3->mapped_tool == 1);

        // Both losing sides give up their claim: lane 1 does not answer to T1,
        // and T3 does not resolve to the lane T1 just took.
        const auto* lane1 = info.get_slot_global(1);
        REQUIRE(lane1 != nullptr);
        CHECK(lane1->mapped_tool == -1);
        CHECK(after[3] == -1);
    }

    SECTION("a remap onto a lane the box has not reported is refused") {
        // Slot 5 lives in unit T2, which this payload reports as state "None"
        // — not connected. Sending BOX_MODIFY_TN anyway would leave firmware's
        // routing table pointing at a bay that cannot feed, so the remap is
        // refused outright and neither direction of the local map moves.
        // (#1623)
        REQUIRE(helper.set_tool_mapping(1, 5).result == AmsResult::INVALID_SLOT);
        REQUIRE(helper.captured.empty());

        auto after = helper.get_tool_mapping();
        CHECK(after == baseline);

        // The invariant, stated directly: every forward entry names a lane that
        // exists and claims that tool back.
        auto info = helper.get_system_info();
        for (int t = 0; t < static_cast<int>(after.size()); ++t) {
            int slot = after[static_cast<size_t>(t)];
            if (slot < 0) {
                continue;
            }
            INFO("T" << t << " resolves to slot " << slot);
            const auto* lane = info.get_slot_global(slot);
            REQUIRE(lane != nullptr);
            CHECK(lane->mapped_tool == t);
        }
    }
}

// Slot-index bounds (prestonbrown/helixscreen#1623)
//
// total_slots is the attached unit count (four bays per unit), so a one-unit
// box has bays 0-3 and index 7 names a bay on hardware that is not attached.
// Every entry point must answer that index with the same bound: the attached
// count. The bypass sentinel is exempt everywhere — it is a target, not a bay.
TEST_CASE("CFS slot bounds agree across entry points (#1623)", "[ams][cfs][slot-bounds][1623]") {
    helix::test::StartedBackend<CfsRemapHelper> helper;

    helper.fire_status(make_cfs_notification(make_single_unit_box(
        {"101001", "101001", "101001", "101001"}, {"0000000", "0FFFFFF", "00A2989", "0C12E1F"})));
    REQUIRE(helper.get_system_info().total_slots == 4);

    SECTION("set_tool_mapping refuses a slot on an unattached unit") {
        auto err = helper.set_tool_mapping(0, 7);
        REQUIRE(err.result == AmsResult::INVALID_SLOT);
        REQUIRE(helper.captured.empty());
    }

    SECTION("a remap's tool number is bounded by the alphabet, not the attached count") {
        auto err = helper.set_tool_mapping(7, 1);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE_FALSE(helper.captured.empty());
    }

    SECTION("load_filament refuses a slot on an unattached unit") {
        auto err = helper.load_filament(7);
        REQUIRE(err.result == AmsResult::INVALID_SLOT);
        REQUIRE(helper.dispatched.empty());
    }

    SECTION("change_tool's bound is the TNN alphabet, not the attached count") {
        // `tool` is a routing key over firmware's T0-T15 table, not a bay
        // index: plan_load() feeds a lane's mapped_tool here verbatim, and
        // that key can sit high while fewer units are attached. Which bay the
        // key resolves to is the table's business; encodability is the only
        // question this bound answers.
        auto err = helper.change_tool(7);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE_FALSE(helper.dispatched.empty());
    }

    SECTION("push_slot_identity_to_firmware skips an unattached bay") {
        helper.push(7, "PLA", "Generic", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
    }

    SECTION("every refusal quotes the attached count, not the family maximum") {
        auto mapping_err = helper.set_tool_mapping(0, 7);
        REQUIRE(mapping_err.result == AmsResult::INVALID_SLOT);

        SlotInfo edit;
        edit.material = "PLA";
        edit.color_rgb = 0xFF5500;
        auto edit_err = helix::test::apply_edit(helper, 7, edit);
        REQUIRE(edit_err.result == AmsResult::INVALID_SLOT);

        // set_slot_info has always quoted the attached count; a mismatch here
        // is one of the entry points still spelling the bound as 16 or 15.
        REQUIRE(mapping_err.technical_msg == edit_err.technical_msg);
        REQUIRE(mapping_err.technical_msg.find("(0-3)") != std::string::npos);
    }

    SECTION("an attached bay still passes: set_tool_mapping and load_filament") {
        REQUIRE(helper.set_tool_mapping(0, 3).result == AmsResult::SUCCESS);
        REQUIRE(helper.load_filament(3).result == AmsResult::SUCCESS);
    }

    SECTION("an attached bay still passes: change_tool") {
        // Its own section: a dispatched load marks the backend busy, so the
        // two ops cannot share a run.
        REQUIRE(helper.change_tool(3).result == AmsResult::SUCCESS);
    }
}

// A backend that has parsed no box frame: total_slots is 0, which means the
// box size is UNKNOWN, not that the box has no bays. The print-start mapping
// restore fires on klippy READY, unordered against the first box frame, and
// its recovery record is deleted when a send is refused - so the pre-frame
// window must stay permissive, bounded only by the TNN alphabet.
TEST_CASE("CFS slot bounds before the first box frame (#1623)", "[ams][cfs][slot-bounds][1623]") {
    CfsRemapHelper helper;
    helper.mark_running();
    REQUIRE(helper.get_system_info().total_slots == 0);

    SECTION("a remap before the first box frame is not refused") {
        auto err = helper.set_tool_mapping(0, 2);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE_FALSE(helper.captured.empty());
    }

    SECTION("a load before the first box frame dispatches") {
        auto err = helper.load_filament(2);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE_FALSE(helper.dispatched.empty());
    }

    SECTION("an identity write before the first box frame still leaves the app") {
        helper.push(2, "PLA", "Generic", "", 0xFF0000);
        REQUIRE_FALSE(helper.captured.empty());
    }

    SECTION("a slot past the TNN alphabet is refused even unparsed") {
        auto err = helper.set_tool_mapping(0, 16);
        REQUIRE(err.result == AmsResult::INVALID_SLOT);
    }
}

TEST_CASE("CFS declares a persistent remap route and owns its table", "[ams][cfs][remap]") {
    CfsRemapHelper helper;
    REQUIRE(helper.owns_tool_mapping_table());
    REQUIRE(helix::printer::can_remap(helper));
    REQUIRE(helix::printer::remap_is_persistent(helper.get_remap_strategy()));
}

// CFS action state in operations

TEST_CASE("CFS GCode generation for all operations", "[ams][cfs]") {
    SECTION("load gcode uses TNN for multi-unit addressing") {
        // Unit 1 slots
        REQUIRE(AmsBackendCfs::load_gcode(0).find("TNN=T1A") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(3).find("TNN=T1D") != std::string::npos);
        // Unit 4 last slot
        REQUIRE(AmsBackendCfs::load_gcode(15).find("TNN=T4D") != std::string::npos);
    }

    SECTION("load gcode rejects out of range") {
        REQUIRE(AmsBackendCfs::load_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16).empty());
    }
}

TEST_CASE("CFS has no per-slot prep sensors", "[ams][cfs]") {
    // CFS tracks slot inventory via material database (RFID/software), not
    // per-gate optical sensors. slot_has_prep_sensor must return false so the
    // filament path canvas draws continuous lines without sensor dot gaps.
    helix::test::RegisteredBackend<AmsBackendCfs> backend_reg(nullptr, nullptr);
    AmsBackendCfs& backend = *backend_reg;

    SECTION("all slots report no prep sensor") {
        for (int i = 0; i < 16; i++) {
            REQUIRE_FALSE(backend.slot_has_prep_sensor(i));
        }
    }
}

// Filament slot override integration

TEST_CASE("CFS apply_user_edit writes to store", "[ams][cfs][filament_slot_override]") {
    CfsTmpCacheDir tmp("task14_persist_true");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Prime the backend with 4 slots so apply_user_edit's index check passes.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = helix::test::apply_edit(backend, 0, edit);
    REQUIRE(err.success());
    // Klipper's answer to the box write lands through the update queue.
    helix::ui::UpdateQueue::instance().drain();

    // In-memory map carries the override.
    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async.
    auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");

    // Legacy namespace NOT touched — CFS does not write there.
    CHECK(mock_printer.client.mock_db_get("helix-screen", "cfs_slot_overrides").is_null());
}

TEST_CASE("CFS clear_slot_override drops the whole Spoolman link",
          "[ams][cfs][filament_slot_override][1625]") {
    CfsTmpCacheDir tmp("clear_spoolman_link");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // A box parse populates the slots a clear walks.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box));
    REQUIRE(CfsTestAccess::seed_live_spoolman_link(backend, 0, 42, 77, 3));
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 77);
    const SlotInfo before = backend.get_slot_info(0);
    REQUIRE_FALSE(before.brand.empty());

    backend.clear_slot_override(0);

    // All three handles die together: a slot cleared of its override must not
    // keep naming a Spoolman record, and a surviving filament handle would.
    // The RFID-kept fields (brand / color_name / total_weight_g) are pinned
    // by the hardware-event clear test above.
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.spoolman_filament_id == 0);
    CHECK(info.spool_name.empty());
    // The RFID material database supplies these, so the clear keeps them.
    CHECK(info.brand == before.brand);
    CHECK(info.color_name == before.color_name);
    CHECK(info.total_weight_g == before.total_weight_g);
}

TEST_CASE("CFS first RFID observation does NOT clear override",
          "[ams][cfs][filament_slot_override]") {
    // Even when the override was saved against a different (now-stale)
    // fingerprint, the very first observation is a BASELINE and must never
    // fire a clear. Matches Snapmaker semantics.
    CfsTmpCacheDir tmp("task14_first_uid_baseline");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a fingerprint on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear. Override survives.
    json box = make_single_unit_box({"999001", "101001", "101001", "101001"},
                                    {"0123456", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box));

    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Second parse of the SAME fingerprint stays the baseline — no clear.
    backend.fire_status(make_cfs_notification(box));

    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
}

TEST_CASE("CFS empty RFID fingerprint does not update baseline or clear",
          "[ams][cfs][filament_slot_override]") {
    // Sentinel material_type "-1" / color_value "-1" = no tag / reader
    // disabled / unreadable. Must not update the baseline and must not clear.
    // This is the contract that keeps transient tag-read failures from
    // masking a genuine hardware swap on the next good read.
    CfsTmpCacheDir tmp("task14_empty_uid_noop");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid fingerprint — baseline established.
    json box1 = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box1));
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");

    // Second parse: slot 0 has SENTINEL material_type and color — empty
    // fingerprint. Must NOT update baseline and must NOT clear the override.
    json box2 = make_single_unit_box({"-1", "101001", "101001", "101001"},
                                     {"-1", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box2));
    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500"); // unchanged
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Third parse: same original fingerprint — matches baseline, no clear.
    // Proves the sentinel-UID pass didn't corrupt state.
    backend.fire_status(make_cfs_notification(box1));
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
}

// Presence/loaded-truth fixes. The firmware `box` object's RFID-derived fields
// (color_value/material_type) latch stale data and read sentinels for untagged
// 3rd-party spools, and box.filament is a SELECTION index — not a loaded flag.

// box.filament is a stale active-lane SELECTION pointer, NOT a loaded
// flag. With box.filament=1 but T1.filament="None" and no toolhead sensor, the
// system must report current_slot == -1 and filament_loaded == false.
TEST_CASE("CFS: box.filament selection index does not fake a loaded slot", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_box_filament");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // box.filament = 1 (a lane is "selected"), but no lane is active
    // (T1.filament = "None") and there is no toolhead sensor in this update.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    box["filament"] = 1;
    box["T1"]["filament"] = "None";
    backend.fire_status(make_cfs_notification(box));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_slot == -1);
    REQUIRE(sys.filament_loaded == false);
}

// A partial top-level-only box update (e.g.
// box:{filament:N} when a lane is selected during a tool change) carries no
// per-unit lane data, so it must NOT clear a still-valid active slot. The clear
// is gated on has_unit_data.
TEST_CASE("CFS: partial box.filament update does not clear active slot", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_partial_no_clobber");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // 1) Full update with lane A active → current_slot = 0.
    json full = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    full["T1"]["filament"] = "A";
    backend.fire_status(make_cfs_notification(full));
    REQUIRE(backend.get_system_info().current_slot == 0);

    // 2) Partial top-level-only update (selection index changes, no T-unit
    //    data). Must leave the active slot untouched.
    json partial = json::object();
    partial["filament"] = 2;
    backend.fire_status(make_cfs_notification(partial));

    REQUIRE(backend.get_system_info().current_slot == 0);
}

// The toolhead sensor is the SOLE writer of
// filament_loaded. A sensor update sets it true; a SUBSEQUENT box-only update
// (carrying box.filament but no sensor param) must NOT clobber it back to false.
TEST_CASE("CFS: box-only update does not clobber sensor-derived filament_loaded", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_sensor_authority");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // 1) Toolhead sensor trips: filament present at the nozzle.
    json sensor_update =
        json{{"params", json::array({json{{"filament_switch_sensor filament_sensor",
                                           {{"filament_detected", true}}}},
                                     0})}};
    backend.fire_status(sensor_update);
    REQUIRE(backend.get_system_info().filament_loaded == true);

    // 2) A box-only update (no sensor param) arrives. box.filament is present
    //    but is a selection index, not a loaded flag — it must NOT reset
    //    filament_loaded.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    box["filament"] = 0;
    box["T1"]["filament"] = "None";
    backend.fire_status(make_cfs_notification(box));

    REQUIRE(backend.get_system_info().filament_loaded == true);
}

TEST_CASE("CFS a box write Klipper rejects saves nothing and reports the edit",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    b.fire_status(make_cfs_notification(
        make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));
    b.reject = true;

    SlotInfo edit = b.get_slot_info(0);
    edit.color_rgb = 0x00FF00;
    edit.brand = "Sunlu";
    (void)b.commit_user_edit(0, b.get_slot_info(0), edit);

    // The record, if the box's own reading was mirrored into one, carries
    // none of the rejected edit.
    const auto record = CfsTestAccess::get_override(b, 0);
    CHECK((!record.has_value() || (record->brand != "Sunlu" && record->color_rgb != 0x00FF00u)));
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(0)).pending.has_value());
    CHECK(b.get_slot_info(0).color_rgb == 0xFF5500u);
}
