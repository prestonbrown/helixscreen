// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 101 to 133 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly [bypass],
// [968], [1250], [presence][probe_on_insert][1387], [clear_fault], and more

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

TEST_CASE("CFS defers the insert probe while the box latched runout",
          "[ams][cfs][presence][probe_on_insert][1387]") {
    // The #1387 shape: the box is mid-operation (runout pause, filament_useup
    // latched at 1) when the user seats a fresh spool. Probing a busy box makes
    // it answer busy, firmware raises key843, and the failed probe pins the
    // bay's remain_len at 255. The probe must wait for an idle poll and fire
    // exactly once when it comes.
    helix::test::StartedBackend<CfsRemapHelper> backend;
    // An insert the reader cannot name queues the same-spool notice; drain
    // each poll so the queue empties with the test.
    auto poll = [&backend](const json& box) {
        backend.fire_status(make_cfs_notification(box));
        helix::ui::UpdateQueue::instance().drain();
    };

    const json empty_bcd =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    poll(empty_bcd); // seeds bay_occupied_ without probing
    backend.captured.clear();

    // Spool into bay B on the same frame the box reports runout.
    json busy = make_unit_box_explicit(
        {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
        {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
    busy["filament_useup"] = 1;
    poll(busy);
    CHECK(backend.captured.empty()); // deferred, not dropped

    // The reload lands: useup clears, and exactly one deferred re-attempt
    // fires on that poll.
    json idle = busy;
    idle["filament_useup"] = 0;
    poll(idle);
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");

    // And only once: a further idle poll must not re-probe.
    backend.captured.clear();
    poll(idle);
    CHECK(backend.captured.empty());
}

TEST_CASE("CFS drops a deferred insert probe when the bay empties first",
          "[ams][cfs][presence][probe_on_insert][1387]") {
    // A spool inserted into a busy box defers its probe; if the user pulls it
    // back out before the box goes idle, there is nothing left to probe and
    // the deferred mask must not fire.
    helix::test::StartedBackend<CfsRemapHelper> backend;
    // An insert the reader cannot name queues the same-spool notice; drain
    // each poll so the queue empties with the test.
    auto poll = [&backend](const json& box) {
        backend.fire_status(make_cfs_notification(box));
        helix::ui::UpdateQueue::instance().drain();
    };

    const json empty_bcd =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    poll(empty_bcd); // seeds bay_occupied_ without probing
    backend.captured.clear();

    json busy = make_unit_box_explicit(
        {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
        {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
    busy["filament_useup"] = 1;
    poll(busy);
    CHECK(backend.captured.empty()); // deferred

    // Bay B emptied again while the deferral was pending; the idle poll must
    // release nothing.
    json idle_and_empty =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    idle_and_empty["filament_useup"] = 0;
    poll(idle_and_empty);
    CHECK(backend.captured.empty());

    // The deferred set is consumed: re-seating the spool starts a fresh edge
    // and probes normally on the idle box.
    json reseat = busy;
    reseat["filament_useup"] = 0;
    poll(reseat);
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");
}

TEST_CASE("CFS defers the insert probe while a print holds the machine",
          "[ams][cfs][presence][probe_on_insert][1387]") {
    // The other busy signal from #1387: the job owns the toolhead. A runout
    // pause leaves CFS's own system_info_.action IDLE (it is only synthesized
    // around OUR dispatched scripts), so the pause has to be read from the
    // print lifecycle.
    helix::test::StartedBackend<CfsLifecycleHelper> backend;
    auto poll = [&backend](const json& box) { backend.fire_status(make_cfs_notification(box)); };

    const json empty_bcd =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    poll(empty_bcd); // seeds bay_occupied_ without probing
    backend.captured.clear();

    // publish_lifecycle_state() runs synchronously inside update_from_status,
    // so the subject the gate reads is current without draining the queue. The
    // drain is only hygiene: update_from_status queues unrelated work (the
    // filament-sensor manager) that must not leak past this test.
    helix::test::set_wire_state(backend.state, helix::PrintJobState::PAUSED);
    helix::ui::UpdateQueue::instance().drain();

    const json b_in = make_unit_box_explicit(
        {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
        {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
    poll(b_in);
    CHECK(backend.captured.empty()); // deferred while the print holds the machine

    // Job ends: the deferred probe fires once on the next poll. This poll has
    // NO new insert edge (bay_occupied_ already latched), so the dispatch can
    // only come from the deferred set.
    helix::test::set_wire_state(backend.state, helix::PrintJobState::STANDBY);
    helix::ui::UpdateQueue::instance().drain();
    poll(b_in);
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");
}

TEST_CASE("CFS runout: 'no identical supplies' raises one runout fault", "[ams][cfs][1250]") {
    CfsRemapHelper backend;

    auto ev = backend.classify_error("// no identical supplies", paused_ctx());
    REQUIRE(ev.has_value());
    // The runout fault the hook raises is what the trait claims.
    CHECK(backend.traits().raises_runout_fault);
    CHECK(backend.traits().raises_runout_fault_during_job);
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->sticky);
    // Titled, so modal_title_for() does not relabel it "Filament System Error".
    CHECK(ev->title == std::string("Filament runout"));
    // The user is told the CFS found nothing to switch to, not just "error".
    CHECK(ev->detail.find("no matching spool") != std::string::npos);
    // Raw firmware wording is preserved for cross-channel dedup, `//` stripped.
    CHECK(ev->raw_detail == "no identical supplies");

    // Exactly the two vetted actions, in order.
    REQUIRE(ev->recovery_actions.size() == 2);
    CHECK(ev->recovery_actions[0].gcode == "RESUME");
    CHECK(ev->recovery_actions[0].style == "primary");
    // Resuming extrudes on the next move — a cold nozzle fails the same way the
    // print did.
    CHECK(ev->recovery_actions[0].needs_hot_nozzle);
    // NOT BOX_ERROR_RESUME_PROCESS: that only drives the box half and leaves the
    // job paused (it is reached FROM RESUME via RESUME_EXTERNAL_PROCESS).
    CHECK(ev->recovery_actions[0].gcode != AmsBackendCfs::recover_gcode());

    CHECK(ev->recovery_actions[1].gcode == AmsBackendCfs::reset_gcode());
    CHECK(ev->recovery_actions[1].gcode == "BOX_ERROR_CLEAR");
    // State only — must stay tappable on a cold nozzle.
    CHECK_FALSE(ev->recovery_actions[1].needs_hot_nozzle);
}

// The give-up lines BOX_CHECK_MATERIAL_REFILL prints, each naming its cause.
// Matching is by fragment, not sentence: these are untranslated literals from one
// firmware build and the surrounding words are the least durable part. "no auto
// refill" (the 2.3.5.34 box_wrapper's terse form) shares no fragment with the
// long form. "no tray with ingredients found" means a same_material group exists
// but every member is empty (docs/devel/printer-research/CREALITY_CFS_K1_INTERNALS.md),
// so it says to load one, not to buy matching filament; like the other strong
// matches it needs no box latch, so a fresh backend classifies it.
TEST_CASE("CFS runout: each give-up wording raises one runout fault naming its cause",
          "[ams][cfs][968][1250]") {
    struct Row {
        const char* line;
        const char* detail_has;
        const char* raw;
        const char* detail_lacks;
    };
    const Row rows[] = {
        {"// disable material automatic refill", "Auto-Refill is off",
         "disable material automatic refill", nullptr},
        {"// no auto refill", "Auto-Refill is off", "no auto refill", nullptr},
        {"// no tray with ingredients found", "empty", "no tray with ingredients found",
         "no matching spool"},
        {"// No tray with ingredients found!", "empty", nullptr, nullptr},
        {"// There are no identical supplies available!", "no matching spool", nullptr, nullptr},
        {"// Material automatic refill is disabled", "Auto-Refill is off", nullptr, nullptr},
        {"no identical supplies", "no matching spool", "no identical supplies", nullptr},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.line) {
            CfsRemapHelper backend;
            auto ev = backend.classify_error(r.line, paused_ctx());
            REQUIRE(ev.has_value());
            CHECK(ev->source == helix::ErrorSource::CFS);
            CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
            CHECK(ev->sticky);
            CHECK(ev->title == std::string("Filament runout"));
            CHECK(ev->detail.find(r.detail_has) != std::string::npos);
            if (r.detail_lacks) {
                CHECK(ev->detail.find(r.detail_lacks) == std::string::npos);
            }
            if (r.raw) {
                CHECK(ev->raw_detail == r.raw);
            }
            REQUIRE(ev->recovery_actions.size() == 2);
            CHECK(ev->recovery_actions[0].gcode == "RESUME");
            CHECK(ev->recovery_actions[1].gcode == "BOX_ERROR_CLEAR");
        }
    }
}

// Post-operation phase verification (#968)
//
// The BOX_* primitives record and queue failures instead of raising at the
// failing command, so a load that never fed filament still drains the script
// and reports success at every RPC layer. These pin the rule that reads the
// one independent physical witness we have: the toolhead filament switch.

// A load must end with filament at the switch and a bay unload without it. A
// bypass unload has no lane: QUIT_MATERIAL and our fallback pull ~10 mm to clear
// the melt zone and leave the user to pull the rest, so filament still at the
// switch is its success; the exemption never covers a load. Until the switch has
// taken a reading (filament_detected is null at first, and absent on a printer
// without one) the stored value is a default, not an observation, so nothing is
// claimed either way. Synthesized sub-phases and non-material actions have no
// filament end-state contract and are never judged.
TEST_CASE("CFS phase verify judges load and unload by the toolhead switch",
          "[ams][cfs][968][bypass]") {
    using V = AmsBackendCfs::PhaseVerdict;
    struct Row {
        AmsAction op;
        bool sensor_ever_read;
        bool filament_at_end;
        bool bypass_unload;
        V verdict;
    };
    const Row rows[] = {
        // The #968 failure: script drained clean, nozzle still empty.
        {AmsAction::LOADING, true, false, false, V::LoadDidNotReachNozzle},
        {AmsAction::LOADING, true, true, false, V::Ok},
        {AmsAction::UNLOADING, true, true, false, V::UnloadLeftFilament},
        {AmsAction::UNLOADING, true, false, false, V::Ok},
        {AmsAction::UNLOADING, true, true, true, V::Ok},
        {AmsAction::LOADING, true, false, true, V::LoadDidNotReachNozzle},
        {AmsAction::LOADING, false, false, false, V::Unverifiable},
        {AmsAction::LOADING, false, true, false, V::Unverifiable},
        {AmsAction::UNLOADING, false, true, false, V::Unverifiable},
    };
    for (const Row& r : rows) {
        CAPTURE(static_cast<int>(r.op), r.sensor_ever_read, r.filament_at_end, r.bypass_unload);
        CHECK(AmsBackendCfs::verify_phase_outcome(r.op, r.sensor_ever_read, r.filament_at_end,
                                                  r.bypass_unload) == r.verdict);
    }
    for (AmsAction op : {AmsAction::IDLE, AmsAction::CUTTING, AmsAction::PURGING,
                         AmsAction::SELECTING, AmsAction::RESETTING, AmsAction::HEATING}) {
        CAPTURE(static_cast<int>(op));
        CHECK(AmsBackendCfs::verify_phase_outcome(op, true, false) == V::Ok);
        CHECK(AmsBackendCfs::verify_phase_outcome(op, true, true) == V::Ok);
    }
}

TEST_CASE("CFS bypass load gcode: LOAD_MATERIAL when the printer defines it",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    // Creality's own external-spool load, the mirror of QUIT_MATERIAL. Unlike
    // the unload it needs no tail of ours: its FILAMENT_RACK_FLUSH already
    // drives the feed, gated on the toolhead switch the user fed to.
    CHECK(AmsBackendCfs::bypass_load_gcode(V::K2, /*has_load_material=*/true) == "LOAD_MATERIAL");
    CHECK(AmsBackendCfs::bypass_load_gcode(V::K1, true) == "LOAD_MATERIAL");
}

TEST_CASE("CFS bypass load gcode: fallback feeds the same path the unload backs out",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    const std::string load = AmsBackendCfs::bypass_load_gcode(V::K2, /*has_load_material=*/false);
    const std::string unload = AmsBackendCfs::bypass_unload_gcode(V::K2, false);

    // Same distance and rate, opposite sign — the load pushes back down exactly
    // the path the unload backs out of.
    REQUIRE(load.find("G0 E80 F600") != std::string::npos);
    REQUIRE(load.find("G0 E-") == std::string::npos);
    REQUIRE(unload.find("G0 E-80 F600") != std::string::npos);

    // Positioning and state bracketing, same as the unload.
    REQUIRE(load.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
    REQUIRE(load.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
    REQUIRE(load.find("SAVE_GCODE_STATE") != std::string::npos);
    REQUIRE(load.find("RESTORE_GCODE_STATE") != std::string::npos);

    // A load must not cut. The unload's cut primitive has no business here.
    REQUIRE(load.find("CR_BOX_CUT") == std::string::npos);
    REQUIRE(load.find("BOX_CUT_MATERIAL") == std::string::npos);
}

TEST_CASE("CFS load routes the bypass sentinel instead of refusing it", "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear(); // no LOAD_MATERIAL — exercise the fallback

    SECTION("stock K2: -2 dispatches a load instead of invalid_slot") {
        helix::test::StartedBackend<CfsRemapHelper> backend;

        // slot_to_tnn(-2) has no answer, but the load must not refuse outright:
        // an external spool has to be loadable from the app, which is also the
        // only way to reach the state the bypass UNLOAD needs.
        REQUIRE(backend.load_filament(helix::ui::kExternalSpoolSlot).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("G0 E80 F600") != std::string::npos);
    }

    SECTION("stock K2: a real bay still gets the bay script") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_multi_unit_box(1)));

        REQUIRE(backend.load_filament(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("CR_BOX_EXTRUDE") != std::string::npos);
    }

    SECTION("Fork keeps its own T<external> attended load") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_flat_fork_box()));

        // Fork resolves the external bay through its own T command, so the
        // sentinel must NOT be diverted into our stock-dialect script.
        REQUIRE(backend.load_filament(3).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] == "T3");
    }
}

// The unclaimed-toolhead prompt's Unload: no bay is loaded, so the bay
// script's retract has nothing to key on; the lane-free clear is the bypass
// unload.
TEST_CASE("CFS clears an unclaimed toolhead with the bypass unload, not a bay script",
          "[ams][cfs][bypass][unclaimed_toolhead]") {
    helix::MacroParamCache::instance().clear();

    SECTION("stock K2") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        REQUIRE(backend.can_clear_unaccounted_toolhead());
        const int slot = backend.unaccounted_toolhead_clear_slot();
        REQUIRE(backend.unload_filament(slot).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] ==
              AmsBackendCfs::bypass_unload_gcode(CfsMacroVariant::K2, false));
    }

    SECTION("Fork routes its own unload, so the prompt offers none") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_flat_fork_box()));
        CHECK_FALSE(backend.can_clear_unaccounted_toolhead());
    }
}

TEST_CASE("CFS bypass unload gcode: QUIT_MATERIAL when the printer defines it",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    // Creality's own external-spool unload owns the heat, the cut and the park.
    const std::string k2 = AmsBackendCfs::bypass_unload_gcode(V::K2, /*has_quit_material=*/true);
    REQUIRE(k2.rfind("QUIT_MATERIAL", 0) == 0);
    CHECK(AmsBackendCfs::bypass_unload_gcode(V::K1, true) == k2);

    // But QUIT_MATERIAL does not finish the pull. Its built-in retract is [box]
    // tn_retrude = -10 against tn_extrude = 140: the extruder only breaks the
    // grip and the box's feeder reels the rest back down the tube. A bypass
    // spool has no feeder. Measured on a K2 Plus 2026-08-18 — QUIT_MATERIAL
    // alone moved E by -13.99 mm, and another 50 mm by hand freed it.
    CHECK(k2.find("G0 E-80 F600") != std::string::npos);
    CHECK(k2.find("G91") != std::string::npos);
    CHECK(k2.find("G90") != std::string::npos);
}

TEST_CASE("CFS bypass unload gcode: fallback cuts and retracts with the extruder",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    const std::string k2 = AmsBackendCfs::bypass_unload_gcode(V::K2, /*has_quit_material=*/false);
    const std::string k1 = AmsBackendCfs::bypass_unload_gcode(V::K1, false);

    for (const std::string& g : {k2, k1}) {
        // The retract is the whole point: the box primitive is TNN-keyed and
        // no-ops under bypass, so the extruder has to do it.
        REQUIRE(g.find("G91") != std::string::npos);
        REQUIRE(g.find("G0 E-80 F600") != std::string::npos);
        REQUIRE(g.find("G90") != std::string::npos);
        REQUIRE(g.find("CR_BOX_RETRUDE") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);

        // No bay handshake: a stood-down box cannot answer any of these.
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
        REQUIRE(g.find("CR_BOX_PRE_OPT") == std::string::npos);
        REQUIRE(g.find("CR_BOX_END_OPT") == std::string::npos);
        REQUIRE(g.find("BOX_CHECK_MATERIAL") == std::string::npos);

        // Positioning and state bracketing still happen.
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("SAVE_GCODE_STATE") != std::string::npos);
        REQUIRE(g.find("RESTORE_GCODE_STATE") != std::string::npos);
    }

    // Both dialects cut with the firmware's positioned cut, the one their own
    // unload already emits.
    REQUIRE(k2.find("BOX_CUT_MATERIAL") != std::string::npos);
    REQUIRE(k1.find("BOX_CUT_MATERIAL") != std::string::npos);
    REQUIRE(k1.find("CR_BOX_CUT") == std::string::npos);
}

TEST_CASE("CFS K2 scripts never strike with the bare cutter primitive", "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    // CR_BOX_CUT (and M8200 C, its slicer-facing alias) rams the cutter from
    // wherever the toolhead stands; BOX_GO_TO_EXTRUDE_POS leaves it at the purge
    // chute, not at [box] pre_cut_pos. Every K2 cut goes through BOX_CUT_MATERIAL.
    std::vector<std::string> scripts = {AmsBackendCfs::unload_gcode(V::K2),
                                        AmsBackendCfs::bypass_unload_gcode(V::K2, false)};
    for (int idx : {0, 1, 5, 15}) {
        scripts.push_back(AmsBackendCfs::swap_gcode(idx, V::K2));
    }
    for (const std::string& g : scripts) {
        INFO(g);
        REQUIRE(script_has_command(g, "BOX_CUT_MATERIAL"));
        REQUIRE_FALSE(script_has_command(g, "CR_BOX_CUT"));
        REQUIRE(g.find("M8200 C") == std::string::npos);
    }
}

TEST_CASE("CFS unload routes the bypass sentinel away from the bay script", "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear(); // no QUIT_MATERIAL — exercise the fallback

    SECTION("stock K2: -2 gets the bypass script, a real bay still gets the bay script") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true, /*current_slot=*/-2);

        REQUIRE(backend.unload_filament(helix::ui::kExternalSpoolSlot).result ==
                AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        // CR_BOX_RETRUDE is keyed on a TNN and silently no-ops with the box
        // stood down: the cut would run and nothing would come out.
        CHECK(backend.dispatched[0].find("CR_BOX_RETRUDE") == std::string::npos);
        CHECK(backend.dispatched[0].find("G0 E-80 F600") != std::string::npos);
        CHECK(CfsTestAccess::phase_bypass_unload(backend));
    }

    SECTION("stock K2: a real bay keeps the box retract") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_loaded_state(backend, true, /*current_slot=*/0);

        REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("CR_BOX_RETRUDE") != std::string::npos);
        CHECK_FALSE(CfsTestAccess::phase_bypass_unload(backend));
    }

    SECTION("Fork keeps BOX_UNLOAD — its own external branch handles the holder") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_macro_variant_fork(backend);
        CfsTestAccess::set_loaded_state(backend, true, -2);

        REQUIRE(backend.unload_filament(helix::ui::kExternalSpoolSlot).result ==
                AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] == "BOX_UNLOAD");
        CHECK_FALSE(CfsTestAccess::phase_bypass_unload(backend));
    }
}

TEST_CASE("CFS unload of a used-up spool still runs and its failure says to pull by hand",
          "[ams][cfs][runout]") {
    using V = helix::printer::CfsMacroVariant;
    using Verdict = AmsBackendCfs::PhaseVerdict;
    helix::MacroParamCache::instance().clear();
    const std::string generic = AmsBackendCfs::phase_verdict_message(Verdict::UnloadLeftFilament);
    const std::string by_hand =
        AmsBackendCfs::phase_verdict_message(Verdict::UnloadLeftFilament, /*spool_used_up=*/true);
    REQUIRE(by_hand != generic);

    auto* config = Config::get_instance();
    const std::string type_path = config->df() + "type";
    const std::string saved_type = config->get<std::string>(type_path, "");

    for (V variant : {V::K2, V::K1}) {
        const std::string name = variant == V::K1 ? "K1" : "K2";
        // The dialect comes from the printer type the backend is built under.
        config->set<std::string>(type_path, variant == V::K1 ? "Creality K1C" : "Creality K2 Plus");
        CfsOverrideRig<HeldAckCfs> rig("cfs_useup_unload_" + name);
        HeldAckCfs& backend = *rig.backend;
        // A homed printer sends the script without a G28 ahead of it.
        lv_subject_set_string(rig.state.motion_state().get_homed_axes_subject(), "xyz");
        const auto box = [&rig](int useup) {
            rig.poll(make_runout_seated_box(useup, /*active=*/"A"));
        };
        const auto switch_reads = [&rig](bool detected) {
            rig.backend->fire_status(json{
                {"filament_switch_sensor filament_sensor", {{"filament_detected", detected}}}});
        };
        // What the Filament panel shows: AmsState syncs it only on a backend event,
        // so a settle that emits none leaves the panel on the running action.
        const auto shown = [] {
            helix::ui::UpdateQueue::instance().drain_until_empty();
            return static_cast<AmsAction>(
                lv_subject_get_int(helix::AmsState::instance().get_ams_action_subject()));
        };
        box(/*useup=*/0);
        switch_reads(true);
        REQUIRE(backend.get_system_info().current_slot == 0);
        REQUIRE(backend.get_system_info().filament_loaded);

        SECTION("a used-up spool still sends the unload script, " + name) {
            // filament_useup can be stale (it survives until a successful
            // extrude), so it never stops an unload.
            box(/*useup=*/1);
            REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
            CHECK(backend.sent == std::vector<std::string>{AmsBackendCfs::unload_gcode(variant)});

            // The retract could not reach the strand: the switch still reads
            // filament, and the fault says to remove it by hand.
            backend.ack();
            const AmsSystemInfo info = backend.get_system_info();
            CHECK(info.action == AmsAction::ERROR);
            CHECK(info.operation_detail == by_hand);

            // Pulling it out by hand resolves the fault like any other.
            switch_reads(false);
            CHECK(backend.get_system_info().action == AmsAction::IDLE);
        }

        SECTION("a failed unload without a used-up spool keeps the generic message, " + name) {
            REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
            CHECK(backend.sent == std::vector<std::string>{AmsBackendCfs::unload_gcode(variant)});
            backend.ack();
            const AmsSystemInfo info = backend.get_system_info();
            CHECK(info.action == AmsAction::ERROR);
            CHECK(info.operation_detail == generic);
            CHECK(shown() == AmsAction::ERROR);
        }

        SECTION("a used-up spool whose unload cleared the switch raises nothing, " + name) {
            box(/*useup=*/1);
            REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
            switch_reads(false);
            REQUIRE(shown() != AmsAction::IDLE);
            backend.ack();
            CHECK(backend.get_system_info().action == AmsAction::IDLE);
            CHECK_FALSE(backend.current_error().has_value());
            CHECK(shown() == AmsAction::IDLE);
        }

        SECTION("a script Klipper rejects settles the panel to IDLE, " + name) {
            REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
            REQUIRE(shown() != AmsAction::IDLE);
            backend.reject();
            CHECK(shown() == AmsAction::IDLE);
        }
    }
    config->set<std::string>(type_path, saved_type);
}

TEST_CASE("CFS bypass unload completes instead of erroring with filament still detected",
          "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear();

    CfsRemapHelper backend;
    backend.mark_running();
    CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true, /*current_slot=*/-2);
    REQUIRE(backend.unload_filament(helix::ui::kExternalSpoolSlot).result == AmsResult::SUCCESS);

    // The end state a bypass unload actually leaves: tip clear of the melt zone,
    // filament still across the toolhead switch, waiting on the user's hand.
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::complete_action(backend);

    // ERROR here is what killed the manual-pull prompt on the K2: op_failed()
    // runs disarm_manual_pull_prompt().
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE("CFS phase verify: failure verdicts carry actionable wording", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    const std::string load_msg = AmsBackendCfs::phase_verdict_message(V::LoadDidNotReachNozzle);
    const std::string unload_msg = AmsBackendCfs::phase_verdict_message(V::UnloadLeftFilament);

    REQUIRE_FALSE(load_msg.empty());
    REQUIRE_FALSE(unload_msg.empty());
    // The two must not read the same — they need opposite user responses.
    CHECK(load_msg != unload_msg);
    // Non-failures say nothing, so a caller can treat empty as "no fault".
    CHECK(AmsBackendCfs::phase_verdict_message(V::Ok).empty());
    CHECK(AmsBackendCfs::phase_verdict_message(V::Unverifiable).empty());
}

TEST_CASE("CFS phase verify: a failed load raises a fault through current_error",
          "[ams][cfs][968]") {
    CfsRemapHelper backend;

    // No error while nothing has gone wrong.
    CHECK_FALSE(backend.current_error().has_value());

    // Drive the real completion path: intent latched at dispatch, sensor says
    // the nozzle is still empty when the script drains.
    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/false);
    CfsTestAccess::complete_action(backend);

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    // Must name the real problem rather than a generic failure.
    CHECK(ev->detail.find("did not reach") != std::string::npos);
    // NOT the runout action set. "Resume" is meaningless here — a manual load
    // that failed has no paused job to restart, and offering it would send
    // RESUME to an idle printer.
    for (const auto& a : ev->recovery_actions) {
        CHECK(a.gcode != "RESUME");
    }
    // Clearing the latched box error is the one safe lever, and it must stay
    // tappable on a cold nozzle since it moves no filament.
    REQUIRE(ev->recovery_actions.size() == 1);
    CHECK(ev->recovery_actions[0].gcode == AmsBackendCfs::reset_gcode());
    CHECK_FALSE(ev->recovery_actions[0].needs_hot_nozzle);
}

TEST_CASE("CFS phase verify: a raised fault survives later status frames", "[ams][cfs][968]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.current_error().has_value());

    // The box keeps streaming after the failure. Phase synthesis runs off those
    // frames and must not overwrite `action` with LOADING/CUTTING —
    // that would erase the fault and dismiss the modal the user is reading.
    nlohmann::json n = {
        {"params", nlohmann::json::array(
                       {{{"filament_switch_sensor filament_sensor", {{"filament_detected", false}}},
                         {"extruder", {{"temperature", 210.0}, {"target", 220.0}}}}})}};
    backend.fire_status(n);

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    CHECK(backend.current_error().has_value());
}

TEST_CASE("CFS clear_fault sends nothing, so a Reset mid-print cannot cancel it",
          "[ams][cfs][clear_fault]") {
    // The same backend state sends CANCEL_PRINT through cancel(), so a
    // clear_fault() that fell back to it would be caught here.
    helix::test::StartedBackend<CfsRemapHelper> canceller;
    REQUIRE(canceller.cancel().success());
    REQUIRE(canceller.captured == std::vector<std::string>{"CANCEL_PRINT"});

    helix::test::StartedBackend<CfsRemapHelper> backend;
    CHECK(backend.clear_fault(0).success());
    CHECK(backend.clear_fault(-1).success());
    CHECK(backend.captured.empty());
}

TEST_CASE("CFS cancel counts as a cancel the screen sent, for the print outcome",
          "[ams][cfs][telemetry]") {
    auto& tm = TelemetryManager::instance();
    helix::test::StartedBackend<CfsRemapHelper> backend;
    const auto before = tm.print_cancels_requested();
    REQUIRE(backend.cancel().success());
    CHECK(tm.print_cancels_requested() == before + 1);
}

TEST_CASE("CFS clear_fault retires a phase fault without a command", "[ams][cfs][clear_fault]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);
    REQUIRE(backend.current_error().has_value());
    backend.captured.clear();

    REQUIRE_FALSE(backend.get_system_info().operation_detail.empty());
    CHECK(backend.clear_fault(0).success());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.captured.empty());

    // The cleared verdict does not come back on the next status frame.
    backend.fire_status(
        {{"params", nlohmann::json::array({{{"filament_switch_sensor filament_sensor",
                                             {{"filament_detected", false}}}}})}});
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(backend.current_error().has_value());
}

TEST_CASE("CFS phase verify: starting a new operation clears the fault", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.current_error().has_value());

    // A retry must not inherit the previous failure — otherwise the modal can
    // never be dismissed by doing the obvious thing.
    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
}

TEST_CASE("CFS phase verify: clearing the toolhead by hand retires an unload fault",
          "[ams][cfs][968][i18n]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    // Raised in one language, retired in another: the fault text is a catalog
    // key, so the shown text follows the switch and the retire still matches it.
    std::optional<ScopedLanguage> lang(std::in_place, "de");
    CfsTestAccess::force_phase_intent(backend, AmsAction::UNLOADING);
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);
    REQUIRE(backend.current_error().has_value());
    lang.emplace("fr");
    CHECK(std::string(lv_tr(backend.get_system_info().operation_detail.c_str()))
              .rfind("Le filament est encore", 0) == 0);

    // Still blocked: the condition the fault names has not changed.
    backend.fire_status(toolhead_sensor_frame(true));
    REQUIRE(backend.current_error().has_value());

    // The user pulls the stub out and the switch reads empty: the fault named
    // exactly this condition, so the fault clears.
    backend.fire_status(toolhead_sensor_frame(false));
    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("CFS phase verify: filament arriving retires a load fault", "[ams][cfs][968]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.current_error().has_value());

    backend.fire_status(toolhead_sensor_frame(true));
    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("CFS phase verify: only the matching sensor edge retires a fault", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;
    CHECK(AmsBackendCfs::phase_fault_resolved(V::UnloadLeftFilament, /*filament_detected=*/false));
    CHECK_FALSE(AmsBackendCfs::phase_fault_resolved(V::UnloadLeftFilament, true));
    CHECK(AmsBackendCfs::phase_fault_resolved(V::LoadDidNotReachNozzle, true));
    CHECK_FALSE(AmsBackendCfs::phase_fault_resolved(V::LoadDidNotReachNozzle, false));
    CHECK_FALSE(AmsBackendCfs::phase_fault_resolved(V::Ok, false));
    CHECK_FALSE(AmsBackendCfs::phase_fault_resolved(V::Unverifiable, true));
}

TEST_CASE("CFS box runout notice: a fresh mid-job runout with a fed toolhead",
          "[ams][cfs][runout]") {
    using B = AmsBackendCfs;
    CHECK(B::box_runout_needs_notice(0, 1, /*job_running=*/true, /*toolhead=*/true));

    // The latch reads 1 on an idle machine and on the first frame after
    // connect; neither is a runout that just happened.
    CHECK_FALSE(B::box_runout_needs_notice(-1, 1, true, true));
    CHECK_FALSE(B::box_runout_needs_notice(1, 1, true, true));
    // Reload, not runout.
    CHECK_FALSE(B::box_runout_needs_notice(1, 0, true, true));
    // No job: nothing is consuming the tube.
    CHECK_FALSE(B::box_runout_needs_notice(0, 1, false, true));
    // Toolhead already empty: the firmware's own runout fault covers it.
    CHECK_FALSE(B::box_runout_needs_notice(0, 1, true, false));
}

TEST_CASE("CFS box runout mid-job warns that the print continues on the tube",
          "[ams][cfs][runout][toast]") {
    helix::test::StartedBackend<CfsLifecycleHelper> backend;
    helix::test::ToastLog toasts;
    const auto frame = [](int useup) {
        json box = make_multi_unit_box(1);
        box["filament_useup"] = useup;
        return json{{"params", json::array({json{{"filament_switch_sensor filament_sensor",
                                                  {{"filament_detected", true}}},
                                                 {"box", box}},
                                            0})}};
    };
    helix::test::set_wire_state(backend.state, helix::PrintJobState::PRINTING);
    backend.fire_status(frame(0));
    backend.fire_status(frame(1));
    toasts.expect_one(
        ToastSeverity::WARNING,
        "The CFS ran out of filament. Printing continues on what is left in the tube.", 10000);
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

TEST_CASE("CFS runout: weak-hint fallback needs the box latch AND a pause", "[ams][cfs][1250]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    // Wording neither matcher recognizes, but the line is about refilling.
    const std::string line = "// material refill aborted";

    SECTION("latch clear: not enough evidence, defer to the generic classifier") {
        backend.fire_status(make_cfs_notification(make_runout_box(0)));
        CHECK_FALSE(backend.classify_error(line, paused_ctx()).has_value());
    }

    SECTION("latch set + paused: fires, surfacing the firmware's own words") {
        backend.fire_status(make_cfs_notification(make_runout_box(1)));
        auto ev = backend.classify_error(line, paused_ctx());
        REQUIRE(ev.has_value());
        // Weaker evidence, so it must NOT claim which give-up path ran.
        CHECK(ev->detail == "material refill aborted");
        CHECK(ev->recovery_actions.size() == 2);
    }

    SECTION("latch set but not paused: still nothing") {
        backend.fire_status(make_cfs_notification(make_runout_box(1)));
        helix::ClassifyContext idle_ctx;
        idle_ctx.is_paused = false;
        CHECK_FALSE(backend.classify_error(line, idle_ctx).has_value());
    }
}

TEST_CASE("CFS runout: an unpaused give-up line is not a runout", "[ams][cfs][1250]") {
    CfsRemapHelper backend;
    helix::ClassifyContext idle_ctx; // is_paused = false

    // The firmware pauses BEFORE running BOX_CHECK_MATERIAL_REFILL, so an
    // unpaused occurrence is a human echoing the words or poking the macro.
    CHECK_FALSE(backend.classify_error("// no identical supplies", idle_ctx).has_value());
    CHECK_FALSE(
        backend.classify_error("// disable material automatic refill", idle_ctx).has_value());
}

TEST_CASE("CFS runout: `!!` lines stay with the generic key8xx classifier", "[ams][cfs][1250]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    // The box latch is set, so nothing but the `!!` gate can be refusing these.
    backend.fire_status(make_cfs_notification(make_runout_box(1)));

    // key840 keeps its existing generic decode + hardcoded "Reset CFS" action.
    // Claiming it here would double-surface (a backend modal AND the generic one)
    // or silently replace the coded decode.
    CHECK_FALSE(
        backend.classify_error(R"(!! {"code":"key840","values":[]})", paused_ctx()).has_value());

    // Even a `!!` that happens to carry the give-up wording is refused — the
    // inverted gate is unconditional, so the two channels cannot both claim it.
    CHECK_FALSE(backend.classify_error("!! no identical supplies", paused_ctx()).has_value());
    CHECK_FALSE(backend.classify_error("!! material refill aborted", paused_ctx()).has_value());
}

TEST_CASE("CFS runout: ordinary console chatter is ignored", "[ams][cfs][1250]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_runout_box(1)));

    // A paused print with the latch set is the state most likely to produce a
    // false positive, so that is the state these are checked in.
    for (const char* line : {"ok", "// echo: busy", "// Klipper state: Ready",
                             "// probe at 10.000,10.000 is z=0.100", ""}) {
        CAPTURE(line);
        CHECK_FALSE(backend.classify_error(line, paused_ctx()).has_value());
    }
}

// Endless spool capabilities
//
// CFS had no capability test at all, and its old answer was
// {supported=true, editable=false, description="Auto-refill enabled"|"disabled"} —
// so auto-refill on and off were indistinguishable to any UI, and the only place
// the real state lived was an untranslated English string.

TEST_CASE("CFS endless spool: auto-refill on and off are distinguishable",
          "[ams][cfs][endless_spool][1391]") {
    SECTION("auto_refill=1 with a two-lane same_material group reads as available and ON") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json box = make_runout_box(0);
        // make_runout_box colors: T1A=0FFFFFF, T1B/T1C/T1D=01A1A1A, all
        // material 101001. T1B+T1C form one real pairing; the live-K2 shape
        // from CREALITY_CFS_K2_INTERNALS.md § "Top-Level Fields".
        box["same_material"] =
            json::array({json::array({"101001", "01A1A1A", json::array({"T1B", "T1C"}), "PLA"}),
                         json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1D"}), "PLA"})});
        backend.fire_status(make_cfs_notification(box));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::On);
        CHECK(caps.editability == EndlessSpoolEditability::ReadOnly);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
        CHECK_FALSE(caps.editable());
    }

    SECTION("auto_refill=1 with same_material present but no pairing reads as ON WITHOUT "
            "BACKUP") {
        // The honest negative (#1391): the setting is on, but the firmware's
        // own grouping says every group is a singleton, so a runout would stop
        // the print anyway. Advertising plain On here is the false promise the
        // issue exists to remove.
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json box = make_runout_box(0);
        box["same_material"] =
            json::array({json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1B"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1C"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1D"}), "PLA"})});
        backend.fire_status(make_cfs_notification(box));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::OnWithoutBackup);
        // Still firmware-managed: the honest state changes WHAT WE SAY, not
        // what we claim to control.
        CHECK(caps.editability == EndlessSpoolEditability::ReadOnly);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
    }

    SECTION("auto_refill=1 with no same_material key stays ON (no data is not a negative)") {
        // The flat dialect never sends same_material, and a stock delta frame
        // may omit it; "we did not read the grouping" must not be reported as
        // "nothing groups". Only a parsed list with no pairing justifies the
        // degraded answer.
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_runout_box(0)));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::On);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
    }

    SECTION("same_material lane lists parse into per-slot group ordinals") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json box = make_runout_box(0);
        box["same_material"] =
            json::array({json::array({"101001", "01A1A1A", json::array({"T1B", "T1C"}), "PLA"}),
                         json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"})});
        backend.fire_status(make_cfs_notification(box));

        const auto info = backend.get_system_info();
        CHECK(info.endless_spool_groups_reported);
        // TNN address space: one entry per addressable bay T1A..T4D.
        REQUIRE(info.endless_spool_group_ids.size() == 16);
        CHECK(info.endless_spool_group_ids[0] == 1); // T1A: singleton group
        CHECK(info.endless_spool_group_ids[1] == 0); // T1B: paired with T1C
        CHECK(info.endless_spool_group_ids[2] == 0);
        CHECK(info.endless_spool_group_ids[3] == -1); // T1D: not grouped
        for (int i = 4; i < 16; ++i) {
            CHECK(info.endless_spool_group_ids[i] == -1); // unaddressed bays
        }
    }

    SECTION("no same_material key leaves the grouping data empty and unreported") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_runout_box(0)));

        const auto info = backend.get_system_info();
        CHECK_FALSE(info.endless_spool_groups_reported);
        CHECK(info.endless_spool_group_ids.empty());
    }

    SECTION("a stock delta frame without same_material retains the known grouping") {
        // A stock delta carries a changed T1 subtree but omits same_material
        // (the field rides full frames). The honest warning must survive that
        // frame rather than reverting to plain On until the firmware next
        // re-sends the list - the same presence-gating rule filament_runout
        // already follows.
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json grouped = make_runout_box(0);
        grouped["same_material"] =
            json::array({json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1B"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1C"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1D"}), "PLA"})});
        backend.fire_status(make_cfs_notification(grouped));
        REQUIRE(backend.get_endless_spool_capabilities().enabled ==
                EndlessSpoolEnabled::OnWithoutBackup);

        // Delta: units present, no same_material key at all.
        json delta = make_runout_box(0);
        delta["T1"]["remain_len"] = std::vector<std::string>{"40", "0", "46", "50"};
        backend.fire_status(make_cfs_notification(delta));

        CHECK(backend.get_endless_spool_capabilities().enabled ==
              EndlessSpoolEnabled::OnWithoutBackup);
        const auto info = backend.get_system_info();
        CHECK(info.endless_spool_groups_reported);
        CHECK(info.endless_spool_group_ids.size() == 16);
    }

    SECTION("a flat frame drops stock-era grouping (schema transition, not a delta)") {
        // The flat dialect never sends same_material, so the presence gate
        // alone would keep answering from the stock frame's grouping forever
        // after a mid-session firmware swap. A schema change is a real
        // transition: the grouping is cleared, and the capability falls back
        // to the plain On answer (unknown, not a negative).
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json grouped = make_runout_box(0);
        grouped["same_material"] =
            json::array({json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1B"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1C"}), "PLA"}),
                         json::array({"101001", "01A1A1A", json::array({"T1D"}), "PLA"})});
        backend.fire_status(make_cfs_notification(grouped));
        REQUIRE(backend.get_endless_spool_capabilities().enabled ==
                EndlessSpoolEnabled::OnWithoutBackup);

        json flat = json::parse(R"({
            "driver_ready": true, "api_version": 1, "state": "IDLE",
            "runout": null, "runout_swap_enabled": true,
            "slots": [
                {"index": 0, "present": true, "loaded": false,
                 "material": "PLA", "color": "#111111"},
                {"index": 1, "present": true, "loaded": false,
                 "material": "PC", "color": "#F2F2F2"},
                {"index": 2, "present": true, "loaded": false,
                 "material": "PLA", "color": "#111111"},
                {"index": 3, "present": false, "loaded": false,
                 "material": "", "color": ""}
            ]
        })");
        backend.fire_status(make_cfs_notification(flat));

        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
        const auto info = backend.get_system_info();
        CHECK_FALSE(info.endless_spool_groups_reported);
        CHECK(info.endless_spool_group_ids.empty());
    }

    SECTION("auto_refill=0 reads as available and OFF") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json box = make_runout_box(0);
        box["auto_refill"] = 0;
        backend.fire_status(make_cfs_notification(box));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::Off);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
    }

    SECTION("no per-slot relation is reported, so the UI has no dropdown to draw") {
        // The old shape (available + an empty per-slot config) made the context
        // menu render a backup dropdown that could only ever read "None", with no
        // way to tell that apart from "no backup configured".
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_runout_box(0)));

        CHECK(backend.get_endless_spool_config().empty());
        CHECK(endless_spool_backup_for(backend.get_endless_spool_config(), 0) == -1);
    }

    SECTION("writes are refused with the firmware-managed reason") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_runout_box(0)));

        auto result = backend.set_endless_spool_backup(0, 1);
        CHECK_FALSE(result.success());
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
        CHECK(result.user_msg ==
              endless_spool_restriction_text(EndlessSpoolRestriction::FirmwareManaged));

        CHECK_FALSE(backend.reset_endless_spool().success());
    }
}
