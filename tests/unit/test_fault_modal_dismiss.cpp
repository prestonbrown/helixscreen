// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fault_modal_dismiss.cpp
 * @brief Regression tests for #1266 — printer-fault modals outlive the fault.
 *
 * Field report: an MCU disconnect shut Klipper down and raised a "Printer
 * Error / Lost communication with MCU 'BoxTurtle'" alert. The reporter
 * recovered with a FIRMWARE_RESTART from Mainsail; HelixScreen kept showing the
 * alert, and a cascade of them had to be acknowledged one by one before the
 * screen was usable again — on a printer that was already fine.
 *
 * Same class of bug as #1185 (AMS error modal never auto-dismissed), and the
 * same rule: the dialog describes a condition, and once the condition is gone
 * the dialog is lying. Klipper returning to READY is that signal.
 *
 * These drive helix::ui::track_fault_modal() / dismiss_fault_modals() directly
 * rather than going through ui_notification_printer_fault(), because
 * ui_notification.o is excluded from the test link (mk/tests.mk) and its stub
 * builds no modal at all. The registry is where the logic lives and it IS
 * linked, so this is the real code — but note the consequence: the one-line
 * track_fault_modal() call inside ui_notification.cpp is wiring these tests
 * cannot reach.
 */

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "fault_modal_registry.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::KlippyState;
using helix::ui::dismiss_fault_modals;
using helix::ui::track_fault_modal;
using helix::ui::UpdateQueue;

namespace {

class FaultModalFixture : public XMLTestFixture {
  public:
    FaultModalFixture() {
        // modal_configure() silently no-ops without these, leaving the button
        // captions at their defaults — the app does this at startup.
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("modal_dialog"));
    }

    ~FaultModalFixture() override {
        // The registry is process-wide and Catch2 runs the whole suite in one
        // process, so a test that leaves something tracked hands it to the next.
        dismiss_fault_modals();
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
    }

    /// Drain until the queue actually stops producing work. A drained callback
    /// can enqueue more (the klippy READY branch hops through async_call before
    /// it touches any modal), so a single drain leaves work in flight — which
    /// showed up as a sweep firing in the middle of a later test step.
    void settle() {
        for (int i = 0; i < 16 && UpdateQueue::instance().pending_count() > 0; i++) {
            UpdateQueue::instance().drain();
        }
        UpdateQueue::instance().drain();
    }

    /// A fault alert exactly as ui_notification_printer_fault() builds it:
    /// modal_show_alert() + track_fault_modal() on the returned dialog.
    lv_obj_t* raise_fault(const char* title, const char* message) {
        lv_obj_t* dialog = helix::ui::modal_show_alert(title, message, ModalSeverity::Error, "OK");
        REQUIRE(dialog != nullptr);
        track_fault_modal(dialog);
        settle();
        return dialog;
    }

    /// A HelixScreen-side alert: same widget, never registered as a fault.
    lv_obj_t* raise_untracked(const char* title, const char* message) {
        lv_obj_t* dialog = helix::ui::modal_show_alert(title, message, ModalSeverity::Error, "OK");
        REQUIRE(dialog != nullptr);
        settle();
        return dialog;
    }
};

} // namespace

TEST_CASE_METHOD(FaultModalFixture, "A printer-fault modal is swept when the fault clears",
                 "[1266][faultmodal]") {
    raise_fault("Printer Error", "Lost communication with MCU 'BoxTurtle'");
    REQUIRE(Modal::get_top() != nullptr);

    CHECK(dismiss_fault_modals() == 1);
    settle();
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(FaultModalFixture, "A HelixScreen-side error modal survives the sweep",
                 "[1266][faultmodal]") {
    // ui_notification_error defaults to modal=true, so a wizard load failure
    // renders the identical widget — only the origin differs. Klipper
    // recovering says nothing about whether that screen will load, so sweeping
    // it away would hide a problem that is still real.
    raise_untracked("Wizard Error", "Failed to load heater configuration screen.");
    REQUIRE(Modal::get_top() != nullptr);

    CHECK(dismiss_fault_modals() == 0);
    settle();
    CHECK(Modal::get_top() != nullptr);
}

TEST_CASE_METHOD(FaultModalFixture, "Sweeping with nothing raised is a no-op",
                 "[1266][faultmodal]") {
    CHECK(dismiss_fault_modals() == 0);
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(FaultModalFixture, "A fault modal the user already acknowledged is not re-counted",
                 "[1266][faultmodal]") {
    raise_fault("Printer Error", "Lost communication with MCU 'BoxTurtle'");
    REQUIRE(Modal::get_top() != nullptr);

    // User taps OK.
    Modal::hide(Modal::get_top());
    settle();
    REQUIRE(Modal::get_top() == nullptr);

    // The registry must not still hold the handle and count it as a dismissal —
    // that is how "dismissed 2 modals" gets logged for a single dialog.
    CHECK(dismiss_fault_modals() == 0);
    CHECK(helix::ui::tracked_fault_modal_count() == 0);
}

TEST_CASE_METHOD(FaultModalFixture, "Cascading fault modals all clear in one sweep",
                 "[1266][faultmodal]") {
    // The reporter's second complaint: several errors stacked up and each
    // needed its own acknowledgement on an already-healthy printer.
    raise_fault("Printer Error", "Lost communication with MCU 'BoxTurtle'");
    raise_fault("Filament System Error", "Lane 2 reset failed");
    REQUIRE(helix::ui::tracked_fault_modal_count() == 2);

    CHECK(dismiss_fault_modals() == 2);
    settle();
    CHECK(Modal::get_top() == nullptr);
    CHECK(helix::ui::tracked_fault_modal_count() == 0);
}

TEST_CASE_METHOD(FaultModalFixture, "A swept fault modal leaves an untracked one alone",
                 "[1266][faultmodal]") {
    // Both on screen at once: the sweep must be selective, not "clear the stack".
    raise_untracked("Wizard Error", "Failed to load heater configuration screen.");
    raise_fault("Printer Error", "Lost communication with MCU 'BoxTurtle'");

    CHECK(dismiss_fault_modals() == 1);
    settle();
    lv_obj_t* survivor = Modal::get_top();
    REQUIRE(survivor != nullptr);
    lv_obj_t* title = lv_obj_find_by_name(survivor, "dialog_title");
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "Wizard Error");
}

// ============================================================================
// The wiring: a Klipper READY transition must actually reach the sweep.
// ============================================================================

namespace {

class KlippyRecoveryFixture : public FaultModalFixture {
  public:
    KlippyRecoveryFixture() {
        auto& estop = EmergencyStopOverlay::instance();
        estop.init(state(), &api());
        estop.init_subjects();
        estop.create(); // subscribes the klippy_state observer

        // Burn the initial-fire guard: production deliberately ignores the
        // first fire, which carries the subject's placeholder value. Burn it
        // with ERROR rather than READY — a READY fire would queue a sweep, and
        // a drained callback can queue more work, so it would still be in
        // flight when the test raises its modal and would eat it.
        state().set_klippy_state_sync(KlippyState::ERROR);
        settle();
        settle();
    }

    ~KlippyRecoveryFixture() override {
        EmergencyStopOverlay::instance().deinit_subjects();
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(KlippyRecoveryFixture, "Klipper returning to READY dismisses the fault modal",
                 "[1266][faultmodal][recovery]") {
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();

    raise_fault("Printer Error", "Lost communication with MCU 'BoxTurtle'");
    REQUIRE(Modal::get_top() != nullptr);

    // FIRMWARE_RESTART from another client.
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle(); // the READY branch hops through async_call before touching modals

    CHECK(helix::ui::tracked_fault_modal_count() == 0);
}

TEST_CASE_METHOD(KlippyRecoveryFixture, "A fault raised while Klipper stays READY is left alone",
                 "[1266][faultmodal][recovery]") {
    // A CRITICAL gcode error mid-print never moves klippy_state, so no READY
    // edge ever arrives and the alert waits for its OK. Pins the scope: this is
    // a Klipper-recovery response, not a blanket modal reaper.
    // Get to a settled READY first, the way a healthy printer sits.
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle();

    raise_fault("Printer Error", "Move out of range");
    REQUIRE(Modal::get_top() != nullptr);

    // Already READY, so the subject does not change and no observer fires.
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle();

    CHECK(Modal::get_top() != nullptr);
    CHECK(helix::ui::tracked_fault_modal_count() == 1);
}
