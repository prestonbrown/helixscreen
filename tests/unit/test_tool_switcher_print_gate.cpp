// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_print_gate.cpp
 * @brief The home-panel tool switcher answers to the same print gate as every
 *        other filament surface, and never refuses in silence.
 *
 * Two defects are pinned here.
 *
 * 1. handle_tool_selected() showed a "Tool Change During Print" confirmation
 *    while PRINTING and dispatched on confirm — but every AmsSubscriptionBackend
 *    refuses PRINTING unconditionally. The user was warned, consented, tapped
 *    Change Tool, and the pill did not move. The UI must not offer what the
 *    backend will refuse; the gate is helix::ui::print_blocks_filament_op(), the
 *    same predicate the filament panel, the AMS sidebar and the AMS context menu
 *    already use, NOT a tool-change policy of its own.
 *
 * 2. Both request_tool_change() call sites passed no on_error, so a refusal that
 *    DID reach the backend vanished: no toast, no log on that branch. The only
 *    trace was the backend's own warning inside refuse_if_printing().
 *
 * PAUSED stays permitted on every backend that does not self-home — that is the
 * runout / colour-change recovery workflow, and greying it would make the
 * backend relaxation invisible (see test_ams_paused_filament_ops.cpp).
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Backend double: the tool map is what makes ToolState delegate to the backend
/// instead of the Tn gcode fallback, and change_tool() answers straight from a
/// flag so the refusal has somewhere to travel FROM. self_homes is settable
/// because it is the input that decides whether PAUSED is allowed.
///
/// change_tool() deliberately does NOT chain to AmsBackendMock: the mock runs
/// the change on its own thread and emits slot events, whose deferred AmsState
/// writes can outlive this fixture's teardown and land in a LATER test's
/// async-lifetime skip window (test_filament_dispatch_surfaces.cpp counts those
/// exactly). Nothing here asserts simulated motion, only the dispatch result.
class GateBackend : public AmsBackendMock {
  public:
    explicit GateBackend(int slots) : AmsBackendMock(slots) {}

    bool self_homes = false;
    bool tool_change_should_fail = false;
    int change_tool_calls = 0;

    bool filament_ops_self_home() const override {
        return self_homes;
    }

    AmsError change_tool(int /*tool_number*/) override {
        ++change_tool_calls;
        if (tool_change_should_fail) {
            return AmsError(AmsResult::TOOL_CHANGE_FAILED, "forced failure",
                            "Tool change did not complete", "Check the filament path and retry");
        }
        return AmsErrorHelper::success();
    }
};

struct ToolSwitcherGateFixture : public LVGLTestFixture {
    ToolSwitcherGateFixture() {
        printer_state.init_subjects(false);

        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);
        helix::PrinterDiscovery hw;
        nlohmann::json objects =
            nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "extruder",
                                   "extruder1", "extruder2", "heater_bed", "gcode_move"});
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);
        REQUIRE(ToolState::instance().tool_count() == 3);

        auto backend_owned = std::make_unique<GateBackend>(3);
        backend_owned->set_tool_changer_mode(true);
        backend_owned->set_operation_delay(0);
        REQUIRE(backend_owned->start());
        backend = backend_owned.get();
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(false);
        AmsState::instance().set_backend(std::move(backend_owned));

        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
    }

    ~ToolSwitcherGateFixture() override {
        helix::ui::set_test_notification_warning_hook(nullptr);
        helix::ui::set_test_notification_error_hook(nullptr);
        if (backend) {
            backend->wait_for_operation_thread();
            backend->stop();
        }
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(printer_state, s);
    }

    /// A host-side pre-print block: the wire still reads standby.
    void set_preprint_phase(helix::PrintStartPhase phase) {
        printer_state.set_print_start_state(phase, "", 0);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    helix::PrinterState printer_state;
    GateBackend* backend = nullptr;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

} // namespace

// ============================================================================
// Defect 2: the refusal has to reach the user
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture,
                 "tool switcher surfaces a backend tool-change failure instead of swallowing it",
                 "[ams][tool-switcher][tool-change]") {
    // STANDBY: the print gate lets this through, so the only thing that can go
    // wrong is the backend itself — which is exactly the branch that had no
    // on_error and therefore produced no toast and no log.
    set_print_state(helix::PrintJobState::STANDBY);
    backend->tool_change_should_fail = true;

    ToolSwitcherWidget widget(printer_state);
    ToolSwitcherTestAccess::select_tool(widget, 1);

    CHECK(backend->change_tool_calls == 1);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Tool change did not complete") != std::string::npos);
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "a successful tool change raises no error toast",
                 "[ams][tool-switcher][tool-change]") {
    set_print_state(helix::PrintJobState::STANDBY);

    ToolSwitcherWidget widget(printer_state);
    ToolSwitcherTestAccess::select_tool(widget, 1);

    CHECK(backend->change_tool_calls == 1);
    CHECK(errors.empty());
    CHECK(warnings.empty());
}

// ============================================================================
// Defect 1: PRINTING is refused up front, not offered behind a modal
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PRINTING refuses the tool change and says why",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PRINTING);

    ToolSwitcherWidget widget(printer_state);

    const AmsError refusal = ToolSwitcherTestAccess::refusal(widget);
    REQUIRE_FALSE(refusal.success());
    CHECK(refusal.result == AmsResult::WRONG_STATE);
    CHECK(refusal.user_msg == "Cannot run filament operation while printing");
    // This backend permits ops on a paused job, so pausing is the recovery to
    // name — not "finish or cancel", which throws away a savable print.
    CHECK(refusal.suggestion.find("Pause the print") != std::string::npos);

    ToolSwitcherTestAccess::select_tool(widget, 1);

    // Nothing dispatched: the previous code opened a confirmation modal and
    // dispatched on confirm, into a backend that always refuses.
    CHECK(backend->change_tool_calls == 0);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("Cannot run filament operation while printing") != std::string::npos);
}

// ============================================================================
// PAUSED: allowed, except on a backend whose filament macros home themselves
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture,
                 "PAUSED still permits the tool change when the backend never homes",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);
    backend->self_homes = false;

    ToolSwitcherWidget widget(printer_state);
    CHECK(ToolSwitcherTestAccess::refusal(widget).success());
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PREPARING refuses the tool change on every backend",
                 "[ams][tool-switcher][safety][preparing]") {
    // A host-side pre-start block owns the toolhead and print_stats cannot say
    // so. The backend's self-homing capability is irrelevant here: the app's own
    // block is already moving the toolhead.
    set_print_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);

    for (bool self_homes : {false, true}) {
        backend->self_homes = self_homes;
        ToolSwitcherWidget widget(printer_state);
        CHECK_FALSE(ToolSwitcherTestAccess::refusal(widget).success());
    }
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PAUSED refuses on a self-homing backend (AD5X IFS)",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);
    backend->self_homes = true;

    ToolSwitcherWidget widget(printer_state);

    const AmsError refusal = ToolSwitcherTestAccess::refusal(widget);
    REQUIRE_FALSE(refusal.success());
    CHECK(refusal.user_msg == "Can't move filament while the print is paused");
    // "while printing" on a paused job reads as a bug (bundle JX2FVRB9).
    CHECK(refusal.user_msg.find("printing") == std::string::npos);

    ToolSwitcherTestAccess::select_tool(widget, 1);
    CHECK(backend->change_tool_calls == 0);
    REQUIRE(warnings.size() == 1);
}

// ============================================================================
// The greying has to track the print state LIVE
//
// PanelWidget instances are recycled across home-panel rebuilds, so the gate is
// applied from attach() (via the print-state observer) and from both rebuild
// paths. Registering the observer only in on_size_changed() would leave a reused
// instance correct on first build and stale forever after.
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "pills grey and un-grey as the print state changes",
                 "[ams][tool-switcher][safety]") {
    lv_obj_t* host = lv_obj_create(lv_screen_active());
    ToolSwitcherWidget widget(printer_state);
    widget.attach(host, lv_screen_active());

    auto& pills = ToolSwitcherTestAccess::pills(widget);
    pills.push_back(lv_obj_create(host));
    pills.push_back(lv_obj_create(host));

    // observe_int_sync() defers through the UpdateQueue, so the gate lands on
    // the next tick — drain before asserting.
    auto tick = [] {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    };

    set_print_state(helix::PrintJobState::PRINTING);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    // PAUSED on a backend that does not self-home is the recovery workflow —
    // greying it there would hide the backend relaxation entirely.
    set_print_state(helix::PrintJobState::PAUSED);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK_FALSE(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    set_print_state(helix::PrintJobState::STANDBY);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK_FALSE(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    widget.detach();
    lv_obj_delete(host);
}

// ============================================================================
// States that were never gated stay ungated
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "inactive print states never block a tool change",
                 "[ams][tool-switcher][safety][paused]") {
    ToolSwitcherWidget widget(printer_state);

    for (helix::PrintJobState s : {helix::PrintJobState::STANDBY, helix::PrintJobState::COMPLETE,
                                   helix::PrintJobState::CANCELLED, helix::PrintJobState::ERROR}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(ToolSwitcherTestAccess::refusal(widget).success());
    }
}
