// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file timelapse_local_installer.h
 * @brief Installs moonraker-timelapse on the host HelixScreen shares with Moonraker
 *
 * moonraker-timelapse's data for the shared local plugin installer
 * (local_plugin_installer.h), and the setup wizard's route decision. The recipe
 * does what moonraker-timelapse's own `make install` does, minus the steps that
 * need a password: clone the repository, link component/timelapse.py and
 * klipper_macro/timelapse.cfg (install.sh --link-timelapse), and make sure
 * ffmpeg is there. The [timelapse] config sections and the Moonraker restart are
 * the setup wizard's own steps, over Moonraker's API, so the remote route
 * shares them.
 */

#include "local_plugin_installer.h"
#include "system/process_runner.h"

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

/// moonraker-timelapse's recipe for the local plugin installer: clone it to
/// ~/moonraker-timelapse, `install.sh --link-timelapse` links its component and
/// macro file, and it needs ffmpeg to render videos.
const LocalPluginRecipe& timelapse_plugin_recipe();

/// The runner a local install uses: real processes, or the simulated host under --test.
ProcessRunner timelapse_install_runner();

} // namespace helix
