// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_lane_faults.h
 * @brief Capability of a filament system whose faults name a lane
 *
 * Reached with AmsBackend::as<ILaneFaults>(). AFC implements it: its lane faults
 * name the lane, latch until cleared, and are told apart from a tool changer's
 * carriage fault. Callers spell out the answer for a backend without it.
 */

#pragma once

#include "error_event.h"

#include <optional>
#include <string>

namespace helix {

class ILaneFaults {
  public:
    /**
     * @brief current_error() told by @p cause: why the print paused
     *        (print_stop_reason), or the newest recent `!!` line, empty when
     *        nothing said.
     *
     * Reads the faulted lane from the cause when no lane is current. Without
     * this capability a non-empty cause is simply the detail of current_error(),
     * since a backend's own status text can belong to an earlier fault.
     */
    [[nodiscard]] virtual std::optional<ErrorEvent>
    current_error_with_cause(const std::string& cause) const = 0;

    /**
     * @brief True when the current ERROR is about the carriage or the changer,
     *        so a docked head may not be where the changer thinks.
     *
     * Asked only while the action reads ERROR. Without this capability every
     * error is a changer error exactly where carriage_can_be_empty(); this
     * answers true only for the carriage's own faults.
     */
    [[nodiscard]] virtual bool fault_concerns_carriage() const = 0;

    /**
     * @brief Whether this slot carries a latched fault that clear_fault() clears
     *
     * A fault can outlive both the operation that raised it and the error dialog
     * whose dismissal clears it: a recovery action that closes the dialog leaves
     * the firmware still reporting the error, with nothing else on screen able to
     * reach clear_fault(). The lane menu offers "Clear error" on this.
     *
     * False while an operation that may clear the fault itself is still running.
     */
    [[nodiscard]] virtual bool has_clearable_fault(int slot_index) const = 0;

  protected:
    ~ILaneFaults() = default;
};

} // namespace helix
