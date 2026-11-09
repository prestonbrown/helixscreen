// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file timelapse_local_installer.h
 * @brief Installs moonraker-timelapse on the host HelixScreen shares with Moonraker
 *
 * What moonraker-timelapse's own `make install` does, minus the steps that
 * need a password: clone the repository, link component/timelapse.py into
 * Moonraker's components directory and klipper_macro/timelapse.cfg into the
 * config directory (both through install.sh --link-timelapse, which knows
 * where Moonraker lives), then make sure ffmpeg is there. ffmpeg is installed
 * only through passwordless sudo; without it the user is shown the command.
 * The [timelapse] config sections and the Moonraker restart are the setup
 * wizard's own steps.
 */

#include "system/process_runner.h"

#include <functional>
#include <string>

namespace helix {

/// The repository the local install clones. The [update_manager timelapse]
/// section the setup wizard writes names the same one at ~/moonraker-timelapse.
inline constexpr const char* kTimelapseRepoUrl =
    "https://github.com/mainsail-crew/moonraker-timelapse.git";
inline constexpr const char* kTimelapseRepoDirName = "moonraker-timelapse";

/// What a user runs to install ffmpeg when HelixScreen cannot.
inline constexpr const char* kFfmpegInstallCommand = "sudo apt-get install -y ffmpeg";

/// The route the setup wizard takes once it knows whether the plugin answers.
enum class TimelapseSetupPath {
    AlreadyInstalled,   ///< the plugin answers; nothing to install
    LocalInstall,       ///< Moonraker runs on this host: HelixScreen installs the plugin
    RemoteInstructions, ///< HelixScreen cannot install it: show the SSH commands
    WaitForJob,         ///< a local install, held while a job holds the machine
};

struct TimelapseSetupFacts {
    bool plugin_present = false;      ///< the timelapse plugin answers Moonraker's API
    bool moonraker_local = false;     ///< Moonraker is known to run on this host
    bool can_install_locally = false; ///< this build can run the install (install.sh found)
    bool job_holds = false;           ///< job_holds_machine() of the current print lifecycle
};

/**
 * @brief Pick the wizard's route
 *
 * A Moonraker whose locality is not known yet counts as remote: the SSH
 * commands are right on either host, a local install is not. A local install
 * never starts while a job holds the machine, as it ends in a Moonraker restart.
 */
TimelapseSetupPath decide_timelapse_setup_path(const TimelapseSetupFacts& facts);

/// The step a local install is on, or the one it failed in.
enum class TimelapseInstallStage {
    Clone,         ///< git clone of the repository
    Link,          ///< install.sh --link-timelapse
    CheckFfmpeg,   ///< looking for ffmpeg
    InstallFfmpeg, ///< apt-get through passwordless sudo
};

enum class TimelapseInstallOutcome {
    Installed,         ///< plugin linked, ffmpeg present
    InstalledNoFfmpeg, ///< plugin linked; ffmpeg missing and not installable without a password
    Failed,            ///< plugin not installed; failed_stage names the step
};

struct TimelapseInstallResult {
    TimelapseInstallOutcome outcome = TimelapseInstallOutcome::Failed;
    TimelapseInstallStage failed_stage = TimelapseInstallStage::Clone;
    /// The failing command's last output line, for the log and the screen; empty on success.
    std::string detail;
};

/// Where a local install works.
struct TimelapseInstallPlan {
    std::string install_script; ///< install.sh beside this app
    std::string repo_dir;       ///< the clone: ~/moonraker-timelapse
    bool repo_present = false;  ///< repo_dir already holds component/timelapse.py
};

/// The plan for @p home and @p install_script; looks at the filesystem.
TimelapseInstallPlan make_timelapse_install_plan(const std::string& home,
                                                 const std::string& install_script);

/**
 * @brief Run a local install through @p run
 *
 * Blocks for as long as the commands take (an ffmpeg install through apt can
 * take minutes), so it belongs on a worker thread. @p on_stage, when set, is
 * called on that same thread as each step starts.
 */
TimelapseInstallResult
install_timelapse_locally(const TimelapseInstallPlan& plan, const ProcessRunner& run,
                          const std::function<void(TimelapseInstallStage)>& on_stage = {});

/// The runner a local install uses: real processes, or the simulated host under --test.
ProcessRunner timelapse_install_runner();

} // namespace helix
