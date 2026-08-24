// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_emergency_stop.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "fault_modal_registry.h"
#include "gcode_error_router.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_recovery_service.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

using namespace helix;

namespace {
// Recovery dialog content per reason
struct RecoveryContent {
    const char* title;
    const char* message;
};

RecoveryContent get_recovery_content(RecoveryReason reason) {
    switch (reason) {
    case RecoveryReason::SHUTDOWN:
        return {lv_tr("Printer Shutdown"),
                lv_tr("Klipper has entered shutdown state. This may be due to an emergency stop, "
                      "thermal runaway, or configuration error.")};
    case RecoveryReason::ERROR:
        return {lv_tr("Printer Error"),
                lv_tr("Klipper has entered an error state. This is typically caused by an MCU "
                      "connection failure or configuration error. Try a firmware restart.")};
    case RecoveryReason::DISCONNECTED:
        return {lv_tr("Printer Firmware Disconnected"),
                lv_tr("Klipper firmware has disconnected from the host. "
                      "Try restarting Klipper or performing a firmware restart.")};
    default:
        return {lv_tr("Printer Error"), lv_tr("An unexpected printer error occurred.")};
    }
}
const char* recovery_reason_str(RecoveryReason reason) {
    switch (reason) {
    case RecoveryReason::SHUTDOWN:
        return "SHUTDOWN";
    case RecoveryReason::ERROR:
        return "ERROR";
    case RecoveryReason::DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "NONE";
    }
}

} // namespace

using helix::ui::observe_int_sync;

EmergencyStopOverlay& EmergencyStopOverlay::instance() {
    static EmergencyStopOverlay instance;
    return instance;
}

void EmergencyStopOverlay::init(PrinterState& printer_state, IMoonrakerAPI* api) {
    printer_state_ = &printer_state;
    api_ = api;
    spdlog::debug("[EmergencyStop] Initialized with dependencies");
}

void EmergencyStopOverlay::set_require_confirmation(bool require) {
    require_confirmation_ = require;
    spdlog::debug("[EmergencyStop] Confirmation requirement set to: {}", require);
}

void EmergencyStopOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize visibility subject (default hidden)
    UI_MANAGED_SUBJECT_INT(estop_visible_, 0, "estop_visible", subjects_);

    // Recovery dialog subjects (bound in klipper_recovery_dialog.xml)
    UI_MANAGED_SUBJECT_STRING(recovery_title_subject_, recovery_title_buf_, "Printer Shutdown",
                              "recovery_title", subjects_);
    UI_MANAGED_SUBJECT_STRING(recovery_message_subject_, recovery_message_buf_, "",
                              "recovery_message", subjects_);
    UI_MANAGED_SUBJECT_INT(recovery_can_restart_, 1, "recovery_can_restart", subjects_);
    UI_MANAGED_SUBJECT_STRING(recovery_code_subject_, recovery_code_buf_, "", "recovery_code",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(recovery_has_code_, 0, "recovery_has_code", subjects_);

    // Register click callbacks for XML event binding
    register_xml_callbacks({
        {"emergency_stop_clicked", emergency_stop_clicked},
        {"estop_dialog_cancel_clicked", estop_dialog_cancel_clicked},
        {"estop_dialog_confirm_clicked", estop_dialog_confirm_clicked},
        {"recovery_restart_klipper_clicked", recovery_restart_klipper_clicked},
        {"recovery_firmware_restart_clicked", recovery_firmware_restart_clicked},
        {"recovery_dismiss_clicked", recovery_dismiss_clicked},
        // Advanced panel button callbacks (reuse same logic)
        {"advanced_estop_clicked", advanced_estop_clicked},
        {"advanced_restart_klipper_clicked", advanced_restart_klipper_clicked},
        {"advanced_firmware_restart_clicked", advanced_firmware_restart_clicked},
        // Home panel firmware restart button (shown during klippy SHUTDOWN)
        {"firmware_restart_clicked", home_firmware_restart_clicked},
    });

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy(
        "EmergencyStopSubjects", []() { EmergencyStopOverlay::instance().deinit_subjects(); });

    spdlog::debug("[EmergencyStop] Subjects initialized");
}

void EmergencyStopOverlay::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;

    // Both dialogs live on the active screen and are owned by ModalStack; neither
    // survives the display teardown this runs ahead of (shutdown, or the soft
    // restart after Add Printer). Deleting them here is not our job — the screen
    // and the stack are torn down right after — but keeping the raw pointers is a
    // dangling-reference hazard: show_recovery_for_main() and
    // show_confirmation_dialog() both branch on non-null, so a stale pointer
    // suppresses the next dialog on the rebuilt display.
    recovery_dialog_ = nullptr;
    confirmation_dialog_ = nullptr;
    recovery_reason_ = RecoveryReason::NONE;

    // create() subscribed these to PrinterState's subjects, not to subjects_, so
    // deinit_all() above does not touch them. In the app that has been harmless
    // — singleton and PrinterState both live for the process — but it leaves the
    // guards pointing at subjects they do not own, and create() is documented as
    // re-runnable (soft restart after Add Printer). Releasing here makes the
    // pair symmetric.
    print_state_observer_.reset();
    klippy_state_observer_.reset();

    // Same reasoning as the dialog pointers above, one level up: init() stored
    // borrowed pointers, and neither object survives what this runs ahead of.
    // Production always re-inits before the next create() (a soft restart re-runs
    // Application::init_panel_subjects()), but tests own a PrinterState per
    // fixture and never re-init, so leaving these set hands the next test a
    // singleton pointing at freed objects. Every consumer is null-guarded.
    printer_state_ = nullptr;
    api_ = nullptr;

    spdlog::debug("[EmergencyStop] Subjects deinitialized");
}

void EmergencyStopOverlay::create() {
    if (!printer_state_ || !api_) {
        spdlog::error("[EmergencyStop] Cannot create: dependencies not initialized");
        return;
    }

    if (!subjects_initialized_) {
        spdlog::error("[EmergencyStop] Cannot create: subjects not initialized");
        return;
    }

    // This singleton outlives any given PrinterState — tests own one per fixture,
    // and a soft restart (Add Printer) tears the whole tree down and rebuilds it.
    // deinit_subjects() flips this token before lv_subject_deinit() frees the
    // observer nodes, which is the only thing that stops the guards below from
    // calling lv_observer_remove() on freed memory (THREADING.md §5).
    const SubjectLifetime ps_subjects = printer_state_->get_subjects_lifetime();

    // Subscribe to print state changes for automatic visibility updates
    // The estop_visible subject drives XML bindings in home_panel, controls_panel,
    // and print_status_panel (no FAB - buttons are embedded in each panel)
    // One observer, on print_lifecycle. It already merges both axes this used to
    // watch separately: the raw job state does not move during a host-side
    // pre-start block, and print_start_phase does not move on PRINTING->PAUSED,
    // so covering the button needed two subscriptions and a hand-rolled OR.
    // derive_print_state() does that merge once, for everyone.
    print_state_observer_ = observe_int_sync<EmergencyStopOverlay>(
        printer_state_->get_print_lifecycle_subject(), this,
        [](EmergencyStopOverlay* self, int /*lifecycle*/) { self->update_visibility(); },
        ps_subjects);

    // Reset the initial-fire guard so each (re)subscription — including
    // soft-restart after Add Printer — drops the subject's placeholder
    // SHUTDOWN before Moonraker reports the new printer's real state.
    klippy_state_initial_seen_ = false;

    // Subscribe to klippy state changes for recovery dialog auto-popup
    klippy_state_observer_ = observe_int_sync<EmergencyStopOverlay>(
        printer_state_->get_klippy_state_subject(), this,
        [](EmergencyStopOverlay* self, int state) {
            auto klippy_state = static_cast<KlippyState>(state);

            // First fire carries the subject's default (SHUTDOWN) emitted before
            // Moonraker has reported real state. Acting on it produced a
            // recovery-dialog flash on every launch once UpdateQueue stopped
            // dropping freeze-window callbacks (1d13ed6b4). A genuine
            // shutdown-at-startup is still delivered via the
            // MoonrakerEventType::KLIPPY_SHUTDOWN event in MoonrakerManager.
            if (!self->klippy_state_initial_seen_) {
                self->klippy_state_initial_seen_ = true;
                spdlog::debug(
                    "[KlipperRecovery] Skipping initial klippy_state observer fire (state={})",
                    state);
                return;
            }

            if (klippy_state == KlippyState::SHUTDOWN) {
                // Unified recovery path - all suppression checks are in show_recovery_for()
                self->show_recovery_for(RecoveryReason::SHUTDOWN);
            } else if (klippy_state == KlippyState::ERROR) {
                self->show_recovery_for(RecoveryReason::ERROR);
            } else if (klippy_state == KlippyState::READY) {
                // Auto-dismiss recovery dialog when Klipper is back to READY
                // NOTE: Must defer to main thread - observer may fire from WebSocket thread
                helix::ui::async_call(
                    [](void*) {
                        auto& inst = EmergencyStopOverlay::instance();

                        // Sampled before the flag reset below. An expected
                        // restart (recovery Restart button, a panel's
                        // SAVE_CONFIG, power/host flows) suppresses the
                        // recovery dialog by design, so the dialog-dismiss
                        // path below cannot signal completion for those flows
                        // - the restart flag and suppression window are the
                        // record that this READY ends a UI-initiated restart.
                        // Mutual coverage: if the suppression window expires
                        // before klippy returns, the recovery dialog shows and
                        // the dialog path below fires instead. The flag is
                        // atomic and klippy is READY here, so the extra tick
                        // of trueness cannot swallow a real SHUTDOWN.
                        const bool expected_restart = inst.is_expected_restart();

                        // Reset restart flag - operation complete
                        inst.restart_in_progress_ = false;

                        // Klipper is back, so any "Printer Error" alert raised
                        // while it was down describes a condition that no longer
                        // exists. Leaving them up made a recovered printer look
                        // broken and forced an OK per cascaded fault (#1266).
                        // Independent of recovery_dialog_ below: the user may
                        // have recovered from another client without HelixScreen
                        // ever showing its own recovery dialog.
                        helix::ui::dismiss_fault_modals();

                        // Guard against async callback firing after display destruction
                        if (inst.recovery_dialog_) {
                            if (!ModalStack::instance().backdrop_for(inst.recovery_dialog_)) {
                                // Dialog was dismissed externally — clear stale pointer
                                inst.recovery_dialog_ = nullptr;
                            } else {
                                spdlog::info(
                                    "[KlipperRecovery] Klipper is READY, dismissing recovery "
                                    "dialog");
                                inst.dismiss_recovery_dialog();
                                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                              lv_tr("Printer ready"), 3000);
                            }
                        } else if (expected_restart) {
                            // The restart completed with the dialog suppressed
                            // by its initiating flow, so still say so. Direct
                            // ToastManager call, deliberately not
                            // ui_notification_*: every severity there writes a
                            // history row, and klippy-being-ready is not
                            // history. A READY with nothing expected (first
                            // ready at app start) stays silent - the status
                            // icon already carries it.
                            ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                          lv_tr("Printer ready"), 3000);
                        }
                    },
                    nullptr);
            }
        },
        ps_subjects);

    // Initial visibility update
    update_visibility();

    spdlog::debug("[EmergencyStop] Initialized visibility subject for contextual E-Stop buttons");
}

void EmergencyStopOverlay::update_visibility() {
    if (!printer_state_) {
        return;
    }

    // The contextual E-Stop must be reachable from the moment the machine starts
    // moving, which is BEFORE Moonraker reports a print: a host-side pre-start
    // block homes and probes while print_stats still reads standby (or the
    // previous job's terminal state).
    //
    // This used to hand-OR the raw job state with `start_phase != 0` and watch
    // both subjects to catch each half. That is job_holds_machine() spelled out,
    // so it asks the lifecycle once instead - one predicate, one observer, and
    // no second spelling to drift.
    const auto lifecycle = printer_state_->get_print_lifecycle();

    int new_value = job_holds_machine(lifecycle) ? 1 : 0;
    int current_value = lv_subject_get_int(&estop_visible_);

    if (new_value != current_value) {
        lv_subject_set_int(&estop_visible_, new_value);
        spdlog::debug("[EmergencyStop] Visibility changed: {} (lifecycle={})", new_value,
                      static_cast<int>(lifecycle));
    }
}

void EmergencyStopOverlay::handle_click() {
    spdlog::info("[EmergencyStop] Button clicked");

    if (require_confirmation_) {
        show_confirmation_dialog();
    } else {
        execute_emergency_stop();
    }
}

void EmergencyStopOverlay::execute_emergency_stop() {
    if (!api_) {
        spdlog::error("[EmergencyStop] Cannot execute: API not available");
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Emergency stop failed: not connected"), 4000);
        return;
    }

    spdlog::warn("[EmergencyStop] Executing emergency stop (M112)!");

    api_->emergency_stop(
        []() {
            spdlog::info("[EmergencyStop] Emergency stop command sent successfully");
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Emergency stop activated"),
                                          5000);

            // Proactively show recovery dialog after E-stop
            // We know Klipper will be in SHUTDOWN state - don't wait for notification
            // which may not arrive due to WebSocket timing/disconnection
            EmergencyStopOverlay::instance().show_recovery_for(RecoveryReason::SHUTDOWN);
        },
        [](const MoonrakerError& err) {
            spdlog::error("[EmergencyStop] Emergency stop failed: {}", err.message);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          ("Emergency stop failed: " + err.user_message()).c_str(),
                                          5000);
        });
}

void EmergencyStopOverlay::show_confirmation_dialog() {
    // Don't show if already visible
    if (confirmation_dialog_) {
        spdlog::debug("[EmergencyStop] Confirmation dialog already visible");
        return;
    }

    spdlog::debug("[EmergencyStop] Showing confirmation dialog");

    // Create dialog via Modal system (handles backdrop, z-order, animations)
    confirmation_dialog_ = helix::ui::modal_show("estop_confirmation_dialog");

    if (!confirmation_dialog_) {
        spdlog::error("[EmergencyStop] Failed to create confirmation dialog, executing directly");
        execute_emergency_stop();
        return;
    }

    spdlog::info("[EmergencyStop] Confirmation dialog shown");
}

void EmergencyStopOverlay::dismiss_confirmation_dialog() {
    if (confirmation_dialog_) {
        helix::ui::modal_hide(confirmation_dialog_);
        confirmation_dialog_ = nullptr;
        spdlog::debug("[EmergencyStop] Confirmation dialog dismissed");
    }
}

void EmergencyStopOverlay::show_recovery_dialog() {
    // Don't show if already visible
    spdlog::debug("[KlipperRecovery] show_recovery_dialog() called, recovery_dialog_={}",
                  static_cast<void*>(recovery_dialog_));
    if (recovery_dialog_) {
        spdlog::debug("[KlipperRecovery] Recovery dialog already visible, skipping");
        return;
    }

    spdlog::info("[KlipperRecovery] Creating recovery dialog (reason: {})",
                 recovery_reason_str(recovery_reason_));

    // Use Modal system — backdrop is created programmatically
    recovery_dialog_ = helix::ui::modal_show("klipper_recovery_dialog");
    spdlog::debug("[KlipperRecovery] Dialog created, recovery_dialog_={}",
                  static_cast<void*>(recovery_dialog_));

    if (!recovery_dialog_) {
        spdlog::error("[KlipperRecovery] Failed to create recovery dialog");
        return;
    }

    // XML <view name="..."> is not applied by lv_xml_create — set explicitly for lookups
    lv_obj_set_name(recovery_dialog_, "klipper_recovery_card");
}

void EmergencyStopOverlay::dismiss_recovery_dialog() {
    if (recovery_dialog_) {
        helix::ui::modal_hide(recovery_dialog_);
        recovery_dialog_ = nullptr;
        recovery_reason_ = RecoveryReason::NONE;
        spdlog::debug("[KlipperRecovery] Recovery dialog dismissed");
    }
}

void EmergencyStopOverlay::show_recovery_for(RecoveryReason reason) {
    // Suppression is checked here rather than on the main thread because the
    // deadline is atomic and a suppressed event should cost nothing — an
    // intentional restart bursts these, and queueing each one only to drop it
    // later is waste.
    if (is_recovery_suppressed()) {
        spdlog::info("[KlipperRecovery] Suppressing recovery dialog (suppression active)");
        return;
    }

    // Everything below reads or writes recovery_dialog_ and recovery_reason_ and
    // queries ModalStack — main-thread state with no locking. Callers reach this
    // from the libhv event-loop thread (MoonrakerClient's event handler) and from
    // AbortManager, both of which already rely on this deferring for them, so the
    // hop belongs here where every caller gets it.
    helix::ui::queue_update([reason]() { instance().show_recovery_for_main(reason); });
}

void EmergencyStopOverlay::show_recovery_for_main(RecoveryReason reason) {
    // Don't show during wizard
    if (is_wizard_active()) {
        spdlog::debug("[KlipperRecovery] Ignoring {} during setup wizard",
                      recovery_reason_str(reason));
        return;
    }

    // Don't show if restart is in progress (expected shutdown cycle)
    if (restart_in_progress_) {
        spdlog::debug("[KlipperRecovery] Ignoring {} during restart operation",
                      recovery_reason_str(reason));
        return;
    }

    // Don't show if AbortManager is handling controlled shutdown
    if (helix::AbortManager::instance().is_handling_shutdown()) {
        spdlog::debug("[KlipperRecovery] Ignoring {} - AbortManager handling recovery",
                      recovery_reason_str(reason));
        return;
    }

    // A backdrop tap dismisses the dialog through Modal::hide() without going
    // through dismiss_recovery_dialog(), so recovery_dialog_ can still point at a
    // modal that already left the stack. Drop the stale pointer first and let the
    // rest of this function build a fresh dialog — the next SHUTDOWN or
    // DISCONNECTED after a manual dismiss must still reach the user.
    if (recovery_dialog_ && !ModalStack::instance().backdrop_for(recovery_dialog_)) {
        spdlog::debug("[KlipperRecovery] Previous dialog was dismissed externally, "
                      "clearing stale reference");
        recovery_dialog_ = nullptr;
        recovery_reason_ = RecoveryReason::NONE;
    }

    // If dialog is still showing, update reason if it's worse (SHUTDOWN -> DISCONNECTED means
    // can't restart)
    if (recovery_dialog_) {
        if (reason == RecoveryReason::DISCONNECTED &&
            recovery_reason_ == RecoveryReason::SHUTDOWN) {
            spdlog::info("[KlipperRecovery] Connection dropped while SHUTDOWN dialog showing, "
                         "updating buttons");
            recovery_reason_ = RecoveryReason::DISCONNECTED;
            helix::ui::async_call(
                [](void*) { EmergencyStopOverlay::instance().update_recovery_dialog_content(); },
                nullptr);
        } else {
            spdlog::debug("[KlipperRecovery] Recovery dialog already visible, ignoring {}",
                          recovery_reason_str(reason));
        }
        return;
    }

    recovery_reason_ = reason;

    // Already on the main thread (show_recovery_for marshalled us here), so this
    // is no longer the thread hop it once was — it is kept because the dialog is
    // built one tick later, after any modal currently mid-teardown has finished
    // leaving the stack. The re-entrancy guard below covers the gap.
    helix::ui::async_call(
        [](void*) {
            auto& inst = EmergencyStopOverlay::instance();
            // Guard: dialog may have been shown by another async call in the meantime
            if (inst.recovery_dialog_) {
                if (ModalStack::instance().backdrop_for(inst.recovery_dialog_)) {
                    return;
                }
                // Dialog was dismissed externally — clear stale pointer
                inst.recovery_dialog_ = nullptr;
            }
            spdlog::info("[KlipperRecovery] Showing recovery dialog (reason: {})",
                         recovery_reason_str(inst.recovery_reason_));
            inst.show_recovery_dialog();
            inst.update_recovery_dialog_content();
        },
        nullptr);
}

void EmergencyStopOverlay::suppress_recovery_dialog(uint32_t duration_ms) {
    // Callable from the WebSocket background thread (Moonraker gcode callbacks) —
    // the deadline is atomic so main-thread readers never see a torn value.
    suppress_recovery_until_.store(lv_tick_get() + duration_ms, std::memory_order_relaxed);
    spdlog::info("[KlipperRecovery] Suppressing recovery dialog for {}ms", duration_ms);
}

bool EmergencyStopOverlay::is_recovery_suppressed() const {
    // Single load — re-reading the member would let a concurrent
    // suppress_recovery_dialog() change the value between the zero check and the
    // elapsed comparison.
    const uint32_t deadline = suppress_recovery_until_.load(std::memory_order_relaxed);
    if (deadline == 0) {
        return false;
    }
    return lv_tick_elaps(deadline) > (UINT32_MAX / 2);
}

bool EmergencyStopOverlay::is_expected_restart() const {
    // Load the restart flag first: a writer that sets restart_in_progress_ and
    // then the deadline can otherwise slip between the two reads and produce a
    // false negative for a restart that is genuinely in flight.
    const bool restarting = restart_in_progress_.load(std::memory_order_relaxed);
    return restarting || is_recovery_suppressed();
}

void EmergencyStopOverlay::update_recovery_dialog_content() {
    auto content = get_recovery_content(recovery_reason_);

    // Use actual Klipper state_message if available (e.g. "Max force exceeded...")
    std::string message;
    std::string code;
    if (printer_state_ && (recovery_reason_ == RecoveryReason::SHUTDOWN ||
                           recovery_reason_ == RecoveryReason::ERROR)) {
        const auto& state_msg = printer_state_->get_klippy_state_message();
        if (!state_msg.empty()) {
            message = state_msg;
            // Klipper sometimes reports the reason as a JSON envelope
            // (`{"code":"key1","msg":"..."}`), sometimes as prose with one spliced
            // in. Reuse the gcode router's decoder rather than showing the user raw
            // JSON: it lifts msg into the text and hands the code back separately for
            // the header slot. Prose and malformed envelopes come back untouched.
            GcodeErrorRouter::clean_error_text(message, code);
        }
    }
    if (message.empty()) {
        message = lv_tr(content.message);
    }

    // Update subjects — XML bindings in klipper_recovery_dialog.xml react automatically
    lv_subject_copy_string(&recovery_title_subject_, lv_tr(content.title));
    lv_subject_copy_string(&recovery_message_subject_, message.c_str());
    lv_subject_copy_string(&recovery_code_subject_, code.c_str());
    lv_subject_set_int(&recovery_has_code_, code.empty() ? 0 : 1);
    lv_subject_set_int(&recovery_can_restart_,
                       recovery_reason_ != RecoveryReason::DISCONNECTED ? 1 : 0);

    spdlog::debug("[KlipperRecovery] Updated dialog content: reason={}, can_restart={}",
                  recovery_reason_str(recovery_reason_),
                  recovery_reason_ != RecoveryReason::DISCONNECTED);
}

void EmergencyStopOverlay::restart_klipper() {
    if (!api_) {
        spdlog::error("[KlipperRecovery] Cannot restart: API not available");
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Restart failed: not connected"),
                                      4000);
        return;
    }

    // Suppress recovery dialog during restart - Klipper briefly enters SHUTDOWN
    restart_in_progress_ = true;

    spdlog::info("[KlipperRecovery] Restarting Klipper...");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting Klipper..."), 3000);

    api_->restart_klipper(
        []() {
            spdlog::info("[KlipperRecovery] Klipper restart command sent");
            // Toast will update when klippy_state changes to READY
        },
        [](const MoonrakerError& err) {
            spdlog::error("[KlipperRecovery] Klipper restart failed: {}", err.message);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          ("Restart failed: " + err.user_message()).c_str(), 5000);
        });
}

void EmergencyStopOverlay::firmware_restart() {
    if (!api_) {
        spdlog::error("[KlipperRecovery] Cannot firmware restart: API not available");
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Restart failed: not connected"),
                                      4000);
        return;
    }

    // Suppress recovery dialog during restart - Klipper briefly enters SHUTDOWN
    restart_in_progress_ = true;

    spdlog::info("[KlipperRecovery] Firmware restarting (via recovery service)...");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Firmware restarting..."), 3000);

    // Route through PrinterRecoveryService so platforms with deeper recovery
    // requirements (K2's klipper_mcu daemon, AD5M-style RS-485 bridges, etc.)
    // get the platform-correct restart sequence. Stock Klipper / RatOS Pi
    // automatically fall back to printer.firmware_restart — same behavior as
    // before from the user's perspective.
    helix::PrinterRecoveryService recovery(api_);
    recovery.recover(
        []() {
            spdlog::info("[KlipperRecovery] Recovery initiated");
            // Toast will update when klippy_state changes to READY
        },
        [](const MoonrakerError& err) {
            spdlog::error("[KlipperRecovery] Recovery failed: {}", err.message);
            ToastManager::instance().show(
                ToastSeverity::ERROR, ("Firmware restart failed: " + err.user_message()).c_str(),
                5000);
        });
}

// Static callback trampolines
void EmergencyStopOverlay::emergency_stop_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    EmergencyStopOverlay::instance().handle_click();
}

void EmergencyStopOverlay::estop_dialog_cancel_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::debug("[EmergencyStop] Cancel clicked - aborting E-Stop");
    EmergencyStopOverlay::instance().dismiss_confirmation_dialog();
}

void EmergencyStopOverlay::estop_dialog_confirm_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::debug("[EmergencyStop] Confirm clicked - executing E-Stop");
    auto& instance = EmergencyStopOverlay::instance();
    instance.dismiss_confirmation_dialog();
    instance.execute_emergency_stop();
}

void EmergencyStopOverlay::recovery_restart_klipper_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::debug("[KlipperRecovery] Restart Klipper clicked");
    auto& instance = EmergencyStopOverlay::instance();
    instance.dismiss_recovery_dialog();
    instance.restart_klipper();
}

void EmergencyStopOverlay::recovery_firmware_restart_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::debug("[KlipperRecovery] Firmware Restart clicked");
    auto& instance = EmergencyStopOverlay::instance();
    instance.dismiss_recovery_dialog();
    instance.firmware_restart();
}

void EmergencyStopOverlay::recovery_dismiss_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::debug("[KlipperRecovery] Dismiss clicked");
    EmergencyStopOverlay::instance().dismiss_recovery_dialog();
}

// Advanced panel button callbacks
void EmergencyStopOverlay::advanced_estop_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::info("[Advanced] E-Stop clicked from Advanced panel");
    EmergencyStopOverlay::instance().handle_click();
}

void EmergencyStopOverlay::advanced_restart_klipper_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::info("[Advanced] Restart Klipper clicked from Advanced panel");
    EmergencyStopOverlay::instance().restart_klipper();
}

void EmergencyStopOverlay::advanced_firmware_restart_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::info("[Advanced] Firmware Restart clicked from Advanced panel");
    EmergencyStopOverlay::instance().firmware_restart();
}

void EmergencyStopOverlay::home_firmware_restart_clicked(lv_event_t* e) {
    LV_UNUSED(e);
    spdlog::info("[Home] Firmware Restart clicked from Home panel");

    // If Klipper is in SHUTDOWN or ERROR, restart immediately (no confirmation needed —
    // printer is already stopped). For READY or STARTUP, show confirmation since
    // restarting could interrupt active operations or connection attempts.
    lv_subject_t* klippy = lv_xml_get_subject(nullptr, "klippy_state");
    int klippy_val = klippy ? lv_subject_get_int(klippy) : 0;
    bool in_error_state = (klippy_val == static_cast<int>(KlippyState::SHUTDOWN) ||
                           klippy_val == static_cast<int>(KlippyState::ERROR));

    if (in_error_state) {
        EmergencyStopOverlay::instance().firmware_restart();
    } else {
        helix::ui::modal_show_confirmation(
            lv_tr("Restart Firmware?"),
            lv_tr("This will restart Klipper firmware. Any active operations will be interrupted."),
            ModalSeverity::Warning, lv_tr("Restart"),
            [](lv_event_t*) {
                lv_obj_t* top = Modal::get_top();
                if (top) {
                    Modal::hide(top);
                }
                EmergencyStopOverlay::instance().firmware_restart();
            },
            nullptr, nullptr);
    }
}

namespace helix {
namespace ui {

void begin_expected_klippy_restart(const char* message) {
    // The suppression writes are atomic deadline stores, safe right here on
    // any thread; the toast is LVGL-facing so it hops to the main thread.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);
    if (auto* api = get_moonraker_api()) {
        api->suppress_disconnect_modal(EXPECTED_RESTART_DISCONNECT_MODAL_MS);
    }
    queue_update("begin_expected_klippy_restart", [message]() {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr(message), 3000);
    });
}

} // namespace ui
} // namespace helix
