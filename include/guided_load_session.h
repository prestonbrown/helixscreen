// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file guided_load_session.h
 * @brief Runs one guided filament load against the printer, with no widgets.
 *
 * Two kinds of load walk the same steps (push the filament in, extrude until new
 * filament shows, Done):
 *   - a printer's stock load wizard, once its first macro has run
 *     (filament_macros::guided_load());
 *   - a manual load (prestonbrown/helixscreen#2000), for a head the user feeds by
 *     hand: the screen heats the nozzle through FilamentOpHeater first.
 *
 * GuidedLoadFlow decides every step; this class sends what it returns, feeds it
 * the replies, and ends it when the printer goes away or a job takes the machine.
 * GuidedLoadModal is the view over it.
 *
 * Heat for a manual load is FilamentOpHeater's: it heats, and while the user
 * pushes it holds the heat (FilamentOpHeater::hold_heat()), so teardown and
 * SIGTERM turn the heater off. Every ending reports to it once: Done schedules the
 * post-op cooldown (op_succeeded()), every other ending turns the heater off
 * (op_cancelled()).
 */

#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "filament_macro_profiles.h"
#include "filament_op_heater.h"
#include "guided_load_flow.h"
#include "print_lifecycle_state.h"

#include <functional>
#include <string>

namespace helix {
class AmsBackend;
class PrinterState;
} // namespace helix

namespace helix::ui {

/// Why a manual load cannot start, or None.
enum class ManualLoadRefusal : int {
    None = 0,
    JobHoldsMachine, ///< Printing, paused or preparing: the heater and the extruder are the job's
    NotReady,        ///< Not connected, or Klippy not ready
    PreheatPending,  ///< Another filament op is heating the nozzle
    ColdPullRunning, ///< The cold pull owns the heater
    ToolUnknown,     ///< The slot names no tool the printer has an extruder for
    ToolNotActive,   ///< The slot's nozzle is not the active extruder, which is what G1 E moves
};

/// Pure: the refusal for these inputs, checked in the order of the enum.
/// @param slot_extruder manual_load_extruder() for the slot; empty when unknown.
[[nodiscard]] ManualLoadRefusal manual_load_refusal(PrintState lifecycle, bool printer_reachable,
                                                    bool preheat_pending, bool cold_pull_running,
                                                    const std::string& slot_extruder,
                                                    const std::string& active_extruder);

/// The Klipper extruder a manual load on @p slot pushes: the active one for a
/// negative slot or no backend, else the slot's mapped tool's. Empty when the slot
/// names no tool, or a tool with no extruder the printer reports: a load there must
/// refuse, not fall back to the active nozzle.
[[nodiscard]] std::string manual_load_extruder(AmsBackend* backend, int slot, PrinterState& state);

/// manual_load_refusal() for @p slot (negative: the active nozzle) against the live printer.
[[nodiscard]] ManualLoadRefusal manual_load_refusal_now(PrinterState& state, int slot);

/// Translated toast text for @p refusal; empty for None.
[[nodiscard]] std::string manual_load_refusal_text(ManualLoadRefusal refusal);

class GuidedLoadSession {
  public:
    /// The printer's own wizard, once its first macro has run.
    GuidedLoadSession(PrinterState& state, filament_macros::GuidedLoad steps, bool waits_for_push);
    /// A manual load on the nozzle @p slot maps to (negative: the active one).
    /// begin() starts the heat.
    GuidedLoadSession(PrinterState& state, int slot);
    /// A live session ends as cancel() does.
    ~GuidedLoadSession();

    GuidedLoadSession(const GuidedLoadSession&) = delete;
    GuidedLoadSession& operator=(const GuidedLoadSession&) = delete;

    /// Runs on the main thread whenever the stage or the heat reading changes.
    void set_on_changed(std::function<void()> cb) {
        on_changed_ = std::move(cb);
    }

    /// Starts watching the printer and, for a manual load, heating. Once.
    void begin();

    // User events.
    void extrude();
    void done();
    void cancel();
    /// The filament sensor went from empty to detected.
    void filament_detected();

    [[nodiscard]] GuidedLoadStage stage() const {
        return flow_.stage();
    }
    [[nodiscard]] bool ended() const {
        return flow_.ended();
    }
    [[nodiscard]] bool manual() const {
        return manual_;
    }
    /// While Heating: the nozzle's reading and the temperature waited for.
    [[nodiscard]] int heat_current_c() const {
        return heat_current_c_;
    }
    [[nodiscard]] int heat_target_c() const {
        return heat_target_c_;
    }

  private:
    void on_heat_progress(const PreheatProgress& progress);
    void on_heated(FilamentOpTicket ticket);
    /// The session cannot go on (a job took the machine, the printer went away):
    /// end it sending nothing more. @p toast, when set, tells the user why.
    void lose(const char* why, const char* toast = nullptr);
    /// A manual load pushes the extruder it heated; any other active one is a
    /// different nozzle. Always true for the printer's own wizard.
    [[nodiscard]] bool on_heated_extruder() const;
    void on_printer_changed();
    void send(const std::string& script, bool report_outcome);
    void on_command_finished(bool ok);
    /// The heater's share of the ending, once.
    void finish();
    void changed();

    PrinterState& state_;
    GuidedLoadFlow flow_;
    bool manual_ = false;
    int slot_ = -1;
    /// manual_load_extruder() at construction; empty for the wizard or an unknown tool.
    std::string extruder_;
    bool begun_ = false;
    bool finished_ = false;
    bool heat_requested_ = false;
    FilamentOpTicket ticket_ = 0;
    int heat_current_c_ = 0;
    int heat_target_c_ = 0;
    std::function<void()> on_changed_;
    ObserverGuard lifecycle_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard klippy_observer_;
    ObserverGuard active_extruder_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
