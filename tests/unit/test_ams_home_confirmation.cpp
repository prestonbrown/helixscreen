// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_toolchanger.h"
#include "filament_op_router.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/scoped_home_confirm_prompter.h"
#include "test_helpers/toolchanger_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Captures dispatched gcode and lets a test drive the homed answer.
/// Overrides BOTH execute_gcode forms so neither falls through to the base.
///
/// fail_next_gcode simulates a command failure with no live api_ to carry a
/// real async MoonrakerError: with api_ null, ensure_homed_then()'s only
/// failure signal for the (fixture-driven) G28/payload dispatch is the
/// AmsError these overrides return, which it translates into the
/// MoonrakerError passed to on_error. Deliberately NOT a stored
/// std::function<void(const MoonrakerError&)> callback invoked directly by
/// the override -- the override has no way to receive ensure_homed_then's
/// on_error as a parameter (the 1-arg/2-arg execute_gcode virtuals predate
/// on_error and can't grow it without breaking ~20 other fixtures), so the
/// return-value channel is what's actually reachable here.
class HomingProbeBackend : public AmsBackendAfc {
  public:
    HomingProbeBackend() : AmsBackendAfc(nullptr, nullptr) {}

    AmsError execute_gcode(const std::string& gcode) override {
        if (fail_next_gcode) {
            return AmsError(AmsResult::COMMAND_FAILED, "boom", "boom");
        }
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        if (fail_next_gcode) {
            return AmsError(AmsResult::COMMAND_FAILED, "boom", "boom");
        }
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;
    bool fail_next_gcode = false;
    std::vector<std::string> captured;
};

} // namespace

TEST_CASE("ensure_homed_then dispatches directly when already homed", "[ams][homing]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = true;

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "CHANGE_TOOL LANE=lane1");
}

TEST_CASE("ensure_homed_then sends G28 before the payload when unhomed", "[ams][homing]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane1");
}

TEST_CASE("ensure_homed_then skip_homing bypasses the home entirely", "[ams][homing]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    backend.ensure_homed_then("BOX_LOAD", nullptr, nullptr, MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS,
                              /*skip_homing=*/true);

    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "BOX_LOAD");
}

TEST_CASE("ensure_homed_then reports G28 failure through on_error", "[ams][homing]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;
    backend.fail_next_gcode = true;

    std::string seen;
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1", nullptr,
                              [&seen](const MoonrakerError& e) { seen = e.message; });

    CHECK(seen == "boom");
    CHECK(backend.captured.empty());
}

// =====================================================================
// The confirmation prompt (Task 8)
// =====================================================================
// HomingProbeBackend's api_ is null, so these stay on the synchronous
// fixture-only leg of ensure_homed_then() -- the prompter itself, and
// on_confirm/on_cancel, all run inline with no queue drain needed. That is
// exactly what proves the no-prompter default in test 4 below: nothing here
// (or in any of the ~4600 other pre-existing tests) installs a prompter, so
// request_home_confirmation() invoking on_confirm() synchronously is the only
// thing keeping today's "just home it" behaviour intact.

TEST_CASE("unhomed load asks before homing, and confirming proceeds", "[ams][homing][confirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()>) {
            ++prompts;
            confirm();
        });

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    CHECK(prompts == 1);
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane1");
}

TEST_CASE("cancelling the home sends nothing and lands IDLE", "[ams][homing][confirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    ScopedHomeConfirmPrompter guard(
        [](std::function<void()>, std::function<void()> cancel) { cancel(); });

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    CHECK(backend.captured.empty());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);

    // A cancelled op must not wedge the backend: the next load still works.
    // homed=true means ensure_homed_then() never consults the prompter, so
    // the still-installed cancel lambda above is simply never invoked.
    backend.homed = true;
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane2");
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "CHANGE_TOOL LANE=lane2");
}

TEST_CASE("an already-homed printer is never prompted", "[ams][homing][confirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = true;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()>) {
            ++prompts;
            confirm();
        });

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    CHECK(prompts == 0);
    REQUIRE(backend.captured.size() == 1);
}

TEST_CASE("with no prompter installed the home proceeds silently", "[ams][homing][confirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;
    ScopedHomeConfirmPrompter guard;

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");

    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
}

// =====================================================================
// arm_home_preconfirmed() / clear_home_preconfirmed() -- the pre-preheat
// confirmation seam (toolhead-homing-dry, option B)
// =====================================================================
// A UI surface that preheats before dispatching (FilamentPanel::LOAD,
// AmsOperationSidebar::handle_load_with_preheat) asks "home printer first?"
// BEFORE it starts heating, so a decline never wastes a heat cycle. On
// confirm it arms this flag and starts the preheat; ensure_homed_then() -
// called later, after the preheat, right before the tier-1 dispatch - must
// then skip asking AGAIN, but the physical G28 still fires exactly where it
// always has: nothing here is a substitute for toolhead_homed(), only for
// the prompt.

TEST_CASE("arm_home_preconfirmed is consumed single-shot", "[ams][homing][preconfirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()>) {
            ++prompts;
            confirm();
        });

    backend.arm_home_preconfirmed();

    // First dispatch: pre-confirmed, so no prompt -- but G28 still fires,
    // unchanged, because the toolhead is genuinely unhomed.
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");
    CHECK(prompts == 0);
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane1");

    // Second dispatch, still unhomed: the flag was consumed by the first
    // call, so this one DOES prompt again.
    backend.captured.clear();
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane2");
    CHECK(prompts == 1);
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane2");
}

TEST_CASE("arm_home_preconfirmed is NOT consumed by a dispatch on an already-homed toolhead",
          "[ams][homing][preconfirm]") {
    // The ordering trap: a guard written as
    //   skip_homing || std::exchange(home_preconfirmed_, false) || toolhead_homed()
    // consumes the flag on ANY call, homed or not, because operator|| evaluates
    // left to right. The correct guard checks toolhead_homed() first, so the
    // homed branch short-circuits before ever touching the flag.
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = true;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()>) {
            ++prompts;
            confirm();
        });

    backend.arm_home_preconfirmed();

    // Homed: dispatches straight through the toolhead_homed() branch, which
    // must leave the armed flag untouched.
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");
    CHECK(prompts == 0);
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "CHANGE_TOOL LANE=lane1");

    // Now go unhomed. If the homed call above had wrongly consumed the flag,
    // this one would prompt. It must not -- the still-armed flag from before
    // is what should skip the prompt here (and it's the one that gets
    // consumed by THIS call).
    backend.captured.clear();
    backend.homed = false;
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane2");
    CHECK(prompts == 0);
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane2");
}

TEST_CASE("clear_home_preconfirmed undoes an armed-but-abandoned confirmation",
          "[ams][homing][preconfirm]") {
    // Models a UI surface that armed consent, then abandoned the load before
    // it ever dispatched (preheat cancelled, panel torn down, op aborted).
    // Without an explicit clear, the armed flag would leak forward into a
    // later, completely unrelated dispatch on the same backend.
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()>) {
            ++prompts;
            confirm();
        });

    backend.arm_home_preconfirmed();
    backend.clear_home_preconfirmed();

    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");
    CHECK(prompts == 1); // the abandoned confirmation must not have leaked forward
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane1");
}

TEST_CASE("declining before a dispatch never arms anything for a later one",
          "[ams][homing][preconfirm]") {
    LVGLTestFixture fixture;
    HomingProbeBackend backend;
    backend.homed = false;

    int prompts = 0;
    ScopedHomeConfirmPrompter guard(
        [&prompts](std::function<void()> confirm, std::function<void()> cancel) {
            ++prompts;
            if (prompts == 1) {
                cancel(); // first dispatch: user declines
            } else {
                confirm(); // second dispatch: user confirms this time
            }
        });

    // Declined: no G28, no payload -- "commands no heat" at the backend
    // contract level (the UI-surface preheat call is covered by the live
    // ctl transcript, since FilamentPanel/AmsOperationSidebar are not
    // unit-instantiable -- see test_filament_load_preheat.cpp).
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane1");
    CHECK(backend.captured.empty());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);

    // A later, unrelated dispatch must still ask -- the decline armed nothing.
    backend.ensure_homed_then("CHANGE_TOOL LANE=lane2");
    CHECK(prompts == 2);
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "CHANGE_TOOL LANE=lane2");
}

// =====================================================================
// A dismissal that never resolves synchronously must still unwedge
// dispatch_operation's optimistic action (Task 8 review fix)
// =====================================================================
// The tests above all use ensure_homed_then() directly on a HomingProbeBackend
// whose AmsAction starts and stays IDLE -- ensure_homed_then() itself never
// touches system_info_.action except on its own cancel/failure branches, so
// none of them can express the bug a reviewer found in the first cut of the
// real prompter (src/application/subject_initializer.cpp): it wired
// modal_show_confirmation() through the STATIC Modal::show() path, and on
// that path backdrop-tap and ESC call Modal::hide(dialog) directly --
// bypassing the confirm/cancel lv_event_cb_t entirely. Neither callback ever
// fired, so a dismissal by anything other than the two dialog buttons left
// whichever AmsBackend that had called dispatch_operation() stuck: its
// begin_dispatch_locked() sets the AmsAction optimistically *before*
// ensure_homed_then() ever runs, and only the confirm/cancel callback can
// resolve it -- ensure_homed_then() always returns success() once it decides
// to prompt, so dispatch_operation()'s own `if (!result) abandon_dispatch()`
// safety net can't fire either. AmsBackendToolChanger has no other watchdog
// at all (unlike AFC's stuck-action timeout), so this was a permanent lockout
// short of an app restart.
//
// A prompter that truly never calls back, ever, cannot be expressed as a
// terminating test -- the busy state would really be permanent, by
// construction, with or without a fix. So this models what a real dialog
// does instead: request_home_confirmation() returns having invoked NEITHER
// callback synchronously (exactly what showing a modal does -- resolution
// comes later, from an LVGL event), and the test holds onto the callback pair
// itself, standing in for "the dialog is still open." Resolving it later
// proves the same unwind the Cancel button uses also runs for a dismissal
// that isn't a direct, synchronous confirm()/cancel() call in the same
// stack frame -- which is exactly the shape backdrop-tap/ESC/the fixed
// HomeConfirmModal's on_hide() fallback net all have.
class ToolChangerHomingProbeBackend : public AmsBackendToolChanger {
  public:
    ToolChangerHomingProbeBackend() : AmsBackendToolChanger(nullptr, nullptr) {}

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;
    std::vector<std::string> captured;
};

TEST_CASE("a dismissal that resolves asynchronously still unwedges dispatch_operation's "
          "optimistic action, exactly like the Cancel button",
          "[ams][homing][confirm]") {
    LVGLTestFixture fixture;
    ToolChangerHomingProbeBackend backend;
    backend.homed = false;

    std::function<void()> pending_cancel;
    ScopedHomeConfirmPrompter guard(
        [&pending_cancel](std::function<void()>, std::function<void()> cancel) {
            // Neither callback is invoked here -- the dialog is "open" and
            // will resolve later, from an LVGL event (button, ESC, or
            // backdrop-tap), not from this call.
            pending_cancel = std::move(cancel);
        });

    ToolChangerTestAccess::call_dispatch_operation(backend, "SELECT_TOOL T=1",
                                                   AmsAction::SELECTING);

    // The prompter didn't resolve synchronously, so the optimistic action
    // dispatch_operation() set before ever reaching ensure_homed_then() is
    // still busy -- expected while the dialog is open, not the bug.
    REQUIRE(pending_cancel);
    CHECK(backend.get_system_info().action == AmsAction::SELECTING);
    CHECK(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.captured.empty());

    // Resolve it -- standing in for backdrop-tap/ESC/Cancel, all of which
    // reach this same callback through HomeConfirmModal's fallback net,
    // which (final-review I2) routes through AmsBackendToolChanger::
    // on_home_confirmation_declined() -> abandon_dispatch() -- the SAME
    // unwind a direct Cancel-button tap uses. Check more than just the
    // action: a partial unwind that only reset action to IDLE would leave
    // pending_dispatch_action_ armed (so the next macro ack, or a newer
    // dispatch, would resolve against a generation nothing is tracking
    // anymore) and operation_detail stale (the sidebar would keep showing
    // "Tool swap" under an IDLE action) -- that gap is exactly what made the
    // "exactly like the Cancel button" claim in this test's name untrue
    // before the fix.
    pending_cancel();

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.get_system_info().operation_detail.empty());
    CHECK(backend.captured.empty());

    // Not wedged: a subsequent dispatch still works.
    backend.homed = true;
    auto err = ToolChangerTestAccess::call_dispatch_operation(backend, "SELECT_TOOL T=2",
                                                              AmsAction::SELECTING);
    REQUIRE(err.success());
    REQUIRE(backend.captured.size() == 1);
    CHECK(backend.captured[0] == "SELECT_TOOL T=2");
}

// =====================================================================
// dispatch_payload's "custom" branch (integration)
// =====================================================================
// Unit-style tests above use HomingProbeBackend, whose api_ is null -- they
// never leave dispatch_payload()'s legacy branch (the hardcoded execute_gcode
// virtuals), so they cannot prove a caller's non-default on_error/timeout_ms/
// silent actually reach MoonrakerAPI::execute_gcode(). MoonrakerAPIMock does
// NOT override execute_gcode() -- it inherits the real implementation and
// round-trips through MoonrakerClientMock, exactly like
// "QIDI Box on_started dispatches printer.objects.query (integration)" in
// test_ams_backend_qidi.cpp. That is the only way to reach the custom branch
// from a test: a live api_/client_ so !api_ doesn't short-circuit it first.
TEST_CASE("ensure_homed_then custom timeout/silent bypass the hardcoded virtuals and reach "
          "MoonrakerAPI::execute_gcode (integration)",
          "[ams][homing][integration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAfc backend(&api, &client);

    constexpr uint32_t CUSTOM_TIMEOUT_MS = 12345;
    REQUIRE(CUSTOM_TIMEOUT_MS != MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
    // The mock handler for printer.gcode.script runs synchronously inside
    // send_jsonrpc(), so this fires before ensure_homed_then() even returns --
    // exercising the on_error leg of dispatch_payload()'s custom branch, not
    // just the dispatch itself.
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "boom", "BOX_LOAD");

    std::string seen;
    // skip_homing=true keeps this test on the payload leg, not G28 -- that leg
    // is already covered by the two tests above.
    auto err = backend.ensure_homed_then(
        "BOX_LOAD", nullptr, [&seen](const MoonrakerError& e) { seen = e.message; },
        CUSTOM_TIMEOUT_MS, /*skip_homing=*/true, /*silent=*/false);
    REQUIRE(err.success());

    // Dispatch went straight to MoonrakerAPI::execute_gcode() carrying OUR
    // timeout_ms/silent -- the hardcoded 1-arg/2-arg execute_gcode virtuals fix
    // AMS_OPERATION_TIMEOUT_MS/true and can never produce these values.
    CHECK(client.last_send_method() == "printer.gcode.script");
    CHECK(client.last_send_script() == "BOX_LOAD");
    CHECK(client.last_send_timeout_ms() == CUSTOM_TIMEOUT_MS);
    CHECK_FALSE(client.last_send_silent());

    // The error callback is marshalled through token.defer() (L081 Mechanism
    // C) rather than invoked inline -- drain the queue to run it.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(seen == "boom");
}

// =====================================================================
// AmsBackendCfs::dispatch_action_script (Task 5: collapse the CFS fork)
// =====================================================================
// dispatch_action_script used to hand-duplicate ensure_homed_then's
// query/parse/G28 sequence purely to get on_error plumbing the base lacked.
// It now collapses to a single ensure_homed_then() call with on_error set and
// silent=false -- which always lands on dispatch_payload()'s "custom" branch
// (see the integration test above), so these tests need a live api_/client_
// exactly like that one: a null-api_ probe would short-circuit dispatch_payload
// before it ever reaches MoonrakerAPI::execute_gcode(), and the payload gcode
// would never be recorded.
//
// dispatch_action_script stays private in AmsBackendCfs -- these tests reach
// the REAL production implementation through the ::CfsTestAccess friend shim
// (tests/test_helpers/cfs_test_access.h), not by subclassing to widen access.

TEST_CASE("CFS dispatch_action_script routes through ensure_homed_then and homes when unhomed",
          "[ams][homing][cfs]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // homed_axes defaults to "" (not homed) -- exercises the G28-then-payload leg.
    helix::printer::AmsBackendCfs backend(&api, nullptr);

    auto err = CfsTestAccess::call_dispatch_action_script(backend, "BOX_LOAD LANE=1");
    REQUIRE(err.success());

    // G28's success callback is marshalled through token.defer() (L081
    // Mechanism C) before the payload is dispatched -- drain to let it run.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(client.gcode_script_history().size() == 2);
    CHECK(client.gcode_script_history()[0] == "G28");
    CHECK(client.gcode_script_history()[1] == "BOX_LOAD LANE=1");

    // The payload send is the last one recorded -- confirms silent=false
    // (CFS's own timeout-toast behaviour) survived the collapse into
    // ensure_homed_then()'s "custom" dispatch_payload() branch.
    CHECK(client.last_send_method() == "printer.gcode.script");
    CHECK(client.last_send_script() == "BOX_LOAD LANE=1");
    CHECK_FALSE(client.last_send_silent());
}

TEST_CASE("CFS Fork variant never homes via dispatch_action_script", "[ams][homing][cfs][fork]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::printer::AmsBackendCfs backend(&api, nullptr);
    CfsTestAccess::set_macro_variant_fork(backend);

    // homed_axes is STILL "" (not homed) here -- proves the skip comes from
    // skip_homing=true (Fork maps to it), not from the toolhead happening to
    // already be homed.
    auto err = CfsTestAccess::call_dispatch_action_script(backend, "BOX_LOAD LANE=1");
    REQUIRE(err.success());

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(client.gcode_script_history().size() == 1);
    CHECK(client.gcode_script_history()[0] == "BOX_LOAD LANE=1");
    CHECK_FALSE(client.last_send_silent());
}
