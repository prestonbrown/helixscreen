// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "chamber_wait_line.h"
#include "core/observer_guard.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

/// What the chamber prompt reads on each evaluation.
struct ChamberStallInputs {
    bool job_active = false;   ///< a job holds the machine (printing, paused or preparing)
    bool chamber_wait = false; ///< the job is held by a heater wait on the chamber
    bool rejected = false;     ///< the heater turned down our last target
    int rejection_seq = 0;     ///< rejection event count; a new value is a new rejection
    int chamber_target_deci = 0;
};

enum class ChamberStallAction { None, Prompt, Toast };

/// Decides when a chamber that is not heating is put to the user, so that the
/// common case (the appliance retries a conflict and heats) stays invisible
/// and the rare one never sits silent.
///
/// - A print held by a chamber wait while the heater refused our target:
///   prompt once per rejection, so a Try Again that is refused again asks again.
/// - A print held by a chamber wait with the heater at target 0 and no
///   rejection: prompt once per job.
/// - A rejection while no job holds the machine: a toast, once per rejection.
/// - A rejection mid-print outside a chamber wait: nothing here; the
///   print-status banner carries it.
class ChamberStallArbiter {
  public:
    ChamberStallAction decide(const ChamberStallInputs& in);

    /// The prompt has nothing left to ask: the job ended, the wait cleared
    /// (the chamber reached its temperature from any source), or the heater
    /// took a target.
    static bool should_close(const ChamberStallInputs& in);

  private:
    int toasted_seq_ = 0;
    int prompted_seq_ = 0;
    bool prompted_unheated_ = false;
};

/// How Cancel print ends a print held by a chamber wait.
/// - ClearTargetThenCancel (M191): a target of 0 by the route around the
///   queue ends the wait, then printer.print.cancel; no emergency stop.
/// - EmergencyStop (TEMPERATURE_WAIT): nothing short of M112 + FIRMWARE_RESTART
///   ends it, so the user confirms that first.
/// An unknown command (a macro holding the wait, an unreadable file) takes
/// the M191 path: the target cleared in case it is one, then the cancel.
enum class ChamberCancelPath { ClearTargetThenCancel, EmergencyStop };
ChamberCancelPath chamber_cancel_path(WaitKind kind);

namespace ui {

/// The modal and toast for a chamber heater that did not start. Owned by
/// SubjectInitializer; observes the rejection latch, the chamber-wait flag,
/// the chamber target and the job-holds-machine state, and routes every
/// change through one ChamberStallArbiter.
class ChamberStallPrompt {
  public:
    ChamberStallPrompt();
    ~ChamberStallPrompt();

    ChamberStallPrompt(const ChamberStallPrompt&) = delete;
    ChamberStallPrompt& operator=(const ChamberStallPrompt&) = delete;

    /// The target a retry sends: the one the heater reported turning down,
    /// else the last chamber target this app sent, else @p wait_target_c
    /// (the waiting command's own temperature); 0 when none is known.
    static double retry_target_c(double wait_target_c = 0);

    /// Re-send retry_target_c() by the route around the gcode queue. Returns
    /// false, sending nothing, when there is no target or no such route.
    static bool retry();

    /// End the print the way @p kind's wait allows (chamber_cancel_path). The
    /// emergency-stop path asks for confirmation and acts only on it.
    static void cancel_print(WaitKind kind);

    /// The emergency-stop confirmation cancel_print() raised is open.
    static bool emergency_stop_confirm_open();

    /// The confirmed emergency stop, re-checked first: a job still holds the
    /// machine and the chamber wait it was asked for still blocks it. Returns
    /// whether it was sent.
    static bool confirm_emergency_stop();

  private:
    void evaluate();
    void show_prompt();
    void refresh_content();
    void probe_wait_line();
    /// The device's own report of its wait (M191) when it gives one, else
    /// what the line at the print position said.
    WaitLine current_wait() const;
    void on_try_again();
    void on_cancel();
    void on_wait_line(uint32_t gen, std::optional<WaitLine> line);
    void reset_wait_line();
    void run_pending_cancel();
    friend class ChamberStallModalAccess;
    friend class ChamberStallPromptTestAccess;

    /// How long a Cancel waits for the waiting command to be read.
    static constexpr uint32_t kCancelClassifyWaitMs = 3000;

    ObserverGuard rejected_observer_;
    ObserverGuard seq_observer_;
    ObserverGuard wait_observer_;
    ObserverGuard target_observer_;
    ObserverGuard text_observer_;
    ObserverGuard job_observer_;
    ChamberStallArbiter arbiter_;
    AsyncLifetimeGuard lifetime_;

    WaitLine wait_line_;
    bool wait_line_probed_ = false;
    bool wait_line_pending_ = false; ///< the read is in flight
    uint32_t wait_line_gen_ = 0;     ///< bumps per read, so a stale answer is dropped
    bool pending_cancel_ = false;    ///< Cancel tapped while the read was in flight
    LvglTimerGuard cancel_wait_timer_;
    bool retry_busy_ = false;
    std::string note_; ///< the last Try Again's outcome, "" when none
};

} // namespace ui
} // namespace helix
