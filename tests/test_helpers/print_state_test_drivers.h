// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file print_state_test_drivers.h
 * @brief How a test should drive print state now that consumers read the lifecycle.
 *
 * Two helpers, both consequences of the same migration:
 *
 *   set_wire_state() / wire_name() — drive print state through
 *   update_from_status(), the real input, so print_lifecycle is republished.
 *   Writing print_state_enum directly no longer reaches anything.
 *
 *   lifecycle_from_bools() — an adapter for suites that feed
 *   print_blocks_filament_op() through lambdas whose SECTIONs read
 *   `can_load(/*printing=* /true, ...)`. Rewriting those call sites wholesale
 *   would be churn that obscures the change it documents.
 *
 * lifecycle_from_bools() deliberately cannot produce PrintState::Preparing —
 * that is the case the bool pair could never express, and it is what the
 * migration exists to fix, so tests must reach it by naming the enum value.
 */

#include "print_lifecycle_state.h"
#include "printer_state.h"

#include "hv/json.hpp"

namespace helix::test {

/**
 * @brief The Moonraker wire string for a PrintJobState.
 *
 * Tests that used to drive print state by writing `print_state_enum` directly
 * must go through `PrinterState::update_from_status()` instead: consumers now
 * gate on `print_lifecycle`, which is published by `publish_lifecycle_state()`
 * from inside `update_from_status()`. Writing the enum subject by hand leaves
 * the lifecycle stale, so the code under test never re-gates and the assertion
 * fails as though the production guard were missing.
 *
 * Production cannot desync the two - `printer_print_state.cpp` has exactly one
 * writer of `print_state_enum_` and `publish_lifecycle_state()` is the next
 * statement - so driving the real input is both the correct and the faithful
 * thing to do.
 */
[[nodiscard]] inline const char* wire_name(PrintJobState s) {
    switch (s) {
    case PrintJobState::PRINTING:
        return "printing";
    case PrintJobState::PAUSED:
        return "paused";
    case PrintJobState::COMPLETE:
        return "complete";
    case PrintJobState::CANCELLED:
        return "cancelled";
    case PrintJobState::ERROR:
        return "error";
    case PrintJobState::STANDBY:
    default:
        return "standby";
    }
}

/// Drive print state the way Moonraker does, so print_lifecycle is republished.
inline void set_wire_state(PrinterState& st, PrintJobState s) {
    st.update_from_status(nlohmann::json{{"print_stats", {{"state", wire_name(s)}}}});
}

[[nodiscard]] inline PrintState lifecycle_from_bools(bool printing, bool paused) {
    if (printing) {
        return PrintState::Printing;
    }
    if (paused) {
        return PrintState::Paused;
    }
    return PrintState::Idle;
}

} // namespace helix::test
