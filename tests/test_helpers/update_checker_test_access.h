// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system/update_checker.h"

#include <mutex>
#include <string>
#include <utility>

// Test-only seam. UpdateChecker declares this class as a friend, so a test can
// put the singleton into the "an update is waiting" state without a manifest
// fetch. has_update_available() is `status_ == UpdateAvailable && cached_info_`,
// and both halves are private — every consumer of "is there an update" (the
// About row, the upgrade nudge, start_download) is unreachable in a test until
// they are seeded together.
//
// Kept out of the production header so no shipped code path can manufacture a
// fake release (the "no _for_testing methods in headers" lint, L065/L088).
class UpdateCheckerTestAccess {
  public:
    /// Seed a cached release and flip the status so has_update_available()
    /// answers true. The URL is deliberately unroutable: nothing in these tests
    /// should reach the download thread, and a bogus host makes it obvious if
    /// something does.
    static void seed_available_update(UpdateChecker& c, const std::string& version = "9.9.9") {
        std::lock_guard<std::mutex> lock(c.mutex_);
        UpdateChecker::ReleaseInfo info;
        info.version = version;
        info.tag_name = "v" + version;
        info.download_url = "https://invalid.test/helixscreen-pi.zip";
        info.download_bytes = 1024;
        c.cached_info_ = std::move(info);
        c.status_ = UpdateChecker::Status::UpdateAvailable;
    }

    /// Undo seed_available_update(). The checker is a process singleton, so a
    /// seeded release outlives the test that made it unless this runs.
    static void clear(UpdateChecker& c) {
        c.clear_cache();
    }
};
