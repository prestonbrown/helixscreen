// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When a filament system's standing fault is raised as a dialog: the backend
// reports one while the print is paused, or while no print is running. AFC
// prints its `!!`, pauses, and may have held error_state since an earlier
// fault, so neither the ERROR action edge nor the pause alone is a reliable
// trigger; this is fed on every change of either. Shared by AmsErrorBridge
// (desktop) and the firmware's fault dialog.
//
// Compiled into the ESP32 firmware.

#include <string>

namespace helix {

/// Why a filament fault stands: why the print paused (@p stop_reason) while it
/// is @p paused, else the newest recent `!!` line (@p recent_error), else
/// empty. AmsState holds the first non-empty answer for the life of the fault.
///
/// A stop reason that predates a filament operation run during the pause
/// (@p stop_reason_predates_op) explains no fault after it: that fault is the
/// operation's. The backend's own text for it (@p backend_has_detail) names
/// it, so the answer is empty and the backend's text shows; without any, the
/// `!!` line is the description there is.
inline std::string standing_fault_cause(bool paused, const std::string& stop_reason,
                                        const std::string& recent_error,
                                        bool stop_reason_predates_op = false,
                                        bool backend_has_detail = false) {
    if (paused && stop_reason_predates_op) {
        return backend_has_detail ? std::string() : recent_error;
    }
    if (paused && !stop_reason.empty()) {
        return stop_reason;
    }
    return recent_error;
}

/// What the dialog does on an update: nothing, raise the fault, or replace the
/// backend's own text it shows with the cause that now explains the fault.
enum class StandingFaultStep { None, Raise, Explain };

/// One fault is raised once. It stands from the update that first sees it
/// until one sees it clear or the print run, and its text is not its identity:
/// while it stands the `!!` line ages out of its window and leaves the
/// backend's own text, which can belong to an earlier fault, and the stop
/// reason can arrive later from gcode_store.
class StandingFaultTrigger {
  public:
    /// @p fault holds while the print is @p paused or not @p printing.
    /// @p explained: the fault's text is its cause (the stop reason or a
    /// recent `!!` line), not the backend's own. Raise the first time; Explain
    /// once, when a fault raised unexplained gains a cause. A fault that
    /// clears, or a print that starts or resumes, arms it again.
    StandingFaultStep update(bool fault, bool paused, bool printing, bool explained) {
        if (!fault || (!paused && printing)) {
            raised_ = false;
            explained_ = false;
            return StandingFaultStep::None;
        }
        if (!raised_) {
            raised_ = true;
            explained_ = explained;
            return StandingFaultStep::Raise;
        }
        if (explained && !explained_) {
            explained_ = true;
            return StandingFaultStep::Explain;
        }
        return StandingFaultStep::None;
    }

    /// True while a raised fault still stands.
    [[nodiscard]] bool raised() const {
        return raised_;
    }

  private:
    bool raised_ = false;
    bool explained_ = false;
};

} // namespace helix
