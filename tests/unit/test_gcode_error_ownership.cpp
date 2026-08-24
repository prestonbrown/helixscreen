// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_error_ownership.cpp
 * @brief Who owns the user-visible report for a failed printer.gcode.script.
 *
 * `IMoonrakerAPI::execute_gcode`'s trailing `caller_surfaces_errors` answers one
 * question: does the caller's `on_error` actually SHOW a human something? When
 * it claims yes, MoonrakerRequestTracker (and the mock's inline gcode registry,
 * which shares the same helix::rpc_error_policy::decide() call) records the
 * message through rpc_error_correlation::record_caller_handled(), and
 * GcodeErrorRouter then suppresses its own toast for Klipper's matching `!!`
 * broadcast. A callback that only logs must therefore NOT claim it — otherwise
 * a rejected macro is reported by nobody at all.
 *
 * These tests assert on the dedup record itself
 * (rpc_error_correlation::was_recently_handled) rather than a toast, because
 * that record IS the mechanism that mutes the router. A `true` here for a
 * log-only callback is exactly the silent-failure bug.
 *
 * The mock's printer.gcode.script handler runs synchronously inside
 * send_jsonrpc(), so the record (if any) exists by the time the call returns.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_cfs.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "rpc_error_correlation.h"
#include "temperature_controller.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/update_queue_test_access.h"
#include "toolhead_homing.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Clears the process-wide correlation window on both sides of a test so a
/// record left by an earlier case can never be mistaken for this one's.
class OwnershipFixture : public LVGLTestFixture {
  public:
    OwnershipFixture() {
        helix::rpc_error_correlation::clear_for_test();
    }
    ~OwnershipFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        helix::rpc_error_correlation::clear_for_test();
    }
};

constexpr const char* G28_REJECTION = "Must home axis first";

/// ensure_homed_then() reads the process-wide PrinterState via
/// get_printer_state() (it takes no state parameter), so the unhomed answer has
/// to be driven there — a local PrinterState owns unrelated subject storage.
helix::PrinterState& unhomed_global_state() {
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    lv_subject_copy_string(state.get_homed_axes_subject(), "");
    return state;
}

} // namespace

// ============================================================================
// helix::ensure_homed_then() — the highest-leverage site. Every Happy Hare /
// QIDI / AFC / toolchanger / AD5X pre-operation G28 routes through here, and
// its on_error wrapper (which logs, then forwards) is non-null even when the
// caller passed nullptr.
// ============================================================================

TEST_CASE_METHOD(OwnershipFixture,
                 "ensure_homed_then with no caller on_error leaves the `!!` router free",
                 "[error-center][gcode-ownership][homing]") {
    helix::PrinterState& state = unhomed_global_state();
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, G28_REJECTION, "G28");

    helix::AsyncLifetimeGuard guard;
    bool then_ran = false;
    helix::ensure_homed_then(&api, guard, [&then_ran]() { then_ran = true; }, nullptr);

    // The G28 was actually attempted and actually failed — otherwise the
    // assertions below would pass vacuously.
    REQUIRE(client.last_send_method() == "printer.gcode.script");
    REQUIRE(client.last_send_script() == "G28");
    CHECK_FALSE(then_ran);

    // The caller promised nothing, so the internal log-and-forward wrapper must
    // not claim the report on its behalf.
    CHECK_FALSE(client.current_send_intent().surfaces_errors);
    CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(G28_REJECTION));
}

TEST_CASE_METHOD(OwnershipFixture,
                 "ensure_homed_then with a caller on_error records dedup for the `!!` copy",
                 "[error-center][gcode-ownership][homing]") {
    helix::PrinterState& state = unhomed_global_state();
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);

    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, G28_REJECTION, "G28");

    helix::AsyncLifetimeGuard guard;
    std::string seen;
    helix::ensure_homed_then(&api, guard, nullptr,
                             [&seen](const MoonrakerError& e) { seen = e.message; });

    REQUIRE(client.last_send_script() == "G28");

    // A real caller-supplied handler is presumed to raise its own UI, so the
    // `!!` broadcast of the same rejection must dedup against it rather than
    // double-report. This is the other half of the contract: the fix must not
    // simply hardcode false.
    CHECK(client.current_send_intent().surfaces_errors);
    CHECK(helix::rpc_error_correlation::was_recently_handled(G28_REJECTION));

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(seen == G28_REJECTION);
}

// ============================================================================
// AMS: CFS's action script. Its on_error is non-null but only logs, unwinds
// phase tracking and fires best-effort cleanup gcode — so it must declare
// caller_surfaces_errors=false explicitly rather than inherit the derivation.
// ============================================================================

TEST_CASE_METHOD(OwnershipFixture,
                 "CFS action script does not claim the report for a rejected payload",
                 "[error-center][gcode-ownership][ams][cfs]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::printer::AmsBackendCfs backend(&api, nullptr);

    constexpr const char* PAYLOAD_REJECTION = "Extrude below minimum temp";
    // Scoped to the payload, so the G28 that precedes it (homed_axes is "")
    // still succeeds and the failure lands on the macro itself.
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, PAYLOAD_REJECTION,
                                  "BOX_LOAD");

    auto err = CfsTestAccess::call_dispatch_action_script(backend, "BOX_LOAD LANE=1");
    REQUIRE(err.success());

    // G28's ack is marshalled through token.defer(); drain so the payload send
    // (the one that fails) actually happens.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // last_send_* is NOT usable here: the error handler's best-effort unwind
    // (RESTORE_GCODE_STATE) is dispatched from the same drain and overwrites it.
    // The history proves the payload really was sent and really was rejected.
    const auto& history = client.gcode_script_history();
    REQUIRE(std::find(history.begin(), history.end(), "BOX_LOAD LANE=1") != history.end());

    CHECK_FALSE(helix::rpc_error_correlation::was_recently_handled(PAYLOAD_REJECTION));
}

// ============================================================================
// TemperatureController: opts.toast is the flag that decides whether a human
// sees anything, because the NOTIFY_ERROR in its on_err is gated on it.
// ============================================================================

TEST_CASE_METHOD(OwnershipFixture, "TemperatureController ownership follows opts.toast",
                 "[error-center][gcode-ownership][temperature]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);
    helix::TemperatureController controller(state, &api);

    SECTION("toast=false sends a target nobody will report on") {
        helix::SendOptions opts;
        opts.toast = false;
        controller.set_target("extruder", 200.0, opts);

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        CHECK_FALSE(client.current_send_intent().surfaces_errors);
    }

    SECTION("toast=true keeps the caller's own error toast as the report") {
        helix::SendOptions opts;
        opts.toast = true;
        controller.set_target("extruder", 200.0, opts);

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        CHECK(client.current_send_intent().surfaces_errors);
    }
}
