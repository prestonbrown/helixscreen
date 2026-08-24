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
    static void set_loaded_jobs(PrintHistoryManager& m, std::vector<PrintHistoryJob> jobs) {
        m.cached_jobs_ = std::move(jobs);
        m.build_filename_stats();
        m.is_loaded_ = true;
    }

    /// Hold the in-flight guard down so a fetch() call is dropped, which is the
    /// state a second delete notification arrives in on a real printer (RTT).
    static void set_fetching(PrintHistoryManager& m, bool fetching) {
        m.is_fetching_.store(fetching);
    }

    /// Deliver a fetch result on the calling thread, as the deferred success
    /// callback does on the main thread.
    static void complete_fetch(PrintHistoryManager& m, std::vector<PrintHistoryJob> jobs) {
        m.is_fetching_.store(false);
        m.on_history_fetched(std::move(jobs));
    }
};

} // namespace helix
