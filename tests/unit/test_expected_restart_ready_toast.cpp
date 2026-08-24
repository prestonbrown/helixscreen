// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The klippy_state READY observer is the user-facing completion signal for
// klippy coming back. It has two ways to say so: dismiss the recovery dialog
// (with a "Printer ready" success toast), or - when the flow that initiated
// the restart suppressed the dialog by design (SAVE_CONFIG, PID tuning,
// power/host toggles) - fire that toast directly, since no dialog exists to
// dismiss. These pin both the firing and the silence: a READY with nothing
// expected, like the first one at app start, must stay quiet.
//
// ToastManager is stubbed in the test binary (tests/ui_test_utils.cpp), so the
// toast is captured through the stub's test hook - the same mechanism the
// runout toast-suppression tests use at the ui_notification_* layer.

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../ui_test_utils.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::KlippyState;
using helix::ui::UpdateQueue;

namespace {

class ReadyToastFixture : public XMLTestFixture {
  public:
    ReadyToastFixture() {
        auto& estop = EmergencyStopOverlay::instance();
        estop.init(state(), &api());
        estop.init_subjects();
        estop.create(); // subscribes the klippy_state observer

        // Park klippy in SHUTDOWN with the recovery dialog suppressed - the
        // exact state a mid-restart bounce leaves behind: initial-fire guard
        // burned, no dialog live, subject one transition away from READY.
        // Burning with an unsuppressed ERROR/SHUTDOWN instead would build a
        // recovery dialog, and every READY below would take the dialog
        // branch rather than the path under test.
        estop.suppress_recovery_dialog(RecoverySuppression::LONG);
        state().set_klippy_state_sync(KlippyState::SHUTDOWN);
        settle();
        settle();
        EmergencyStopOverlayTestAccess::reset_suppression(estop);
    }

    ~ReadyToastFixture() override {
        auto& estop = EmergencyStopOverlay::instance();
        EmergencyStopOverlayTestAccess::reset_suppression(estop);
        EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);
        estop.deinit_subjects();
        helix::ui::set_test_toast_hook(nullptr);
        UpdateQueue::instance().drain();
    }

    // Drain until the queue actually stops producing work. The READY branch
    // hops through async_call before it touches any toast, so a single drain
    // leaves the toast in flight.
    void settle() {
        for (int i = 0; i < 16 && UpdateQueue::instance().pending_count() > 0; i++) {
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
    }

    struct Shown {
        ToastSeverity severity;
        std::string message;
    };
    std::vector<Shown> shown;

    /// Drive one klippy READY transition with every toast routed into `shown`.
    void go_ready() {
        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& message) {
            shown.push_back({severity, message});
        });
        state().set_klippy_state_sync(KlippyState::READY);
        settle();
        settle(); // the READY branch hops through async_call before toasting
        helix::ui::set_test_toast_hook(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(ReadyToastFixture,
                 "A READY ending a suppressed restart announces the printer ready",
                 "[recovery][readytoast]") {
    // A SAVE_CONFIG-style flow: dialog suppressed for the restart, klippy
    // bounces and comes back with no dialog ever shown.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);
    go_ready();

    REQUIRE(shown.size() == 1);
    CHECK(shown[0].severity == ToastSeverity::SUCCESS);
    CHECK(shown[0].message == "Printer ready");
}

TEST_CASE_METHOD(ReadyToastFixture,
                 "A READY ending a user-initiated restart announces the printer ready",
                 "[recovery][readytoast]") {
    // The recovery dialog's own Restart path: flag armed, no time-based
    // suppression window at all.
    EmergencyStopOverlayTestAccess::set_restart_in_progress(EmergencyStopOverlay::instance(), true);
    go_ready();

    REQUIRE(shown.size() == 1);
    CHECK(shown[0].severity == ToastSeverity::SUCCESS);
    CHECK(shown[0].message == "Printer ready");
}

TEST_CASE_METHOD(ReadyToastFixture, "A plain READY with nothing expected stays silent",
                 "[recovery][readytoast]") {
    // First ready at app start: no dialog to dismiss, no restart in flight,
    // no suppression window. The status icon carries it; a toast here would
    // be noise for good news nobody asked about.
    go_ready();

    CHECK(shown.empty());
}
