// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system/update_checker.h"

#include <cstdint>
#include <functional>
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
    /// answers true. The default URL is deliberately unroutable: most tests
    /// must never reach the download thread, and a bogus host makes it obvious
    /// if something does. Pass @p download_url only when the test genuinely
    /// wants the worker to run (the download-worker lifetime tests point it at
    /// a local socket that never answers).
    static void seed_available_update(UpdateChecker& c, const std::string& version = "9.9.9",
                                      const std::string& download_url = "") {
        std::lock_guard<std::mutex> lock(c.mutex_);
        UpdateChecker::ReleaseInfo info;
        info.version = version;
        info.tag_name = "v" + version;
        info.download_url =
            download_url.empty() ? "https://invalid.test/helixscreen-pi.zip" : download_url;
        info.download_bytes = 1024;
        c.cached_info_ = std::move(info);
        c.status_ = UpdateChecker::Status::UpdateAvailable;
    }

    /// Undo seed_available_update(). The checker is a process singleton, so a
    /// seeded release outlives the test that made it unless this runs.
    static void clear(UpdateChecker& c) {
        c.clear_cache();
    }

    // ------------------------------------------------------------------
    // Post-install restart sequence
    //
    // The real terminal step is ::_exit(0), so the ordering it depends on --
    // the Complete/Restarting frames reaching the screen BEFORE the process
    // leaves -- cannot be observed without standing in for the exit. These
    // three seams do exactly that and nothing else; production never sets
    // restart_action_, so the shipped path is unchanged.
    // ------------------------------------------------------------------

    /// Replace ::_exit(0) (and the fork+exec that precedes it on unsupervised
    /// installs) with @p action. Pass nullptr to restore the real behaviour.
    /// Set it before anything can reach the restart; it is not synchronised.
    static void set_restart_action(UpdateChecker& c, std::function<void()> action) {
        c.restart_action_ = std::move(action);
    }

    /// Shrink the two "let the frame paint" holds so a test does not wait the
    /// production 2s + 1s.
    static void set_status_hold_ms(UpdateChecker& c, uint32_t complete_ms, uint32_t restart_ms) {
        c.complete_hold_ms_ = complete_ms;
        c.restart_hold_ms_ = restart_ms;
    }

    /// perform_update_restart() is once-only (the worker's backstop races the
    /// marshalled main-thread call). Reset it so a second test can run the
    /// sequence again.
    static void reset_restart_state(UpdateChecker& c) {
        c.restart_initiated_.store(false);
    }

    /// Drive the worker-thread half of the post-install sequence directly.
    /// do_install() itself runs install.sh and cannot be unit-tested.
    static void finish_install_and_restart(UpdateChecker& c, const std::string& install_root,
                                           const std::string& version) {
        c.finish_install_and_restart(install_root, version);
    }
};
