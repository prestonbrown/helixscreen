// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The print status pre-print checklist: which start-sequence steps are done,
// which one runs now and which are still to come, with a time for each. Pure,
// so every rule is unit-testable without LVGL or a collector.

#include "print_start_phase.h"

#include <cstddef>
#include <map>
#include <vector>

namespace helix {

enum class PreprintStepStatus { Done = 0, Current = 1, Upcoming = 2 };

struct PreprintStepRow {
    PrintStartPhase phase = PrintStartPhase::IDLE;
    PreprintStepStatus status = PreprintStepStatus::Upcoming;
    /// Done: seconds it took. Current: predicted seconds left (never below 0).
    /// Upcoming: predicted seconds. -1 when nothing predicts it.
    int seconds = -1;
};

/// What the collector knows right now.
struct PreprintChecklistInput {
    std::vector<PrintStartPhase> done_in_order; ///< phases already finished, oldest first
    PrintStartPhase current = PrintStartPhase::IDLE;
    int current_elapsed_seconds = 0;
    std::map<int, int> actual_seconds; ///< phase int -> seconds spent, for done phases
    /// The start sequence as far as anything predicts it: the file's start
    /// block, else the macro analysis. Empty when nothing does.
    std::vector<PrintStartPhase> expected_order;
    std::map<int, int> predicted_seconds; ///< phase int -> seconds, from history
};

/// Rows the XML declares; the list never grows past this.
inline constexpr size_t kPreprintChecklistMaxRows = 8;

/// The rows, oldest first. IDLE, INITIALIZING and COMPLETE never appear.
/// An expected step ahead of the current one in expected_order that never
/// ran visibly is a done row with no time, in its expected place.
/// Upcoming rows come only from expected_order: with it empty, the list is the
/// done rows and the current one. Past kPreprintChecklistMaxRows, the oldest
/// done rows drop first; the current row never drops.
[[nodiscard]] std::vector<PreprintStepRow>
build_preprint_checklist(const PreprintChecklistInput& in);

/// Compact form: 1-based position of the current step and the step count,
/// both 0 when there is no current step.
struct PreprintStepPosition {
    int index = 0;
    int count = 0;
};
[[nodiscard]] PreprintStepPosition preprint_step_position(const std::vector<PreprintStepRow>& rows);

} // namespace helix
