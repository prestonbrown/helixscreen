// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h"

namespace helix {

/**
 * @brief Edge + boot-join arming state for the pre-print collector
 *
 * Holds the two values the collector-arming decision needs across calls: the
 * previous print state, and whether the next transition is the first one after
 * connecting to a printer.
 *
 * "First transition after connecting" is what distinguishes booting into an
 * already-running print (suppress the collector, we joined mid-print) from a
 * user-started reprint (run it). Because the app re-connects on every printer
 * switch, that arming must be re-established per connection, not per process.
 */
class PrintCollectorArming {
  public:
    /// Re-arm for a fresh connection. Runs on every printer switch.
    void reset() {
        // RAW_PRINT_STATE_OK: the collector arms on the printer's transitions.
        // FIRST-TICK: seeding to STANDBY is safe here ONLY because
        // is_initial_transition_ carries the "we just connected" fact
        // explicitly - that flag, not the seed, distinguishes joining a running
        // print from a user-started reprint.
        prev_state_ = PrintJobState::STANDBY;
        is_initial_transition_ = true;
    }

    bool is_initial_transition() const {
        return is_initial_transition_;
    }
    void consume_initial_transition() {
        is_initial_transition_ = false;
    }

    PrintJobState prev_state() const {
        return prev_state_;
    }
    void note_transition(PrintJobState new_state) {
        prev_state_ = new_state;
    }

  private:
    /// RAW_PRINT_STATE_OK: see reset() for the first-tick contract.
    PrintJobState prev_state_ = PrintJobState::STANDBY;
    bool is_initial_transition_ = true;
};

} // namespace helix
