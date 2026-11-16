// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_patterns.h"
#include "print_start_phase.h"

#include <optional>
#include <string_view>
#include <vector>

namespace helix {

/// Untranslated i18n key of a phase's short step name ("Z tilt"): what a
/// widget stores as its translation tag. "" for IDLE, INITIALIZING and
/// COMPLETE.
[[nodiscard]] const char* print_start_phase_step_key(PrintStartPhase phase);

/// Short translated step name for a pre-print checklist row, no ellipsis.
/// "" for IDLE, INITIALIZING and COMPLETE.
[[nodiscard]] const char* print_start_phase_step_label(PrintStartPhase phase);

/// Long translated banner text while a phase runs, with its ellipsis
/// ("Z Tilt Adjust..."). "" for IDLE.
[[nodiscard]] const char* print_start_phase_signal_message(PrintStartPhase phase);

/// A pre-print option that toggles a phase.
struct PhaseOption {
    std::string_view id;
    PrintStartPhase phase;
};

/// Every pre-print option id that toggles a phase, with that phase.
[[nodiscard]] const std::vector<PhaseOption>& print_start_phase_options();

/// The phase a pre-print option id toggles: bed_mesh, qgl, z_tilt,
/// nozzle_clean, purge_line, heat_soak. nullopt for every other id (timelapse,
/// ai_detect, ...).
[[nodiscard]] std::optional<PrintStartPhase>
print_start_phase_of_option(std::string_view option_id);

/// The one OperationCategory -> PrintStartPhase mapping. nullopt for
/// categories that are not a pre-print phase of their own (BED_LEVEL,
/// SKEW_CORRECT, START_PRINT, UNKNOWN).
[[nodiscard]] std::optional<PrintStartPhase> phase_of_category(OperationCategory category);

} // namespace helix
