// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 226 to 264 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs][runout][1250], [ad5x_ifs][unload], [ad5x_ifs], [ad5x_ifs][homing_guard],
// [ad5x][ifs][poll], and more

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

// FIX 4: clear-spool persists firmware-native '?' / "" sentinels

// What a slot write puts in Adventurer5M.json. An empty material persists the
// firmware's own "no filament" pair (ffmColor "", ffmType "?"). A real material
// never persists an empty ffmColor: zmod builds the "Change type" button as
// `CHANGE_ZCOLOR SLOT=n HEX={zhex}`, which an empty colour turns into a command
// cmd_CHANGE_ZCOLOR rejects after it has closed the dialog, so the empty and the
// 808080 placeholder colours become zmod's default #161616.
TEST_CASE("AD5X IFS write_adventurer_json_local writes the firmware's slot encoding",
          "[ams][ad5x_ifs][local_write]") {
    struct Row {
        const char* name;
        const char* seed;
        int slot;
        const char* hex;
        const char* material;
        const char* expected_color;
        const char* expected_type;
    };
    const Row rows[] = {
        {"a cleared slot", R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})", 0, "808080",
         "", "", "?"},
        {"an explicitly empty colour", R"({"FFMInfo":{"ffmColor2":"#00FF00","ffmType2":"PETG"}})",
         1, "", "", "", "?"},
        {"empty slot, empty hex", R"({"FFMInfo":{}})", 2, "", "", "", "?"},
        {"empty slot, placeholder hex", R"({"FFMInfo":{}})", 2, "808080", "", "", "?"},
        {"a normal slot", R"({"FFMInfo":{}})", 2, "AABBCC", "TPU", "#AABBCC", "TPU"},
        {"a fully-specified slot", R"({"FFMInfo":{}})", 3, "FF7700", "ABS", "#FF7700", "ABS"},
        {"material with an empty colour", R"({"FFMInfo":{}})", 0, "", "PLA", "#161616", "PLA"},
        {"material with the placeholder colour", R"({"FFMInfo":{}})", 1, "808080", "PETG",
         "#161616", "PETG"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            Ad5xIfsTmpJsonFile tmp("slot_encoding", r.seed);
            helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
            AmsBackendAd5xIfs& backend = *backend_reg;
            Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

            REQUIRE(
                Ad5xIfsTestAccess::write_adventurer_json_local(backend, r.slot, r.hex, r.material)
                    .success());

            std::ifstream f(tmp.path);
            const auto doc = json::parse(f);
            const std::string n = std::to_string(r.slot + 1);
            CHECK(doc["FFMInfo"]["ffmColor" + n] == r.expected_color);
            CHECK(doc["FFMInfo"]["ffmType" + n] == r.expected_type);
        }
    }
}

// HEATING detail must not assert a target the printer never reported.

// HEATING detail never asserts a target the printer has not reported: no frame
// at all, the live temperature with the heater off, or the real target once a
// pre-op extruder frame carries one (begin_phase seeds from it). The line, and
// its timed-out form, re-word on a language switch with no new frame.
TEST_CASE("AD5X IFS phase: HEATING detail names only what the printer reported",
          "[ams][ad5x_ifs][phase][i18n]") {
    struct Row {
        const char* name;
        bool extruder_frame;
        double temp;
        double target;
        const char* expected;
        const char* french;
    };
    const Row rows[] = {
        {"nothing known", false, 0, 0, "Heating nozzle", "Chauffe de la buse"},
        {"live temp, heater off", true, 62.0, 0.0, "Heating nozzle (62°C)",
         "Chauffe de la buse (62°C)"},
        {"a real target", true, 62.0, 220.0, "Heating nozzle to 220°C (62°C)",
         "Chauffe de la buse à 220°C (62°C)"},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
            auto& backend = *backend_reg;
            Ad5xIfsTestAccess::set_head_filament(backend, true);
            if (r.extruder_frame) {
                backend.fire_status(make_extruder(r.temp, r.target));
            }
            Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
            REQUIRE(backend.get_current_action() == AmsAction::HEATING);
            const auto detail = [&] { return backend.get_system_info().operation_detail; };
            CHECK(detail() == r.expected);
            {
                helix::ScopedLanguage fr("fr");
                backend.refresh_translated_texts();
                CHECK(detail() == r.french);
                Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(600));
            }
            backend.refresh_translated_texts();
            CHECK(detail() == std::string(r.expected) + " (timed out)");

            // recover() and reset() retire the composed line: a later re-word leaves it gone.
            REQUIRE((r.extruder_frame ? backend.reset() : backend.recover()).success());
            backend.refresh_translated_texts();
            CHECK(detail().empty());
        }
    }
}

// The firmware-native unset sentinel '?' and an empty ffmType both read as no
// material, so the UI renders '--', even on a port whose colour is set.
TEST_CASE("AD5X IFS parse_adventurer_json maps '?' and empty ffmType to no material (FIX 4b)",
          "[ams][ad5x_ifs]") {
    for (const char* type : {"?", ""}) {
        DYNAMIC_SECTION("ffmType '" << type << "'") {
            helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
            AmsBackendAd5xIfs& backend = *backend_reg;
            Ad5xIfsTestAccess::parse_adventurer_json(
                backend, json{{"FFMInfo", {{"ffmColor1", "#FF5500"}, {"ffmType1", type}}}}.dump());
            CHECK(backend.get_slot_info(0).material.empty());
        }
    }
}

TEST_CASE("AD5X IFS slot_unloads_to_toolhead matches unload routing across the authority matrix",
          "[ams][ad5x_ifs]") {
    struct Case {
        const char* name;
        int current_slot;
        int seated_chan; // 1-based, 0 = none
        bool head_loaded;
        int slot;
        bool expect_toolhead; // true = heated cut, false = cold eject
    };
    // clang-format off
    const Case cases[] = {
        {"known active, tap active -> cut",            2,  0, true,  2, true},
        {"known active, tap other -> eject",           2,  0, true,  0, false},
        {"known active disagrees, tap seated -> cut",  2,  4, true,  3, true},  // #981 seated authority
        {"pointer lost, tap seated -> cut",           -1,  2, true,  1, true},
        {"pointer lost, tap non-seated -> eject",     -1,  2, true,  3, false}, // bundle 5HR3HHS6
        {"pointer lost, nothing seated, head -> cut", -1,  0, true,  2, true},  // unknown-origin recovery
        {"empty toolhead -> eject",                    2,  0, false, 2, false},
    };
    // clang-format on

    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            auto backend = make_routing_backend(c.current_slot, c.seated_chan, c.head_loaded);

            // Predicate (label/dispatch) — must ignore the recovery-broadened hint.
            const bool pred_true = backend->slot_unloads_to_toolhead(c.slot, /*loaded_hint=*/true);
            const bool pred_false =
                backend->slot_unloads_to_toolhead(c.slot, /*loaded_hint=*/false);
            CHECK(pred_true == c.expect_toolhead);
            CHECK(pred_false == c.expect_toolhead); // hint is ignored for AD5X

            // Actual routing — the contract: predicate == route taken.
            const bool routed_toolhead = unload_routed_to_toolhead(*backend, c.slot);
            CHECK(routed_toolhead == c.expect_toolhead);
            CHECK(routed_toolhead == pred_true);
        }
    }
}

TEST_CASE("AD5X IFS unload of the active slot via -1 (unload active) toolhead-cuts",
          "[ams][ad5x_ifs]") {
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
    REQUIRE(backend->unload_filament(-1).success());
    CHECK(backend->has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend->has_gcode_containing("IFS_F11"));
}

// The unload router's "is the toolhead empty" decision must come from the
// toolhead SWITCH pair, not the conflated head_filament_.
//
// parse_head_sensor() writes head_filament_ from BOTH the switch sensor and
// ifs_motion_sensor, and the motion sensor is device-confirmed to read
// filament_detected=false on a lane that is loaded but idle. Believing that
// false negative sends seated, un-cut filament to the cold IFS_F11 retract,
// which grinds it (#981; bundle 5HR3HHS6 is the same hazard by a different
// route). The reverse error - calling a truly empty head "loaded" - only costs
// a firmware no-op, so the predicate is biased toward "loaded" on purpose.

TEST_CASE("AD5X IFS unload: switch says present, motion says absent -> heated cut, never a cold "
          "eject (grinding regression)",
          "[ams][ad5x_ifs][unload]") {
    // The case this predicate exists for. The last frame to write
    // head_filament_ came from ifs_motion_sensor on a loaded-but-idle lane, so
    // head_filament_ is false while filament is physically at the nozzle. The
    // switch sensor still says present, and it is the authority.
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/false);
    Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(*backend)); // motion false-negative in place

    // Label and dispatch must both say "heated toolhead unload".
    CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
    CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/false));

    REQUIRE(backend->unload_filament(2).success());
    CHECK(backend->has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend->has_gcode_containing("IFS_F11")); // the cold retract that grinds
    CHECK_FALSE(backend->has_gcode_containing("IFS_F24"));
}

TEST_CASE("AD5X IFS unload: the motion false-negative arrives through the real sensor plumbing",
          "[ams][ad5x_ifs][unload]") {
    // Same case, but reached the way a device reaches it: a switch frame
    // says present, then a motion frame says absent and clobbers head_filament_
    // through parse_head_sensor(). Nothing here pokes head_filament_ directly -
    // if the conflation ever gets fixed at the source, this test stops proving
    // anything but must still pass.
    helix::test::StartedBackend<TestableAd5xIfsBackend> backend;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    backend.fire_status(make_zmod_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    backend.fire_status(make_motion_sensor(false));

    // The conflation is real: head_filament_ now lies, the switch pair does not.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    // Pin the active slot AFTER the frames — handle_status() recomputes
    // current_slot from tool_map_/seated_chan_ on every sensor change.
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);

    CHECK(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
    REQUIRE(backend.unload_filament(2).success());
    CHECK(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend.has_gcode_containing("IFS_F11"));
}

TEST_CASE("AD5X IFS unload: switch seen and absent -> cold eject even when motion says present "
          "(7AC4SDEX)",
          "[ams][ad5x_ifs][unload]") {
    // Bundle 7AC4SDEX: head_switch_sensor empty, ifs_motion_sensor present.
    // The filament is in the lane, not the toolhead, and
    // _IFS_REMOVE_CURRENT_PRUTOK would early-return on the extruder sensor after
    // homing ("homes and nothing happens"). Switch-absent is authoritative, so
    // this must reach the cold retract regardless of what motion reports.
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
    Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/false);
    REQUIRE(Ad5xIfsTestAccess::head_filament(*backend)); // motion says present; switch overrules

    CHECK_FALSE(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));

    REQUIRE(backend->unload_filament(2).success());
    CHECK(backend->has_gcode("IFS_F24 PRUTOK=3"));
    CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=3"));
    CHECK(backend->has_gcode("IFS_F39 PRUTOK=3"));
    CHECK_FALSE(backend->has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
}

TEST_CASE("AD5X IFS unload: motion-only firmware (switch never seen) keeps the historical rule",
          "[ams][ad5x_ifs][unload]") {
    // No switch sensor is published at all, so there is no positive evidence to
    // require and no behaviour to change: the routing must be exactly what
    // head_filament_ said before this predicate existed. Both polarities, so a
    // fallback that hardcoded either answer fails.
    for (bool head : {true, false}) {
        DYNAMIC_SECTION("head_filament_ = " << head) {
            auto backend =
                make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/head);
            Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/false, /*present=*/false);
            REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_seen(*backend));

            CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true) == head);
            CHECK(unload_routed_to_toolhead(*backend, 2) == head);
        }
    }
}

TEST_CASE("AD5X IFS unload: the slot-identity branches still win over the head predicate",
          "[ams][ad5x_ifs][unload]") {
    // The non-active-slot (#981 HKHZFYB2) and dropped-pointer (5HR3HHS6) guards
    // run BEFORE the head test and are about which lane the user tapped, not
    // about the toolhead. A switch that reads present must not promote either of
    // them to a heated cut — that is the wrong-lane heat+cut both were added to
    // stop. Conversely the unknown-origin recovery case (both authorities lost)
    // must still cut.
    SECTION("known active slot, tap a different lane -> cold eject") {
        auto backend =
            make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK_FALSE(backend->slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
        CHECK_FALSE(unload_routed_to_toolhead(*backend, 0));
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=1"));
    }
    SECTION("pointer lost, seated known, tap a non-seated lane -> cold eject") {
        auto backend =
            make_routing_backend(/*current_slot=*/-1, /*seated_chan=*/2, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK_FALSE(backend->slot_unloads_to_toolhead(3, /*loaded_hint=*/true));
        CHECK_FALSE(unload_routed_to_toolhead(*backend, 3));
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=4"));
    }
    SECTION("pointer lost, nothing seated, switch present -> unknown-origin cut") {
        auto backend =
            make_routing_backend(/*current_slot=*/-1, /*seated_chan=*/0, /*head_loaded=*/false);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
        CHECK(unload_routed_to_toolhead(*backend, 2));
    }
}

TEST_CASE("AD5X IFS eject_lane's seated refusal uses the same head predicate as the router",
          "[ams][ad5x_ifs][unload]") {
    // The two must agree or they deadlock against each other: the router hands
    // an empty-head unload to eject_lane(), and a refusal keyed on a different
    // notion of "empty" would bounce it back with "Unload from toolhead first".
    SECTION("switch present, motion false-negative -> a direct Eject tap is refused") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/false);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);

        AmsError err = backend->eject_lane(1);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Unload from toolhead first");
        CHECK(backend->captured_gcodes.empty()); // no cold retract against seated filament
    }
    SECTION("switch absent, motion says present -> the router's eject is NOT bounced") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/false);

        REQUIRE(backend->unload_filament(1).success());
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=2"));
    }
    SECTION("motion-only firmware keeps the historical head_filament_ refusal") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/false, /*present=*/false);

        AmsError err = backend->eject_lane(1);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
    }
}

TEST_CASE("AD5X IFS eject_lane maps each slot to its 1-based port", "[ams][ad5x_ifs]") {
    for (int slot = 0; slot < AmsBackendAd5xIfs::kDefaultPorts; ++slot) {
        DYNAMIC_SECTION("slot " << slot) {
            TestableAd5xIfsBackend backend;
            Ad5xIfsTestAccess::set_running(backend, true);
            Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
            REQUIRE(backend.eject_lane(slot).success());
            const std::string port = std::to_string(slot + 1);
            REQUIRE(backend.captured_gcodes.size() == 3);
            CHECK(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=" + port);
            CHECK(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=" + port + " LEN=1000 SPEED=1200");
            CHECK(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=" + port);
        }
    }
}

TEST_CASE("AMS base slot_unloads_to_toolhead defaults to the loaded hint (no AD5X override)",
          "[ams][ad5x_ifs]") {
    // Non-AD5X backends keep the legacy rule: toolhead unload iff the menu's
    // is_loaded snapshot says so. AD5X overrides; this guards the default.
    helix::test::RegisteredBackend<AmsBackendAfc> afc_reg(nullptr, nullptr);
    AmsBackendAfc& afc = *afc_reg;
    CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/false));
}

TEST_CASE_METHOD(Ad5xHomingGuardFixture,
                 "check_preconditions gates toolhead motion on active print",
                 "[ams][ad5x_ifs][homing_guard]") {
    SECTION("PRINTING refuses motion ops but allows no-motion ops") {
        set_print_state(helix::PrintJobState::PRINTING);
        AmsError motion = backend->check_preconditions(/*requires_toolhead_motion=*/true);
        CHECK_FALSE(motion.success());
        CHECK(motion.result == AmsResult::WRONG_STATE);
        CHECK(motion.user_msg == "Cannot run filament operation while printing");
        // Case 7: no-motion ops (eject_lane / select / unlock) are NOT blocked.
        CHECK(backend->check_preconditions(/*requires_toolhead_motion=*/false).success());
    }

    SECTION("PAUSED refuses motion ops (head parked over the print)") {
        set_print_state(helix::PrintJobState::PAUSED);
        AmsError motion = backend->check_preconditions(true);
        CHECK_FALSE(motion.success());
        // A runout pause is the case users actually hit: Klipper prints "load it
        // and press RESUME", so they reach for Load. "while printing" reads as a
        // bug and "finish or cancel the print" is the opposite of what they want.
        // Point them at the recovery that works instead (bundle JX2FVRB9).
        CHECK(motion.user_msg == "Can't move filament while the print is paused");
        CHECK(motion.suggestion.find("Resume") != std::string::npos);
    }

    SECTION("STANDBY allows motion ops") {
        set_print_state(helix::PrintJobState::STANDBY);
        CHECK(backend->check_preconditions(true).success());
    }

    SECTION("COMPLETE allows motion ops") {
        set_print_state(helix::PrintJobState::COMPLETE);
        CHECK(backend->check_preconditions(true).success());
    }
}

TEST_CASE_METHOD(Ad5xHomingGuardFixture,
                 "load/unload/change_tool refuse and emit nothing while printing",
                 "[ams][ad5x_ifs][homing_guard]") {
    set_print_state(helix::PrintJobState::PRINTING);

    SECTION("unload_filament refused, no gcode/macro emitted") {
        AmsError err = backend->unload_filament(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }

    SECTION("load_filament refused, no gcode/macro emitted") {
        AmsError err = backend->load_filament(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }

    SECTION("change_tool refused, no gcode/macro emitted") {
        AmsError err = backend->change_tool(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }
}

TEST_CASE_METHOD(Ad5xHomingGuardFixture, "eject_lane is not blocked by an active print",
                 "[ams][ad5x_ifs][homing_guard]") {
    // eject_lane is a cold, lane-only op (no toolhead motion) — it must remain
    // available during a print. Whatever its outcome, it must NOT be refused
    // *because* a print is active.
    set_print_state(helix::PrintJobState::PRINTING);
    Ad5xIfsTestAccess::set_zcolor_supported(*backend, false);
    AmsError err = backend->eject_lane(0);
    CHECK(err.user_msg != "Cannot run filament operation while printing");
}

// External TYPE-change detection (check_external_type_change)
// prestonbrown/helixscreen#981 / #1065: a type-only firmware edit (same
// color) never tripped the color detector, so a non-locked override's baked
// material went stale and masked firmware truth — color updated on screen,
// material stuck (e.g. on "PLA"). A firmware type change must now refresh a
// non-locked override so the new type surfaces, while a user-locked material
// (#965) is still preserved.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: firmware type change refreshes a non-locked override (#981)",
                 "[ams][ad5x_ifs][override][981]") {
    seed_standard_colors(backend); // slot 1: firmware PETG / #00FF00, baselines set

    // Auto-mirror left a non-locked override matching firmware-at-the-time.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // Firmware type changes externally (color unchanged): PETG -> TPU.
    Ad5xIfsTestAccess::set_material(backend, 1, "TPU");

    // The new firmware type must surface, and the stale override is refreshed.
    CHECK(backend.get_slot_info(1).material == "TPU");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "TPU");
}

// #1065: insert AFTER an empty lane must refresh a stale non-locked material.
// Root cause: check_external_type_change early-returned on an empty material
// observation, so an empty lane never recorded a baseline. The FIRST insert
// then hit the "first observation" branch (baseline-only, no sync) instead of
// "changed", so the stale override.material masked firmware truth — color
// updated on screen, material stuck. The fix baselines the empty state (like
// the color path's #808080 placeholder), making "" -> PETG a genuine edit.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: insert after an empty lane refreshes a stale non-locked material "
                 "(empty -> PETG) (#1065)",
                 "[ams][ad5x_ifs][override][1065]") {
    // A previous spool left a non-locked override baked with the OLD material.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // Firmware reports a color (so the sync's color-availability guard passes),
    // then the lane goes empty: presence false + empty material. The sync
    // // baselines the material to "".
    Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
    Ad5xIfsTestAccess::set_port_presence(backend, 1, false);
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    // Sanity: while empty, the retained override still shows (lane keeps its
    // assignment across eject — #1071).
    CHECK(backend.get_slot_info(1).material == "PLA");

    // Insert a DIFFERENT material: lane present + firmware PETG. This is now a
    // genuine "" -> PETG delta and must fire the sync, refreshing the override.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    CHECK(backend.get_slot_info(1).material == "PETG");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "PETG");
}

TEST_CASE("AD5X IFS: an empty first material observation is a baseline, not a spurious sync "
          "(#1065)",
          "[ams][ad5x_ifs][override][1065]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    // A pre-existing non-locked override must survive the FIRST (empty) firmware
    // observation untouched — baselining "" must not fire a sync that rewrites
    // the override.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "ABS";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // First-ever observation for slot 1 is an empty lane: presence false, empty
    // material. This establishes the "" baseline and must NOT sync.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, false);
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    // Override is undisturbed; no external edit was detected.
    CHECK(backend.get_slot_info(1).material == "ABS");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "ABS");
}

// Bundle 77TDH9N6: a cold-start IFS load (INSERT_PRUTOK_IFS) legitimately runs
// past the 300s AMS RPC ceiling (heat-from-cold + load + double purge + clean).
// The macro completes fine, but the RPC times out at 300s and — unless silent —
// surfaces a false "printer.gcode.script timed out after 300000ms" toast. The
// backend owns completion via its phase tracker + IFS_STATUS, so the RPC timeout
// is advisory here. execute_gcode() must dispatch silent=true (suppress the
// timeout toast and the tracker's generic fallback) while leaving the 300s
// ceiling unchanged.
//
// It must ALSO declare caller_surfaces_errors=false: the backend's error_cb only
// writes to the log, which the user never sees. Claiming otherwise would record
// the message for dedup and silence GcodeErrorRouter, leaving a real IFS macro
// rejection with no surface at all. See include/rpc_error_policy.h.

TEST_CASE("AD5X IFS execute_gcode dispatches silent with the AMS timeout ceiling",
          "[ams][mock][ad5x_ifs]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    // execute_gcode()'s klippy-halted gate rejects everything until a real
    // state update arrives (subjects initialize to SHUTDOWN).
    state.set_klippy_state_sync(helix::KlippyState::READY);
    client.connect("ws://mock/websocket", []() {}, []() {});

    // Real MoonrakerAPI path: MoonrakerAPIMock does NOT override execute_gcode,
    // so the silent/timeout args reach the client mock's send capture.
    MoonrakerAPIMock api(client, state);
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, &client);
    AmsBackendAd5xIfs& backend = *backend_reg;

    SECTION("plain execute_gcode overload") {
        auto err = backend.execute_gcode("INSERT_PRUTOK_IFS PRUTOK=2");
        REQUIRE(err.success());

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        // silent=true suppresses the false REQUEST_TIMEOUT toast.
        REQUIRE(client.last_send_silent() == true);
        // Ceiling unchanged — proves we did NOT alter kAmsOperationTimeoutMs.
        REQUIRE(client.last_send_timeout_ms() == 300000u);
        // The log-only error_cb must NOT claim the report, or the `!!` router
        // goes quiet for every AFC / Happy Hare / CFS / IFS macro rejection.
        REQUIRE(client.current_send_intent().silent == true);
        REQUIRE(client.current_send_intent().surfaces_errors == false);
    }

    SECTION("on_complete execute_gcode overload") {
        auto completed = std::make_shared<bool>(false);
        auto err = backend.execute_gcode("INSERT_PRUTOK_IFS PRUTOK=2",
                                         [completed]() { *completed = true; });
        REQUIRE(err.success());

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        REQUIRE(client.last_send_silent() == true);
        REQUIRE(client.last_send_timeout_ms() == 300000u);
        REQUIRE(client.current_send_intent().silent == true);
        REQUIRE(client.current_send_intent().surfaces_errors == false);
        // The completion is queued; run it while `completed` is still in scope.
        helix::ui::UpdateQueue::instance().drain();
    }
}

// Adventurer5M.json freshness-poll cadence

TEST_CASE("AD5X IFS: JSON poll backs off to 30s while printing", "[ams][ad5x][ifs][poll]") {
    using namespace std::chrono;
    using B = AmsBackendAd5xIfs;

    SECTION("idle cadence is 5s") {
        REQUIRE_FALSE(B::should_poll_json(false, false, seconds(4)));
        REQUIRE(B::should_poll_json(false, false, seconds(5)));
    }

    SECTION("printing cadence is 30s — 5s and 29s must NOT fire") {
        REQUIRE_FALSE(B::should_poll_json(true, true, seconds(5)));
        REQUIRE_FALSE(B::should_poll_json(true, true, seconds(29)));
        REQUIRE(B::should_poll_json(true, true, seconds(30)));
    }

    SECTION("a pause keeps the fast cadence — that is when spools get relabelled") {
        // printing_now=false covers PAUSED as well as idle/complete.
        REQUIRE(B::should_poll_json(false, false, seconds(5)));
    }
}

TEST_CASE("AD5X IFS: print end forces an off-cadence poll", "[ams][ad5x][ifs][poll][965]") {
    using namespace std::chrono;
    using B = AmsBackendAd5xIfs;

    // printing -> not printing: fire immediately even with no elapsed time, so the
    // firmware's post-print FFMInfo revert isn't delayed by the slow interval.
    REQUIRE(B::should_poll_json(false, true, seconds(0)));

    // The edge is one-shot: once was_printing clears, normal cadence resumes.
    REQUIRE_FALSE(B::should_poll_json(false, false, seconds(0)));

    // Entering a print is NOT an edge — no reason to poll off-cadence there.
    REQUIRE_FALSE(B::should_poll_json(true, false, seconds(0)));
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: head drop while paused and idle raises the fault",
                 "[ams][ad5x_ifs][runout][1250]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    // The drop happens mid-print, which is when a real runout happens.
    set_print_state(helix::PrintJobState::PRINTING);
    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));

    // Still PRINTING: an empty head here is a firmware tool change, not a
    // runout. However long it stays empty, nothing is raised.
    Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));

    // Klipper's runout handling pauses the job. Now it is a runout.
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(backend.runout_active());
    REQUIRE(Ad5xIfsTestAccess::filament_runout(backend));
    // AmsState reads the flag from the snapshot, not from the backend's fields.
    REQUIRE(backend.get_system_info().filament_runout);
    // ERROR is the only edge AmsErrorBridge watches, so it is the only route to
    // current_error() and the recovery modal.
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());

    // Idempotent: re-evaluating must not report a second state change (which
    // would re-emit EVENT_STATE_CHANGED on every status frame).
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS clear_fault retires a raised runout and its latch",
                 "[ams][ad5x_ifs][runout][clear_fault]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PRINTING);
    seat_then_drop_head(backend);
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(runs_out_after_dwell(backend));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE(Ad5xIfsTestAccess::filament_runout(backend));

    backend.clear_fault(0);

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(backend.runout_active());
    CHECK_FALSE(Ad5xIfsTestAccess::filament_runout(backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: the confirm dwell is load-bearing",
                 "[ams][ad5x_ifs][runout][1250]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    set_print_state(helix::PrintJobState::PAUSED);
    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));

    // Freshly armed: every other condition holds, but the dwell has not elapsed.
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);

    REQUIRE(runs_out_after_dwell(backend));
    REQUIRE(backend.runout_active());
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: not raised while the print is not paused",
                 "[ams][ad5x_ifs][runout][1250]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::STANDBY,
                       helix::PrintJobState::COMPLETE}) {
        set_print_state(state);
        seat_then_drop_head(backend);
        Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
        CHECK_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
        CHECK_FALSE(backend.runout_active());
        CHECK(backend.get_current_action() == AmsAction::IDLE);
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: a motion-sensor-only drop is not an empty toolhead",
                 "[ams][ad5x_ifs][runout][1250]") {
    // The conflation trap: parse_head_sensor() writes head_filament_ from BOTH
    // the switch and ifs_motion_sensor, and the motion sensor is device-confirmed
    // to read filament_detected=false on a loaded-but-idle lane. Gating on
    // head_filament_ would fire a runout on a perfectly healthy paused print.
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PAUSED);

    backend.fire_status(make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    backend.fire_status(make_motion_sensor(false));
    // head_filament_ IS now false - and that is exactly why it cannot be trusted.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    // The switch, the actual authority, still says loaded.
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: a head drop during a tracked load/unload is not a runout",
                 "[ams][ad5x_ifs][runout][phase][1250]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PAUSED);

    backend.fire_status(make_head_sensor(true));
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    // The cut drops the head sensor. That is progress, not a runout.
    backend.fire_status(make_head_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(backend));
    REQUIRE_FALSE(backend.runout_active());

    // Existing phase behaviour is untouched: CUTTING -> UNLOADING on the drop.
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Retracting filament from nozzle");
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(Ad5xRunoutOpFixture,
                 "AD5X IFS runout: a head drop during a tool-change swap is not a runout",
                 "[ams][ad5x_ifs][runout][1250]") {
    // The "operation in flight" gate exists because do_change_tool()
    // sets LOADING but never arms the phase tracker, so gating on
    // phase_tracker_.active alone would pop a runout modal on every multicolour
    // swap that happens to land on a paused job.
    REQUIRE(backend->set_tool_mapping(0, 1).success()); // T0 -> port 2

    backend->fire_status(make_head_sensor(true));
    REQUIRE(backend->change_tool(0).success());
    REQUIRE(backend->has_gcode_containing("A_CHANGE_FILAMENT CHANNEL=2"));
    REQUIRE(backend->get_current_action() == AmsAction::LOADING);

    // Mid-swap the head empties, and the job is paused.
    set_print_state(helix::PrintJobState::PAUSED);
    backend->fire_status(make_head_sensor(false));

    // Nothing armed: the action is LOADING, so an operation is in flight.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(*backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(*backend));
    REQUIRE_FALSE(backend->runout_active());
    REQUIRE(backend->get_current_action() == AmsAction::LOADING);

    // Even if the action snaps back to IDLE (the legacy head-rise finalize) the
    // dispatch stamp still attributes the window to the swap.
    Ad5xIfsTestAccess::set_action(*backend, AmsAction::IDLE);
    backend->fire_status(make_head_sensor(true));
    backend->fire_status(make_head_sensor(false));
    Ad5xIfsTestAccess::age_head_empty(*backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(*backend));
}

TEST_CASE_METHOD(Ad5xRunoutOpFixture,
                 "AD5X IFS runout: an eject-routed unload suppresses the fault window",
                 "[ams][ad5x_ifs][runout][1250]") {
    // do_unload_filament() has three early returns that hand off to eject_lane()
    // without touching phase_tracker_ or system_info_.action, so the backend
    // stays IDLE right through an operation that legitimately empties the head.
    // The dispatch stamp is the only thing standing between that and a false
    // runout.
    auto& op_backend = *backend;
    set_print_state(helix::PrintJobState::PRINTING);

    seat_then_drop_head(op_backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(op_backend));
    set_print_state(helix::PrintJobState::PAUSED);

    // Empty head + a requested unload routes to the cold per-lane eject.
    Ad5xIfsTestAccess::set_current_slot(op_backend, 0, false);
    REQUIRE(op_backend.unload_filament(0).success());
    REQUIRE(op_backend.has_gcode_containing("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
    REQUIRE_FALSE(op_backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));

    // The dispatch disarmed the candidate outright.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(op_backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(op_backend));

    // And a candidate that re-arms inside the suppression window is still
    // attributed to that eject, not to a runout.
    seat_then_drop_head(op_backend);
    Ad5xIfsTestAccess::age_head_empty(op_backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(op_backend));

    // Once the window expires, the same state IS a runout.
    Ad5xIfsTestAccess::age_op_dispatch(op_backend, std::chrono::seconds(600));
    REQUIRE(Ad5xIfsTestAccess::evaluate_runout(op_backend));
    REQUIRE(op_backend.runout_active());
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: clears on refill, resume and recover",
                 "[ams][ad5x_ifs][runout][1250]") {
    auto raise = [this](auto& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(b);
        REQUIRE(runs_out_after_dwell(b));
        REQUIRE(b.get_current_action() == AmsAction::ERROR);
    };

    SECTION("filament back at the toolhead clears it") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        backend.fire_status(make_head_sensor(true));
        REQUIRE_FALSE(backend.runout_active());
        REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("resuming the print clears it (this is what dismisses the modal)") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        set_print_state(helix::PrintJobState::PRINTING);
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
        REQUIRE_FALSE(backend.runout_active());
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("recover() clears it") {
        helix::test::StartedBackend<TestableAd5xIfsBackend> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        REQUIRE(backend.recover().success());
        REQUIRE_FALSE(backend.runout_active());
        REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }
}

// An idle backend shows no status line: the runout's text goes with the fault,
// or the panel keeps describing a runout that is over.
TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: the status line goes with the cleared fault",
                 "[ams][ad5x_ifs][runout][1250][2044]") {
    auto raise = [this](auto& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(b);
        REQUIRE(runs_out_after_dwell(b));
        REQUIRE(b.get_current_action() == AmsAction::ERROR);
        REQUIRE_FALSE(b.get_system_info().operation_detail.empty());
    };

    SECTION("filament back at the toolhead") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        backend.fire_status(make_head_sensor(true));
        CHECK(backend.get_current_action() == AmsAction::IDLE);
        CHECK(backend.get_system_info().operation_detail.empty());
    }

    SECTION("the print resumes") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        set_print_state(helix::PrintJobState::PRINTING);
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
        CHECK(backend.get_current_action() == AmsAction::IDLE);
        CHECK(backend.get_system_info().operation_detail.empty());
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: current_error distinguishes runout from an op timeout",
                 "[ams][ad5x_ifs][runout][error-center][1250]") {
    SECTION("runout on an unhomed toolhead: no Load, and Resume does not lead") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(backend);
        REQUIRE(runs_out_after_dwell(backend));

        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        // The runout fault the hook raises is what the trait claims. It arrives
        // only while paused, so it is not owned during the job.
        REQUIRE(backend.traits().raises_runout_fault);
        REQUIRE_FALSE(backend.traits().raises_runout_fault_during_job);
        REQUIRE(ev->source == helix::ErrorSource::IFS);
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->sticky);
        REQUIRE(ev->title == std::string("Filament runout"));
        CHECK(ev->detail.find("Load filament to the toolhead, then tap Resume.") !=
              std::string::npos);

        REQUIRE(ev->recovery_actions.size() == 3);
        // zMod's RESUME refuses while the head switch reads empty.
        REQUIRE(ev->recovery_actions[0].gcode == "RESUME");
        CHECK(ev->recovery_actions[0].style.empty());
        REQUIRE(ev->recovery_actions[0].needs_hot_nozzle);
        // A plain extruder move in place, no homing, so it cannot reach the
        // loadcell _G28 that shuts the AD5X down mid-job.
        REQUIRE(ev->recovery_actions[1].gcode == "M83\nG1 E50 F600");
        REQUIRE(ev->recovery_actions[1].needs_hot_nozzle);
        REQUIRE(ev->recovery_actions[2].gcode == "IFS_UNLOCK");
        REQUIRE_FALSE(ev->recovery_actions[2].needs_hot_nozzle);

        // Unhomed, INSERT_PRUTOK_IFS's _G28 would probe into the part.
        for (const auto& a : ev->recovery_actions) {
            CHECK(a.gcode.find("INSERT_PRUTOK_IFS") == std::string::npos);
            CHECK(a.gcode.find("A_CHANGE_FILAMENT") == std::string::npos);
        }

        // The same runout on zMod: over the bin with the stock purge, no _G28.
        helix::PrinterDiscovery zmod;
        zmod.parse_objects(json::array(
            {"gcode_macro _G28", "gcode_macro _GOTO_TRASH", "gcode_macro PURGE_FILAMENT"}));
        backend.set_discovery(zmod);
        CHECK(backend.current_error()->recovery_actions[1].gcode == "_GOTO_TRASH\nPURGE_FILAMENT");
    }

    SECTION("runout on a homed toolhead: Load of the lane that ran out leads") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xyz");
        seat_then_drop_head(backend);
        Ad5xIfsTestAccess::set_current_slot(backend, 2);
        REQUIRE(runs_out_after_dwell(backend));
        REQUIRE(Ad5xIfsTestAccess::runout_slot(backend) == 2);

        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        REQUIRE(ev->recovery_actions.size() == 4);
        const auto& load = ev->recovery_actions[0];
        CHECK(load.log_tag == "ifs::load");
        CHECK(load.gcode == "INSERT_PRUTOK_IFS PRUTOK=3");
        CHECK(load.style == "primary");
        CHECK(load.requires_homed);
        // The macro heats to the lane's own temperature.
        CHECK_FALSE(load.needs_hot_nozzle);
        CHECK(ev->recovery_actions[1].gcode == "RESUME");
        CHECK(ev->recovery_actions[1].style.empty());
        CHECK(ev->recovery_actions[3].gcode == "IFS_UNLOCK");
    }

    SECTION("operation timeout keeps the historical single Recover action") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_head_filament(backend, true);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(600));
        REQUIRE(backend.get_current_action() == AmsAction::ERROR);
        REQUIRE_FALSE(backend.runout_active());

        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        REQUIRE(ev->title == std::string("Filament System Error"));
        REQUIRE(ev->recovery_actions.size() == 1);
        REQUIRE(ev->recovery_actions[0].gcode == "IFS_UNLOCK");
        REQUIRE(ev->recovery_actions[0].style == "primary");
    }
}

TEST_CASE("AD5X IFS runout actions follow the toolhead switch", "[ams][ad5x_ifs][runout]") {
    AmsBackendAd5xIfs::RunoutRecoveryInputs in;
    in.load_gcode = "INSERT_PRUTOK_IFS PRUTOK=1";
    in.purge_gcode = "M83\nG1 E50 F600";
    in.unlock_gcode = "IFS_UNLOCK";

    SECTION("head empty: Load leads, Resume is offered but not primary") {
        in.head_has_filament = false;
        const auto actions = AmsBackendAd5xIfs::runout_recovery_actions(in);
        REQUIRE(actions.size() == 4);
        CHECK(actions[0].gcode == "INSERT_PRUTOK_IFS PRUTOK=1");
        CHECK(actions[0].style == "primary");
        CHECK(actions[1].gcode == "RESUME");
        CHECK(actions[1].style.empty());
    }
    SECTION("Recover reads as danger only where it resets the module's driver") {
        in.head_has_filament = true;
        auto recover = [&] {
            const auto actions = AmsBackendAd5xIfs::runout_recovery_actions(in);
            REQUIRE_FALSE(actions.empty());
            REQUIRE(actions.back().log_tag == "ifs::unlock");
            return actions.back().style;
        };
        CHECK(recover().empty());
        in.unlock_gcode = "IFS_RESET_DRIVER";
        CHECK(recover() == "danger");
    }
    SECTION("head empty with no safe load: nothing is primary, Recover included") {
        in.head_has_filament = false;
        in.load_gcode.clear();
        const auto actions = AmsBackendAd5xIfs::runout_recovery_actions(in);
        REQUIRE(actions.size() == 3);
        for (const auto& a : actions) {
            CHECK(a.style != "primary");
        }
    }
    SECTION("head loaded: Resume leads and Load is gone") {
        in.head_has_filament = true;
        const auto actions = AmsBackendAd5xIfs::runout_recovery_actions(in);
        REQUIRE(actions.size() == 3);
        CHECK(actions[0].gcode == "RESUME");
        CHECK(actions[0].style == "primary");
        CHECK(actions[0].needs_hot_nozzle);
        for (const auto& a : actions) {
            CHECK(a.gcode.find("INSERT_PRUTOK_IFS") == std::string::npos);
        }
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: detail text names the plugin situation",
                 "[ams][ad5x_ifs][runout][1250]") {
    auto raise = [this](auto& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(b);
        REQUIRE(runs_out_after_dwell(b));
        return Ad5xIfsTestAccess::operation_detail(b);
    };

    SECTION("stock zMod: Infinite Spool Mode names the rule, no plugin mention") {
        // Source-verified: ANALOG_PRUTOK (zmod_ifs.py:cmd_ANALOG_PRUTOK) runs
        // always-on on head runout. The detail must name the feature and state
        // the type+colour+present match — NOT claim no switchover will happen.
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        const std::string detail = raise(backend);
        CHECK(detail.find("Infinite Spool Mode") != std::string::npos);
        // The retired string is gone.
        CHECK(detail.find("will not change to a backup spool") == std::string::npos);
        CHECK(detail.find("No auto-switchover plugin") == std::string::npos);
        // The same type+colour+present rule the plugin path promises.
        CHECK(detail.find("type") != std::string::npos);
        CHECK(detail.find("color") != std::string::npos);
        CHECK(detail.find("port sensor") != std::string::npos);
    }

    SECTION("plugin installed, backup off") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 0}}));
        const std::string detail = raise(backend);
        CHECK(detail.find("lessWaste") != std::string::npos);
        CHECK(detail.find("turned off") != std::string::npos);
    }

    SECTION("plugin installed, backup on: state the strict matching rule") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));
        const std::string detail = raise(backend);
        CHECK(detail.find("bambufy") != std::string::npos);
        // Both criteria promised, never one.
        CHECK(detail.find("type") != std::string::npos);
        CHECK(detail.find("color") != std::string::npos);
        CHECK(detail.find("port sensor") != std::string::npos);
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: any switchover path buys the longer confirm delay",
                 "[ams][ad5x_ifs][runout][1250]") {
    // Three switchover paths now qualify for the longer dwell: stock zMod's
    // ANALOG_PRUTOK (always-on), bambufy/lessWaste with variable_backup on.
    // The short delay survives only when a plugin is installed AND backup is
    // definitively off (or was never read).
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    SECTION("stock zMod (always-on ANALOG_PRUTOK) gets the longer dwell") {
        const auto delay = Ad5xIfsTestAccess::runout_confirm_delay(backend);
        // Compare against a plugin-with-backup-off baseline, which stays short.
        helix::test::StartedBackend<AmsBackendAd5xIfs> plugin_off;
        Ad5xIfsTestAccess::set_zcolor_supported(plugin_off, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(plugin_off, true);
        plugin_off.fire_status(make_ifs_vars_macro(json{{"variable_backup", 0}}));
        const auto short_delay = Ad5xIfsTestAccess::runout_confirm_delay(plugin_off);
        REQUIRE(delay > short_delay);
    }

    SECTION("plugin with backup on gets the longer dwell (unchanged)") {
        // Baseline is the SAME plugin with backup OFF; that is the only config
        // that still gets the short delay under the new matrix (stock zMod
        // always-on now also qualifies for the long delay).
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 0}}));
        const auto plain = Ad5xIfsTestAccess::runout_confirm_delay(backend);

        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));
        const auto with_backup = Ad5xIfsTestAccess::runout_confirm_delay(backend);
        REQUIRE(with_backup > plain);
    }
}

// The matrix above only pins what runout_confirm_delay_locked() RETURNS. Nothing
// there reaches evaluate_runout_locked(), and every other runout test ages the
// candidate by 600s — past both thresholds — so a predicate that ignored
// runout_confirm_delay_locked() and hardcoded either constant would sail through
// the whole file. This drives the real predicate on both sides of each config's
// own dwell.
TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: the predicate honours the per-config confirm delay",
                 "[ams][ad5x_ifs][runout][1250]") {
    // Probe ages are DERIVED from the backend's own runout_confirm_delay()
    // rather than written down, so this asserts the wiring rather than a
    // number: a predicate hardcoding the short constant fires early on a
    // long-dwell config, and one hardcoding the long constant never fires on a
    // short-dwell config. Either way a REQUIRE below goes red.
    constexpr auto kSlack = std::chrono::seconds(5);
    auto dwell_is_load_bearing = [this, kSlack](auto& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(b);
        REQUIRE(Ad5xIfsTestAccess::head_empty_armed(b));

        const auto dwell = Ad5xIfsTestAccess::runout_confirm_delay(b);
        REQUIRE(dwell > kSlack); // the two probes have to straddle it

        // Just short of the dwell: armed, paused, idle — everything else holds.
        // A too-short threshold raises here.
        Ad5xIfsTestAccess::age_head_empty(b, dwell - kSlack);
        REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(b));
        REQUIRE_FALSE(b.runout_active());
        REQUIRE(b.get_current_action() == AmsAction::IDLE);
        // The failed probe must leave the candidate armed, or the second probe
        // would be testing nothing.
        REQUIRE(Ad5xIfsTestAccess::head_empty_armed(b));

        // Just past it. A too-long threshold fails to raise here.
        Ad5xIfsTestAccess::age_head_empty(b, dwell + kSlack);
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(b));
        REQUIRE(b.runout_active());
        REQUIRE(b.get_current_action() == AmsAction::ERROR);
        return dwell;
    };

    // Stock zMod: no plugin, ANALOG_PRUTOK always-on -> the long dwell.
    helix::test::StartedBackend<AmsBackendAd5xIfs> stock;
    Ad5xIfsTestAccess::set_zcolor_supported(stock, false);
    const auto stock_dwell = dwell_is_load_bearing(stock);

    // Plugin installed with backup definitively OFF -> the short dwell. This is
    // the only config that still gets it.
    helix::test::StartedBackend<AmsBackendAd5xIfs> backup_off;
    Ad5xIfsTestAccess::set_zcolor_supported(backup_off, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backup_off, true);
    Ad5xIfsTestAccess::set_var_prefix(backup_off, "less_waste");
    backup_off.fire_status(make_ifs_vars_macro(json{{"variable_backup", 0}}));
    const auto backup_off_dwell = dwell_is_load_bearing(backup_off);

    // Plugin installed with backup ON -> back to the long dwell.
    helix::test::StartedBackend<AmsBackendAd5xIfs> backup_on;
    Ad5xIfsTestAccess::set_zcolor_supported(backup_on, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backup_on, true);
    Ad5xIfsTestAccess::set_var_prefix(backup_on, "bambufy");
    backup_on.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));
    const auto backup_on_dwell = dwell_is_load_bearing(backup_on);

    // Without this the three probes above could all be straddling one shared
    // number, and "honours the per-config delay" would mean nothing.
    CHECK(backup_off_dwell < stock_dwell);
    CHECK(backup_off_dwell < backup_on_dwell);
    CHECK(stock_dwell == backup_on_dwell);
}

TEST_CASE("AD5X IFS plugin visibility: which plugin, and is backup on",
          "[ams][ad5x_ifs][runout][1250]") {
    using B = AmsBackendAd5xIfs;

    SECTION("stock zMod: no plugin, switchover is a definite ON (ANALOG_PRUTOK)") {
        helix::test::StartedBackend<B> backend;
        REQUIRE(backend.get_plugin() == B::IfsPlugin::None);
        // No plugin -> plugin_backup_enabled() is nullopt (no flag to read), but
        // backup_state_locked() is ON: stock zMod's ANALOG_PRUTOK is always-on.
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupOn);
    }

    SECTION("lessWaste detected from its own save_variables") {
        helix::test::StartedBackend<B> backend;
        // The macro-existence latch has to be cleared first; stale save_variables
        // rows left by an uninstalled plugin must NOT read as installed.
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(
            make_save_variables(json{{"less_waste_tools", json::array({1, 2, 3, 4})}}));
        REQUIRE(backend.get_plugin() == B::IfsPlugin::LessWaste);
        // Nothing read yet -> unknown, NOT off.
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupUnknown);
    }

    SECTION("bambufy detected from its own save_variables") {
        helix::test::StartedBackend<B> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(
            make_save_variables(json{{"bambufy_tools", json::array({1, 2, 3, 4})}}));
        REQUIRE(backend.get_plugin() == B::IfsPlugin::Bambufy);
    }

    SECTION("a stale prefix without the macro is still None") {
        helix::test::StartedBackend<B> backend;
        // ifs_macro_confirmed_missing_ starts true (pessimistic).
        backend.fire_status(
            make_save_variables(json{{"less_waste_tools", json::array({1, 2, 3, 4})}}));
        REQUIRE(backend.get_plugin() == B::IfsPlugin::None);
        // Treated as stock zMod -> ANALOG_PRUTOK always-on.
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupOn);
    }

    SECTION("variable_backup is read from the macro dict, int or bool") {
        helix::test::StartedBackend<B> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

        REQUIRE(macro_changes_state(backend, json{{"variable_backup", 0}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{false});
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupOff);

        REQUIRE(macro_changes_state(backend, json{{"variable_backup", 1}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{true});
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupOn);

        REQUIRE(macro_changes_state(backend, json{{"variable_backup", false}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{false});

        // Unchanged value reports no change (so it does not force an event).
        REQUIRE_FALSE(macro_changes_state(backend, json{{"variable_backup", 0}}));
    }

    SECTION("a dict without variable_backup leaves the answer UNKNOWN") {
        helix::test::StartedBackend<B> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        REQUIRE_FALSE(
            macro_changes_state(backend, json{{"variable_tools", json::array({1, 2, 3, 4})}}));
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::kBackupUnknown);
    }

    SECTION("an empty dict is Klipper's 'no such object' answer, not data") {
        helix::test::StartedBackend<B> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        REQUIRE_FALSE(macro_changes_state(backend, json::object()));
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS runout: backup-slot match needs type AND colour AND presence",
                 "[ams][ad5x_ifs][runout][1250]") {
    // Lane 0 is the one that ran out: PLA / FF0000.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    SECTION("nothing else loaded: no match") {
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("same type, different colour: no match") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "00FF00");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("same colour, different type: no match") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("both match but the port reads empty: no match") {
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("all three: match, case-insensitively on the hex") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "pla");
        Ad5xIfsTestAccess::set_color(backend, 2, "ff0000");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == 2);
    }

    SECTION("the ran-out lane never matches itself") {
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) != 0);
    }
}
