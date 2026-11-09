// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "print_start_phase.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// phase_enum -> declared seconds; 0 means the phase will not run
using PreprintDeclaredDurations = std::map<int, int>;

inline constexpr size_t kMaxPlanLineBytes = 512;
inline constexpr size_t kMaxPlanTokens = 32;
/// A typo must not hold Preparing open for days; six hours is past any real soak.
inline constexpr int kMaxDeclaredSeconds = 21600;

/// One HELIX:PLAN console line, parsed.
struct PrintStartPlanLine {
    PreprintDeclaredDurations durations; ///< valid tokens, last duplicate wins
    std::vector<std::string> dropped;    ///< rejected tokens, at most 4 kept for the warning
    bool rejected_whole = false;         ///< over a bound; durations empty
};

/// False for the transitions a macro cannot give a duration to
[[nodiscard]] bool phase_accepts_declared_duration(PrintStartPhase phase);

/// @p seconds as whole declared seconds, or nullopt when it is not finite or out of range
[[nodiscard]] std::optional<int> declared_seconds_from_number(double seconds);

/// nullopt when the line is not a plan line; a plan line always yields a value
[[nodiscard]] std::optional<PrintStartPlanLine> parse_print_start_plan(std::string_view line);

/// Merge @p incoming into @p plan: a finished phase is ignored, the current phase
/// value becomes @p current_elapsed_seconds + value (0 stays 0), anything else replaces
void merge_declared(PreprintDeclaredDurations& plan, const PreprintDeclaredDurations& incoming,
                    int current_phase, int current_elapsed_seconds,
                    const std::vector<int>& finished_phases);

/// Signed overtime: finished phases (actual - expected) plus the current phase overrun
[[nodiscard]] int overtime_balance_seconds(const std::map<int, int>& expected,
                                           const std::map<int, int>& actual_finished,
                                           int current_phase, int current_elapsed_seconds);

/// Whether the current phase has outlasted the duration the macro declared for it
[[nodiscard]] bool declared_phase_running_over(const PreprintDeclaredDurations& declared,
                                               int current_phase, int current_elapsed_seconds);

/// Overlay a plan on the checklist inputs: declared seconds replace predicted ones,
/// a declared phase joins @p expected_order at its enum position, and a phase
/// declared 0 leaves both
void apply_declared_to_checklist(const PreprintDeclaredDurations& declared,
                                 std::vector<PrintStartPhase>& expected_order,
                                 std::map<int, int>& predicted_seconds);

} // namespace helix
