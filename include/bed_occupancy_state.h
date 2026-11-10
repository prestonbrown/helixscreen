// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Owns "a finished print may still be on the bed" for the active printer.
//
// Created per printer session beside PrintHistoryManager and reached through
// get_bed_occupancy_state(). It loads the RECENT history on connect itself (no widget has
// to be on screen), re-decides on every history change, print-lifecycle change and
// Cleared tap, captures the exclude-object outlines when a print ends, and persists the
// acknowledged job, the last occupying job and the outline snapshot under the printer's
// config prefix, so a restart of the app or of Klipper forgets none of them. Until the
// RECENT history has loaded, the last occupying job stands in for the newest job, and a
// failed load is retried every kHistoryRetryMs while connected. Main thread only.

#include "async_lifetime_guard.h"
#include "bed_occupancy.h"
#include "core/observer_guard.h"
#include "core/timer_guard.h"
#include "print_history_manager.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix {

class PrinterState;

/// Per-printer config keys, under Config::df().
inline constexpr const char* kBedClearedJobKey = "bed_occupancy/cleared";
inline constexpr const char* kBedLastOccupiedKey = "bed_occupancy/last_occupied";
inline constexpr const char* kBedOutlineSnapshotKey = "bed_occupancy/outlines";

/// How often a RECENT history load that has not succeeded is asked for again while connected.
inline constexpr uint32_t kHistoryRetryMs = 10000;

class BedOccupancyState {
  public:
    BedOccupancyState(PrinterState& printer_state, PrintHistoryManager* history);
    ~BedOccupancyState();

    BedOccupancyState(const BedOccupancyState&) = delete;
    BedOccupancyState& operator=(const BedOccupancyState&) = delete;

    /// Registers "bed_occupied" (0/1) and "bed_occupancy_version" (bumped on every change
    /// of the verdict, the outlines or whether a print holds the machine) and starts observing
    /// history and the print lifecycle.
    void init_subjects();
    void deinit_subjects();

    [[nodiscard]] const BedOccupancy& current() const {
        return current_;
    }

    /// Outlines to draw: the live exclude-object geometry while it is populated, else the
    /// persisted snapshot when it belongs to the occupying job's file. Empty when not
    /// occupied and no print is active.
    [[nodiscard]] std::vector<BedObjectOutline> outlines() const;

    /// The user says the bed is clear: persist the occupying job's key and re-decide.
    /// Does nothing when not occupied.
    void mark_cleared();

    lv_subject_t* get_occupied_subject() {
        return &occupied_;
    }
    lv_subject_t* get_version_subject() {
        return &version_;
    }
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /// Re-run the decision from the history and lifecycle as they are now. Public so a test
    /// can drive it after changing the history mock.
    void refresh();

    /// Stop observing the history manager. The session calls it before it destroys the
    /// manager, which happens before this object is destroyed.
    void detach_history();

  private:
    friend class BedOccupancyStateTestAccess;

    void on_history_changed();
    void on_print_lifecycle(PrintState state);
    void capture_outline_snapshot();
    /// The history's newest job once RECENT has loaded, else the persisted last occupying job.
    [[nodiscard]] std::optional<BedJobSummary> newest_job() const;
    [[nodiscard]] bool history_loaded() const;
    /// Record the history's verdict as the last occupying job, or forget it when clear.
    void persist_last_occupied(const BedOccupancy& verdict,
                               const std::optional<BedJobSummary>& job);
    void on_connection_state(int state);
    void start_history_retry();
    static void history_retry_cb(lv_timer_t* timer);

    PrinterState& printer_state_;
    PrintHistoryManager* history_;
    BedOccupancy current_;
    std::optional<BedOutlineSnapshot> snapshot_;
    std::optional<BedJobSummary> last_occupied_;
    helix::HistoryChangedCallback history_cb_;

    SubjectManager subjects_;
    lv_subject_t occupied_{};
    lv_subject_t version_{};
    bool subjects_initialized_ = false;

    void bump_version();

    /// The lifecycle the last observer call saw, so only a real end of a print captures.
    PrintState last_lifecycle_ = PrintState::Idle;
    bool history_attached_ = false;

    ObserverGuard lifecycle_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard objects_observer_;
    ui::LvglTimerGuard history_retry_timer_;

    // Declared last so it is torn down first.
    AsyncLifetimeGuard lifetime_;
};

/// Global accessor, set by the printer session; nullptr outside a session.
BedOccupancyState* get_bed_occupancy_state();
void set_bed_occupancy_state(BedOccupancyState* state);

} // namespace helix
