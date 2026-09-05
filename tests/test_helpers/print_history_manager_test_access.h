// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_history_manager.h"

#include <utility>
#include <vector>

namespace helix {

// Friend access to PrintHistoryManager's cache, so a test can install an exact
// jobs list instead of taking whatever the mock printer happens to generate.
// Tests of the "newest job whose file still exists" selection need one job with
// exists == false ahead of one with exists == true, which no mock profile emits.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than adding
// _for_testing() setters to the production API. Requires
// `friend class helix::PrintHistoryManagerTestAccess;` on PrintHistoryManager.
class PrintHistoryManagerTestAccess {
  public:
    /// Install `jobs` as the loaded cache, exactly as a completed fetch would.
    ///
    /// `scope` and `requested` are what the load asked for, and they are what
    /// decides the cache's fidelity: a response shorter than its limit holds
    /// every job the printer has, whatever scope requested it. Defaults install
    /// a whole-history cache, which is what tests about job selection want.
    ///
    /// Runs the production completion handler rather than assigning the cache
    /// fields: a helper that derived fidelity itself would be a second copy of
    /// that rule, and a test resting on it could not fail when the real one
    /// changed.
    static void set_loaded_jobs(PrintHistoryManager& m, std::vector<PrintHistoryJob> jobs,
                                HistoryScope scope = HistoryScope::COMPLETE,
                                int requested = PrintHistoryManager::kCompleteJobLimit) {
        complete_fetch(m, std::move(jobs), scope, requested);
    }

    /// Hold the in-flight guard down so a fetch() call is dropped, which is the
    /// state a second delete notification arrives in on a real printer (RTT).
    /// A request in flight always has a scope, and ensure_loaded() reads it to
    /// decide whether that request serves the caller, so it is set in step.
    static void set_fetching(PrintHistoryManager& m, bool fetching,
                             HistoryScope scope = HistoryScope::COMPLETE) {
        m.is_fetching_.store(fetching);
        m.in_flight_scope_.store(fetching ? static_cast<int>(scope)
                                          : PrintHistoryManager::kNoFetch);
    }

    /// Deliver a fetch result on the calling thread, as the deferred success
    /// callback does on the main thread. `requested` is the limit the request
    /// carried, which is what tells the cache whether it now holds everything.
    static void complete_fetch(PrintHistoryManager& m, std::vector<PrintHistoryJob> jobs,
                               HistoryScope scope = HistoryScope::COMPLETE,
                               int requested = PrintHistoryManager::kCompleteJobLimit) {
        m.is_fetching_.store(false);
        m.on_history_fetched(std::move(jobs), scope, requested);
    }
};

} // namespace helix
