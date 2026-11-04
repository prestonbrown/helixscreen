// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_panel_op_timeout.cpp
 * @brief Regression guard (#1183): a filament operation that never reports back
 *        must not leave its button spinning forever. A second section at the
 *        bottom covers the mirror-image defect — an operation that reports back
 *        `ok` for a macro that aborted part-way through.
 *
 * Run with: ./build/bin/helix-tests "[ui_integration][filament][regression][1183]"
 *
 * Two defects, both reproduced against a real FilamentPanel built from
 * filament_panel.xml over a recording AMS backend:
 *
 *   1. Every operation_guard_.begin() callsite used to pass a capture-nothing
 *      lambda that only raised a toast. OperationTimeoutGuard clears its own
 *      filament_operation_in_progress subject, so the buttons re-enabled at 120s,
 *      but the per-op state subject (filament_op_*_state: 0 idle / 1 busy / 2 done)
 *      stayed at 1 for the rest of the session and backend_op_active_ /
 *      op_in_flight_ stayed set. The shared handle_operation_timeout() now fails
 *      whichever op is showing busy.
 *
 *   2. The AMS completion observer accepted only AmsAction::IDLE. AFC's
 *      stuck-action backstop resolves a wedged operation to AmsAction::ERROR and
 *      nothing else, so on this panel it did nothing at all and the spinner ran
 *      until the timeout. ERROR now terminates the op via op_failed().
 *
 * The timeout is driven by re-arming the guard's REAL timer (period 1ms +
 * lv_timer_ready) rather than waiting out the 120s budget, so the handler under
 * test is the one the production callsite installed — a callsite that regresses to
 * the old lambda fails these tests.
 *
 * Mutation checks (each must break exactly the listed test):
 *   - restore the capture-nothing lambda at a begin() callsite  -> "times out"
 *     tests for that path fail
 *   - make handle_operation_timeout() clear the flags but not the op state
 *     -> "spinner stops" tests fail, "starts clean" still passes
 *   - drop the ERROR branch in ams_action_observer_ -> the ERROR test fails
 *   - make the ERROR branch call op_succeeded() -> the ERROR test fails (state
 *     lands on 2/done, not 0/idle)
 */

#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "filament_op_router.h"
#include "tool_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::AmsAction;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::PathTopology;

using helix::ToolState;
using helix::ToolTopology;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {

// Minimal AFC-shaped backend: the panel's op path needs a slot-selecting system
// whose load/unload succeed synchronously and then stay silent, which is exactly
// the fire-and-forget shape that makes a stall observable.
class StubBackend : public helix::AmsBackendMock {
  public:
    StubBackend() : helix::AmsBackendMock(4) {}

    AmsSystemInfo sys_{};
    int loaded_slot_ = -1;
    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        return sys_;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] AmsType get_type() const override {
        return sys_.type;
    }
    [[nodiscard]] bool requires_slot_selection_for_load() const override {
        return true;
    }
    [[nodiscard]] bool slot_is_actively_loaded(int slot) const override {
        return slot == loaded_slot_;
    }
    [[nodiscard]] bool slot_has_filament_at_toolhead(int slot) const override {
        return slot == loaded_slot_;
    }
    helix::AmsError load_filament(int) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError unload_filament(int) override {
        return helix::AmsErrorHelper::success();
    }

    int restart_calls = 0;
    void on_klipper_restarted() override {
        ++restart_calls;
    }
};

AmsSystemInfo afc_sys() {
    AmsSystemInfo sys;
    sys.type = AmsType::AFC;
    sys.total_slots = 4;
    sys.current_slot = 3;
    sys.filament_loaded = true;
    sys.tool_to_slot_map = {0, 1, 2, 3};
    return sys;
}

ToolTopology identity_topo() {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    return topo;
}

// Real FilamentPanel over the stub backend, torn down in the order the singletons
// require (UI subtree, panel, then the shared AMS/Tool state).
struct TimeoutHarness {
    LVGLUITestFixture& fx;
    StubBackend* mock = nullptr;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    explicit TimeoutHarness(LVGLUITestFixture& f) : fx(f) {
        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);

        auto owned = std::make_unique<StubBackend>();
        owned->sys_ = afc_sys();
        owned->loaded_slot_ = 3; // slot 0 stays free so a Load can proceed
        mock = owned.get();
        helix::AmsState::instance().set_backend(std::move(owned));
        helix::AmsState::instance().sync_from_backend();
        ToolState::instance().set_ams_topology(identity_topo());

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);

        TA::populate_extruder_dropdown(*panel);
        select_tool(0);
    }

    void select_tool(int idx) {
        lv_obj_t* dd = TA::extruder_dropdown(*panel);
        REQUIRE(dd != nullptr);
        REQUIRE(lv_dropdown_get_option_count(dd) >= static_cast<uint32_t>(idx + 1));
        lv_dropdown_set_selected(dd, static_cast<uint32_t>(idx));
    }

    /// Fire the guard's armed timer now instead of at the 120s budget.
    void fire_operation_timeout() {
        lv_timer_t* t = TA::operation_timer(*panel);
        REQUIRE(t != nullptr);
        lv_timer_set_period(t, 1);
        lv_timer_ready(t);
        fx.process_lvgl(20);
    }

    void publish_action(AmsAction a, int settle_ms) {
        lv_subject_set_int(helix::AmsState::instance().get_ams_action_subject(),
                           static_cast<int>(a));
        fx.process_lvgl(settle_ms);
    }

    ~TimeoutHarness() {
        if (root) {
            lv_obj_delete(root);
        }
        fx.process_lvgl(10);
        panel.reset();
        helix::AmsState::instance().set_backend(nullptr);
        ToolState::instance().clear_ams_topology();
        helix::AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "#1183: a timed-out backend load stops the spinner",
                 "[ui_integration][filament][regression][1183]") {
    // The reported hang: AFC no-op'd the Load and never moved its action, so the
    // only thing that could ever end the op was the guard's timeout. It has to put
    // the Load button back to idle, not just raise a toast.
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::operation_active(*h.panel));
    REQUIRE(TA::op_load_state(*h.panel) == 1); // spinner running

    h.fire_operation_timeout();

    CHECK(TA::op_load_state(*h.panel) == 0); // spinner cleared
    CHECK_FALSE(TA::operation_active(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "#1183: a timed-out backend op clears the in-flight bookkeeping",
                 "[ui_integration][filament][regression][1183]") {
    // backend_op_active_ / op_in_flight_ gate the AMS completion observer. Left set
    // after a stall they mis-attribute the NEXT operation's completion, so the
    // timeout must reset both.
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::backend_op_active(*h.panel));
    REQUIRE(TA::op_in_flight(*h.panel));

    h.fire_operation_timeout();

    CHECK_FALSE(TA::backend_op_active(*h.panel));
    CHECK_FALSE(TA::op_in_flight(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture, "A timed-out backend op leaves the backend's action alone",
                 "[ui_integration][filament][op_timeout_expire]") {
    // A long operation (a cold-nozzle swap) can run past the panel's budget, so
    // the timeout only warns: ending the backend's operation here would
    // re-enable Load, Unload and tool changes in the middle of it.
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::backend_op_active(*h.panel));
    h.mock->sys_.action = AmsAction::LOADING;

    h.fire_operation_timeout();

    REQUIRE_FALSE(TA::operation_active(*h.panel));
    CHECK(h.mock->restart_calls == 0);
    CHECK(h.mock->get_system_info().action == AmsAction::LOADING);
}

TEST_CASE_METHOD(LVGLUITestFixture, "#1183: AmsAction::ERROR ends the operation and fails the op",
                 "[ui_integration][filament][regression][1183]") {
    // AmsBackendAfc::check_action_timeout()'s entire output is a flip to ERROR.
    // Before the fix the observer accepted only IDLE, so that backstop changed
    // nothing on this panel and the spinner ran to the 120s timeout.
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::operation_active(*h.panel));

    h.publish_action(AmsAction::LOADING, 10);
    h.publish_action(AmsAction::ERROR, 600); // > kMinSpinnerVisibleMs, so a
                                             // mistaken op_succeeded() would have
                                             // reached the done state by now

    CHECK_FALSE(TA::operation_active(*h.panel));
    CHECK(TA::op_load_state(*h.panel) == 0); // idle, NOT 2/done
    CHECK_FALSE(TA::backend_op_active(*h.panel));
    CHECK_FALSE(TA::op_in_flight(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture, "#1183: AmsAction::IDLE still completes the op as a success",
                 "[ui_integration][filament][regression][1183]") {
    // Happy path must not regress into the failure path: success shows the
    // checkmark (state 2), which is what distinguishes it from op_failed().
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::operation_active(*h.panel));

    h.publish_action(AmsAction::LOADING, 10);
    h.publish_action(AmsAction::IDLE, 600); // clears the min-spinner floor

    CHECK_FALSE(TA::operation_active(*h.panel));
    CHECK(TA::op_load_state(*h.panel) == 2); // done/checkmark, not merely non-busy
    CHECK_FALSE(TA::backend_op_active(*h.panel));
    CHECK_FALSE(TA::op_in_flight(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture, "#1183: a timed-out gcode op stops its spinner too",
                 "[ui_integration][filament][regression][1183]") {
    // Extrude is one of the six non-backend begin() callsites (raw gcode, no AMS
    // completion observer involved). It proves the shared handler reached every
    // callsite, not just the two backend ones.
    //
    // The test API is disconnected, so execute_gcode's error callback fires
    // synchronously and queues an op_failed via async_call. Freezing the update
    // queue parks that callback so the ONLY thing that can clear the spinner is
    // the timeout handler — otherwise the assertion would pass on the error path
    // and survive every mutation.
    TimeoutHarness h(*this);

    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze("op-timeout-test");

        TA::execute_extrude(*h.panel);
        REQUIRE(TA::operation_active(*h.panel));
        REQUIRE(TA::op_extrude_state(*h.panel) == 1);

        lv_timer_t* t = TA::operation_timer(*h.panel);
        REQUIRE(t != nullptr);
        lv_timer_set_period(t, 1);
        lv_timer_ready(t);
        // The frozen callback sits in the freeze buffer, which drain() does not
        // touch, so this only runs the timeout timer.
        lv_timer_handler_safe();

        CHECK(TA::op_extrude_state(*h.panel) == 0);
        CHECK_FALSE(TA::operation_active(*h.panel));
    }

    process_lvgl(20); // let the thawed error callback drain before teardown
}

// ============================================================================
// Unknown-command abort
//
// Run with: ./build/bin/helix-tests "[ui_integration][filament][unknown_command]"
//
// Captured verbatim from a live BoxTurtle rig; see test_afc_console_corpus.cpp
// and docs/devel/FILAMENT_MANAGEMENT.md § "AFC console response contract".
//
//     PURGE_FILAMENT
//     // Unknown command:"STATUS_PURGING"
//
// The user's purge_filament macro aborts on line 4 of its own body because their
// LED config has no STATUS_PURGING. Klipper reports that through respond_info —
// a `//` line, NOT `!!` — and Moonraker still returns `ok` for the script, so the
// success callback fires and the button shows a green checkmark for a macro that
// did nothing. It happened four times in the captured window.
//
// Mutation checks (each must break the listed test):
//   - make fail_op_on_unknown_command() ignore op_showing_busy_ -> "with no op in
//     flight is inert" fails (it fires a toast and arms op_aborted_, which then
//     eats the next op's success)
//   - drop the op_aborted_ check in op_succeeded() -> "survives the macro's late
//     ok" fails (state lands on 2/done)
//   - drop the command name from the toast -> "fails the running op" fails
//   - remove the op_aborted_.reset() in op_started() -> "a later operation is
//     not eaten by a stale abort" fails
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "unknown-command response fails the running filament op",
                 "[ui_integration][filament][unknown_command]") {
    TimeoutHarness h(*this);

    std::string toast;
    helix::ui::set_test_notification_error_hook([&](const std::string& m) { toast = m; });

    {
        // The test API is disconnected, so execute_gcode's error callback fires
        // synchronously and queues its own op_failed. Freezing the queue parks
        // that callback, leaving the unknown-command path as the only thing that
        // can clear the spinner.
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze("unknown-command-test");

        TA::execute_extrude(*h.panel);
        REQUIRE(TA::operation_active(*h.panel));
        REQUIRE(TA::op_extrude_state(*h.panel) == 1);

        h.panel->fail_op_on_unknown_command("STATUS_PURGING");

        CHECK(TA::op_extrude_state(*h.panel) == 0); // spinner cleared, no checkmark
        CHECK_FALSE(TA::operation_active(*h.panel));
        CHECK_FALSE(TA::backend_op_active(*h.panel));
        CHECK_FALSE(TA::op_in_flight(*h.panel));
        // The missing command is the only actionable part of the message.
        CHECK(toast.find("STATUS_PURGING") != std::string::npos);
    }

    helix::ui::set_test_notification_error_hook(nullptr);
    process_lvgl(20); // let the thawed error callback drain before teardown
}

TEST_CASE_METHOD(LVGLUITestFixture, "unknown-command abort survives the macro's late ok",
                 "[ui_integration][filament][unknown_command]") {
    TimeoutHarness h(*this);

    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze("unknown-command-late-ok");

        TA::execute_extrude(*h.panel);
        REQUIRE(TA::op_extrude_state(*h.panel) == 1);

        h.panel->fail_op_on_unknown_command("STATUS_PURGING");
        REQUIRE(TA::op_extrude_state(*h.panel) == 0);
    }

    // Drain the thawed gcode-error callback FIRST. It calls op_failed(), which
    // cancels the shared op timer — run it after the late success and it would
    // clear the very checkmark this test is trying to observe, masking a
    // regression in the suppression.
    process_lvgl(20);

    // Moonraker acknowledges the script regardless of the aborted body, so the
    // op's success callback still arrives.
    TA::op_succeeded_extrude(*h.panel);

    // Past kMinSpinnerVisibleMs, so a success that got through would have
    // reached the done state by now.
    process_lvgl(700);
    CHECK(TA::op_extrude_state(*h.panel) == 0); // still idle, never 2/done
}

TEST_CASE_METHOD(LVGLUITestFixture, "template residue warns and leaves the running op alone",
                 "[ui_integration][filament][unknown_command]") {
    // `// Unknown command:"%}"` is a malformed tag in the macro's own template.
    // Klipper skips that line and runs the rest, so the op is still live and no
    // command is missing.
    TimeoutHarness h(*this);

    int errors = 0;
    std::string warning;
    helix::ui::set_test_notification_error_hook([&](const std::string&) { ++errors; });
    helix::ui::set_test_notification_warning_hook([&](const std::string& m) { warning = m; });

    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze("template-residue-test");

        TA::execute_extrude(*h.panel);
        REQUIRE(TA::op_extrude_state(*h.panel) == 1);

        h.panel->fail_op_on_unknown_command("%}");

        CHECK(TA::op_extrude_state(*h.panel) == 1); // still spinning
        CHECK(TA::operation_active(*h.panel));
        CHECK(errors == 0);
        CHECK(warning.find("%}") != std::string::npos);
    }

    helix::ui::set_test_notification_error_hook(nullptr);
    helix::ui::set_test_notification_warning_hook(nullptr);
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "unknown-command response with no op in flight is inert",
                 "[ui_integration][filament][unknown_command]") {
    // An unknown-command line can come from any client on the same printer. With
    // nothing running there is no operation to invalidate, so it must not toast
    // and must not leave the abort marker armed for the next operation.
    TimeoutHarness h(*this);

    int errors = 0;
    helix::ui::set_test_notification_error_hook([&](const std::string&) { ++errors; });
    h.panel->fail_op_on_unknown_command("STATUS_PURGING");
    helix::ui::set_test_notification_error_hook(nullptr);

    CHECK(errors == 0);
    CHECK(TA::op_extrude_state(*h.panel) == 0);
    CHECK(TA::op_load_state(*h.panel) == 0);
    CHECK_FALSE(TA::operation_active(*h.panel));

    // The next real operation must still be able to succeed.
    TA::execute_load(*h.panel);
    REQUIRE(TA::op_load_state(*h.panel) == 1);
    h.publish_action(AmsAction::LOADING, 10);
    h.publish_action(AmsAction::IDLE, 600);
    CHECK(TA::op_load_state(*h.panel) == 2); // done/checkmark
}

TEST_CASE_METHOD(LVGLUITestFixture, "a later operation is not eaten by a stale abort",
                 "[ui_integration][filament][unknown_command]") {
    // The abort marker exists to swallow ONE late success. If the aborted op's
    // RPC never delivers that success — it errored, or the connection dropped —
    // the marker is still armed, and the next operation of the same kind would
    // have its own success swallowed instead: no checkmark, forever.
    TimeoutHarness h(*this);

    TA::execute_load(*h.panel);
    REQUIRE(TA::op_load_state(*h.panel) == 1);
    h.panel->fail_op_on_unknown_command("STATUS_LOADING");
    REQUIRE(TA::op_load_state(*h.panel) == 0);

    // No `ok` for that first Load ever arrives. Start a fresh one and let it
    // complete normally.
    TA::execute_load(*h.panel);
    REQUIRE(TA::op_load_state(*h.panel) == 1);
    h.publish_action(AmsAction::LOADING, 10);
    h.publish_action(AmsAction::IDLE, 600);

    CHECK(TA::op_load_state(*h.panel) == 2); // done/checkmark
}
