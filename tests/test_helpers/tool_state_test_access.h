// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tool_state.h"

namespace helix {

/**
 * @brief Reaches ToolState's spool-persistence bookkeeping for tests.
 *
 * `spool_dirty_` is the flag `save_spool_assignments_if_dirty()` consults, and
 * whether a given `assign_spool()` call sets it is the whole point of the
 * identity-vs-weight split (see ToolState::assign_spool). Nothing in production
 * reads or clears it directly, so it stays private and the tests come through
 * here rather than widening the class's API.
 */
class ToolStateTestAccess {
  public:
    /// Whether a spool assignment has changed since the last save.
    [[nodiscard]] static bool spool_dirty(const ToolState& ts) {
        return ts.spool_dirty_;
    }

    /// Drop the unsaved-changes flag without writing, so a test can assert on
    /// what the NEXT assign_spool() call does rather than on setup residue.
    static void clear_spool_dirty(ToolState& ts) {
        ts.spool_dirty_ = false;
    }
};

} // namespace helix
