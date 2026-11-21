// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Starting a vendor firmware update and following its log. Every start goes through
// start_vendor_apply(), which refuses unless the row's apply is enabled and nothing
// blocks; tests hand it a fake ProcessRunner, so no test or mock ever runs a flasher.

#include "firmware_update.h"
#include "system/process_runner.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::fwupdate {

enum class StartOutcome {
    Started,
    ApplyDisabled, ///< the row has no apply plan, or needs verification and the flag is off
    Blocked,       ///< first_blocker() found a reason; StartResult::blocker says which
    LaunchFailed,  ///< the launcher did not run or did not exit 0
};

struct StartResult {
    StartOutcome outcome = StartOutcome::ApplyDisabled;
    Blocker blocker = Blocker::None;
    std::string error; ///< LaunchFailed: the launcher's last output line or spawn error
};

/// Start @p plan for @p p. @p inputs are the preconditions as read right before this
/// call; the plan's lock file, when present, counts as an update running.
/// BLOCKING (runs the launcher, which returns at once): never on the LVGL thread.
StartResult start_vendor_apply(const Provider& p, const ApplyPlan& plan, ApplyInputs inputs,
                               bool unverified_allowed, const ProcessRunner& run);

/// A new directory under @p parent that only this process's user can enter (mode
/// 0700), for one run's log and pid file: in a shared /tmp, a fixed name would let
/// another local user plant a symlink the root-run flasher then writes through.
/// nullopt when it cannot be created.
std::optional<std::string> make_private_run_dir(const std::string& parent);

/// Whether the process whose pid @p pid_file holds is still alive. False when the
/// file is missing, unreadable or holds no pid.
bool pid_file_process_alive(const std::string& pid_file);

/// Where a running vendor flash stands when its log says nothing conclusive.
enum class VendorRunCheck {
    Running,    ///< alive and not silent for too long
    Restarting, ///< gone after the point of no return: the printer is rebooting
    Unknown,    ///< gone without an end line, or silent past the limit: check the printer
};

/// The liveness decision. Never claims success or failure: only an end line in the
/// log does that.
VendorRunCheck check_vendor_run(bool process_alive, bool reached_no_return,
                                std::chrono::seconds silent_for,
                                std::chrono::seconds silence_limit);

/// Reads the lines a growing log file gains between calls. A partial last line waits
/// for its newline. A file that shrinks (rotated, truncated) is read from its start.
class LogTail {
  public:
    explicit LogTail(std::string path) : path_(std::move(path)) {}
    /// Complete lines appended since the last call; empty when there are none or the
    /// file does not exist yet.
    std::vector<std::string> poll();

  private:
    std::string path_;
    uint64_t offset_ = 0;
    std::string partial_;
};

} // namespace helix::fwupdate
