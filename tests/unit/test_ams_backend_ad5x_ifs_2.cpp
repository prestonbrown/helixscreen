// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 47 to 76 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs], [ad5x_ifs][phase], [ad5x][ifs][1065], [ad5x_ifs][phase][1065], [general], and more

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

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: RESPOND line sets target before any extruder frame",
                 "[ams][ad5x_ifs][phase]") {
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    // Firmware emits the heat target via RESPOND before the first temp tick.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Heating the nozzle to 240 degrees");

    // Detail should reflect the parsed 240°C target even with no extruder frame.
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle to 240°C");

    // A temp frame whose target is 240 reaches target → CUTTING.
    backend.fire_status(make_extruder(240.0, 240.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);
}

TEST_CASE("AD5X IFS phase: RESPOND direction disambiguation", "[ams][ad5x_ifs][phase]") {
    SECTION("\"Unloading filament\" marks an unload op") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_head_filament(backend, true);
        // Begin as a load, then a RESPOND line corrects the direction to unload.
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unloading filament from IFS");
        // Reaching target on an unload op → CUTTING (not LOADING).
        backend.fire_status(make_extruder(230.0, 230.0));
        REQUIRE(backend.get_current_action() == AmsAction::CUTTING);
    }

    SECTION("\"Loading filament\" marks a load op") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        // Begin as an unload, then a RESPOND line corrects the direction to load.
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Loading filament into IFS");
        // Reaching target on a load op → LOADING (not CUTTING).
        backend.fire_status(make_extruder(230.0, 230.0));
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    }
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: full unload with ZERO RESPOND lines (fork robustness)",
                 "[ams][ad5x_ifs][phase]") {
    // Proves the English RESPOND strings are NOT load-bearing: temp + head
    // sensor alone drive the entire HEATING → CUTTING → UNLOADING sequence.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    backend.fire_status(make_extruder(100.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: HEATING does not finalize before target at 90s",
                 "[ams][ad5x_ifs][phase]") {
    // A real AD5X cold-start unload heats ~26°C→230°C in ~158s, which exceeds
    // the 90s ACTION_TIMEOUT. HEATING must get a longer dedicated budget so the
    // timeout doesn't snap to IDLE mid-heat. Later phases keep the short 90s.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    // Still heating (120°C of 230°C target).
    backend.fire_status(make_extruder(120.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // 120s elapsed: under the 300s HEATING budget → must NOT finalize.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Reach target → CUTTING. The transition resets the start clock, so a fresh
    // op clock now governs the 90s budget for this phase.
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    // 120s elapsed against the freshly-reset clock, with the 90s non-heating
    // budget → now surfaces ERROR (operation timed out).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: clock resets on phase transition (no immediate timeout)",
                 "[ams][ad5x_ifs][phase]") {
    // A phase transition occurring at elapsed > 90s must not immediately time
    // out the new phase — the start clock is reset on transition.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    backend.fire_status(make_extruder(150.0, 230.0));
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    // Immediately after the transition, a short elapsed must NOT finalize.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);
}

// Swap-aware LOADING timeout (#1065 v0.99.94, bundle NJB2U558).
//
// `INSERT_PRUTOK_IFS PRUTOK=N` when another lane is currently loaded runs an
// IMPLICIT UNLOAD of the seated lane first (heat → cut → retract), THEN loads
// the new lane. HelixScreen's LOAD phase tracker only watches `seen_head_rise`
// (filament reaching nozzle), so the implicit-unload head drop is invisible to
// the timeout clock. Result: LOADING times out at 90s while the macro is still
// mid-swap, surfacing a false "Loading error, feeding filament to nozzle
// (timed out)" popup even though the op completes successfully.
//
// Two fixes:
//   1. on_head_transition_locked: a head drop during a LOAD op resets
//      action_start_time_ (the implicit unload is genuine progress — it just
//      isn't surfaced as a separate phase today).
//   2. load_filament: when dispatched with another lane currently seated,
//      set swap_expected so check_action_timeout can extend the LOADING
//      budget and cover the implicit unload + load sequence.

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: head drop during LOAD resets the timeout clock (#1065 swap)",
                 "[ams][ad5x_ifs][phase][1065]") {
    // A LOAD op's implicit unload of the seated lane drives a head-sensor drop,
    // which resets action_start_time_ so the LOADING phase does not time out
    // mid-swap (NJB2U558).
    //
    // Test helper note: check_action_timeout(elapsed) OVERWRITES
    // action_start_time_ before running the check, so it can't be used to
    // verify a reset that happened earlier. We use set_action_age to plant a
    // stale clock, fire the head drop, then run_action_timeout (no overwrite)
    // — the timeout's behavior tells us whether the reset stuck.
    // Swap context: another lane is currently seated, so the head reads
    // loaded at LOADING-phase start. The implicit unload will drive
    // head_filament_ true → false; starting false would make that transition
    // a no-op and on_head_transition_locked would never run.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // Heat to target → LOADING begins. apply_phase_action_locked reset
    // action_start_time_ on the transition.
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);

    // Simulate 100s of LOADING-phase time passing — past the 90s default
    // budget, so without a reset the next check_action_timeout fires ERROR.
    // The state sync the last frame queued reads the clock, so it runs first.
    helix::ui::UpdateQueue::instance().drain();
    Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(100));

    // Implicit unload of the seated lane: head sensor drops. The swap has just
    // BEGUN — the actual load of the new lane hasn't started. This must reset
    // action_start_time_ to ~now so the LOADING budget counts from here, not from
    // 100s ago.
    backend.fire_status(make_head_sensor(false));

    // Run check_action_timeout WITHOUT set_action_age's overwrite — uses
    // whatever action_start_time_ the head drop left behind: ~0s, so no timeout.
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS phase: head drop during LOAD clears operation_indeterminate (#1065 swap)",
    "[ams][ad5x_ifs][phase][1065]") {
    // Companion to the timeout-reset test: the implicit-unload head drop is
    // a genuine progress signal, so the indeterminate ("Working…") detector
    // must clear: a stalled progress feed must not false-fire while the macro is
    // actively cutting/retracting the old lane.
    // Swap context: another lane is already seated at the head. The implicit
    // unload will drive head_filament_ true → false. Starting false would
    // miss the transition (was==detected==false) and never reach
    // on_head_transition_locked at all.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);

    // Stall the progress feed past the indeterminate threshold (~8s).
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(15));
    // Run the detector head — it should raise the indeterminate flag.
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Implicit-unload head drop is a progress signal — flag must clear on the
    // next check_action_timeout call (which is when the detector recomputes).
    backend.fire_status(make_head_sensor(false));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: head drop during UNLOAD still works as before (#1065 regression)",
                 "[ams][ad5x_ifs][phase][1065]") {
    // The unload path uses seen_head_drop to advance HEATING → CUTTING →
    // UNLOADING; the head drop's phase-advancement contract for UNLOAD ops is
    // unaffected by the swap handling.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);

    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS phase: plain load (no swap) still uses 90s LOADING budget (#1065 regression)",
    "[ams][ad5x_ifs][phase][1065]") {
    // The LOADING timeout is extended ONLY when another lane is currently seated
    // at dispatch. A plain load into an empty toolhead must keep the 90s budget —
    // otherwise a genuinely stuck load would hang the UI for 180s before surfacing
    // ERROR.
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    // No seated lane — swap_expected must NOT be set.
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);

    // 100s elapsed > 90s LOADING budget → must surface ERROR. The swap
    // extension must NOT apply here.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(100));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS phase: swap load (another lane seated) extends LOADING budget (#1065)",
                 "[ams][ad5x_ifs][phase][1065]") {
    // Reproduces bundle NJB2U558: load ch2 while ch4 is seated. The implicit
    // unload of ch4 eats ~50s; the load of ch2 then runs another ~40s. The
    // 90s LOADING budget fires mid-swap. Fix 2 extends LOADING to
    // kSwapLoadingTimeoutSeconds when seated_chan_ differs from the target.
    //
    // We can't easily call load_filament() here (it requires the gcode API +
    // check_preconditions). Instead we drive begin_phase directly + flip the
    // swap flag via the test accessor — exercising exactly the code path
    // check_action_timeout will consult.
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    // Seed another lane as currently seated — this is what load_filament
    // sees when dispatching and what triggers swap_expected.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 4); // slot 3 (0-based) seated
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    // Manually flip swap_expected — mirroring what load_filament does at
    // dispatch when seated_chan_ != target slot.
    Ad5xIfsTestAccess::set_swap_expected(backend, true);

    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);

    // 100s elapsed > 90s default LOADING budget. With swap_expected the
    // extended budget applies → must NOT surface ERROR yet.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(100));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Past the extended budget → ERROR fires (the extension is finite, not
    // unlimited — a genuinely stuck swap still surfaces). kSwapLoadingTimeoutSeconds
    // is 180; check at 190 to exceed it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(190));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

// A fresh GET_ZCOLOR is the early terminal signal for a tracked op, finalizing
// to IDLE within a second instead of waiting out the 90s backstop, but only once
// the op has physically progressed past its head transition (the post-dispatch
// query fires before the cut) and only on a valid read.
TEST_CASE("AD5X IFS phase: GET_ZCOLOR quick-finishes an op only after its head transition",
          "[ams][ad5x_ifs][phase]") {
    struct Row {
        const char* name;
        bool unload;
        bool head_transition;
        std::optional<int> extruder_slot;
        bool valid;
        AmsAction expected;
    };
    const Row rows[] = {
        {"unload after the head drop", true, true, std::nullopt, true, AmsAction::IDLE},
        {"unload before the head drop", true, false, std::nullopt, true, AmsAction::CUTTING},
        {"load after the head rise", false, true, 0, true, AmsAction::IDLE},
        {"an invalid read after the head drop", true, true, std::nullopt, false,
         AmsAction::UNLOADING},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
            auto& backend = *backend_reg;
            Ad5xIfsTestAccess::set_head_filament(backend, r.unload);
            Ad5xIfsTestAccess::begin_phase(backend, r.unload);
            backend.fire_status(make_extruder(230.0, 230.0));
            REQUIRE(backend.get_current_action() ==
                    (r.unload ? AmsAction::CUTTING : AmsAction::LOADING));
            if (r.head_transition) {
                backend.fire_status(make_head_sensor(!r.unload));
                REQUIRE(backend.get_current_action() ==
                        (r.unload ? AmsAction::UNLOADING : AmsAction::PURGING));
            }

            Ad5xIfsTestAccess::apply_zcolor_result(backend, make_zcolor(r.extruder_slot, r.valid));

            CHECK(backend.get_current_action() == r.expected);
            const bool finished = r.expected == AmsAction::IDLE;
            CHECK(Ad5xIfsTestAccess::phase_active(backend) == !finished);
            if (finished) {
                CHECK(Ad5xIfsTestAccess::operation_detail(backend).empty());
            }
        }
    }
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: inactive tracker preserves legacy snap-to-IDLE",
                 "[ams][ad5x_ifs][phase]") {
    // When the phase tracker is INACTIVE (external/firmware-initiated action set
    // via set_action), a head transition must still snap directly to IDLE — the
    // legacy backward-compat path. This mirrors the existing "action state
    // tracking" cases but asserts the gating contract explicitly.
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);
}

// 21. Native ZMOD IFS active slot inferred from head sensor

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS native ZMOD infers active slot from head sensor",
                 "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Seed colors_ + port_presence_ to simulate the post-GET_ZCOLOR / Adventurer5M
    // state. parse_save_variables does not set these fields (they live in zmod's
    // namespace, not lessWaste's).
    seed_standard_colors(backend);

    // No per-port sensors — only motion sensor and save_variables
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};

    backend.fire_status(notification);

    // Active tool is T0 → port 1 → slot 0. With head filament detected and no
    // per-port sensors, the active slot should be inferred as LOADED.
    auto info = backend.get_slot_info(0);
    REQUIRE(info.status == SlotStatus::LOADED);

    // Non-active slots with port_presence_ true are AVAILABLE (not EMPTY).
    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.status == SlotStatus::AVAILABLE);
}

// 22. has_ifs_vars_ detection

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS has_ifs_vars detection", "[ams][ad5x_ifs]") {
    SECTION("defaults to false") {
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when lessWaste variables found and macro verified") {
        // Simulate initial query confirming macro exists (clears pessimistic latch)
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when bambufy variables found and macro verified") {
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        backend.fire_status(make_save_variables(vars));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables arrive before macro check completes") {
        // Latch starts true (pessimistic) — save_variables alone can't enable has_ifs_vars
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables has no recognized prefix") {
        json vars;
        vars["some_other_var"] = 42;
        backend.fire_status(make_save_variables(vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS has_ifs_vars reset when macro missing",
                 "[ams][ad5x_ifs]") {
    // Scenario: lessWaste/bambufy plugins partially installed — save_variables data
    // exists but _IFS_VARS gcode macro is not loaded. parse_save_variables() sets
    // has_ifs_vars_ true, but on_started() should reset it when the macro is absent.
    // This test verifies the parse step sets the flag (the reset happens in on_started).

    SECTION("pessimistic latch prevents flag before macro check") {
        // Latch starts true — parse_save_variables can't set has_ifs_vars_
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Clearing the latch (as initial query would for a present macro) allows it
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION(
        "later save_variables notify cannot re-enable has_ifs_vars once macro confirmed missing") {
        // Simulate macro verified + save_variables arriving → has_ifs_vars_ = true
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Simulate on_started() discovering the macro is absent.
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, true);

        // A later subscription update carrying lessWaste save_variables arrives.
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Same for bambufy prefix.
        json bambufy_vars;
        bambufy_vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        bambufy_vars["bambufy_tools"] =
            json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        backend.fire_status(make_save_variables(bambufy_vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }
}

// lessWaste/bambufy save_variables rows persist in printer_data/database/ even
// after the user uninstalls the plugin and the gcode_macro _ifs_vars goes away.
// parse_save_variables reads <prefix>_tools / _current_tool / _external only
// while has_ifs_vars_ is set (the plugin is actively loaded); otherwise a user
// with stale rows would keep using the dead plugin's last-known tool map and
// active-tool guess as truth on every boot.
TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS stale save_variables ignored when plugin macro missing",
                 "[ams][ad5x_ifs]") {
    // Confirm latch defaults to "macro missing" — the on_started() initial
    // query never confirmed the macro exists.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Build a save_variables blob that looks like the user once had lessWaste
    // installed: tools/active/external all set to non-default values that
    // would VISIBLY change behavior if applied.
    auto stale = standard_variables();
    stale["less_waste_current_tool"] = 2; // not the default 0
    stale["less_waste_external"] = 1;     // bypass mode active
    stale["less_waste_tools"] =           // T0 -> port 4 (not 1)
        json::array({4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

    backend.fire_status(make_save_variables(stale));

    // Macro confirmed missing → none of the stale plugin state should have
    // taken effect. active_tool stays at its default -1, external_mode stays
    // false, and the slot-tool mapping in SlotRegistry was never overwritten.
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == -1);
    CHECK_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    CHECK_FALSE(backend.is_bypass_active());

    // Now flip the latch as on_started() would when it sees the macro is
    // present, then replay — the SAME save_variables payload now applies
    // cleanly. This proves the gate, not some other guard, is what blocked
    // the read.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.fire_status(make_save_variables(stale));
    CHECK(Ad5xIfsTestAccess::has_ifs_vars(backend));
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == 2);
    CHECK(Ad5xIfsTestAccess::external_mode(backend));
    CHECK(backend.is_bypass_active());
}

// Native-ZMOD users without lessWaste/bambufy who happen to have stale prefixed
// save_variables data can land in a state where has_ifs_vars_ is true: Klipper/
// Kalico return `{}` for non-existent objects, so key presence alone falsely
// satisfies the macro-existence check. Every Adventurer5M.json poll would then
// fire `_IFS_VARS colors=...` / `types=...` and Klipper would reject them with
// `// Unknown command:"_IFS_VARS"`. This asserts the self-heal path: as soon as
// Klipper rejects the command, we demote has_ifs_vars_ and latch the macro as
// missing.
TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS self-heals on Unknown command:\"_IFS_VARS\" response",
                 "[ams][ad5x_ifs]") {
    // Seed the wrong-state: macro 'confirmed present' and has_ifs_vars_ true.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Klipper's gcode.cmd_default emits this exact line via respond_info
    // when an unknown command is dispatched (klippy/gcode.py).
    const std::string rejection = "// Unknown command:\"_IFS_VARS\"";
    Ad5xIfsTestAccess::on_gcode_response_line(backend, rejection);

    CHECK_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Once latched missing, replaying save_variables can't re-enable it
    // (same contract as the stale-save_variables test above).
    backend.fire_status(make_save_variables(standard_variables()));
    CHECK_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
}

// 23. parse_adventurer_json (native ZMOD Adventurer5M.json)

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS parse_adventurer_json", "[ams][ad5x_ifs]") {
    SECTION("standard 4-slot JSON with # prefixed hex colors") {
        std::string content = R"({
            "FFMInfo": {
                "channel": 2,
                "ffmColor0": "",
                "ffmColor1": "#FF0000",
                "ffmColor2": "#00FF00",
                "ffmColor3": "#0000FF",
                "ffmColor4": "#FFFFFF",
                "ffmType0": "?",
                "ffmType1": "PLA",
                "ffmType2": "PETG",
                "ffmType3": "ABS",
                "ffmType4": "TPU"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xFF0000);
        REQUIRE(info0.material == "PLA");

        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.color_rgb == 0x00FF00);
        REQUIRE(info1.material == "PETG");

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x0000FF);
        REQUIRE(info2.material == "ABS");

        auto info3 = backend.get_slot_info(3);
        REQUIRE(info3.color_rgb == 0xFFFFFF);
        REQUIRE(info3.material == "TPU");
    }

    SECTION("lowercase hex is uppercased") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ff8800",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PLA");
    }

    SECTION("missing FFMInfo section is graceful no-op") {
        std::string content = R"({"OtherSection": {"key": "value"}})";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("partial slots — only 2 of 4 populated") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#AABBCC",
                "ffmType1": "PLA",
                "ffmColor3": "#112233",
                "ffmType3": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xAABBCC);
        REQUIRE(info0.material == "PLA");

        // Slot 1 (port 2) not in JSON — stays at default
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.material.empty());

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x112233);
        REQUIRE(info2.material == "PETG");
    }

    SECTION("# prefix stripping") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ABCDEF",
                "ffmType1": "ABS"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xABCDEF);
    }

    SECTION("empty color string defaults to gray") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0x808080);
        REQUIRE(info.material == "PLA");
    }

    SECTION("invalid JSON is graceful no-op") {
        std::string content = "this is not json {{{";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("color without # prefix still works") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "FF8800",
                "ffmType1": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PETG");
    }
}

TEST_CASE("AD5X IFS parse_adventurer_json overwrites a slot synced in memory", "[ams][ad5x_ifs]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    helix::test::apply_edit(backend, 0, edit);

    // The file is the firmware's colour and type: a re-read wins
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#AABBCC",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCC);
    REQUIRE(info.material == "ABS");
}

// Native ZMOD: parse_adventurer_json does NOT own presence (GET_ZCOLOR does)
//
// parse_adventurer_json must not infer presence from the persisted ffmColorN
// field (#716): "non-empty color means filament present" is wrong on native
// ZMOD, which persists the colour/material assignment across unload/eject and
// never writes an empty colour, so a previously-emptied channel would be
// resurrected to "present" on the next content-changed poll (external unload not
// reflected; an edit to one channel resurrecting another). Presence is owned
// solely by GET_ZCOLOR (apply_zcolor_result) — the RS-485 silk sensor — and the
// JSON parse must not touch port_presence_. It still refreshes colors_/materials_
// for clean slots.

// A slot write carries filament IDENTITY. On native ZMOD the RS-485 silk
// sensors (IFS_STATUS "Ports") own presence, and identity metadata survives an
// eject by design (#1071), so inferring presence from "this lane has a colour
// and a material" resurrects a lane the sensors just reported empty. The
// inference is a fallback for devices with no silk reading at all, so it must
// stand down once IFS_STATUS Ports has been parsed.
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS a slot write does not own presence once IFS_STATUS Ports has spoken",
                 "[ams][ad5x_ifs]") {
    // Native ZMOD: no lessWaste/bambufy per-port sensor exporter.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // The silk sensors report every lane empty. That parse latches them as the
    // presence authority for the life of the backend. Ports rides the same
    // IFS_STATUS frame as Chan, so a frame carrying Ports always carries Chan
    // too; 0 is "nothing engaged", which is what an all-empty carousel reports.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_chan = 0;
    r.ifs_ports = std::vector<bool>{false, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(Ad5xIfsTestAccess::ifs_status_ports_seen(backend));
    for (int i = 0; i < 4; ++i) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, i));
    }

    SECTION("a weight-only refresh leaves the emptied lane empty") {
        // The shape SpoolmanManager's weight poll produces: read the lane back,
        // change only the weights, write the whole struct. The identity it reads
        // back is the one #1071 deliberately retains across the eject.
        SlotInfo slot = backend.get_slot_info(0);
        slot.material = "PETG";
        slot.color_rgb = 0xED2C2C;
        slot.spoolman_id = 7;
        slot.remaining_weight_g = 218.0f;
        slot.total_weight_g = 1000.0f;

        helix::test::apply_edit(backend, 0, slot);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    }

    SECTION("a user identity edit also leaves presence to the sensors") {
        SlotInfo slot = backend.get_slot_info(1);
        slot.material = "PLA";
        slot.color_rgb = 0x8000FF;

        helix::test::apply_edit(backend, 1, slot);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }

    SECTION("every other lane is left alone too") {
        // The reporter's bundle shows one poll resurrecting all four lanes at
        // once, because the poll visits every Spoolman-linked lane in turn.
        for (int i = 0; i < 4; ++i) {
            SlotInfo slot = backend.get_slot_info(i);
            slot.material = "PETG";
            slot.color_rgb = 0x010462;
            helix::test::apply_edit(backend, i, slot);
        }
        for (int i = 0; i < 4; ++i) {
            REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, i));
        }
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS parse_adventurer_json does not own presence on native ZMOD",
                 "[ams][ad5x_ifs]") {
    // No per-port sensors — this is native ZMOD
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    SECTION("non-empty colour does NOT set presence — only GET_ZCOLOR can") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#161616",
                "ffmColor2": "#FFFFFF",
                "ffmColor3": "#D3C4A3",
                "ffmColor4": "#F72224",
                "ffmType1": "PLA+",
                "ffmType2": "PLA+",
                "ffmType3": "PLA+",
                "ffmType4": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Presence is untouched by the parse (default false). The colour/material
        // refresh still happened — but the slot is not "present" until GET_ZCOLOR
        // (the silk sensor) confirms it.
        for (int i = 0; i < 4; ++i) {
            REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, i));
            REQUIRE(backend.get_slot_info(i).status == SlotStatus::EMPTY);
        }
    }

    SECTION("colour/material refresh survives even though presence does not flip") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor2": "#FF0000",
                "ffmType2": "PLA"
            }
        })";
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Parse must not have set presence...
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));

        // ...but once GET_ZCOLOR marks the slot present, the colour/material the
        // parse refreshed is visible. (apply_zcolor_result with an empty HEX in
        // the slot keeps the JSON colour via the old-format / empty-hex guard, so
        // assert presence only here.)
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(backend.get_slot_info(1).is_present());
    }
}

// Native ZMOD presence resurrection
//
// Repro: channel 0 is LOADED (GET_ZCOLOR), then physically unloaded so the next
// GET_ZCOLOR reports it ABSENT (presence cleared). zmod keeps ffmColor1 in
// Adventurer5M.json. A subsequent edit to a DIFFERENT channel triggers a
// content-changed JSON poll. The parse never touches presence, so channel 0
// stays EMPTY.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS emptied channel is not resurrected by a JSON edit to another channel",
                 "[ams][ad5x_ifs]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // 1. Establish channel 0 LOADED via GET_ZCOLOR (the silk sensor's truth).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "161616"};
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FFFFFF"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).is_present());

    // 2. Channel 0 physically unloaded — GET_ZCOLOR now reports it ABSENT.
    //    Channel 1 stays present.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FFFFFF"};
        // slots[0] left empty → channel 0 absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);

    // 3. A content-changed JSON poll edits a DIFFERENT channel (channel 1's
    //    colour) while zmod still persists channel 0's stale non-empty colour.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#161616",
            "ffmType1": "PLA",
            "ffmColor2": "#00AAFF",
            "ffmType2": "PETG"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // 4. Channel 0 must STAY empty — the persisted non-empty ffmColor1 must not
    // (The parse must not infer presence from colour.)
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
}

// Pre-SILENT zmod fallback: JSON inference is the ONLY presence source when
// GET_ZCOLOR SILENT=1 is unsupported (latched false on a prompt-dialog reply)
//
// The resurrection fix makes GET_ZCOLOR the sole presence authority — but that
// authority only exists on modern zmod. On pre-SILENT zmod, GET_ZCOLOR returns
// a prompt dialog, zcolor_silent_supported_ latches false, and schedule_zcolor_
// query()/query_zcolor_silent() no-op forever. Without the gated fallback below
// there would be NO presence source at all (every channel stuck EMPTY). So when
// SILENT is unsupported, parse_adventurer_json must resume the legacy inference.

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS pre-SILENT zmod falls back to JSON presence inference",
                 "[ams][ad5x_ifs]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#161616", "ffmType1": "PLA",
            "ffmColor2": "#FF0000", "ffmType2": "PETG"
        }
    })";

    SECTION("SILENT supported (modern zmod): parse does NOT set presence") {
        REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend)); // default
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }

    SECTION("SILENT unsupported (pre-SILENT zmod): parse infers presence") {
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
        // Non-empty colours → present; this is the only presence source here.
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(backend.get_slot_info(0).is_present());
    }
}

// Override-clear is driven by GET_ZCOLOR's present->absent transition (Change 2)
//
// When a spool is removed, its brand/spool_name/spoolman_id override must be
// cleared so it doesn't haunt the now-empty slot or get re-applied to whatever
// loads next. That cleanup rides on apply_zcolor_result's present->absent
// transition.

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS GET_ZCOLOR present->absent clears the slot override",
                 "[ams][ad5x_ifs]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // Slot 0 present with a user override staged.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "161616"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyTerra Charcoal";
    ovr.spoolman_id = 42;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // GET_ZCOLOR now reports slot 0 ABSENT → present->absent transition must
    // clear the override.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty → absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    // #1071: an emptied lane now KEEPS its Spoolman link (see the dedicated
    // keep-the-link test below) — only firmware-sourced presence/status drops.
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// #1065: after an eject, the RS-485 silk sensor lags ~1s before it reads the
// just-retracted lane clear. The eject-follow-up IFS_STATUS frame can therefore
// still report the lane present and resurrect it — and the LAST-ejected lane has
// no later query to re-correct it, so it kept offering Unload. eject_lane() now
// stamps the eject and apply_zcolor_result ignores a false->true presence flip
// for that lane within the settling window; a true re-insert after the window is
// honored.
TEST_CASE_METHOD(
    RegisteredAd5xIfs,
    "AD5X IFS eject-settling: a lagging Ports read does not resurrect the lane (#1065)",
    "[ams][ad5x_ifs][1065]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // All four lanes present (establish via the presence helper so status is set).
    for (int i = 0; i < 4; ++i) {
        Ad5xIfsTestAccess::set_port_presence(backend, i, true);
    }
    REQUIRE(backend.get_slot_info(0).is_present());

    // Eject lane 0 — optimistic clear + eject stamp.
    Ad5xIfsTestAccess::mark_ejected(backend, 0);
    REQUIRE_FALSE(backend.get_slot_info(0).is_present());

    // Follow-up query lands while the silk sensor is still settling: Ports still
    // reads lane 0 present. Within the window this must NOT resurrect the lane.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 0; // real IFS_STATUS frames carry Chan alongside Ports
        r.ifs_ports = std::vector<bool>{true, true, true, true};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);

    // After the settling window, a genuine re-insert (Ports present) is honored.
    Ad5xIfsTestAccess::expire_eject_window(backend, 0);
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 0;
        r.ifs_ports = std::vector<bool>{true, true, true, true};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).is_present());
}

// #1065 load-complete flash: FFMInfo.channel is sticky (only the ~5s JSON poll
// refreshes it, only an eject clears it), so right after a load into a DIFFERENT
// lane it still names the PREVIOUS lane. If that lane reads empty in the frame's
// Ports snapshot the pointer is stale and must not be adopted as seated — else
// the load-complete frame flashes the previous lane loaded and repaints its
// retained filament. Here FFMInfo.channel=4 is stale (lane 4 empty) while a load
// into lane 3 completes (Chan=3).
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS stale FFMInfo.channel at an empty lane is not adopted as seated (#1065)",
                 "[ams][ad5x_ifs][1065]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    Ad5xIfsTestAccess::set_ffm_channel(backend, 4); // sticky pointer at now-empty lane 4
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 3;                                           // real seated lane = 3
        r.ifs_ports = std::vector<bool>{true, true, true, false}; // lane 4 empty
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "111111"};
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "222222"};
        r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "333333"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // Seated follows the present lane (Chan=3), NOT the stale FFMInfo.channel=4.
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 3);
    REQUIRE(backend.get_current_slot() == 2);             // lane 3 == slot index 2
    REQUIRE_FALSE(backend.get_slot_info(3).is_present()); // lane 4 does not flash loaded
    REQUIRE(backend.get_slot_info(3).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(2).is_present());
}

// #1071: AD5X must match the AFC / Happy Hare backends, which never clear the
// lane->Spoolman link when a lane goes empty (runout / unload / eject). The
// emptied lane renders as removed (presence=false, status != LOADED) but
// retains spoolman_id / brand / spool_name / color / material so a re-inserted
// same spool keeps its assignment. (Previously AD5X diverged by auto-calling
// clear_override_locked on the present->absent transition.)
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: a lane going empty keeps its Spoolman link (AFC/HH parity)",
                 "[ams][ad5x][ifs][spoolman][1071]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // Slot 0 present with a user Spoolman override staged.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    helix::ams::FilamentSlotOverride ovr;
    ovr.spoolman_id = 42;
    ovr.brand = "Polymaker";
    ovr.spool_name = "Polymaker PLA";
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.material = "PLA";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Lane 0 goes empty (eject / runout): GET_ZCOLOR reports slot 0 ABSENT.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty => absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // Presence flips false; the slot renders empty/removed...
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.status != SlotStatus::LOADED);
    // ...but the Spoolman link is RETAINED so a re-inserted same spool keeps it.
    CHECK(info.spoolman_id == 42);
    CHECK(info.brand == "Polymaker");
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// #1065: a physical insert must refresh material/color from firmware truth on a
// lane whose override is auto-tracked (no Spoolman binding) even though the
// override's fields were user-locked (by a menu type-set, or by the pessimistic
// !material.empty() load default). A physical insert emits no CHANGE_ZCOLOR, so
// the #981 external-edit clear never fires — the insert edge itself must unlock.
// Counterpart to the above: a lane with a REAL Spoolman binding is a deliberate
// identity the user attached. #1071 retains it across an eject/insert cycle, so
// the insert edge must NOT unlock it — the bound spool's material/color stick.
// #1065 Fix B: on modern ZMOD the firmware material can surface one parse frame
// BEFORE IFS_STATUS Ports flips the slot present. The type detector must HOLD
// the baseline while the slot reads not-present, so the delta survives until a
// present-lane frame and the sync fires — instead of advancing the baseline and
// swallowing the change (color updated on screen, material stuck).
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: type detector holds baseline through presence lag on insert (#1065)",
                 "[ams][ad5x][ifs][1065]") {
    // Establish a present-lane baseline of PETG.
    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PETG",
                                                                /*slot_has_filament=*/true));
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "PETG");

    // Presence-lag frame: firmware says PLA but the slot still reads not-present.
    // No sync, and — crucially — the baseline is HELD at PETG, not advanced.
    CHECK_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                              /*slot_has_filament=*/false));
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "PETG");

    // Presence catches up: the held PETG->PLA delta is still detectable, so the
    // sync fires (returns true). Without the hold, the baseline would already be
    // PLA and this would return false — the swallow.
    CHECK(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                        /*slot_has_filament=*/true));
}

// #1065 Fix B, color counterpart: same presence-lag hold for the color detector.
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: color detector holds baseline through presence lag on insert (#1065)",
                 "[ams][ad5x][ifs][1065]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF0000,
                                                                 /*slot_has_filament=*/true));
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u);

    CHECK_FALSE(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x00FF00,
                                                               /*slot_has_filament=*/false));
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u); // baseline held

    CHECK(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x00FF00,
                                                         /*slot_has_filament=*/true)); // now syncs
}

// #1065 Fix B: an EMPTY material on an empty lane (eject) still advances the
// baseline to "" so the following insert is a genuine "" -> MATERIAL delta.
TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: eject still baselines material to empty so insert re-detects (#1065)",
                 "[ams][ad5x][ifs][1065]") {
    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PETG",
                                                                /*slot_has_filament=*/true));

    // Eject: empty material, slot empty, a color reading present (the #808080
    // placeholder). Baseline advances to "".
    CHECK_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "",
                                                              /*slot_has_filament=*/false,
                                                              /*observed_color=*/0x808080u));
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "");

    // Insert: "" -> PLA is a real delta on a present lane, so the sync fires.
    CHECK(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                        /*slot_has_filament=*/true));
}
