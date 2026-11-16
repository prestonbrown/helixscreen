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
#include "test_helpers/backend_harness.h"
// AD5X IFS is feature-gated (HELIX_HAS_IFS=0 on the space-constrained cross
// builds, mk/cross.mk), so every self-homing assertion below is guarded. The
// non-self-homing half of the table is unconditional.
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_error.h"
#include "filament_op_execute.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/snapmaker_test_access.h"
#if HELIX_HAS_IFS
#include "test_helpers/ad5x_ifs_test_access.h"
#endif

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;

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
    AmsError call_refuse_load_if_printing() const {
        return this->refuse_load_if_printing();
    }
    /// The running flag this double raises stands in for a subscription; a case
    /// that drives frames needs the real one.
    void subscribe() {
        this->running_.store(false);
        REQUIRE(this->start().success());
    }

    std::vector<std::string> dispatched;

    AmsError execute_gcode(const std::string& gcode) override {
        dispatched.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                           helix::SharedCallback<void(const MoonrakerError&)>, bool) override {
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

    void set_homed_axes(const char* axes) {
        state.update_from_status({{"toolhead", {{"homed_axes", axes}}}});
    }

    /// A host-side pre-print block: the wire still reads standby.
    void set_preprint_phase(helix::PrintStartPhase phase) {
        state.print_state().set_print_start_state(phase, "", 0);
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
    CHECK(make<helix::AmsBackendAd5xIfs>()->filament_ops_self_home());
#endif
    CHECK_FALSE(make<helix::AmsBackendAfc>()->filament_ops_self_home());
    CHECK_FALSE(make<helix::AmsBackendHappyHare>()->filament_ops_self_home());
    CHECK_FALSE(make<helix::AmsBackendQidi>()->filament_ops_self_home());
    // Snapmaker's AUTO_FEEDING does home, but prepare_for_resume() drives
    // `AUTO_FEEDING ... PRINTING=1` on a PAUSED job and was live-verified on
    // physical U1 hardware (#991) — the firmware exposes PRINTING=1 for exactly
    // that recovery. No evidence the home is unsafe, so this stays false and the
    // runout-recovery workflow keeps working.
    CHECK_FALSE(make<helix::AmsBackendSnapmaker>()->filament_ops_self_home());
}

// ============================================================================
// Cell 1 + 2: PRINTING refuses for BOTH kinds of backend
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "PRINTING refuses toolhead-motion ops on every backend",
                 "[ams][safety][paused]") {
    set_print_state(helix::PrintJobState::PRINTING);

#if HELIX_HAS_IFS
    SECTION("self-homing backend (AD5X)") {
        auto backend = make<helix::AmsBackendAd5xIfs>();
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
        auto backend = make<helix::AmsBackendAfc>();
        AmsError err = backend->check_preconditions(true);
        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Cannot run filament operation while printing");
        // Pausing IS a recovery this backend can honour, so name it instead of
        // pushing the user to throw away a print they could have saved.
        CHECK(err.suggestion.find("Pause the print") != std::string::npos);
    }

    SECTION("non-self-homing backend (Happy Hare)") {
        auto backend = make<helix::AmsBackendHappyHare>();
        CHECK_FALSE(backend->check_preconditions(true).success());
        CHECK_FALSE(backend->call_refuse_if_printing().success());
    }

    SECTION("QIDI gates via refuse_if_printing() directly, not check_preconditions()") {
        // QIDI's load/unload/change_tool call refuse_if_printing() themselves
        // because they skip the running_/busy gate. That path must refuse too.
        auto backend = make<helix::AmsBackendQidi>();
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
    auto backend = make<helix::AmsBackendAd5xIfs>();

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
// The load refinement: a self-homing backend whose home is skipped once homed
// ============================================================================

TEST_CASE("print_blocks_filament_load: the paused cell follows the homed toolhead",
          "[ams][safety][paused][ad5x]") {
    using helix::print_blocks_filament_load;
    // Self-homing, home skipped once homed (zMod's conditional _G28).
    CHECK_FALSE(print_blocks_filament_load(PrintState::Paused, true, true, /*homed=*/true, false));
    CHECK(print_blocks_filament_load(PrintState::Paused, true, true, /*homed=*/false, false));
    // A home that always runs never lets a paused job load, homed or not.
    CHECK(print_blocks_filament_load(PrintState::Paused, true, false, true, false));
    // Printing and preparing refuse whatever the toolhead says.
    CHECK(print_blocks_filament_load(PrintState::Printing, true, true, true, false));
    CHECK(print_blocks_filament_load(PrintState::Preparing, true, true, true, false));
    CHECK(print_blocks_filament_load(PrintState::Printing, false, false, true, false));
    // Idle is unchanged.
    CHECK_FALSE(print_blocks_filament_load(PrintState::Idle, true, true, false, false));
    // A backend that never self-homes loads while paused, homed or not.
    CHECK_FALSE(print_blocks_filament_load(PrintState::Paused, false, false, false, false));
    // A load that unloads the toolhead first is an unload: paused and homed,
    // a self-homing backend still refuses it; one that never homes allows it.
    CHECK(print_blocks_filament_load(PrintState::Paused, true, true, true, /*unloads=*/true));
    CHECK_FALSE(print_blocks_filament_load(PrintState::Paused, false, false, true, true));
    CHECK_FALSE(print_blocks_filament_load(PrintState::Idle, true, true, true, true));
}

#if HELIX_HAS_IFS
TEST_CASE_METHOD(PausedGateFixture, "a paused AD5X loads only on a homed toolhead",
                 "[ams][safety][paused][ad5x]") {
    auto backend = make<helix::AmsBackendAd5xIfs>();
    REQUIRE(backend->self_home_skipped_when_homed());
    auto seat_other_lane = [&](bool present) {
        helix::Ad5xIfsTestAccess::set_seated_chan(*backend, 2);
        helix::Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, present);
    };

    SECTION("paused + homed: the load goes through, everything else still refuses") {
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xyz");
        CHECK(backend->call_refuse_load_if_printing().success());
        CHECK(backend->unload_filament(0).user_msg ==
              "Can't move filament while the print is paused");
        // The load reaches the firmware macro, and homes nothing on the way.
        CHECK(backend->load_filament(0).success());
        REQUIRE_FALSE(backend->dispatched.empty());
        CHECK(backend->dispatched.front() == "INSERT_PRUTOK_IFS PRUTOK=1");
        CHECK(
            std::none_of(backend->dispatched.begin(), backend->dispatched.end(),
                         [](const std::string& g) { return g.find("G28") != std::string::npos; }));
        CHECK_FALSE(backend->call_refuse_if_printing().success());
        CHECK_FALSE(backend->check_preconditions(/*requires_toolhead_motion=*/true).success());
    }
    SECTION("paused + homed + another lane seated: the load would unload it, so refuse") {
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xyz");
        seat_other_lane(/*present=*/true);
        REQUIRE(backend->load_unloads_toolhead_now());
        AmsError err = backend->load_filament(0);
        REQUIRE_FALSE(err.success());
        CHECK(err.user_msg == "Can't move filament while the print is paused");
        CHECK(backend->dispatched.empty());
        // Every Load surface asks the same question.
        CHECK(helix::ui::print_blocks_load_now(backend.get(), state));
    }
    SECTION("paused + homed + head switch empty: the load unloads nothing, so it goes through") {
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xyz");
        seat_other_lane(/*present=*/false);
        REQUIRE_FALSE(backend->load_unloads_toolhead_now());
        CHECK(backend->call_refuse_load_if_printing().success());
        CHECK_FALSE(helix::ui::print_blocks_load_now(backend.get(), state));
    }
    SECTION("paused + empty head: the head view's Load follows the homed toolhead") {
        set_print_state(helix::PrintJobState::PAUSED);
        helix::Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/false);
        REQUIRE_FALSE(backend->load_unloads_toolhead_now());

        set_homed_axes("xyz");
        auto view = helix::ui::read_filament_head_view(backend.get(), state);
        REQUIRE_FALSE(view.heads.empty());
        REQUIRE(view.heads[0].ops.load == helix::FilamentOpSupport::Automatic);
        CHECK(view.print_blocks_op);
        CHECK(view.print_blocks_load == std::optional<bool>(false));
        CHECK(helix::ui::decide_slot_actions(view, 0).load.enabled);
        // Unload still answers to the paused refusal.
        CHECK_FALSE(helix::ui::decide_slot_actions(view, 0).unload.enabled);

        set_homed_axes("xy");
        view = helix::ui::read_filament_head_view(backend.get(), state);
        CHECK(view.print_blocks_load == std::optional<bool>(true));
        CHECK_FALSE(helix::ui::decide_slot_actions(view, 0).load.enabled);
    }
    SECTION("paused + homed on the standalone module: IFS_LOAD keeps the paused refusal") {
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xyz");
        helix::Ad5xIfsTestAccess::set_module_live(*backend, true);
        REQUIRE_FALSE(backend->load_unloads_toolhead_now());
        AmsError err = backend->load_filament(0);
        REQUIRE_FALSE(err.success());
        CHECK(err.user_msg == "Can't move filament while the print is paused");
        CHECK(backend->dispatched.empty());
    }
    SECTION("paused + not fully homed: the buried _G28 would fire, so refuse") {
        set_print_state(helix::PrintJobState::PAUSED);
        set_homed_axes("xy");
        AmsError err = backend->call_refuse_load_if_printing();
        REQUIRE_FALSE(err.success());
        CHECK(err.user_msg == "Can't move filament while the print is paused");
    }
    SECTION("printing refuses the load even homed") {
        set_print_state(helix::PrintJobState::PRINTING);
        set_homed_axes("xyz");
        CHECK_FALSE(backend->call_refuse_load_if_printing().success());
    }
    SECTION("idle loads unhomed") {
        set_print_state(helix::PrintJobState::STANDBY);
        set_homed_axes("");
        CHECK(backend->call_refuse_load_if_printing().success());
    }
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
        auto backend = make<helix::AmsBackendAfc>();
        CHECK(backend->check_preconditions(/*requires_toolhead_motion=*/true).success());
        CHECK(backend->call_refuse_if_printing().success());
    }

    SECTION("Happy Hare") {
        auto backend = make<helix::AmsBackendHappyHare>();
        CHECK(backend->check_preconditions(true).success());
    }

    SECTION("QIDI, via its direct refuse_if_printing() call site") {
        auto backend = make<helix::AmsBackendQidi>();
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
    auto backend = make<helix::AmsBackendSnapmaker>();

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
    auto backend = make<helix::AmsBackendSnapmaker>();

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
    auto backend = make<helix::AmsBackendSnapmaker>();

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
    auto afc = make<helix::AmsBackendAfc>();
#if HELIX_HAS_IFS
    auto ad5x = make<helix::AmsBackendAd5xIfs>();
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

    auto afc = make<helix::AmsBackendAfc>();
    auto snap = make<helix::AmsBackendSnapmaker>();
#if HELIX_HAS_IFS
    auto ad5x = make<helix::AmsBackendAd5xIfs>();
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

    auto afc = make<helix::AmsBackendAfc>();
    CHECK_FALSE(afc->check_preconditions(/*requires_toolhead_motion=*/true).success());
}

TEST_CASE_METHOD(PausedGateFixture, "abandoning the pre-print block re-permits filament ops",
                 "[ams][safety][preparing]") {
    // A latched refusal would be worse than the bug it fixes: the user could not
    // load filament again for the rest of the session.
    set_print_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);

    auto afc = make<helix::AmsBackendAfc>();
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

namespace helix {

/// Named (not anonymous-namespace) and befriended in ams_backend_afc.h, matching
/// how every other AFC test helper reaches the private initialize_slots().
class AfcEjectPrintGateHelper : public AmsBackendAfc {
  public:
    AfcEjectPrintGateHelper(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendAfc(api, client) {
        running_.store(true);
        AfcTestAccess::initialize_slots(*this, std::vector<std::string>{"lane1", "lane2"});
    }

    /// Production dispatch goes through api_->execute_gcode with completion
    /// callbacks; capture instead so the assertion is on what would be sent.
    void dispatch_lane_unload(const std::string& lane_name) override {
        dispatched.push_back(lane_name);
        on_lane_unload_done();
    }

    std::vector<std::string> dispatched;
};
} // namespace helix

TEST_CASE_METHOD(PausedGateFixture, "AFC eject is refused while PRINTING, allowed while PAUSED",
                 "[ams][afc][eject][paused]") {
    helix::AfcEjectPrintGateHelper backend(api.get(), &mock_client);

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

TEST_CASE_METHOD(PausedGateFixture,
                 "AFC toolhead calibration is refused while a print holds the printer",
                 "[ams][afc][device_actions][paused]") {
    helix::AfcEjectPrintGateHelper backend(api.get(), &mock_client);
    const nlohmann::json help = {
        {"jsonrpc", "2.0"},
        {"result", {{"AFC_TOOLHEAD_CALIBRATION", "Open the toolhead variable calibration menu"}}}};
    REQUIRE(helix::AfcTestAccess::apply_gcode_help_response(backend, help));
    const auto actions = backend.get_device_actions();
    const auto it = std::find_if(actions.begin(), actions.end(),
                                 [](const auto& a) { return a.id == "toolhead_calibration"; });
    REQUIRE(it != actions.end());
    CHECK(it->moves_machine);

    auto sent_calibration = [&] {
        const auto& sent = mock_client.gcode_script_history();
        return std::any_of(sent.begin(), sent.end(), [](const std::string& g) {
            return g.find("AFC_TOOLHEAD_CALIBRATION") != std::string::npos;
        });
    };
    mock_client.clear_gcode_script_history();

    for (auto s : {helix::PrintJobState::PRINTING, helix::PrintJobState::PAUSED}) {
        set_print_state(s);
        const AmsError err = backend.execute_device_action("toolhead_calibration");
        CHECK(err.user_msg == "Printer is busy with a print");
        CHECK_FALSE(sent_calibration());
    }

    set_print_state(helix::PrintJobState::STANDBY);
    REQUIRE(backend.execute_device_action("toolhead_calibration").success());
    CHECK(sent_calibration());
}

TEST_CASE_METHOD(PausedGateFixture, "AFC is the backend whose cold lane ops the firmware gates",
                 "[ams][afc][eject][paused]") {
    CHECK(make<helix::AmsBackendAfc>()->cold_lane_ops_refused_during_print());
    CHECK_FALSE(make<helix::AmsBackendHappyHare>()->cold_lane_ops_refused_during_print());
    CHECK_FALSE(make<helix::AmsBackendQidi>()->cold_lane_ops_refused_during_print());
#if HELIX_HAS_IFS
    CHECK_FALSE(make<helix::AmsBackendAd5xIfs>()->cold_lane_ops_refused_during_print());
#endif
}

// ============================================================================
// Park Toolhead: the shared gate refuses a paused job for every caller
// ============================================================================

TEST_CASE_METHOD(PausedGateFixture, "park_active_tool is refused while paused or printing",
                 "[ams][safety][paused][park]") {
    auto backend = make<helix::AmsBackendSnapmaker>();
    backend->subscribe();
    helix::test::fire_status(
        mock_client, nlohmann::json{{"toolhead", nlohmann::json{{"extruder", "extruder1"}}}});
    REQUIRE(backend->get_current_tool() == 1);

    SECTION("paused: refused although the backend allows other ops while paused") {
        set_print_state(helix::PrintJobState::PAUSED);
        CHECK(backend->change_tool(2).success());
        backend->dispatched.clear();

        CHECK_FALSE(backend->park_active_tool().success());
        CHECK(backend->dispatched.empty());
    }
    SECTION("printing: refused") {
        set_print_state(helix::PrintJobState::PRINTING);
        CHECK_FALSE(backend->park_active_tool().success());
        CHECK(backend->dispatched.empty());
    }
    SECTION("standby: parks") {
        set_print_state(helix::PrintJobState::STANDBY);
        REQUIRE(backend->park_active_tool().success());
        REQUIRE(backend->dispatched.size() == 1);
        CHECK(backend->dispatched[0] == "PARK_EXTRUDER");
    }
}
