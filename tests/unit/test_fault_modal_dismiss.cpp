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
 * rather than going through ui_notification_printer_fault(), which builds a
 * modal only after ToastManager::init(). The registry is where the logic
 * lives, so the one-line track_fault_modal() call inside ui_notification.cpp is
 * wiring these tests do not reach.
 */

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "display_settings_manager.h"
#include "fault_modal_registry.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::KlippyState;
using helix::ui::dismiss_fault_modals;
using helix::ui::fault_carrier_showing;
using helix::ui::set_fault_carrier;
using helix::ui::track_fault_modal;
using helix::ui::UpdateQueue;

namespace {

class FaultModalFixture : public XMLTestFixture {
  public:
    FaultModalFixture() {
        // The app registers the modal callbacks at startup.
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
        UpdateQueue::instance().drain_until_empty(16);
    }

    /// A fault alert exactly as ui_notification_printer_fault() builds it:
    /// modal_alert() + track_fault_modal() on the returned dialog.
    lv_obj_t* raise_fault(const char* title, const char* message) {
        lv_obj_t* dialog = helix::ui::modal_alert(title, message, ModalSeverity::Error, "OK");
        REQUIRE(dialog != nullptr);
        track_fault_modal(dialog);
        settle();
        return dialog;
    }

    /// A HelixScreen-side alert: same widget, never registered as a fault.
    lv_obj_t* raise_untracked(const char* title, const char* message) {
        lv_obj_t* dialog = helix::ui::modal_alert(title, message, ModalSeverity::Error, "OK");
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

// The sweep is not the dialog's caller: it closes with the External reason so
// a caller holding state the buttons resolve still learns about the close.
// With the plain hide() default (Programmatic) the sweep would silently strand
// exactly what on_dismiss exists to clear - the #1380 leak wearing a system hat.
TEST_CASE_METHOD(FaultModalFixture, "Sweeping a fault modal reports to its on_dismiss",
                 "[1266][faultmodal][1380]") {
    int dismissed = 0;
    helix::ui::AlertOptions opts;
    opts.on_dismiss = [&dismissed] { ++dismissed; };
    lv_obj_t* dialog =
        helix::ui::modal_alert("Printer Error", "Lost communication with MCU 'BoxTurtle'",
                               ModalSeverity::Error, "OK", nullptr, opts);
    REQUIRE(dialog != nullptr);
    track_fault_modal(dialog);
    settle();
    REQUIRE(dismissed == 0);

    CHECK(dismiss_fault_modals() == 1);
    settle();

    CHECK(dismissed == 1);
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

// One fault, one dialog: a dialog that restates the fault retires the alerts
// for it and holds new ones back while it is up.
TEST_CASE_METHOD(FaultModalFixture, "A fault carrier retires the fault alerts it restates",
                 "[faultmodal][faultcarrier]") {
    lv_obj_t* alert =
        raise_fault("Printer Error", "Heater dragonbreath not heating at expected rate");
    lv_obj_t* carrier =
        raise_untracked("Klipper Shutdown", "Heater dragonbreath not heating at expected rate");
    REQUIRE_FALSE(fault_carrier_showing());

    set_fault_carrier(carrier);
    settle();

    CHECK(ModalStack::instance().backdrop_for(alert) == nullptr);
    CHECK(helix::ui::tracked_fault_modal_count() == 0);
    CHECK(Modal::get_top() == carrier);
    CHECK(fault_carrier_showing());

    Modal::hide(carrier);
    settle();
    CHECK_FALSE(fault_carrier_showing());
}

TEST_CASE_METHOD(FaultModalFixture, "A fault alert raised under a carrier is demoted to a toast",
                 "[faultmodal][faultcarrier]") {
    using helix::ui::fault_alert_gets_modal;
    lv_obj_t* carrier = raise_untracked("Klipper Shutdown", "Heater not heating");
    set_fault_carrier(carrier);

    CHECK_FALSE(fault_alert_gets_modal(/*modal=*/true, /*fault=*/true));
    CHECK(fault_alert_gets_modal(/*modal=*/true, /*fault=*/false));
    CHECK_FALSE(fault_alert_gets_modal(/*modal=*/false, /*fault=*/true));

    // With animations on, the hidden carrier stays on the stack while it
    // animates out, and a closing carrier already demotes nothing.
    struct AnimationsOn {
        bool was = helix::DisplaySettingsManager::instance().get_animations_enabled();
        AnimationsOn() {
            helix::DisplaySettingsManager::instance().set_animations_enabled(true);
        }
        ~AnimationsOn() {
            helix::DisplaySettingsManager::instance().set_animations_enabled(was);
        }
    } animations_on;
    Modal::hide(carrier);
    REQUIRE(ModalStack::instance().backdrop_for(carrier) != nullptr);
    CHECK(fault_alert_gets_modal(/*modal=*/true, /*fault=*/true));
    process_lvgl(500);
    CHECK(ModalStack::instance().backdrop_for(carrier) == nullptr);
}

TEST_CASE_METHOD(FaultModalFixture, "A carrier that already left the stack is not adopted",
                 "[faultmodal][faultcarrier]") {
    lv_obj_t* alert = raise_fault("Printer Error", "Heater not heating");
    lv_obj_t* gone = raise_untracked("Klipper Shutdown", "Heater not heating");
    Modal::hide(gone);
    settle();

    set_fault_carrier(gone);
    settle();

    CHECK_FALSE(fault_carrier_showing());
    CHECK(ModalStack::instance().backdrop_for(alert) != nullptr);
}

TEST_CASE_METHOD(FaultModalFixture, "Clearing the fault carrier leaves fault alerts up",
                 "[faultmodal][faultcarrier]") {
    lv_obj_t* carrier = raise_untracked("Klipper Shutdown", "Heater not heating");
    set_fault_carrier(carrier);
    raise_fault("Printer Error", "Heater not heating");

    // The carrier stopped restating the fault (its text went generic), so the
    // alert holds the only copy on screen.
    set_fault_carrier(nullptr);
    settle();

    CHECK_FALSE(fault_carrier_showing());
    CHECK(helix::ui::tracked_fault_modal_count() == 1);
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
        REQUIRE(register_component("klipper_recovery_dialog"));

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

TEST_CASE_METHOD(KlippyRecoveryFixture,
                 "A Klipper shutdown folds its fault alert into the recovery dialog",
                 "[faultmodal][faultcarrier][recovery]") {
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle();

    const char* reason = "Heater dragonbreath not heating at expected rate, temp: 27.20 "
                         "target: 60.00";
    lv_obj_t* alert = raise_fault("Printer Error", reason);
    REQUIRE(helix::ui::tracked_fault_modal_count() == 1);

    state().network_state().set_klippy_state_message(reason);
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();

    lv_obj_t* recovery = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(recovery != nullptr);
    CHECK(Modal::get_top() == recovery);
    CHECK(ModalStack::instance().backdrop_for(alert) == nullptr);
    CHECK(helix::ui::tracked_fault_modal_count() == 0);
    CHECK(fault_carrier_showing());

    // The recovery dialog is the only modal left.
    Modal::hide(recovery);
    settle();
    CHECK(ModalStack::instance().empty());
}

TEST_CASE_METHOD(KlippyRecoveryFixture,
                 "A disconnect under a shutdown dialog keeps the shutdown reason on screen",
                 "[faultmodal][faultcarrier][recovery]") {
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle();

    raise_fault("Printer Error", "Heater dragonbreath not heating at expected rate");
    state().network_state().set_klippy_state_message(
        "Heater dragonbreath not heating at expected rate");
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();
    lv_obj_t* recovery = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(recovery != nullptr);
    REQUIRE(helix::ui::tracked_fault_modal_count() == 0);

    // The alert it retired is gone, so the reason must survive the upgrade even
    // though a stale cached message would read as something else.
    state().network_state().set_klippy_state_message("Printer is ready");
    EmergencyStopOverlay::instance().show_recovery_for(RecoveryReason::DISCONNECTED);
    settle();
    settle();

    lv_obj_t* message = lv_obj_find_by_name(recovery, "recovery_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)).find("dragonbreath") != std::string::npos);
    lv_obj_t* restart_row = lv_obj_find_by_name(recovery, "recovery_restart_actions");
    REQUIRE(restart_row != nullptr);
    CHECK(lv_obj_has_flag(restart_row, LV_OBJ_FLAG_HIDDEN));
    CHECK(fault_carrier_showing());
}

TEST_CASE_METHOD(KlippyRecoveryFixture, "A shutdown with no reason text keeps the fault alert",
                 "[faultmodal][faultcarrier][recovery]") {
    state().set_klippy_state_sync(KlippyState::READY);
    settle();
    settle();

    raise_fault("Printer Error", "Heater dragonbreath not heating at expected rate");

    // The recovery dialog falls back to generic text, so the alert is the only
    // place the reason is on screen.
    state().network_state().set_klippy_state_message("");
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();

    REQUIRE(lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card") != nullptr);
    CHECK(helix::ui::tracked_fault_modal_count() == 1);
    CHECK_FALSE(fault_carrier_showing());
}

// ============================================================================
// The dialog never shows a READY-era state_message under a fault title, in
// either delivery order of the shutdown edge and the webhooks frame.
// ============================================================================

namespace {

std::string recovery_message_text() {
    lv_obj_t* recovery = lv_obj_find_by_name(lv_screen_active(), "klipper_recovery_card");
    REQUIRE(recovery != nullptr);
    lv_obj_t* message = lv_obj_find_by_name(recovery, "recovery_message");
    REQUIRE(message != nullptr);
    return lv_label_get_text(message);
}

void apply_webhooks(helix::PrinterState& ps, const char* state, const char* message) {
    ps.network_state().apply_webhooks({{"state", state}, {"state_message", message}}, 0.0, false,
                                      std::nullopt);
}

constexpr const char* kM112Reason = "Shutdown due to M112 command";

} // namespace

TEST_CASE_METHOD(KlippyRecoveryFixture,
                 "A shutdown edge ahead of its reason shows generic text, then the reason",
                 "[recovery][state_message][faultmodal]") {
    apply_webhooks(state(), "ready", "Printer is ready");
    settle();
    settle();

    // notify_klippy_shutdown applied before the webhooks frame carrying the reason.
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();
    const std::string before = recovery_message_text();
    CHECK(before.find("Printer is ready") == std::string::npos);
    CHECK_FALSE(before.empty());

    apply_webhooks(state(), "shutdown", kM112Reason);
    settle();
    settle();
    CHECK(recovery_message_text().find(kM112Reason) != std::string::npos);
}

TEST_CASE_METHOD(KlippyRecoveryFixture, "A shutdown frame carrying its reason shows the reason",
                 "[recovery][state_message][faultmodal]") {
    apply_webhooks(state(), "ready", "Printer is ready");
    settle();
    settle();

    apply_webhooks(state(), "shutdown", kM112Reason);
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    settle();
    CHECK(recovery_message_text().find(kM112Reason) != std::string::npos);
}
