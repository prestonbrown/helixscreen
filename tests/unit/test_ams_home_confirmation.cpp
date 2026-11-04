// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_modal.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_toolchanger.h"
#include "core/update_queue.h"
#include "filament_op_execute.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/filament_panel_macro_harness.h"
#include "test_helpers/filament_panel_test_access.h"
#include "test_helpers/lane_material_backend.h"
#include "test_helpers/load_filament_expression_default.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/toolchanger_test_access.h"
#include "test_helpers/toolchanger_test_helper.h"

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
/// helix::SharedCallback<void(const MoonrakerError&)> callback invoked directly by
/// the override -- the override has no way to receive ensure_homed_then's
/// on_error as a parameter (the 1-arg/2-arg execute_gcode virtuals predate
/// on_error and can't grow it without breaking ~20 other fixtures), so the
/// return-value channel is what's actually reachable here.
class HomingProbeBackend : public helix::AmsBackendAfc {
  public:
    HomingProbeBackend() : helix::AmsBackendAfc(nullptr, nullptr) {}

    helix::AmsError execute_gcode(const std::string& gcode) override {
        if (fail_next_gcode) {
            return helix::AmsError(helix::AmsResult::COMMAND_FAILED, "boom", "boom");
        }
        captured.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        if (fail_next_gcode) {
            return helix::AmsError(helix::AmsResult::COMMAND_FAILED, "boom", "boom");
        }
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
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

    backend.ensure_homed_then("BOX_LOAD", nullptr, nullptr, MoonrakerAPI::kAmsOperationTimeoutMs,
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
// A tool changer mount on an unhomed printer
// =====================================================================
// dispatch_operation() stamps SELECTING before it calls ensure_homed_then(),
// so any gap between the two leaves the backend busy with nothing on the way.
// With no confirmation to wait on, the G28 and the swap go out in the same
// call and the operation is never parked.
class ToolChangerHomingProbeBackend : public helix::AmsBackendToolChanger {
  public:
    ToolChangerHomingProbeBackend() : helix::AmsBackendToolChanger(nullptr, nullptr) {}

    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }
    // A dispatch that unwinds on a rejection sends through this form.
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        return execute_gcode(gcode, std::move(on_complete));
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;
    std::vector<std::string> captured;
};

TEST_CASE("an unhomed tool changer mount homes and sends the swap in the same call",
          "[ams][homing][toolchanger]") {
    LVGLTestFixture fixture;
    ToolChangerHomingProbeBackend backend;
    backend.homed = false;

    auto err = helix::ToolChangerTestAccess::call_dispatch_operation(backend, "SELECT_TOOL T=1",
                                                                     helix::AmsAction::SELECTING);

    REQUIRE(err.success());
    REQUIRE(backend.captured.size() == 2);
    CHECK(backend.captured[0] == "G28");
    CHECK(backend.captured[1] == "SELECT_TOOL T=1");
    // The macro ack resolves the hold through the main-thread queue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(helix::ToolChangerTestAccess::has_pending_dispatch(backend));
    CHECK(backend.get_system_info().action == helix::AmsAction::IDLE);
}

namespace {
class UnhomedToolChanger : public helix::test::ToolChangerHelper {
  public:
    using ToolChangerHelper::ToolChangerHelper;
    bool toolhead_homed() const override {
        return false;
    }
};
} // namespace

// Bed drying's unload and the filament surfaces all route through
// execute_filament_unload(). On a tool changer the unload is by hand, so even an
// unhomed one neither homes nor sends: nothing would move.
TEST_CASE("an unload through execute_filament_unload on a tool changer homes nothing and sends "
          "nothing",
          "[ams][homing][filament]") {
    LVGLTestFixture fixture;
    UnhomedToolChanger tc(4);
    tc.feed_status("ready", 1);

    helix::ui::execute_filament_unload(&tc, 1, /*target_is_loaded=*/true, "[HomingTest]");

    CHECK(ModalStack::instance().empty());
    CHECK(tc.sent().empty());
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
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    helix::AmsBackendAfc backend(&api, &client);

    constexpr uint32_t kCustomTimeoutMs = 12345;
    REQUIRE(kCustomTimeoutMs != MoonrakerAPI::kAmsOperationTimeoutMs);
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
        kCustomTimeoutMs, /*skip_homing=*/true, /*silent=*/false);
    REQUIRE(err.success());

    // Dispatch went straight to MoonrakerAPI::execute_gcode() carrying OUR
    // timeout_ms/silent -- the hardcoded 1-arg/2-arg execute_gcode virtuals fix
    // kAmsOperationTimeoutMs/true and can never produce these values.
    CHECK(client.last_send_method() == "printer.gcode.script");
    CHECK(client.last_send_script() == "BOX_LOAD");
    CHECK(client.last_send_timeout_ms() == kCustomTimeoutMs);
    CHECK_FALSE(client.last_send_silent());

    // The error callback is marshalled through token.defer() (L081 Mechanism
    // C) rather than invoked inline -- drain the queue to run it.
    helix::ui::UpdateQueue::instance().drain_until_empty();
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
// the REAL production implementation through the helix::CfsTestAccess friend shim
// (tests/test_helpers/cfs_test_access.h), not by subclassing to widen access.

TEST_CASE("CFS dispatch_action_script routes through ensure_homed_then and homes when unhomed",
          "[ams][homing][cfs]") {
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    // homed_axes defaults to "" (not homed) -- exercises the G28-then-payload leg.
    helix::printer::AmsBackendCfs backend(&api, nullptr);

    auto err = helix::CfsTestAccess::call_dispatch_action_script(backend, "BOX_LOAD LANE=1");
    REQUIRE(err.success());

    // G28's success callback is marshalled through token.defer() (L081
    // Mechanism C) before the payload is dispatched -- drain to let it run.
    helix::ui::UpdateQueue::instance().drain_until_empty();

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
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    helix::printer::AmsBackendCfs backend(&api, nullptr);
    helix::CfsTestAccess::set_macro_variant_fork(backend);

    // homed_axes is STILL "" (not homed) here -- proves the skip comes from
    // skip_homing=true (Fork maps to it), not from the toolhead happening to
    // already be homed.
    auto err = helix::CfsTestAccess::call_dispatch_action_script(backend, "BOX_LOAD LANE=1");
    REQUIRE(err.success());

    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(client.gcode_script_history().size() == 1);
    CHECK(client.gcode_script_history()[0] == "BOX_LOAD LANE=1");
    CHECK_FALSE(client.last_send_silent());
}

// =====================================================================
// FilamentPanel: an unhomed macro-tier load homes only when its profile says so
// =====================================================================

namespace {

/// A FilamentPanel whose load reaches the configured-macro tier on an unhomed
/// printer with a hot nozzle.
struct UnhomedMacroLoad {
    UnhomedMacroLoad() {
        // helix::ensure_homed_then() reads the process-wide printer state; the
        // panel reads its own.
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        get_printer_state().update_from_status({{"toolhead", {{"homed_axes", ""}}}});
        h.state.update_from_status({{"toolhead", {{"homed_axes", ""}}},
                                    {"extruder", {{"temperature", 240.0}, {"target", 240.0}}}});
        h.cache_macros({{"LOAD_FILAMENT", helix::test::kLoadFilamentExpressionDefault}});
    }

    void press_load() {
        helix::ui::FilamentPanelTestAccess::handle_load_button(*h.panel);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void execute_load_needing_home() {
        helix::ui::FilamentPanelTestAccess::set_home_before_load_macro(*h.panel, true);
        helix::ui::FilamentPanelTestAccess::execute_load(*h.panel);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// How many sent scripts start with @p word.
    [[nodiscard]] long count_sent(const std::string& word) const {
        long n = 0;
        for (const auto& line : h.client.gcode_script_history()) {
            if (line == word || line.rfind(word + " ", 0) == 0) {
                ++n;
            }
        }
        return n;
    }

    /// Position of the first sent script starting with @p word, or -1.
    [[nodiscard]] long first_sent(const std::string& word) const {
        const auto& sent = h.client.gcode_script_history();
        for (size_t i = 0; i < sent.size(); ++i) {
            if (sent[i] == word || sent[i].rfind(word + " ", 0) == 0) {
                return static_cast<long>(i);
            }
        }
        return -1;
    }

    helix::test::FilamentPanelMacroHarness h;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "An unhomed load through an unrecognized macro sends the macro with no G28",
                 "[filament][homing]") {
    UnhomedMacroLoad t;

    t.press_load();

    CHECK(t.first_sent("LOAD_FILAMENT") >= 0);
    CHECK(t.first_sent("G28") < 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A macro-tier load that needs a home sends G28, then the macro, with no prompt",
                 "[filament][homing]") {
    UnhomedMacroLoad t;

    t.execute_load_needing_home();

    const long home = t.first_sent("G28");
    const long load = t.first_sent("LOAD_FILAMENT");
    REQUIRE(home >= 0);
    REQUIRE(load >= 0);
    CHECK(home < load);
    CHECK(t.count_sent("LOAD_FILAMENT") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A home that fails before a macro-tier load sends no macro",
                 "[filament][homing]") {
    UnhomedMacroLoad t;
    t.h.client.force_next_gcode_error(MoonrakerErrorType::UNKNOWN, "Homing failed", "G28");

    t.execute_load_needing_home();

    CHECK(t.first_sent("G28") >= 0);
    CHECK(t.first_sent("LOAD_FILAMENT") < 0);
    // The op's own failure path ran: nothing is left waiting on the guard.
    CHECK(helix::ui::FilamentPanelTestAccess::operation_timer(*t.h.panel) == nullptr);
}
