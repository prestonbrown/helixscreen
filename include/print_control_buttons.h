// include/print_control_buttons.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_print_cancel_modal.h"

#include "print_control_view.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {

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

    void set_api(IMoonrakerAPI* api) {
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
        return subjects_lifetime_;
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

    /// Klipper broadcast a `!!` error while a Pause/Resume was in flight.
    ///
    /// The optimistic spinner is cleared only by the real print-state transition
    /// or by a 150s backstop, and neither fires when Klipper *aborts* the macro:
    /// RESUME's own runout check prints `!! ... has detected that the filament
    /// has run out` and returns, yet `printer.gcode.script` still answers `ok`,
    /// so the RPC error path never runs. The button then reads "Resuming..." and
    /// stays DISABLED for the full 150s — the user cannot retry even after
    /// fixing the filament (bundle JX2FVRB9).
    ///
    /// Clearing on the error is safe in the other direction too: if the command
    /// did land despite an unrelated error, the state observer clears the same
    /// pending action a moment later and the clear is idempotent.
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

    /// Shared by clear_pending_action(), the StaticSubjectRegistry teardown and
    /// the destructor — a timer cancelled on only some of those paths stays armed
    /// on a freed `this` on the others.
    void cancel_pending_action_timer();

    /// Tear the owned subjects down, signalling death first.
    ///
    /// Shared by the StaticSubjectRegistry entry and by
    /// PrintControlButtonsTestAccess::reset() for the same reason
    /// cancel_pending_action_timer() is shared: a teardown path that skips the
    /// death signal leaves every outside ObserverGuard pointing at observer
    /// nodes deinit_all() just freed.
    void teardown_subjects();

    static void on_primary_clicked(lv_event_t* e);
    static void on_stop_clicked(lv_event_t* e);

    IMoonrakerAPI* api_ = nullptr;
    bool subjects_initialized_ = false;
    PendingAction pending_action_ = PendingAction::None;
    lv_timer_t* pending_action_timeout_ = nullptr;

    SubjectManager subjects_;
    /// See get_subjects_lifetime(). Created with the object and REPLACED (never
    /// nulled) by teardown, so the accessor always hands out a live token.
    SubjectLifetime subjects_lifetime_ = std::make_shared<bool>(true);
    lv_subject_t primary_icon_subject_;
    lv_subject_t primary_label_subject_;
    lv_subject_t primary_enabled_subject_;
    lv_subject_t stop_enabled_subject_;
    lv_subject_t pending_action_subject_;
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
    PrintCancelModal cancel_modal_;

    friend struct PrintControlButtonsTestAccess;
};

} // namespace helix::ui
