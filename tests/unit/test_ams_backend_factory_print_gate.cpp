// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_backend_factory_print_gate.cpp
 * @brief The print-active gate, asserted over everything AmsBackend::create() builds.
 *
 * 329e731e9 added check_preconditions(true) to seven backends and missed the
 * eighth (Snapmaker), which shipped with no print-active gate at all. The gate
 * is opt-in — hand-written at ~24 call sites — so "one backend forgot" is a
 * failure mode the type system does not catch.
 *
 * Neither did the tests. test_ams_paused_filament_ops.cpp names AD5X, AFC,
 * Happy Hare, QIDI and Snapmaker; ACE, CFS and Tool Changer call
 * check_preconditions(true) with nothing asserting that they do. A hand-written
 * coverage list has exactly the omission mode the bug had, one level up.
 *
 * So this file names no backends. It walks the AmsType enum's numeric range and
 * tests whatever the factory hands back. Adding an AmsType and a case in
 * AmsBackend::create() puts the new backend under these assertions with no edit
 * here; forgetting its guard turns this file red.
 *
 * What is pinned:
 *
 *   1. PRINTING refuses load/unload/change_tool on EVERY backend, with the
 *      print-active refusal specifically (not "invalid slot", not
 *      "not supported"), and with nothing reaching the wire.
 *   2. PAUSED follows that backend's own filament_ops_self_home() — refuse when
 *      it is true (AD5X's buried _G28, bundle XWPBR2DX), allow when it is false
 *      (pause-then-swap is the runout recovery workflow).
 *   3. select_slot is gated exactly when it IS a tool change. That classification
 *      is DERIVED, by comparing what select_slot and change_tool actually do on
 *      an idle printer — not declared in a list here. On a Snapmaker U1
 *      select_slot delegates to change_tool and moves the carriage; on Happy
 *      Hare it emits MMU_SELECT and moves no toolhead.
 *
 * Asserting the refusal is the specific print-active error (rather than merely
 * "some failure") is what gives this teeth: a backend with its guard deleted
 * still fails early on an unconfigured slot, so `!err.success()` alone would
 * stay green through the exact regression this file exists to catch.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend.h"
#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// AmsType is a plain 0..N enum with no sentinel. Probing a range well past the
/// last value is what makes discovery automatic: create() returns nullptr for
/// anything it does not know, so an added type is picked up the moment the
/// factory learns to build it, and no list in this file needs touching.
constexpr int AMS_TYPE_PROBE_LIMIT = 64;

/// Backends that are compiled unconditionally. AD5X IFS and CFS are feature
/// gated (HELIX_HAS_IFS / HELIX_HAS_CFS are 0 on the space-constrained cross
/// builds), so they are not part of the floor. This is a lower bound that
/// catches "create() silently stopped producing anything" — it is NOT the
/// coverage list; the loops below cover whatever exists.
constexpr int UNCONDITIONAL_BACKEND_COUNT = 6;

/// True when @p e is the Layer 2 print-active refusal specifically, as opposed
/// to any other failure a backend might return first. Derived from the error
/// factory rather than hardcoding copy, so reworded messages do not silently
/// turn these assertions into tautologies.
bool is_print_refusal(const AmsError& e) {
    if (e.success() || e.result != AmsResult::WRONG_STATE) {
        return false;
    }
    static const std::string printing_msg =
        AmsErrorHelper::print_active(/*is_paused=*/false).user_msg;
    static const std::string paused_msg = AmsErrorHelper::print_active(/*is_paused=*/true).user_msg;
    return e.user_msg == printing_msg || e.user_msg == paused_msg;
}

struct Op {
    const char* name;
    std::function<AmsError(AmsBackend&)> invoke;
};

/// The three ops that are toolhead motion on every backend by definition:
/// pushing or pulling filament through the hotend, and swapping what is on the
/// carriage. select_slot is deliberately absent — it is motion-free on most
/// backends and a physical tool change on others, and gets its own derived
/// classification below.
const std::vector<Op>& motion_ops() {
    static const std::vector<Op> ops = {
        {"load_filament", [](AmsBackend& b) { return b.load_filament(0); }},
        {"unload_filament", [](AmsBackend& b) { return b.unload_filament(0); }},
        {"change_tool", [](AmsBackend& b) { return b.change_tool(0); }},
    };
    return ops;
}

/// Everything observable about one op invocation: what it returned and what
/// reached the wire. Equality across two ops is how select_slot's "this really
/// is a tool change" classification is derived without naming backends.
struct Outcome {
    AmsError err;
    std::vector<std::string> gcode;

    bool operator==(const Outcome& o) const {
        return err.result == o.err.result && err.user_msg == o.err.user_msg &&
               err.technical_msg == o.err.technical_msg && gcode == o.gcode;
    }
};

struct FactoryGateFixture : public LVGLTestFixture {
    FactoryGateFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }

    /// Build one backend through the production factory and get it to the state
    /// the gate is measured in: running and idle. Returns nullptr only for types
    /// this build excludes.
    ///
    /// Every other way a backend could fail to reach that state is a REQUIRE,
    /// not a skip. That matters: a backend stuck at running_ == false refuses
    /// every op with not_connected, which would let a missing print gate sail
    /// through as "well, it refused".
    std::unique_ptr<AmsBackend> build(AmsType type) {
        auto backend = AmsBackend::create(type, api.get(), &mock_client);
        if (!backend) {
            return nullptr;
        }
        // A mock substituted by try_create_mock() would make every assertion in
        // this file vacuous — it is one class answering for eight.
        REQUIRE(backend->get_type() == type);

        // Tool Changer refuses to start until tools are discovered. A future
        // backend with its own start precondition trips the is_running()
        // REQUIRE below rather than quietly testing nothing.
        if (auto* tc = dynamic_cast<AmsBackendToolChanger*>(backend.get())) {
            tc->set_discovered_tools({"tool0", "tool1"});
        }

        REQUIRE(backend->start().success());
        REQUIRE(backend->is_running());
        // check_preconditions() refuses a busy AMS before it ever looks at print
        // state, so a backend that came up busy would mask the gate under test.
        REQUIRE(backend->get_current_action() == AmsAction::IDLE);

        mock_client.clear_gcode_script_history();
        return backend;
    }

    /// Run @p op on a freshly built backend and record everything observable.
    /// Fresh each time so one op's side effects (CFS stamps action=LOADING on
    /// dispatch) cannot change what the next one is allowed to do.
    Outcome run(AmsType type, const std::function<AmsError(AmsBackend&)>& op) {
        auto backend = build(type);
        REQUIRE(backend != nullptr);
        mock_client.clear_gcode_script_history();

        Outcome out;
        out.err = op(*backend);
        // Several backends reach the wire through ensure_homed_then() /
        // dispatch_action_script(), which hop to the main thread via
        // UpdateQueue. Without draining, "nothing was dispatched" would be true
        // of a leaked op merely because its send had not run yet.
        helix::ui::UpdateQueue::instance().drain();
        out.gcode = mock_client.gcode_script_history();
        return out;
    }

    /// Every AmsType this build can actually produce.
    std::vector<AmsType> buildable_types() {
        std::vector<AmsType> types;
        for (int raw = 1; raw < AMS_TYPE_PROBE_LIMIT; ++raw) {
            const auto type = static_cast<AmsType>(raw);
            if (AmsBackend::create(type, api.get(), &mock_client)) {
                types.push_back(type);
            }
        }
        return types;
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

// ============================================================================
// The factory really does hand back one backend per type
//
// Everything below is a loop over what create() produces. If that ever returns
// nothing — a build flag, a refactor, a mock substituted at the top of create()
// — the loops would pass by iterating zero times. This is the assertion that
// makes such a collapse loud instead of green.
// ============================================================================

TEST_CASE_METHOD(FactoryGateFixture, "AmsBackend::create builds a distinct backend per AmsType",
                 "[ams][safety][factory]") {
    const auto types = buildable_types();
    CAPTURE(types.size());
    CHECK(types.size() >= UNCONDITIONAL_BACKEND_COUNT);

    for (AmsType type : types) {
        CAPTURE(ams_type_to_string(type));
        auto backend = AmsBackend::create(type, api.get(), &mock_client);
        REQUIRE(backend != nullptr);
        CHECK(backend->get_type() == type);
    }
}

// ============================================================================
// PRINTING refuses toolhead-motion ops, everywhere, with nothing dispatched
// ============================================================================

TEST_CASE_METHOD(FactoryGateFixture,
                 "every backend the factory builds refuses toolhead-motion ops while PRINTING",
                 "[ams][safety][factory]") {
    set_print_state(helix::PrintJobState::PRINTING);

    int checked = 0;
    for (AmsType type : buildable_types()) {
        for (const Op& op : motion_ops()) {
            CAPTURE(ams_type_to_string(type), op.name);

            const Outcome out = run(type, op.invoke);

            // The specific refusal, not merely "it failed". A backend whose
            // guard was deleted still trips over an unconfigured slot, so
            // anything looser stays green through the regression.
            CAPTURE(out.err.user_msg, out.err.technical_msg);
            CHECK(is_print_refusal(out.err));
            // A guard placed after the command is built returns the refusal AND
            // still sends the G-code.
            CHECK(out.gcode.empty());
            ++checked;
        }
    }
    CHECK(checked >= UNCONDITIONAL_BACKEND_COUNT * static_cast<int>(motion_ops().size()));
}

// ============================================================================
// PAUSED splits on the backend's own filament_ops_self_home()
//
// Pause-then-swap is the runout / colour-change recovery workflow, so a backend
// that never homes must let it through. A backend whose firmware macro homes
// itself must not: AD5X's _IFS_REMOVE_CURRENT_PRUTOK runs a buried _G28 that
// probes the nozzle into the part (bundle XWPBR2DX), and Layer 1 cannot see it.
// ============================================================================

TEST_CASE_METHOD(FactoryGateFixture,
                 "PAUSED behaviour follows each backend's filament_ops_self_home()",
                 "[ams][safety][factory]") {
    set_print_state(helix::PrintJobState::PAUSED);

    for (AmsType type : buildable_types()) {
        auto probe = build(type);
        REQUIRE(probe != nullptr);
        const bool self_homes = probe->filament_ops_self_home();
        probe.reset();

        for (const Op& op : motion_ops()) {
            CAPTURE(ams_type_to_string(type), op.name, self_homes);

            const Outcome out = run(type, op.invoke);
            CAPTURE(out.err.user_msg, out.err.technical_msg);

            if (self_homes) {
                CHECK(is_print_refusal(out.err));
                CHECK(out.gcode.empty());
            } else {
                // It may still fail for an unrelated reason (no slots
                // configured in this fixture) — what it must not do is refuse
                // BECAUSE a print is paused.
                CHECK_FALSE(is_print_refusal(out.err));
            }
        }
    }
}

// ============================================================================
// select_slot: gated exactly when it IS a tool change
//
// This is the per-METHOD classification that eight backends currently re-decide
// for themselves. It is derived here, not listed: run select_slot and
// change_tool against an idle printer and compare everything observable. When
// they are indistinguishable, select_slot IS the tool change (Snapmaker U1,
// Tool Changer, ACE all delegate) and must refuse mid-print. When they differ,
// select_slot is a genuine motion-free select (Happy Hare's MMU_SELECT, AD5X's
// SET_EXTRUDER_SLOT) and must stay allowed — blocking it would strand filament
// the user could have moved.
// ============================================================================

TEST_CASE_METHOD(FactoryGateFixture, "select_slot is print-gated exactly when it is a tool change",
                 "[ams][safety][factory]") {
    auto select_slot = [](AmsBackend& b) { return b.select_slot(0); };
    auto change_tool = [](AmsBackend& b) { return b.change_tool(0); };

    for (AmsType type : buildable_types()) {
        CAPTURE(ams_type_to_string(type));

        // Classify on an idle printer, where no gate is in the way.
        set_print_state(helix::PrintJobState::STANDBY);
        const Outcome idle_select = run(type, select_slot);
        const Outcome idle_change = run(type, change_tool);
        const bool select_is_tool_change = (idle_select == idle_change);
        CAPTURE(select_is_tool_change, idle_select.err.user_msg, idle_change.err.user_msg);

        set_print_state(helix::PrintJobState::PRINTING);
        const Outcome printing = run(type, select_slot);
        CAPTURE(printing.err.user_msg, printing.err.technical_msg);

        if (select_is_tool_change) {
            CHECK(is_print_refusal(printing.err));
            CHECK(printing.gcode.empty());
        } else {
            CHECK_FALSE(is_print_refusal(printing.err));
        }
    }
}
