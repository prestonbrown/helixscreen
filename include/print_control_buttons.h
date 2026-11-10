// include/print_control_buttons.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_print_cancel_modal.h"

#include "core/observer_guard.h"
#include "print_control_view.h"
#include "print_job_ref.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <lvgl.h>
#include <memory>
#include <string>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::ui {

/// How long a pending action waits for the print state before the backstop
/// releases the button with a warning.
uint32_t pending_action_backstop_ms(PendingAction action);

/// Singleton owning the LVGL subjects for the two print-control buttons
/// (primary Pause/Resume + Stop). Observes the GLOBAL `print_state_enum`
/// subject independently and drives the subjects through the pure
/// compute_control_button_view() function. Multiple views (PrintStatusPanel,
/// future home widget) bind these subjects.
///
/// Click handlers and the cancel modal are stubs in this revision; they are
/// wired up in a later task.
class PrintControlButtons {
  public:
    static PrintControlButtons& instance();

    /// Initialize the owned subjects, register them into the global XML scope,
    /// wire the event callbacks, and start observing the print state. Idempotent.
    void init_subjects();

    /// Swapping the API object drops a cancel pending on the old one, whose
    /// answer can no longer arrive. A reconnect keeps the same object.
    void set_api(IMoonrakerAPI* api) {
        if (api != api_) {
            reset_cancel();
        }
        api_ = api;
    }

    lv_subject_t* pending_action_subject() {
        return &pending_action_subject_;
    }

    /**
     * @brief Death signal for the subjects this singleton owns.
     *
     * pending_action_subject() is read by other long-lived objects —
     * PrintStatusPanel observes it to re-derive the paused overlay — and this
     * singleton's subjects are torn down mid-process: by the StaticSubjectRegistry
     * entry at shutdown, and in tests by HelixTestFixture::reset_all() on EVERY
     * fixture construction. deinit_all() frees every observer node on them, so an
     * ObserverGuard held by an observer that outlives the teardown must carry this
     * token or its next reset() calls lv_observer_remove() on freed memory.
     *
     * Never empty: an empty token reads as "already dead" and would suppress
     * removal for live observers instead.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    void handle_primary_button();
    void handle_stop_button();

    /// Resume the paused print: validates the Resume macro is configured, marks
    /// a Resuming pending action (optimistic spinner), and runs the shared
    /// prepare_for_resume → Resume dispatch with a clear-on-failure callback.
    /// The single code path behind BOTH the panel's primary Resume button and
    /// the runout guidance dialog's Resume button (#991), so both produce the
    /// same pending-UI + recovery behavior. Safe to call only on the main thread.
    void request_resume();

  private:
    /// request_resume() past its checks: the pending UI and the resume chain.
    void dispatch_resume();

  public:
    /// Klipper broadcast a `!!` error while a Pause/Resume was in flight.
    ///
    /// The optimistic spinner is cleared only by the real print-state transition
    /// or by the backstop, and neither fires when a macro refuses and returns:
    /// RESUME's own runout check prints `!! ... has detected that the filament
    /// has run out` and ends, yet `printer.gcode.script` still answers `ok`,
    /// so the RPC error path never runs. The button then reads "Resuming..." and
    /// stays DISABLED until the backstop — the user cannot retry even after
    /// fixing the filament (bundle JX2FVRB9).
    ///
    /// A pending Pause is released at once. A pending Resume is released only
    /// once its RESUME has also answered `ok` and the print is still paused
    /// kResumeSettleMs later: a `!!` does not end a macro (RESPOND TYPE=error
    /// prints one and carries on), a Resume re-enabled while RESUME still
    /// reheats queues a second one, and the `ok` can arrive before the status
    /// update that says the print runs again.
    ///
    /// Main thread only. No-op when nothing is pending.
    void notify_printer_error(const std::string& detail);

  private:
    PrintControlButtons() = default;

    ~PrintControlButtons();

    void recompute();

    /**
     * @brief Gather every input and decide both buttons
     *
     * Public so the stop handler can ask the same question the enablement
     * asked, rather than re-deriving the routing condition beside it.
     */
    [[nodiscard]] helix::ui::ControlButtonView current_view() const;
    void start_pending_action(PendingAction action);
    void clear_pending_action();

    /// Stop was confirmed: run whichever route current_view() picks.
    void confirm_stop();

    /// Where a Stop on a start the printer was sent stands. The full table is
    /// in docs/devel/PRINT_CONTROL_BUTTONS.md; Stop is disabled in every state
    /// but Idle.
    enum class CancelState {
        Idle,
        Requested, ///< waiting on the start's answer before anything is sent
        Sent,      ///< printer.print.cancel out, unanswered (or answer lost)
        Answered,  ///< the cancel answered OK; waiting on the job state
    };

    /// A cancel answered OK while the printer still reports the job running is
    /// judged only after this long: a status frame from before the cancel can
    /// reach the main thread after the cancel's answer.
    static constexpr uint32_t kCancelConfirmMs = 10000;

    /// Stop on a start in flight or accepted (StopRoute::CancelSentStart).
    void request_sent_start_cancel();
    /// Send printer.print.cancel for the requested job.
    void send_cancel();
    /// Move a Requested cancel on once the start answers or the job leaves,
    /// and drop any cancel once a different job is preparing.
    void advance_requested_cancel();
    /// A cancel that is out reads the printer's job state.
    void settle_cancel_from_job_state(helix::PrintJobState job_state);
    /// The printer ended our job: retire it if it is still ours, then reset.
    void settle_ended_by_printer();
    /// An answered cancel of a printer-announced start ends with that start:
    /// the printer took the job (settle_cancel_from_job_state()), or the run
    /// stopped while the printer held none.
    void settle_printer_start_cancel();
    void on_cancel_confirm_timeout();
    /// Start the kCancelConfirmMs judgement of a printer still holding the job.
    void arm_cancel_confirm();
    /// Back to Idle, Stop enabled again.
    void clear_cancel();
    /// Back to Idle without recomputing, for teardown and set_api().
    void reset_cancel();
    void cancel_confirm_timer();

    /// Shared by clear_pending_action(), the StaticSubjectRegistry teardown and
    /// the destructor — a timer cancelled on only some of those paths stays armed
    /// on a freed `this` on the others.
    void cancel_pending_action_timer();
    /// Shared the same way as cancel_pending_action_timer().
    void cancel_resume_settle_timer();

    /// Tear the owned subjects down, signalling death first.
    ///
    /// Shared by the StaticSubjectRegistry entry and by
    /// PrintControlButtonsTestAccess::reset() for the same reason
    /// cancel_pending_action_timer() is shared: a teardown path that skips the
    /// death signal leaves every outside ObserverGuard pointing at observer
    /// nodes deinit_all() just freed.
    void teardown_subjects();

    IMoonrakerAPI* api_ = nullptr;
    bool subjects_initialized_ = false;
    PendingAction pending_action_ = PendingAction::None;
    lv_timer_t* pending_action_timeout_ = nullptr;
    /// Bumped per RESUME sent, so a late answer to an earlier one is ignored.
    int resume_seq_ = 0;
    /// The pending resume's RESUME answered `ok`.
    bool resume_answered_ = false;
    /// A `!!` arrived while the resume was pending.
    bool resume_error_seen_ = false;
    /// RESUME number @p seq answered `ok`.
    void on_resume_answered(int seq);
    /// Resume number @p seq ended without RESUME running: a refusal, a failed
    /// prepare, or a restart offered instead.
    void on_resume_failed(int seq);
    /// RESUME answered and a `!!` arrived: release the button once the print
    /// state has had time to report printing, if it is still paused then.
    void arm_resume_settle();
    /// Armed by arm_resume_settle(); deleted by every clear.
    lv_timer_t* resume_settle_timer_ = nullptr;
    /// The resume the settle timer was armed for.
    int resume_settle_seq_ = 0;
    CancelState cancel_state_ = CancelState::Idle;
    /// The preparing epoch the cancel was requested for.
    int cancel_epoch_ = 0;
    /// Bumped per printer.print.cancel sent, so a late answer to an earlier
    /// one cannot advance this one.
    int cancel_seq_ = 0;
    lv_timer_t* cancel_confirm_timer_ = nullptr;
    /// The cancel's answer was lost: the printer holding the job is then
    /// judged like an answered cancel.
    bool cancel_lost_ = false;
    /// The cancel went to a start the printer announced, with no job of ours.
    bool cancel_for_printer_start_ = false;

    SubjectManager subjects_;
    lv_subject_t primary_icon_subject_{};
    lv_subject_t primary_label_subject_{};
    lv_subject_t primary_enabled_subject_{};
    lv_subject_t stop_enabled_subject_{};
    lv_subject_t pending_action_subject_{};
    char primary_icon_buf_[32] = "\xF3\xB0\x8F\xA4";
    char primary_label_buf_[16] = "Pause";

    // print_state_enum is a STATIC global subject (singleton lifetime), so a
    // bare member ObserverGuard with NO SubjectLifetime token is correct.
    // SubjectLifetime is only required for dynamic per-fan/sensor/extruder
    // subjects that can be destroyed and recreated during rediscovery.
    ObserverGuard print_state_observer_;
    /// The UI lifecycle axis. Pause/Cancel affordance is a function of
    /// PrintState, and PrintState changes without print_state_enum changing at
    /// all - a host-side pre-start block leaves the printer reporting the
    /// PREVIOUS job for its whole duration.
    ObserverGuard print_lifecycle_observer_;
    /// The preparing job's start hand-over and its epoch: a Requested cancel
    /// waits on them.
    ObserverGuard preparing_start_observer_;
    ObserverGuard preparing_epoch_observer_;
    PrintCancelModal cancel_modal_;

    friend struct PrintControlButtonsTestAccess;
};

} // namespace helix::ui
