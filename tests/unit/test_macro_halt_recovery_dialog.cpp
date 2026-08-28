// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What execute_macro_gcode() DOES with the classifier's answer, as opposed to
// what the classifier returns. test_macro_restart_analysis.cpp pins the
// verdict; this pins the consequence, and the consequence is the part a user
// feels.
//
// Both ExpectedRestart and ExpectedHalt absorb the dropped rpc - the macro did
// what it said, so "<name> failed" is a lie either way. They part company on
// begin_expected_klippy_restart(): it arms a 15s recovery-dialog suppression
// window plus a disconnect-modal suppression, which is right for a host that
// is coming back and wrong for one that is not. A halted printer needs the
// recovery dialog promptly, and it needs no toast of its own - the dialog is a
// full-screen modal that says more than a 3s INFO line could.
//
// Mock stack follows test_begin_expected_restart.cpp: MoonrakerClientMock +
// MoonrakerAPIMock over a stack PrinterState, with the api published through
// set_moonraker_api() because begin_expected_klippy_restart() reaches it that
// way. The dropped rpc is injected with force_next_gcode_error() rather than
// relying on a mock branch, so the macro name stays arbitrary and the error
// shape is exactly Moonraker's.

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "macro_executor.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// A macro name that no branch of MoonrakerClientMock::gcode_script() matches,
/// so the only thing that decides the rpc's fate is the forced error below.
constexpr const char* HALTING_MACRO = "PANIC_WRAPPER";
constexpr const char* RESTARTING_MACRO = "SAVE_WRAPPER";

class MacroFailureFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};
    helix::PrinterDiscovery hw;

    struct Shown {
        ToastSeverity severity;
        std::string message;
    };
    std::vector<Shown> toasts;

    MacroFailureFixture() {
        state.init_subjects(false);
        set_moonraker_api(&api);
        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& m) {
            toasts.push_back({severity, m});
        });
        // The overlay is a process-wide singleton and Catch2 runs the suite in
        // one process, so an inherited window would make the halt assertion
        // pass for the wrong reason.
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
        hw.set_host_halting_macros({HALTING_MACRO});
        hw.set_host_restarting_macros({RESTARTING_MACRO});
    }

    ~MacroFailureFixture() override {
        set_moonraker_api(nullptr);
        helix::ui::set_test_toast_hook(nullptr);
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
        settle();
    }

    /// Klipper's host went away mid-command, so Moonraker fails the pending
    /// printer.gcode.script. Scoped to @p macro so nothing else in the mock can
    /// consume it.
    void drop_next_rpc_for(const char* macro) {
        client.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST, "Klippy Disconnected",
                                      macro);
    }

    void run(const char* macro) {
        helix::execute_macro_gcode(&api, macro, helix::MacroParamResult{}, "[MacroHaltTest]", hw);
        settle();
    }

    // The rpc callback settles on the calling thread, but the toast and the
    // suppression writes hop the queue, so drain until it stops producing.
    void settle() {
        for (int i = 0; i < 4; i++) {
            helix::ui::UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        }
    }
};

} // namespace

TEST_CASE_METHOD(MacroFailureFixture,
                 "A halting macro's dropped rpc must not arm the restart suppression",
                 "[macro][restart][halt]") {
    REQUIRE(helix::macro_host_effect(HALTING_MACRO, hw) == helix::MacroHostEffect::Halts);

    drop_next_rpc_for(HALTING_MACRO);
    run(HALTING_MACRO);

    // The whole point of the branch: no suppression window, so the klippy
    // SHUTDOWN that follows raises the recovery dialog at once instead of
    // being held back for 15 seconds.
    CHECK_FALSE(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 0);

    // And nothing is said: not the false "<name> failed" (that is the absorb),
    // and not "Firmware restarting..." either, which would promise a recovery
    // that is not coming.
    CHECK(toasts.empty());
}

TEST_CASE_METHOD(MacroFailureFixture,
                 "A restarting macro's dropped rpc still arms the restart suppression",
                 "[macro][restart][halt]") {
    // The control. Without it the halt assertions above could pass because the
    // rpc never failed, or because execute_macro_gcode() never reached its
    // error callback at all.
    REQUIRE(helix::macro_host_effect(RESTARTING_MACRO, hw) == helix::MacroHostEffect::Restarts);

    drop_next_rpc_for(RESTARTING_MACRO);
    run(RESTARTING_MACRO);

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Firmware restarting...");
}

TEST_CASE_METHOD(MacroFailureFixture, "An ordinary macro's failed rpc is still reported",
                 "[macro][restart][halt]") {
    // The third branch, so "no toast" cannot be mistaken for "this path says
    // nothing about anything". A macro that reaches neither family really did
    // fail and the user has to hear about it.
    const char* plain = "PLAIN_WRAPPER";
    REQUIRE(helix::macro_host_effect(plain, hw) == helix::MacroHostEffect::None);

    drop_next_rpc_for(plain);
    run(plain);

    CHECK_FALSE(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 0);

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::ERROR);
}
