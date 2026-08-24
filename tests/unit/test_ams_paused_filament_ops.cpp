// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_paused_filament_ops.cpp
 * @brief Layer 2 print-active gate: what a PAUSED print may and may not do.
 *
 * A user on an AFC BoxTurtle paused a print, tapped Unload, and got "Unload
 * failed: Cannot run filament operation while printing" — then unloaded from
 * Mainsail with no trouble. Pause-to-swap-filament IS the runout / colour-change
 * recovery workflow, so refusing it everywhere made HelixScreen the only surface
 * that could not perform the recovery Klipper had just asked for.
 *
 * The gate is not removed, it is narrowed to where it was earned. AD5X IFS
 * unloads via `_IFS_REMOVE_CURRENT_PRUTOK`, which runs a buried `_G28` inside
 * ZMOD firmware; on that loadcell-Z printer the probe drives the nozzle DOWN
 * into the part, tripping ZCONTROL_AUTO into a Klipper shutdown that needs a
 * firmware restart (bundle XWPBR2DX, commit 329e731e9). Layer 1's gcode-send
 * guard never sees that `_G28`, so AD5X must still refuse while paused.
 *
 * The four cells these tests pin:
 *
 *   state    | self-homes (AD5X) | does not self-home (AFC, HappyHare, QIDI)
 *   ---------+-------------------+------------------------------------------
 *   PRINTING | refuse            | refuse
 *   PAUSED   | refuse            | ALLOW
 *
 * Plus: the copy a paused AD5X user sees still names a recovery that works on
 * that machine, and the default of AmsBackend::filament_ops_self_home() is false.
 *
 * Layer 1 is unchanged and remains the backstop for everything let through —
 * see test_moonraker_api_homing_guard.cpp, which pins that
 * helix::api::reject_homing_during_active_print() refuses app-emitted G28 while
 * PRINTING *and* PAUSED. ensure_homed_then() only emits a G28 when
 * toolhead.homed_axes lacks "xyz", which a paused print never does.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
// AD5X IFS is feature-gated (HELIX_HAS_IFS=0 on the space-constrained cross
// builds, mk/cross.mk), so every self-homing assertion below is guarded. The
// non-self-homing half of the table is unconditional.
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_error.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Doubles exist only to (a) flip running_ so check_preconditions() reaches the
/// print-active gate without a live Moonraker, (b) reach the protected
/// refuse_if_printing() that QIDI-style backends call directly, and (c) capture
/// the G-code a filament op would dispatch, so a refusal can be checked for
/// leakage and not just for its return value. Nothing else is overridden: the
/// production filament_ops_self_home() answer is what is tested.
template <typename Backend> class PausedGateDouble : public Backend {
  public:
    PausedGateDouble(MoonrakerAPI* api, helix::MoonrakerClient* client) : Backend(api, client) {
        this->running_.store(true);
    }
    AmsError call_refuse_if_printing() const {
        return this->refuse_if_printing();
    }

    std::vector<std::string> dispatched;

    AmsError execute_gcode(const std::string& gcode) override {
        dispatched.push_back(gcode);
        return AmsErrorHelper::success();
    }
};

struct PausedGateFixture : public LVGLTestFixture {
    PausedGateFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }

    /// A host-side pre-print block: the wire still reads standby.
    void set_preprint_phase(helix::PrintStartPhase phase) {
        state.set_print_start_state(phase, "", 0);
        helix::ui::UpdateQueue::instance().drain();
    }

    template <typename Backend> std::unique_ptr<PausedGateDouble<Backend>> make() {
        return std::make_unique<PausedGateDouble<Backend>>(api.get(), &mock_client);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

// ============================================================================
// The capability itself
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture,
                 "filament_ops_self_home: AD5X is the only backend that claims it",
                 "[ams][safety][paused]") {
    // Verified by grep: the sole G28 sites in the filament paths are AD5X's
    // `_IFS_REMOVE_CURRENT_PRUTOK` / `INSERT_PRUTOK_IFS`. Every other backend
    // dispatches through ensure_homed_then(), which homes only when unhomed.
#if HELIX_HAS_IFS
    CHECK(make<AmsBackendAd5xIfs>()->filament_ops_self_home());
#endif
    CHECK_FALSE(make<AmsBackendAfc>()->filament_ops_self_home());
    CHECK_FALSE(make<AmsBackendHappyHare>()->filament_ops_self_home());
    CHECK_FALSE(make<AmsBackendQidi>()->filament_ops_self_home());
    // Snapmaker's AUTO_FEEDING does home, but prepare_for_resume() drives
    // `AUTO_FEEDING ... PRINTING=1` on a PAUSED job and was live-verified on
    // physical U1 hardware (#991) — the firmware exposes PRINTING=1 for exactly
    // that recovery. No evidence the home is unsafe, so this stays false and the
    // runout-recovery workflow keeps working.
    CHECK_FALSE(make<AmsBackendSnapmaker>()->filament_ops_self_home());
}

// ============================================================================
// Cell 1 + 2: PRINTING refuses for BOTH kinds of backend
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "PRINTING refuses toolhead-motion ops on every backend",
                 "[ams][safety][paused]") {
    set_print_state(helix::PrintJobState::PRINTING);

#if HELIX_HAS_IFS
    SECTION("self-homing backend (AD5X)") {
        auto backend = make<AmsBackendAd5xIfs>();
        AmsError err = backend->check_preconditions(/*requires_toolhead_motion=*/true);
        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Cannot run filament operation while printing");
        // AD5X cannot do the op paused either, so pausing is NOT offered as a
        // recovery — only finishing or cancelling is.
        CHECK(err.suggestion.find("Finish or cancel") != std::string::npos);
        CHECK(err.suggestion.find("Pause the print") == std::string::npos);
    }
#endif // HELIX_HAS_IFS

    SECTION("non-self-homing backend (AFC) — the reported machine") {
        auto backend = make<AmsBackendAfc>();
        AmsError err = backend->check_preconditions(true);
        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Cannot run filament operation while printing");
        // Pausing IS a recovery this backend can honour, so name it instead of
        // pushing the user to throw away a print they could have saved.
        CHECK(err.suggestion.find("Pause the print") != std::string::npos);
    }

    SECTION("non-self-homing backend (Happy Hare)") {
        auto backend = make<AmsBackendHappyHare>();
        CHECK_FALSE(backend->check_preconditions(true).success());
        CHECK_FALSE(backend->call_refuse_if_printing().success());
    }

    SECTION("QIDI gates via refuse_if_printing() directly, not check_preconditions()") {
        // QIDI's load/unload/change_tool call refuse_if_printing() themselves
        // because they skip the running_/busy gate. That path must refuse too.
        auto backend = make<AmsBackendQidi>();
        AmsError err = backend->call_refuse_if_printing();
        REQUIRE_FALSE(err.success());
        CHECK(err.user_msg == "Cannot run filament operation while printing");
    }
}

// ============================================================================
// Cell 3: PAUSED still refuses on a self-homing backend (XWPBR2DX protection)
// ============================================================================

#if HELIX_HAS_IFS
TEST_CASE_METHOD(PausedGateFixture, "PAUSED still refuses on AD5X — the buried _G28 would collide",
                 "[ams][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);
    auto backend = make<AmsBackendAd5xIfs>();

    AmsError err = backend->check_preconditions(/*requires_toolhead_motion=*/true);
    REQUIRE_FALSE(err.success());
    CHECK(err.result == AmsResult::WRONG_STATE);
    CHECK(err.user_msg == "Can't move filament while the print is paused");

    // The refusal has to leave the user somewhere they can actually go. Both
    // named routes work on an AD5X: feeding filament past the toolhead sensor by
    // hand re-triggers head_switch_sensor so RESUME continues the job, and
    // cancelling returns the printer to STANDBY where Load/Unload are permitted.
    CHECK(err.suggestion.find("Resume") != std::string::npos);
    CHECK(err.suggestion.find("cancel") != std::string::npos);
    // "while printing" on a paused job reads as a bug (bundle JX2FVRB9).
    CHECK(err.user_msg.find("printing") == std::string::npos);

    // No-motion ops (eject_lane, select, unlock) were never blocked and still
    // are not — blocking them would strand filament the user could have ejected.
    CHECK(backend->check_preconditions(/*requires_toolhead_motion=*/false).success());
}
#endif // HELIX_HAS_IFS

// ============================================================================
// Cell 4: PAUSED now SUCCEEDS on a backend that does not self-home
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture,
                 "PAUSED allows toolhead-motion ops when the backend never homes",
                 "[ams][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);

    SECTION("AFC — the reported BoxTurtle unload") {
        // AFC dispatches TOOL_UNLOAD through ensure_homed_then(), which homes
        // only when toolhead.homed_axes lacks "xyz"; a paused job is homed. AFC's
        // own is_printing() is `print_stats.state == "printing"`, so the firmware
        // permits it too — which is why Mainsail's unload succeeded.
        auto backend = make<AmsBackendAfc>();
        CHECK(backend->check_preconditions(/*requires_toolhead_motion=*/true).success());
        CHECK(backend->call_refuse_if_printing().success());
    }

    SECTION("Happy Hare") {
        auto backend = make<AmsBackendHappyHare>();
        CHECK(backend->check_preconditions(true).success());
    }

    SECTION("QIDI, via its direct refuse_if_printing() call site") {
        auto backend = make<AmsBackendQidi>();
        CHECK(backend->call_refuse_if_printing().success());
    }
}

// ============================================================================
// Snapmaker U1 — the backend 329e731e9 missed
//
// Every op below drives the toolhead: AUTO_FEEDING forwards to FEED_AUTO, which
// homes and then switches tools before feeding, and `T{n}` moves the carriage.
// The UI only greys the buttons; ui_panel_ams.cpp's `sidebar_ == nullptr`
// fallback and ui_ams_sidebar.cpp's dispatch_backend_load both reach the backend
// directly, so the refusal has to live in the backend.
//
// Asserting `dispatched` (not just the AmsError) is the point: a guard placed
// after the command is built returns the refusal AND still sends the G-code.
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "Snapmaker refuses toolhead-motion filament ops while PRINTING",
                 "[ams][snapmaker][safety][paused]") {
    set_print_state(helix::PrintJobState::PRINTING);
    auto backend = make<AmsBackendSnapmaker>();

    auto expect_refused = [&](AmsError err) {
        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Cannot run filament operation while printing");
        // Snapmaker can do these paused, so the copy names pausing as the way out.
        CHECK(err.suggestion.find("Pause the print") != std::string::npos);
        CHECK(backend->dispatched.empty());
    };

    SECTION("load_filament") {
        expect_refused(backend->load_filament(2));
    }

    SECTION("unload_filament with an explicit slot") {
        expect_refused(backend->unload_filament(2));
    }

    SECTION("unload_filament with an unknown current slot — the INNER_FILAMENT_UNLOAD fallback") {
        // current_slot defaults to -1, so this is the branch that skips
        // AUTO_FEEDING and emits the firmware's bare leaf macro. It moves the
        // toolhead too, and the guard sits ahead of the fork so it is covered.
        expect_refused(backend->unload_filament(-1));
    }

    SECTION("change_tool") {
        expect_refused(backend->change_tool(1));
    }

    SECTION("select_slot — on the U1 a slot select IS a physical tool change") {
        expect_refused(backend->select_slot(1));
    }
}

TEST_CASE_METHOD(PausedGateFixture, "Snapmaker still allows filament ops while PAUSED",
                 "[ams][snapmaker][safety][paused]") {
    // Runout recovery on a U1 is pause -> feed -> resume, and prepare_for_resume()
    // drives AUTO_FEEDING against a paused job on real hardware (#991). Refusing
    // here would break the workflow the gate exists to protect.
    set_print_state(helix::PrintJobState::PAUSED);
    auto backend = make<AmsBackendSnapmaker>();

    SECTION("load_filament") {
        REQUIRE(backend->load_filament(2).success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "AUTO_FEEDING EXTRUDER=2 LOAD=1");
    }

    SECTION("unload_filament") {
        REQUIRE(backend->unload_filament(2).success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "AUTO_FEEDING EXTRUDER=2 UNLOAD=1");
    }

    SECTION("unload_filament falls back to INNER_FILAMENT_UNLOAD") {
        REQUIRE(backend->unload_filament(-1).success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "INNER_FILAMENT_UNLOAD");
    }

    SECTION("change_tool") {
        REQUIRE(backend->change_tool(1).success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "T1");
    }

    SECTION("select_slot") {
        REQUIRE(backend->select_slot(3).success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "T3");
    }
}

TEST_CASE_METHOD(PausedGateFixture, "Snapmaker filament ops are untouched with no print running",
                 "[ams][snapmaker][safety][paused]") {
    auto backend = make<AmsBackendSnapmaker>();

    for (helix::PrintJobState s : {helix::PrintJobState::STANDBY, helix::PrintJobState::COMPLETE,
                                   helix::PrintJobState::CANCELLED, helix::PrintJobState::ERROR}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));

        backend->dispatched.clear();
        REQUIRE(backend->load_filament(0).success());
        REQUIRE(backend->unload_filament(0).success());
        REQUIRE(backend->change_tool(0).success());
        REQUIRE(backend->select_slot(0).success());
        CHECK(backend->dispatched.size() == 4);
    }
}

// ============================================================================
// States that were never gated stay ungated
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "inactive print states allow motion ops on every backend",
                 "[ams][safety][paused]") {
    auto afc = make<AmsBackendAfc>();
#if HELIX_HAS_IFS
    auto ad5x = make<AmsBackendAd5xIfs>();
#endif

    for (helix::PrintJobState s : {helix::PrintJobState::STANDBY, helix::PrintJobState::COMPLETE,
                                   helix::PrintJobState::CANCELLED, helix::PrintJobState::ERROR}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(afc->check_preconditions(/*requires_toolhead_motion=*/true).success());
#if HELIX_HAS_IFS
        CHECK(ad5x->check_preconditions(true).success());
#endif
    }
}

// ============================================================================
// PREPARING — the window print_stats cannot describe
//
// A host-side pre-start block (the K2's forced bed mesh) runs BEFORE the printer
// is handed the job, so `print_stats.state` reads standby — or the PREVIOUS job's
// terminal state — for its whole duration while the toolhead homes and probes.
// refuse_if_printing() asked that wire value, so every toolhead-motion filament
// op was ACCEPTED through the entire window.
//
// This is the backend-side last line of defence, not an affordance: every
// dispatch route funnels into check_preconditions(true). The UI predicate
// (print_blocks_filament_op) is a different function on a different path and its
// tests do not cover this one.
//
// Mutation check: revert refuse_if_printing() to
// print_occupies_toolhead(get_print_job_state()) and every case below fails.
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "PREPARING refuses toolhead-motion ops on every backend",
                 "[ams][safety][preparing]") {
    // The wire says standby; only the live phase distinguishes this from idle.
    set_print_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);

    auto afc = make<AmsBackendAfc>();
    auto snap = make<AmsBackendSnapmaker>();
#if HELIX_HAS_IFS
    auto ad5x = make<AmsBackendAd5xIfs>();
#endif

    CHECK_FALSE(afc->check_preconditions(/*requires_toolhead_motion=*/true).success());
    CHECK_FALSE(snap->check_preconditions(true).success());
#if HELIX_HAS_IFS
    CHECK_FALSE(ad5x->check_preconditions(true).success());
#endif

    // Cold lane ops are still permitted — they move no toolhead, and blanket-
    // blocking them would strand filament exactly as #995/#1199 describe.
    CHECK(afc->check_preconditions(/*requires_toolhead_motion=*/false).success());
}

TEST_CASE_METHOD(PausedGateFixture,
                 "PREPARING after a finished job still refuses — the wire reads COMPLETE",
                 "[ams][safety][preparing]") {
    // print_stats holds the PREVIOUS job's terminal state through the whole
    // host-side window. Nothing on the wire separates this from a finished print.
    set_print_state(helix::PrintJobState::COMPLETE);
    set_preprint_phase(helix::PrintStartPhase::HOMING);

    auto afc = make<AmsBackendAfc>();
    CHECK_FALSE(afc->check_preconditions(/*requires_toolhead_motion=*/true).success());
}

TEST_CASE_METHOD(PausedGateFixture, "abandoning the pre-print block re-permits filament ops",
                 "[ams][safety][preparing]") {
    // A latched refusal would be worse than the bug it fixes: the user could not
    // load filament again for the rest of the session.
    set_print_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);

    auto afc = make<AmsBackendAfc>();
    REQUIRE_FALSE(afc->check_preconditions(/*requires_toolhead_motion=*/true).success());

    set_preprint_phase(helix::PrintStartPhase::IDLE);
    CHECK(afc->check_preconditions(true).success());
}

// ============================================================================
// The error factory in isolation — the copy is the user-facing contract
// ============================================================================

TEST_CASE("AmsErrorHelper::print_active copy matches the state it describes",
          "[ams][safety][paused]") {
    SECTION("paused names a recovery, never 'finish or cancel the print'") {
        AmsError err = AmsErrorHelper::print_active(/*is_paused=*/true);
        CHECK(err.user_msg == "Can't move filament while the print is paused");
        CHECK(err.suggestion.find("Resume") != std::string::npos);
        CHECK(err.suggestion.find("Finish or cancel") == std::string::npos);
    }
    SECTION("printing on a backend that could do it paused offers pausing") {
        AmsError err = AmsErrorHelper::print_active(/*is_paused=*/false,
                                                    /*pause_allows_ops=*/true);
        CHECK(err.suggestion.find("Pause the print") != std::string::npos);
    }
    SECTION("printing on a self-homing backend does not offer pausing") {
        AmsError err = AmsErrorHelper::print_active(/*is_paused=*/false,
                                                    /*pause_allows_ops=*/false);
        CHECK(err.suggestion.find("Pause the print") == std::string::npos);
        CHECK(err.suggestion.find("Finish or cancel") != std::string::npos);
    }
    SECTION("both refusals are WRONG_STATE and not recoverable-by-retry") {
        CHECK(AmsErrorHelper::print_active(true).result == AmsResult::WRONG_STATE);
        CHECK(AmsErrorHelper::print_active(false).result == AmsResult::WRONG_STATE);
        CHECK_FALSE(AmsErrorHelper::print_active(true).is_recoverable());
    }
}

// ============================================================================
// The cold-lane exemption is about OUR gate, not the firmware's
//
// check_preconditions(false) never consults print state, which is right for
// backends whose cold lane ops the firmware accepts mid-print. AFC's does not:
// cmd_LANE_UNLOAD opens with `if self.function.is_printing(): AFC_error(...);
// return`. That refusal ends in a bare return with the G-code still acked as
// success, so an ungated Eject reports success and moves nothing.
//
// This is the one condition eject_lane() still checks locally. The rest of that
// macro's if/elif chain used to be mirrored too and was removed in
// prestonbrown/helixscreen#1258; this predicate stayed because it is stable
// across every AFC version and is the same rule the context menu greys on.
//
// is_printing() is `print_stats.state == "printing"` exactly — NOT in_print() —
// so PAUSED genuinely reaches the firmware and must stay allowed.
//
// Mutation check: drop the refuse_if_printing() call from eject_lane() and the
// PRINTING section fails; widen it to print_occupies_toolhead() and the PAUSED
// section fails.
// ============================================================================

/// Named (not anonymous-namespace) and befriended in ams_backend_afc.h, matching
/// how every other AFC test helper reaches the private initialize_slots().
class AfcEjectPrintGateHelper : public AmsBackendAfc {
  public:
    AfcEjectPrintGateHelper(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendAfc(api, client) {
        running_.store(true);
        initialize_slots({"lane1", "lane2"});
    }

    /// Production dispatch goes through api_->execute_gcode with completion
    /// callbacks; capture instead so the assertion is on what would be sent.
    void dispatch_lane_unload(const std::string& lane_name) override {
        dispatched.push_back(lane_name);
        on_lane_unload_done();
    }

    std::vector<std::string> dispatched;
};

TEST_CASE_METHOD(PausedGateFixture, "AFC eject is refused while PRINTING, allowed while PAUSED",
                 "[ams][afc][eject][paused]") {
    AfcEjectPrintGateHelper backend(api.get(), &mock_client);

    SECTION("PRINTING refuses, and dispatches nothing") {
        set_print_state(helix::PrintJobState::PRINTING);

        AmsError err = backend.eject_lane(0);

        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Cannot run filament operation while printing");
        CHECK(backend.dispatched.empty());
    }

    SECTION("PAUSED still ejects — clearing a strand mid-pause is the point") {
        set_print_state(helix::PrintJobState::PAUSED);

        REQUIRE(backend.eject_lane(0).success());
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] == "lane1");
    }

    SECTION("STANDBY ejects") {
        set_print_state(helix::PrintJobState::STANDBY);

        REQUIRE(backend.eject_lane(0).success());
        CHECK(backend.dispatched.size() == 1);
    }
}

TEST_CASE_METHOD(PausedGateFixture, "AFC is the backend whose cold lane ops the firmware gates",
                 "[ams][afc][eject][paused]") {
    CHECK(make<AmsBackendAfc>()->cold_lane_ops_refused_during_print());
    CHECK_FALSE(make<AmsBackendHappyHare>()->cold_lane_ops_refused_during_print());
    CHECK_FALSE(make<AmsBackendQidi>()->cold_lane_ops_refused_during_print());
#if HELIX_HAS_IFS
    CHECK_FALSE(make<AmsBackendAd5xIfs>()->cold_lane_ops_refused_during_print());
#endif
}
