// src/ui/print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_buttons.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h" // NOTIFY_WARNING / NOTIFY_ERROR
#include "ui_event_safety.h"    // LVGL_SAFE_EVENT_CB_BEGIN / END
#include "ui_resume_dispatch.h"
#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "app_globals.h" // get_printer_state()
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "print_job_ref.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <memory>

namespace helix::ui {

PrintControlButtons& PrintControlButtons::instance() {
    static PrintControlButtons s_instance;
    return s_instance;
}

void PrintControlButtons::init_subjects() {
    if (subjects_initialized_)
        return;

    UI_MANAGED_SUBJECT_STRING(primary_icon_subject_, primary_icon_buf_, "\xF3\xB0\x8F\xA4",
                              "print_control_primary_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(primary_label_subject_, primary_label_buf_, kControlLabelPause,
                              "print_control_primary_label", subjects_);
    UI_MANAGED_SUBJECT_INT(primary_enabled_subject_, 0, "print_control_primary_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(stop_enabled_subject_, 0, "print_control_stop_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(pending_action_subject_, 0, "print_pending_action", subjects_);

    register_xml_callbacks({
        {"on_print_control_primary", [](lv_event_t*) { instance().handle_primary_button(); }},
        {"on_print_control_stop", [](lv_event_t*) { instance().handle_stop_button(); }},
    });

    // print_state_enum is a static global subject — no SubjectLifetime needed.
    // RAW_PRINT_STATE_OK: TWO observers on purpose. This one clears the
    // optimistic pending action when the PRINTER's own state moves; the
    // lifecycle observer below recomputes the buttons. Collapsing them would
    // clear a pending Pause on the Idle -> Preparing edge, before the printer
    // has answered.
    print_state_observer_ = observe<int>(
        get_printer_state().print_state().get_print_state_enum_subject(), this,
        [](PrintControlButtons* self, int) {
            self->settle_cancel_from_job_state(
                get_printer_state().print_state().get_print_job_state());
            // A real state change clears any optimistic
            // pending action; the new state is authoritative.
            // Otherwise just recompute the buttons.
            if (self->pending_action_ != PendingAction::None)
                self->clear_pending_action();
            else
                self->recompute();
        },
        get_printer_state().get_subjects_lifetime());

    // The lifecycle is the axis the affordance contract is written against, and
    // it moves independently of print_state_enum: during a host-side pre-start
    // block the printer keeps reporting the previous job, so nothing else here
    // would re-evaluate for the entire window.
    print_lifecycle_observer_ = observe<int>(
        get_printer_state().print_state().get_print_lifecycle_subject(), this,
        [](PrintControlButtons* self, int) { self->recompute(); },
        get_printer_state().get_subjects_lifetime());

    // A Requested cancel goes out when the start answers, or is dropped when
    // the job leaves without the printer having it.
    preparing_start_observer_ = observe<int>(
        get_printer_state().print_state().get_preparing_start_subject(), this,
        [](PrintControlButtons* self, int) {
            self->advance_requested_cancel();
            self->recompute();
        },
        get_printer_state().get_subjects_lifetime());
    preparing_epoch_observer_ = observe<int>(
        get_printer_state().print_state().get_preparing_epoch_subject(), this,
        [](PrintControlButtons* self, int) {
            self->advance_requested_cancel();
            self->recompute();
        },
        get_printer_state().get_subjects_lifetime());

    // Self-register cleanup so subjects/observer are torn down before lv_deinit().
    StaticSubjectRegistry::instance().register_deinit("PrintControlButtons", []() {
        auto& self = PrintControlButtons::instance();
        // release() (NOT reset()) is correct here: this runs pre-lv_deinit when
        // the observed subject is already being destroyed by its own owner.
        self.print_state_observer_.release();
        self.print_lifecycle_observer_.release();
        self.preparing_start_observer_.release();
        self.preparing_epoch_observer_.release();
        self.teardown_subjects();
    });

    subjects_initialized_ = true;
    recompute();
}

ControlButtonView PrintControlButtons::current_view() const {
    auto& state = get_printer_state();
    auto& macros = StandardMacros::instance();

    ControlButtonInputs in;
    // RAW_PRINT_STATE_OK: ControlButtonInputs carries BOTH axes on purpose -
    // job_state (the wire) and lifecycle - because "who holds the job" and "what
    // may the user do" are different questions. See print_control_view.cpp.
    in.job_state = state.print_state().get_print_job_state();
    in.lifecycle = state.print_state().get_print_lifecycle();
    in.has_preparing_job = state.print_state().has_preparing_job();
    in.pending = pending_action_;
    in.pause_available = !macros.get(StandardMacroSlot::Pause).is_empty();
    in.resume_available = !macros.get(StandardMacroSlot::Resume).is_empty();
    in.cancel_available = !macros.get(StandardMacroSlot::Cancel).is_empty();
    in.start_sent = state.print_state().preparing_start() != helix::PreparingStart::None;
    in.cancel_requested = cancel_state_ != CancelState::Idle;
    return compute_control_button_view(in);
}

void PrintControlButtons::recompute() {
    if (!subjects_initialized_)
        return;

    ControlButtonView v = current_view();

    std::snprintf(primary_icon_buf_, sizeof(primary_icon_buf_), "%s", v.primary_icon);
    std::snprintf(primary_label_buf_, sizeof(primary_label_buf_), "%s", v.primary_label);
    lv_subject_copy_string(&primary_icon_subject_, primary_icon_buf_);
    lv_subject_copy_string(&primary_label_subject_, primary_label_buf_);
    lv_subject_set_int(&primary_enabled_subject_, v.primary_enabled ? 1 : 0);
    lv_subject_set_int(&stop_enabled_subject_, v.stop_enabled ? 1 : 0);
    lv_subject_set_int(&pending_action_subject_, static_cast<int>(pending_action_));
}

// PrintControlButtons is an app-lifetime singleton, so the stateless callbacks
// below reach back via PrintControlButtons::instance() rather than capturing
// `this` — the established safe pattern (mirrors PrintStatusPanel's old
// get_global_print_status_panel() usage). No use-after-free is possible.
void PrintControlButtons::handle_primary_button() {
    if (!api_) {
        spdlog::warn("[PrintControl] No API - cannot dispatch primary action");
        return;
    }
    // RAW_PRINT_STATE_OK: picks which macro to send; see below.
    auto state = get_printer_state().print_state().get_print_job_state();
    auto& macros = StandardMacros::instance();

    // RAW_PRINT_STATE_OK: chooses WHICH macro to send. Pause is meaningless
    // before the printer holds the job, and print_control_view already refuses
    // the button during Preparing.
    if (state == helix::PrintJobState::PRINTING) {
        if (macros.get(StandardMacroSlot::Pause).is_empty()) {
            NOTIFY_WARNING(lv_tr("Pause macro not configured"));
            return;
        }
        spdlog::info("[PrintControl] Pausing print");
        start_pending_action(PendingAction::Pausing);
        // suppress_auto_toast=true: the on_error below surfaces a contextual
        // toast; the generic RPC_ERROR auto-toast + Klipper's `!!` broadcast
        // for the same root cause would be redundant noise.
        macros.execute(
            StandardMacroSlot::Pause, api_, []() { spdlog::info("[PrintControl] Pause sent"); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintControl] Pause failed: {}", err.message);
                helix::ui::notify_error_tr(TR_NOOP("Failed to pause print: {}"), err);
                PrintControlButtons::instance().clear_pending_action();
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
        // RAW_PRINT_STATE_OK: Resume is only meaningful once the PRINTER says
        // it is paused.
    } else if (state == helix::PrintJobState::PAUSED) {
        request_resume();
    }
}

void PrintControlButtons::request_resume() {
    if (!api_) {
        spdlog::warn("[PrintControl] No API - cannot resume");
        return;
    }
    auto& macros = StandardMacros::instance();
    if (macros.get(StandardMacroSlot::Resume).is_empty()) {
        NOTIFY_WARNING(lv_tr("Resume macro not configured"));
        return;
    }
    spdlog::info("[PrintControl] Resuming print");
    start_pending_action(PendingAction::Resuming);
    // dispatch_prepared_resume runs the backend prepare_for_resume → Resume
    // chain. The optimistic spinner spans the whole window; only clear on
    // failure (success waits on the PrinterState observer confirmation, which
    // clears the pending action when state transitions to Printing).
    helix::ui::dispatch_prepared_resume(
        api_, "[PrintControl]", []() { PrintControlButtons::instance().clear_pending_action(); });
}

void PrintControlButtons::handle_stop_button() {
    spdlog::info("[PrintControl] Stop clicked - confirming");
    if (helix::AbortManager::instance().is_aborting()) {
        NOTIFY_WARNING(lv_tr("Abort already in progress"));
        return;
    }
    cancel_modal_.set_on_confirm([]() { PrintControlButtons::instance().confirm_stop(); });
    cancel_modal_.show(lv_screen_active());
}

void PrintControlButtons::confirm_stop() {
    auto& state = get_printer_state();
    // The same decision that enabled the button picks the mechanism, so the
    // two can never disagree about which one applies.
    switch (current_view().stop_route) {
    case StopRoute::RetirePreparing:
        // Nothing has reached the printer, and every path that sends a start
        // (PrintPreparationManager's choke points, the reprint in
        // PrintStartController) first checks that its job is still the one
        // being prepared, so withdrawing it here is what stops it. Sending
        // CANCEL_PRINT instead would reach an idle printer, which AbortManager
        // reads as an immediate success.
        spdlog::info("[PrintControl] Stop confirmed while preparing - cancelling the start");
        // The notification and the heater cooldown belong to the
        // preparing-exit observer, which sees every retirement rather than
        // just this one path.
        state.print_state().retire_preparing(helix::PreparingExit::Cancelled);
        return;
    case StopRoute::CancelSentStart:
        request_sent_start_cancel();
        return;
    case StopRoute::Abort:
        break;
    }

    spdlog::info("[PrintControl] Stop confirmed - starting AbortManager");
    // AbortManager handles its own UI state (progress modal, button states).
    helix::AbortManager::instance().start_abort();
}

void PrintControlButtons::request_sent_start_cancel() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), lv_tr("Not connected to printer"));
        return;
    }
    // Klipper cannot interrupt a running macro, and a cancel that reaches the
    // printer before the start does is answered at once on an idle printer
    // while the start that follows prints. So CANCEL_PRINT goes out only once
    // the start has answered OK, or has lost its answer while Klipper is busy
    // with it, queueing it behind a start the printer has.
    auto& ps = get_printer_state().print_state();
    cancel_epoch_ = lv_subject_get_int(ps.get_preparing_epoch_subject());
    ps.note_preparing_cancel_requested();
    NOTIFY_INFO(lv_tr("Cancel sent. The printer finishes its start sequence, then stops "
                      "before printing."));
    const auto start = ps.preparing_start();
    if (start == helix::PreparingStart::Accepted || start == helix::PreparingStart::Unanswered) {
        send_cancel();
        return;
    }
    spdlog::info("[PrintControl] Stop confirmed while the start is in flight - cancelling once "
                 "it answers (preparing epoch {})",
                 cancel_epoch_);
    cancel_state_ = CancelState::Requested;
    recompute();
}

void PrintControlButtons::send_cancel() {
    if (!api_) {
        clear_cancel();
        NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), lv_tr("Not connected to printer"));
        return;
    }
    const int seq = ++cancel_seq_;
    spdlog::info("[PrintControl] Cancelling a start the printer has (preparing epoch {})",
                 cancel_epoch_);
    cancel_state_ = CancelState::Sent;
    cancel_lost_ = false;
    recompute();

    // A client that is not connected refuses the request before sending it,
    // calling the error back synchronously. That cancel never left the screen,
    // which is not the same as one whose answer was lost.
    auto sending = std::make_shared<std::atomic<bool>>(true);
    api_->job().cancel_print(
        [seq]() {
            queue_update("PrintControlButtons::send_cancel.ok", [seq]() {
                auto& self = PrintControlButtons::instance();
                if (self.cancel_seq_ != seq || self.cancel_state_ != CancelState::Sent) {
                    return;
                }
                spdlog::info("[PrintControl] Printer answered the cancel");
                self.cancel_state_ = CancelState::Answered;
                // The start answered before this cancel went out, so the
                // cancel ran on our job. A state already terminal will not
                // transition again to tell us.
                // RAW_PRINT_STATE_OK: what the PRINTER reports for the job.
                switch (get_printer_state().print_state().get_print_job_state()) {
                case helix::PrintJobState::CANCELLED:
                case helix::PrintJobState::COMPLETE:
                case helix::PrintJobState::ERROR:
                    self.settle_ended_by_printer();
                    return;
                default:
                    break;
                }
                self.arm_cancel_confirm();
            });
        },
        [seq, sending](const MoonrakerError& err) {
            const bool never_sent = sending->load();
            queue_update("PrintControlButtons::send_cancel.err", [seq, err, never_sent]() {
                auto& self = PrintControlButtons::instance();
                if (self.cancel_seq_ != seq || self.cancel_state_ != CancelState::Sent) {
                    return;
                }
                if (err.is_transport_loss() && !never_sent) {
                    // The cancel may still be queued behind the start sequence,
                    // which can outlast any RPC ceiling. Only the job state
                    // settles it; Stop stays off so no second cancel queues.
                    spdlog::warn("[PrintControl] Cancel unanswered ({}) - waiting on the "
                                 "printer's job state",
                                 err.message);
                    NOTIFY_INFO(lv_tr("The printer has not answered the cancel yet. It "
                                      "stops once its start sequence finishes."));
                    self.cancel_lost_ = true;
                    // RAW_PRINT_STATE_OK: the printer already holding the job.
                    const auto job_state = get_printer_state().print_state().get_print_job_state();
                    if (job_state == helix::PrintJobState::PRINTING ||
                        job_state == helix::PrintJobState::PAUSED) {
                        self.arm_cancel_confirm();
                    }
                    return;
                }
                spdlog::error("[PrintControl] Cancel refused{}: {}",
                              never_sent ? " before sending" : "", err.message);
                self.clear_cancel();
                if (never_sent && err.is_transport_loss()) {
                    NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), lv_tr("Not connected to printer"));
                } else {
                    NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), err.localized_message());
                }
            });
        });
    sending->store(false);
}

void PrintControlButtons::advance_requested_cancel() {
    if (cancel_state_ == CancelState::Idle) {
        return;
    }
    auto& ps = get_printer_state().print_state();
    const int now = lv_subject_get_int(ps.get_preparing_epoch_subject());
    // A new job is preparing: whatever this cancel was for is over.
    if (now != 0 && now != cancel_epoch_) {
        clear_cancel();
        return;
    }
    if (cancel_state_ != CancelState::Requested) {
        return;
    }
    if (now != cancel_epoch_) {
        // Confirmed means the printer reported the job, so it has the start.
        // Any other exit means the start never reached it.
        if (ps.last_preparing_exit() == helix::PreparingExit::Confirmed) {
            send_cancel();
        } else {
            clear_cancel();
        }
        return;
    }
    switch (ps.preparing_start()) {
    case helix::PreparingStart::Accepted:
    case helix::PreparingStart::Unanswered:
        send_cancel();
        break;
    case helix::PreparingStart::None:
        // Refused: nothing reached the printer, so withdrawing it is enough.
        spdlog::info("[PrintControl] Start refused while a cancel waited - withdrawing the job");
        clear_cancel();
        ps.retire_preparing(helix::PreparingExit::Cancelled);
        break;
    case helix::PreparingStart::InFlight:
        break;
    }
}

void PrintControlButtons::settle_cancel_from_job_state(helix::PrintJobState job_state) {
    if (cancel_state_ != CancelState::Sent && cancel_state_ != CancelState::Answered) {
        return;
    }
    // RAW_PRINT_STATE_OK: how the PRINTER ended the job the cancel went to.
    // STANDBY is not an answer: SDCARD_PRINT_FILE passes through it while
    // loading the file. PRINTING is expected while the cancel waits behind a
    // start macro in the file, so it is judged only once the cancel answered
    // or its answer was lost.
    switch (job_state) {
    case helix::PrintJobState::CANCELLED:
    case helix::PrintJobState::COMPLETE:
    case helix::PrintJobState::ERROR:
        settle_ended_by_printer();
        break;
    // RAW_PRINT_STATE_OK: the printer holding the job, as it reports it.
    case helix::PrintJobState::PRINTING:
    case helix::PrintJobState::PAUSED:
        if (cancel_state_ == CancelState::Answered || cancel_lost_) {
            arm_cancel_confirm();
        }
        break;
    default:
        break;
    }
}

void PrintControlButtons::settle_ended_by_printer() {
    auto& ps = get_printer_state().print_state();
    // Its terminal state reaches the completion observer, which reports it
    // once; a job the printer reported earlier was already retired Confirmed.
    if (lv_subject_get_int(ps.get_preparing_epoch_subject()) == cancel_epoch_) {
        ps.retire_preparing(helix::PreparingExit::EndedByPrinter);
    }
    clear_cancel();
}

void PrintControlButtons::arm_cancel_confirm() {
    if (cancel_confirm_timer_) {
        return;
    }
    cancel_confirm_timer_ = lv_timer_create(
        [](lv_timer_t*) {
            auto& s = PrintControlButtons::instance();
            s.cancel_confirm_timer_ = nullptr; // one-shot: LVGL deletes it
            s.on_cancel_confirm_timeout();
        },
        kCancelConfirmMs, nullptr);
    lv_timer_set_repeat_count(cancel_confirm_timer_, 1);
}

void PrintControlButtons::on_cancel_confirm_timeout() {
    if (cancel_state_ != CancelState::Answered && cancel_state_ != CancelState::Sent) {
        return;
    }
    // RAW_PRINT_STATE_OK: the printer still holding the job, as it reports it.
    const auto job_state = get_printer_state().print_state().get_print_job_state();
    if (job_state != helix::PrintJobState::PRINTING && job_state != helix::PrintJobState::PAUSED) {
        if (cancel_state_ == CancelState::Answered) {
            clear_cancel();
        }
        return;
    }
    spdlog::warn("[PrintControl] Printer still holds the job after the cancel");
    NOTIFY_WARNING(lv_tr("The printer kept printing after the cancel."));
    clear_cancel();
}

void PrintControlButtons::clear_cancel() {
    reset_cancel();
    recompute();
}

void PrintControlButtons::reset_cancel() {
    cancel_confirm_timer();
    cancel_state_ = CancelState::Idle;
    cancel_epoch_ = 0;
    cancel_lost_ = false;
}

void PrintControlButtons::cancel_confirm_timer() {
    if (cancel_confirm_timer_) {
        helix::ui::lv_timer_cancel_safe(cancel_confirm_timer_);
        cancel_confirm_timer_ = nullptr;
    }
}

void PrintControlButtons::start_pending_action(PendingAction action) {
    clear_pending_action(); // supersede any in-flight action (also deletes prior timer)
    pending_action_ = action;
    // The authoritative clear is the print_state_observer_ above: when the real
    // print state transitions (paused→printing for Resume, printing→paused for
    // Pause) it clears the pending action. This timer is only a last-resort
    // backstop for a silently-lost command.
    //
    // Pause normally lands in <2s (can stretch to ~20s while buffered moves
    // drain) → 25s backstop. Resume on an auto-feed backend (Snapmaker U1) runs
    // AUTO_FEEDING heat+feed+flush + reheat, which takes 40-90s before the
    // paused→printing transition fires — a 25s backstop false-fired a spurious
    // "Resume command timed out" toast on a resume that actually succeeded
    // (#991). Use a 150s ceiling for Resume so the observer wins for any normal
    // recovery and the timer only trips on a genuinely lost command.
    const uint32_t timeout_ms = (action == PendingAction::Resuming) ? 150000u : 25000u;
    pending_action_timeout_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PrintControlButtons*>(lv_timer_get_user_data(t));
            if (!self)
                return;
            const char* verb =
                (self->pending_action_ == PendingAction::Resuming) ? "Resume" : "Pause";
            spdlog::warn("[PrintControl] {} timed out - clearing pending", verb);
            NOTIFY_WARNING(lv_tr("{} command timed out"), verb);
            self->clear_pending_action();
        },
        timeout_ms, this);
    lv_timer_set_repeat_count(pending_action_timeout_, 1);
    recompute();
}

PrintControlButtons::~PrintControlButtons() {
    // The StaticSubjectRegistry teardown below cancels this timer pre-lv_deinit,
    // but only on a shutdown that actually runs the registry. Cancelling here too
    // means no teardown path leaves pending_action_timeout_ armed on a freed
    // `this`. lv_timer_cancel_safe() self-guards on lv_is_initialized(), which is
    // what makes it safe from a static's destructor (#750, #751, #1173).
    cancel_pending_action_timer();
    cancel_confirm_timer();
}

void PrintControlButtons::teardown_subjects() {
    cancel_pending_action_timer();
    reset_cancel();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrintControlButtons::cancel_pending_action_timer() {
    if (pending_action_timeout_) {
        helix::ui::lv_timer_cancel_safe(pending_action_timeout_);
        pending_action_timeout_ = nullptr;
    }
}

void PrintControlButtons::clear_pending_action() {
    cancel_pending_action_timer();
    pending_action_ = PendingAction::None;
    recompute();
}

void PrintControlButtons::notify_printer_error(const std::string& detail) {
    if (pending_action_ == PendingAction::None) {
        return;
    }
    const char* verb = (pending_action_ == PendingAction::Resuming) ? "Resume" : "Pause";
    spdlog::warn("[PrintControl] Klipper error while {} pending — releasing the button: {}", verb,
                 detail);
    clear_pending_action();
}

} // namespace helix::ui
