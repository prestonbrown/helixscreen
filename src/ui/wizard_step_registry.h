// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step.h"       // helix::wizard::Step, StepId, StepContext
#include "wizard_step_logic.h" // helix::StepSkip

#include <vector>

namespace helix {
namespace wizard {

/// Ordered list of all wizard steps, indexed by StepId (0..STEP_COUNT-1).
/// Returned BY VALUE and rebuilt every call — the step singletons can be
/// destroyed and recreated by StaticPanelRegistry across wizard sessions, so the
/// pointers must NOT be cached (a stale cache dangled and crashed on a 3rd
/// printer add). The get_wizard_*_step() accessors recreate on demand.
std::vector<Step*> steps();

/// Step for the given id, or nullptr if the id is out of range.
Step* step_by_id(StepId id);

/// Build a StepContext from current global state (Config + Moonraker API).
StepContext build_context();

/// Per-step skip decisions in StepId order, given a context.
std::vector<helix::StepSkip> skip_vector(const StepContext& ctx);

} // namespace wizard
} // namespace helix
